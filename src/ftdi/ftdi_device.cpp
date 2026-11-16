#include "ftdi_internal.h"

#include <cinttypes>
#include <cstring>

#include <usb/usb_helpers.h>

// Device lifecycle: attach, scan, detach and endpoint recovery. Service
// task only.

namespace
{
    using namespace Ftdi;

    // A standby toggle can fire attach() before the device answers control
    // transfers.
    constexpr int MAX_CONFIGURE_ATTEMPTS = 5;
    constexpr uint32_t CONFIGURE_RETRY_DELAY_MS = 150;

    // Non-FTDI addresses, skipped by scanDevices() until the device count changes.
    uint8_t rejected[128 / 8] = {};
    int scannedDeviceCount = -1;


    bool findEndpoints(usb_device_handle_t device)
    {
        const usb_config_desc_t *config = nullptr;
        if (usb_host_get_active_config_descriptor(device, &config) != ESP_OK)
            return false;

        int offset = 0;
        const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config, 0, 0, &offset);
        if (!intf)
            return false;

        interfaceNumber = intf->bInterfaceNumber;
        inEndpoint = 0;
        outEndpoint = 0;
        for (int i = 0; i < intf->bNumEndpoints; i++)
        {
            int endpointOffset = offset;
            const usb_ep_desc_t *endpoint = usb_parse_endpoint_descriptor_by_index(intf, i, config->wTotalLength, &endpointOffset);
            if (!endpoint || USB_EP_DESC_GET_XFERTYPE(endpoint) != USB_TRANSFER_TYPE_BULK)
                continue;

            if (USB_EP_DESC_GET_EP_DIR(endpoint))
            {
                inEndpoint = endpoint->bEndpointAddress;
                inPacketSize = USB_EP_DESC_GET_MPS(endpoint);
            }
            else
            {
                outEndpoint = endpoint->bEndpointAddress;
            }
        }

        return inEndpoint && outEndpoint && inPacketSize > FTDI_STATUS_BYTES && inPacketSize <= RX_TRANSFER_BYTES;
    }


    String descriptorString(const usb_str_desc_t *descriptor)
    {
        String text;
        if (!descriptor)
            return text;

        for (int i = 0; i < (descriptor->bLength - 2) / 2; i++)
        {
            uint16_t ch = descriptor->wData[i];
            text += ch < 0x80 ? static_cast<char>(ch) : '?';
        }
        return text;
    }


    void printDevice(uint8_t address, const usb_device_desc_t *descriptor, const usb_device_info_t &info)
    {
        Serial.println();
        Serial.println("==============================");
        Serial.println("USB DEVICE CONNECTED");
        Serial.printf("Address: %u\n", address);
        Serial.printf("VID: %04X\n", descriptor->idVendor);
        Serial.printf("PID: %04X\n", descriptor->idProduct);
        Serial.printf("Manufacturer: %s\n", descriptorString(info.str_desc_manufacturer).c_str());
        Serial.printf("Product: %s\n", descriptorString(info.str_desc_product).c_str());
        Serial.printf("Serial: %s\n", descriptorString(info.str_desc_serial_num).c_str());
    }


    void closeDevice(usb_device_handle_t device, bool claimed)
    {
        if (claimed)
            usb_host_interface_release(client, device, interfaceNumber);
        usb_host_device_close(client, device);
        openDevice = nullptr;
    }


    bool configureWithRetries(uint32_t baud)
    {
        for (int attempt = 1; attempt <= MAX_CONFIGURE_ATTEMPTS; attempt++)
        {
            if (configureLink(baud, true))
                return true;

            Serial.printf("FT232R configure attempt %d/%d failed, retrying...\n", attempt, MAX_CONFIGURE_ATTEMPTS);
            delay(CONFIGURE_RETRY_DELAY_MS);
        }
        return false;
    }
}


