#include "serial_bridge.h"

#include <EspUsbHost.h>

#include <atomic>
#include <cinttypes>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

namespace
{
    constexpr uint16_t FTDI_VID = 0x0403;
    constexpr uint16_t FTDI_LATENCY_TIMER_MS = 1;
    constexpr uint32_t HARDWARE_FLOW_CONTROL_BAUD = 2000000;

    // FTDI vendor requests; wIndex low byte = interface 0.
    constexpr uint8_t FTDI_SET_FLOW_CTRL = 0x02;
    constexpr uint8_t FTDI_SET_LATENCY_TIMER = 0x09;
    constexpr uint16_t FTDI_FLOW_RTS_CTS = 0x0100;

    EspUsbHost usb;

    constexpr size_t RX_RING_BUFFER_SIZE = 16384;
    EspUsbHostCdcSerial cdcSerial(usb);
    constexpr size_t SERIAL_WRITE_QUEUE_DEPTH = 4;
    constexpr size_t SERIAL_WRITE_BUFFER_BYTES = 2560;

    // Backstop in case a wake-up is missed.
    constexpr TickType_t RX_IDLE_WAIT_TICKS = pdMS_TO_TICKS(10);

    // Written by EspUsbHost's client task and web tasks, read by rxForwardTask.
    std::atomic<bool> ftdiConnected{false};
    
    std::atomic<uint8_t> ftdiAddress{ESP_USB_HOST_ANY_ADDRESS};
    std::atomic<uint32_t> currentBaud{DEFAULT_SERIAL_BAUD};
    std::atomic<bool> linkSetupPending{false};
    std::atomic<bool> hardwareFlowControl{false};

    TaskHandle_t rxTask = nullptr;

    std::atomic<SerialBridge::DataCallback> dataCallback{nullptr};
    std::atomic<SerialBridge::StatusCallback> statusCallback{nullptr};


    void wakeRxTask()
    {
        if (rxTask)
            xTaskNotifyGive(rxTask);
    }


    void notifyStatus(bool connected, const String &message)
    {
        if (SerialBridge::StatusCallback callback = statusCallback)
            callback(connected, message);
    }

    void handleDeviceConnected(
        const EspUsbHostDeviceInfo &device)
    {
        Serial.println();
        Serial.println("==============================");
        Serial.println("USB DEVICE CONNECTED");
        Serial.printf("Address: %u\n", device.address);
        Serial.printf("VID: %04X\n", device.vid);
        Serial.printf("PID: %04X\n", device.pid);
        Serial.printf("Manufacturer: %s\n", device.manufacturer);
        Serial.printf("Product: %s\n", device.product);
        Serial.printf("Serial: %s\n", device.serial);

        if (device.vid != FTDI_VID)
        {
            Serial.println("Not an FTDI device");
            return;
        }

        ftdiAddress = device.address;
        ftdiConnected = false;
        cdcSerial.setAddress(device.address);

        Serial.println("FTDI DEVICE DETECTED");

        const uint32_t baud = currentBaud;

        EspUsbHostSerialConfig config;
        config.baud = baud;
        config.dataBits = 8;
        config.parity = ESP_USB_HOST_SERIAL_PARITY_NONE;
        config.stopBits = ESP_USB_HOST_SERIAL_STOP_BITS_1;

        // A standby toggle can fire this before the device answers control transfers.
        constexpr int MAX_CONFIGURE_ATTEMPTS = 5;
        constexpr uint32_t CONFIGURE_RETRY_DELAY_MS = 150;

        bool configured = false;

        for (int attempt = 1; attempt <= MAX_CONFIGURE_ATTEMPTS; attempt++)
        {
            if (cdcSerial.setConfig(config))
            {
                configured = true;
                break;
            }

            Serial.printf(
                "FT232R configure attempt %d/%d failed, retrying...\n",
                attempt,
                MAX_CONFIGURE_ATTEMPTS
            );

            delay(CONFIGURE_RETRY_DELAY_MS);
        }

        if (configured)
        {
            cdcSerial.setRxBufferSize(RX_RING_BUFFER_SIZE);
            cdcSerial.begin(baud);

            if (!usb.serialWriteQueueBegin(SERIAL_WRITE_QUEUE_DEPTH, SERIAL_WRITE_BUFFER_BYTES, device.address))
                Serial.println("WARNING: serial write queue unavailable, large uploads may fail");

            linkSetupPending = true;
            ftdiConnected = true;
            wakeRxTask();

            Serial.println("FT232R configured");
            Serial.printf("Baud: %" PRIu32 "\n", baud);
            Serial.println("Format: 8-N-1");

            notifyStatus(
                true,
                "[FT232R connected - " + String(baud) + " 8-N-1]\r\n"
            );
        }
        else
        {
            Serial.println("ERROR: FT232R configuration failed after retries");

            ftdiAddress = ESP_USB_HOST_ANY_ADDRESS;
            cdcSerial.clearAddress();

            notifyStatus(false, "[FT232R configuration failed]\r\n");
        }
    }

