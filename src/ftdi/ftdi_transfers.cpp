#include "ftdi_internal.h"

#include <cstring>

#include <freertos/task.h>

// Bulk and control transfer plumbing: completion callbacks (USB client
// task), submit helpers and idle checks (service task).

namespace
{
    using namespace Ftdi;

    constexpr uint8_t FTDI_LSR_OVERRUN = 0x02;
    constexpr uint8_t FTDI_LSR_FRAMING = 0x08;
    constexpr uint8_t FTDI_LSR_BREAK = 0x10;


    bool isTransferError(usb_transfer_status_t status)
    {
        return status != USB_TRANSFER_STATUS_COMPLETED &&
               status != USB_TRANSFER_STATUS_NO_DEVICE &&
               status != USB_TRANSFER_STATUS_CANCELED;
    }


    // Strips the two status bytes from every packet and queues the payload.
    void consumePackets(usb_transfer_t *transfer)
    {
        uint8_t *buffer = transfer->data_buffer;
        size_t remaining = transfer->actual_num_bytes;
        size_t offset = 0;
        size_t payload = 0;

        while (remaining > 0)
        {
            size_t packet = remaining < inPacketSize ? remaining : inPacketSize;
            if (packet >= FTDI_STATUS_BYTES)
            {
                uint8_t lineStatus = buffer[offset + 1];
                if (lineStatus & FTDI_LSR_OVERRUN)
                    overrunCount++;
                if (lineStatus & FTDI_LSR_FRAMING)
                    framingCount++;

                bool inBreak = lineStatus & FTDI_LSR_BREAK;
                if (inBreak && !rxInBreak)
                    breakCount++;
                rxInBreak = inBreak;

                size_t length = packet - FTDI_STATUS_BYTES;
                memmove(buffer + payload, buffer + offset + FTDI_STATUS_BYTES, length);
                payload += length;
            }
            offset += packet;
            remaining -= packet;
        }

        if (payload > 0 && xRingbufferSend(rxRing, buffer, payload, 0) != pdTRUE)
            rxDropCount += payload;
    }


    void rxCallback(usb_transfer_t *transfer)
    {
        bool completed = transfer->status == USB_TRANSFER_STATUS_COMPLETED;
        if (completed)
            consumePackets(transfer);

        bool resubmitted = false;
        if (completed)
        {
            xSemaphoreTake(submitLock, portMAX_DELAY);
            resubmitted = rxEnabled && usb_host_transfer_submit(transfer) == ESP_OK;
            bool failed = rxEnabled && !resubmitted;
            xSemaphoreGive(submitLock);
            if (failed)
                notifyService(EVENT_RX_FAILED);
        }
        else if (isTransferError(transfer->status))
        {
            notifyService(EVENT_RX_FAILED);
        }

        if (!resubmitted)
            rxInFlight--;
    }


    void txCallback(usb_transfer_t *transfer)
    {
        if (isTransferError(transfer->status))
            notifyService(EVENT_TX_FAILED);

        uint8_t slot = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(transfer->context));
        xQueueSend(txFreeSlots, &slot, 0);
    }


    void controlCallback(usb_transfer_t * /*transfer*/)
    {
        xSemaphoreGive(controlDone);
    }
}


namespace Ftdi
{

void clientEvent(const usb_host_client_event_msg_t *event, void * /*arg*/)
{
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
    {
        uint8_t address = event->new_dev.address;
        newDeviceEvents++;
        xQueueSend(newDevices, &address, 0);
        notifyService(EVENT_NEW_DEVICE);
    }
    else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE && event->dev_gone.dev_hdl == openDevice)
    {
        notifyService(EVENT_DEVICE_GONE);
    }
}


void usbLibTask(void * /*arg*/)
{
    for (;;)
    {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}


void usbClientTask(void * /*arg*/)
{
    for (;;)
        usb_host_client_handle_events(client, portMAX_DELAY);
}


bool allocateTransfers()
{
    for (usb_transfer_t *&transfer : rxTransfers)
    {
        if (usb_host_transfer_alloc(RX_TRANSFER_BYTES, 0, &transfer) != ESP_OK)
            return false;
        transfer->callback = rxCallback;
    }

    for (size_t i = 0; i < TX_TRANSFER_COUNT; i++)
    {
        if (usb_host_transfer_alloc(TX_TRANSFER_BYTES, 0, &txTransfers[i]) != ESP_OK)
            return false;
        txTransfers[i]->callback = txCallback;
        txTransfers[i]->context = reinterpret_cast<void *>(i);
    }

    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &controlTransfer) != ESP_OK)
        return false;
    controlTransfer->callback = controlCallback;
    return true;
}


void prepareTransfers(usb_device_handle_t device)
{
    for (usb_transfer_t *transfer : rxTransfers)
    {
        transfer->device_handle = device;
        transfer->bEndpointAddress = inEndpoint;
        transfer->num_bytes = (RX_TRANSFER_BYTES / inPacketSize) * inPacketSize;
    }

    xQueueReset(txFreeSlots);
    for (uint8_t i = 0; i < TX_TRANSFER_COUNT; i++)
    {
        txTransfers[i]->device_handle = device;
        txTransfers[i]->bEndpointAddress = outEndpoint;
        xQueueSend(txFreeSlots, &i, 0);
    }
}


bool startRx()
{
    bool ok = true;
    xSemaphoreTake(submitLock, portMAX_DELAY);
    rxEnabled = true;
    for (usb_transfer_t *transfer : rxTransfers)
    {
        rxInFlight++;
        if (usb_host_transfer_submit(transfer) != ESP_OK)
        {
            rxInFlight--;
            ok = false;
        }
    }
    xSemaphoreGive(submitLock);
    return ok;
}


void cancelEndpoint(usb_device_handle_t device, uint8_t endpoint)
{
    usb_host_endpoint_halt(device, endpoint);
    usb_host_endpoint_flush(device, endpoint);
}


bool rxIdle()
{
    return rxInFlight == 0;
}


bool txIdle()
{
    return uxQueueMessagesWaiting(txFreeSlots) == TX_TRANSFER_COUNT;
}


bool controlIdle()
{
    if (controlOwed && xSemaphoreTake(controlDone, 0) == pdTRUE)
        controlOwed = false;
    return !controlOwed;
}


bool allIdle()
{
    return rxIdle() && txIdle() && controlIdle();
}


void setEnabled(bool &flag, bool value)
{
    xSemaphoreTake(submitLock, portMAX_DELAY);
    flag = value;
    xSemaphoreGive(submitLock);
}


bool waitUntil(bool (*done)(), uint32_t timeoutMs)
{
    uint32_t start = millis();
    while (!done())
    {
        if (millis() - start >= timeoutMs)
            return false;
        vTaskDelay(1);
    }
    return true;
}

} // namespace Ftdi