namespace Ftdi
{

void attach(uint8_t address)
{
    if (openDevice || transfersStuck)
        return;

    usb_device_handle_t device = nullptr;
    esp_err_t err = usb_host_device_open(client, address, &device);
    if (err != ESP_OK)
    {
        recordError("device open", err);
        Serial.printf("WARNING: couldn't open USB device %u\n", address);
        return;
    }

    const usb_device_desc_t *descriptor = nullptr;
    usb_device_info_t info = {};
    if (usb_host_get_device_descriptor(device, &descriptor) != ESP_OK ||
        usb_host_device_info(device, &info) != ESP_OK)
    {
        recordError("device info", 0);
        closeDevice(device, false);
        return;
    }

    printDevice(address, descriptor, info);
    lastVid = descriptor->idVendor;

    if (descriptor->idVendor != FTDI_VID)
    {
        Serial.println("Not an FTDI device");
        rejected[address / 8] |= 1 << (address % 8);
        closeDevice(device, false);
        return;
    }

    Serial.println("FTDI DEVICE DETECTED");

    if (!findEndpoints(device))
    {
        Serial.println("ERROR: FT232R bulk endpoints not found");
        recordError("endpoints", inPacketSize);
        closeDevice(device, false);
        return;
    }

    err = usb_host_interface_claim(client, device, interfaceNumber, 0);
    if (err != ESP_OK)
    {
        recordError("interface claim", err);
        Serial.println("ERROR: couldn't claim the FT232R interface");
        closeDevice(device, false);
        return;
    }

    openDevice = device;
    const uint32_t baud = currentBaud;

    if (!configureWithRetries(baud))
    {
        Serial.println("ERROR: FT232R configuration failed after retries");
        hardwareFlowControl = false;
        if (waitUntil(controlIdle, IDLE_WAIT_MS))
        {
            closeDevice(device, true);
        }
        else
        {
            transfersStuck = true;
            openDevice = nullptr;
        }
        notifyStatus(false, "[FT232R configuration failed]\r\n");
        return;
    }

    prepareTransfers(device);
    rxInBreak = false;
    setEnabled(txEnabled, true);
    if (!startRx())
        Serial.println("WARNING: FT232R RX transfers failed to start");

    ftdiConnected = true;

    Serial.println("FT232R configured");
    Serial.printf("Baud: %" PRIu32 "\n", baud);
    Serial.println("Format: 8-N-1");
    Serial.printf(
        "RX: %u x %u B transfers, %u B packets\n",
        static_cast<unsigned>(RX_TRANSFER_COUNT),
        static_cast<unsigned>(rxTransfers[0]->num_bytes),
        inPacketSize
    );

    notifyStatus(true, "[FT232R connected - " + String(baud) + " 8-N-1]\r\n");
}


// Catches devices whose NEW_DEV event never reached this client.
void scanDevices()
{
    usb_host_lib_info_t info = {};
    if (usb_host_lib_info(&info) != ESP_OK)
        return;

    if (info.num_devices != scannedDeviceCount)
    {
        memset(rejected, 0, sizeof(rejected));
        scannedDeviceCount = info.num_devices;
    }

    uint8_t addresses[16] = {};
    int count = 0;
    if (usb_host_device_addr_list_fill(sizeof(addresses), addresses, &count) != ESP_OK)
        return;

    for (int i = 0; i < count && !openDevice; i++)
    {
        uint8_t address = addresses[i];
        if (address == 0 || address >= 128 || (rejected[address / 8] & (1 << (address % 8))))
            continue;

        scannedDevices++;
        attach(address);
    }
}


void detach()
{
    usb_device_handle_t device = openDevice;
    if (!device)
        return;

    ftdiConnected = false;
    hardwareFlowControl = false;
    setEnabled(rxEnabled, false);
    setEnabled(txEnabled, false);
    cancelEndpoint(device, inEndpoint);
    cancelEndpoint(device, outEndpoint);

    // Writers still holding a slot give up once it comes back.
    xSemaphoreTake(txLock, portMAX_DELAY);
    bool idle = waitUntil(allIdle, IDLE_WAIT_MS);
    xSemaphoreGive(txLock);

    Serial.println();
    Serial.println("FT232R DISCONNECTED");

    if (idle)
    {
        closeDevice(device, true);
    }
    else
    {
        // Freeing the device would free transfers still owned by the stack.
        Serial.println("ERROR: USB transfers didn't finish; reboot to reconnect the FT232R");
        transfersStuck = true;
        openDevice = nullptr;
    }

    notifyStatus(false, "[FT232R disconnected]\r\n");
}


void reconfigure()
{
    if (!ftdiConnected)
        return;

    const uint32_t baud = currentBaud;
    if (configureLink(baud, false))
        Serial.printf("FT232R baud changed to %" PRIu32 "\n", baud);
    else
        Serial.println("ERROR: FT232R baud change failed");
}


void recoverRx()
{
    usb_device_handle_t device = openDevice;
    if (!device || !ftdiConnected)
        return;

    Serial.println("WARNING: FT232R RX transfer failed, restarting RX");
    setEnabled(rxEnabled, false);
    cancelEndpoint(device, inEndpoint);
    if (!waitUntil(rxIdle, IDLE_WAIT_MS))
    {
        Serial.println("ERROR: FT232R RX transfers didn't finish");
        return;
    }

    usb_host_endpoint_clear(device, inEndpoint);
    if (!startRx())
        Serial.println("ERROR: FT232R RX restart failed");
}


void recoverTx()
{
    usb_device_handle_t device = openDevice;
    if (!device || !ftdiConnected)
        return;

    Serial.println("WARNING: FT232R TX transfer failed, clearing TX");
    setEnabled(txEnabled, false);
    cancelEndpoint(device, outEndpoint);

    xSemaphoreTake(txLock, portMAX_DELAY);
    bool idle = waitUntil(txIdle, IDLE_WAIT_MS);
    if (idle)
        usb_host_endpoint_clear(device, outEndpoint);
    xSemaphoreGive(txLock);

    if (idle)
        setEnabled(txEnabled, true);
    else
        Serial.println("ERROR: FT232R TX transfers didn't finish");
}

} // namespace Ftdi