    void handleDeviceDisconnected(
        const EspUsbHostDeviceInfo &device)
    {
        if (device.address != ftdiAddress)
            return;

        ftdiConnected = false;
        hardwareFlowControl = false;
        ftdiAddress = ESP_USB_HOST_ANY_ADDRESS;

        cdcSerial.end();
        cdcSerial.clearAddress();

        Serial.println();
        Serial.println("FT232R DISCONNECTED");

        notifyStatus(false, "[FT232R disconnected]\r\n");
    }


    void applyLinkSetup()
    {
        const uint8_t address = ftdiAddress;

        // The 16ms default latency timer delays every short reply.
        if (usb.vendorControlOut(FTDI_SET_LATENCY_TIMER, FTDI_LATENCY_TIMER_MS, 0, nullptr, 0, address))
            Serial.printf("FT232R latency timer: %ums\n", FTDI_LATENCY_TIMER_MS);
        else
            Serial.println("WARNING: couldn't set FT232R latency timer");

        // RTS/CTS lets the RT4K pause our writes. 2 Mbaud only, as the RT4K
        // Profiler does.
        const bool want = currentBaud == HARDWARE_FLOW_CONTROL_BAUD;
        const bool enabled = want && usb.vendorControlOut(FTDI_SET_FLOW_CTRL, 0, FTDI_FLOW_RTS_CTS, nullptr, 0, address);
        hardwareFlowControl = enabled;
        Serial.printf("FT232R RTS/CTS flow control: %s\n", enabled ? "on" : (want ? "FAILED" : "off"));
    }


    void rxForwardTask(void * /*arg*/)
    {
        // Each cdcSerial.read() takes a critical section (there's no bulk
        // read), so read in chunks.
        constexpr size_t FORWARD_CHUNK_CAPACITY = 512;
        uint8_t chunk[FORWARD_CHUNK_CAPACITY];

        for (;;)
        {
            if (linkSetupPending.exchange(false))
                applyLinkSetup();

            if (!ftdiConnected || cdcSerial.available() <= 0)
            {
                // Woken by onSerialData once new bytes are buffered.
                ulTaskNotifyTake(pdTRUE, RX_IDLE_WAIT_TICKS);
                continue;
            }

            size_t length = 0;

            while (length < FORWARD_CHUNK_CAPACITY)
            {
                int c = cdcSerial.read();
                if (c < 0)
                    break;

                chunk[length++] = static_cast<uint8_t>(c);
            }

            SerialBridge::DataCallback callback = dataCallback;
            if (length > 0 && callback)
                callback(chunk, length);
        }
    }
}


namespace SerialBridge
{

void begin()
{
    currentBaud = Config::loadSerialBaud();
    constexpr UBaseType_t RX_FORWARD_TASK_PRIORITY = 6;

    // Core 1, away from WiFi and the USB host task.
    BaseType_t created = xTaskCreatePinnedToCore(
        rxForwardTask,
        "ftdi_rx_forward",
        8192,
        nullptr,
        RX_FORWARD_TASK_PRIORITY,
        &rxTask,
        1
    );

    if (created != pdPASS)
        Serial.println("ERROR: FT232R RX task failed to start");

    usb.onDeviceConnected(handleDeviceConnected);
    usb.onDeviceDisconnected(handleDeviceDisconnected);
    // Called on EspUsbHost's client task after cdcSerial has the bytes.
    usb.onSerialData([](const EspUsbHostSerialData &) { wakeRxTask(); });

    EspUsbHostConfig usbConfig;
    usbConfig.taskPriority = 24;
    usbConfig.taskCore = 0;

    if (!usb.begin(usbConfig))
        Serial.println("ERROR: USB host failed to start");
    else
        Serial.println("USB host started");
}


bool isConnected()
{
    return ftdiConnected;
}


size_t pendingBytes()
{
    if (!ftdiConnected)
        return 0;

    int available = cdcSerial.available();
    return available > 0 ? static_cast<size_t>(available) : 0;
}


uint32_t getBaud()
{
    return currentBaud;
}


void setBaud(uint32_t baud)
{
    currentBaud = baud;

    Config::saveSerialBaud(baud);

    if (ftdiConnected)
    {
        cdcSerial.setBaudRate(baud);
        hardwareFlowControl = false;
        linkSetupPending = true;
        wakeRxTask();
    }
}


bool hardwareFlowControlEnabled()
{
    return ftdiConnected && hardwareFlowControl;
}


bool sendRaw(const uint8_t *data, size_t length)
{
    if (!ftdiConnected)
        return false;

    return cdcSerial.write(data, length) == length;
}


void onData(DataCallback callback)
{
    dataCallback = callback;
}


void onStatus(StatusCallback callback)
{
    statusCallback = callback;
}

} // namespace SerialBridge
