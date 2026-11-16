#include "ftdi_internal.h"

namespace
{
    using namespace Ftdi;

    constexpr uint8_t FTDI_RESET = 0x00;
    constexpr uint8_t FTDI_SET_FLOW_CTRL = 0x02;
    constexpr uint8_t FTDI_SET_BAUD_RATE = 0x03;
    constexpr uint8_t FTDI_SET_DATA = 0x04;
    constexpr uint8_t FTDI_SET_LATENCY_TIMER = 0x09;
    constexpr uint16_t FTDI_FLOW_NONE = 0x0000;
    constexpr uint16_t FTDI_FLOW_RTS_CTS = 0x0100;
    constexpr uint16_t FTDI_DATA_8N1 = 0x0008;


    bool vendorOut(uint8_t request, uint16_t value, uint16_t index)
    {
        usb_device_handle_t device = openDevice;
        if (!device)
            return false;

        if (controlOwed)
        {
            if (xSemaphoreTake(controlDone, pdMS_TO_TICKS(CONTROL_TIMEOUT_MS)) != pdTRUE)
            {
                recordError("control busy", request);
                return false;
            }
            controlOwed = false;
        }

        auto *setup = reinterpret_cast<usb_setup_packet_t *>(controlTransfer->data_buffer);
        setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
        setup->bRequest = request;
        setup->wValue = value;
        setup->wIndex = index;
        setup->wLength = 0;
        controlTransfer->device_handle = device;
        controlTransfer->bEndpointAddress = 0;
        controlTransfer->num_bytes = sizeof(usb_setup_packet_t);

        esp_err_t err = usb_host_transfer_submit_control(client, controlTransfer);
        if (err != ESP_OK)
        {
            recordError("control submit", err);
            return false;
        }

        controlOwed = true;
        if (xSemaphoreTake(controlDone, pdMS_TO_TICKS(CONTROL_TIMEOUT_MS)) != pdTRUE)
        {
            recordError("control timeout", request);
            return false;
        }
        controlOwed = false;

        if (controlTransfer->status != USB_TRANSFER_STATUS_COMPLETED)
        {
            recordError("control status", controlTransfer->status);
            return false;
        }
        return true;
    }


    // FT232R: 14-bit integer divisor, 3-bit fraction code in bits 14-16.
    uint32_t ftdiDivisor(uint32_t baud)
    {
        static const uint8_t FRACTION_CODE[8] = {0, 3, 2, 4, 1, 5, 6, 7};
        uint32_t eighths = (48000000 + baud) / (2 * baud);
        uint32_t divisor = (eighths >> 3) | (static_cast<uint32_t>(FRACTION_CODE[eighths & 0x7]) << 14);
        if (divisor == 1)
            return 0;
        if (divisor == 0x4001)
            return 1;
        return divisor;
    }
}


namespace Ftdi
{

bool configureLink(uint32_t baud, bool reset)
{
    if (reset && !vendorOut(FTDI_RESET, 0, 0))
        return false;

    uint32_t divisor = ftdiDivisor(baud);
    if (!vendorOut(FTDI_SET_BAUD_RATE, divisor & 0xffff, divisor >> 16))
        return false;

    if (!vendorOut(FTDI_SET_DATA, FTDI_DATA_8N1, 0))
        return false;

    if (vendorOut(FTDI_SET_LATENCY_TIMER, FTDI_LATENCY_TIMER_MS, 0))
        Serial.printf("FT232R latency timer: %ums\n", FTDI_LATENCY_TIMER_MS);
    else
        Serial.println("WARNING: couldn't set FT232R latency timer");

    const bool want = baud == HARDWARE_FLOW_CONTROL_BAUD;
    const bool enabled = vendorOut(FTDI_SET_FLOW_CTRL, 0, want ? FTDI_FLOW_RTS_CTS : FTDI_FLOW_NONE) && want;
    hardwareFlowControl = enabled;
    Serial.printf("FT232R RTS/CTS flow control: %s\n", enabled ? "on" : (want ? "FAILED" : "off"));
    return true;
}

} // namespace Ftdi
