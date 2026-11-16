#include "serial_bridge.h"
#include "ftdi_internal.h"

#include <cinttypes>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <freertos/task.h>

#include "../config.h"

// Public API, driver state and start-up. The rest lives in ftdi_*.cpp.

namespace Ftdi
{
    RingbufHandle_t rxRing = nullptr;
    usb_host_client_handle_t client = nullptr;
    QueueHandle_t newDevices = nullptr;

    QueueSetHandle_t serviceSet = nullptr;
    QueueHandle_t serviceWake = nullptr;
    std::atomic<uint32_t> pendingEvents{0};

    usb_transfer_t *rxTransfers[RX_TRANSFER_COUNT] = {};
    usb_transfer_t *txTransfers[TX_TRANSFER_COUNT] = {};
    usb_transfer_t *controlTransfer = nullptr;

    std::atomic<usb_device_handle_t> openDevice{nullptr};
    uint8_t interfaceNumber = 0;
    uint8_t inEndpoint = 0;
    uint8_t outEndpoint = 0;
    uint16_t inPacketSize = 64;
    bool transfersStuck = false;

    SemaphoreHandle_t submitLock = nullptr;
    bool rxEnabled = false;
    bool txEnabled = false;

    std::atomic<uint8_t> rxInFlight{0};
    QueueHandle_t txFreeSlots = nullptr;
    SemaphoreHandle_t txLock = nullptr;

    SemaphoreHandle_t controlDone = nullptr;
    bool controlOwed = false;

    std::atomic<bool> ftdiConnected{false};
    std::atomic<uint32_t> currentBaud{DEFAULT_SERIAL_BAUD};
    std::atomic<bool> hardwareFlowControl{false};

    std::atomic<uint32_t> rxDropCount{0};
    std::atomic<uint32_t> overrunCount{0};
    std::atomic<uint32_t> framingCount{0};
    std::atomic<uint32_t> breakCount{0};
    bool rxInBreak = false;

    std::atomic<uint32_t> newDeviceEvents{0};
    std::atomic<uint32_t> scannedDevices{0};
    std::atomic<uint16_t> lastVid{0};
    std::atomic<const char *> lastErrorStage{"none"};
    std::atomic<int> lastErrorCode{0};

    std::atomic<SerialBridge::DataCallback> dataCallback{nullptr};
    std::atomic<SerialBridge::StatusCallback> statusCallback{nullptr};


    void notifyStatus(bool connected, const String &message)
    {
        if (SerialBridge::StatusCallback callback = statusCallback)
            callback(connected, message);
    }


    void recordError(const char *stage, int code)
    {
        lastErrorCode = code;
        lastErrorStage = stage;
    }
}


namespace
{
    using namespace Ftdi;

    constexpr size_t RX_RING_BYTES = 128 * 1024;
    constexpr size_t RX_RING_FALLBACK_BYTES = 16 * 1024;


    // The core is built with the enumeration filter hook enabled.
    bool acceptDevice(const usb_device_desc_t * /*descriptor*/, uint8_t * /*configuration*/)
    {
        return true;
    }


    bool createSyncObjects()
    {
        rxRing = xRingbufferCreateWithCaps(RX_RING_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
        if (!rxRing)
            rxRing = xRingbufferCreate(RX_RING_FALLBACK_BYTES, RINGBUF_TYPE_BYTEBUF);

        submitLock = xSemaphoreCreateMutex();
        txLock = xSemaphoreCreateMutex();
        controlDone = xSemaphoreCreateBinary();
        newDevices = xQueueCreate(4, sizeof(uint8_t));
        txFreeSlots = xQueueCreate(TX_TRANSFER_COUNT, sizeof(uint8_t));
        serviceWake = xQueueCreate(1, sizeof(uint8_t));
        // One slot per member: the wake queue and the ring's read semaphore.
        serviceSet = xQueueCreateSet(2);

        bool setReady = rxRing && serviceWake && serviceSet &&
            xQueueAddToSet(serviceWake, serviceSet) == pdPASS &&
            xRingbufferAddToQueueSetRead(rxRing, serviceSet) == pdTRUE;

        return setReady && submitLock && txLock && controlDone && newDevices && txFreeSlots;
    }


    bool startTasks()
    {
        return
            xTaskCreatePinnedToCore(serviceTaskMain, "ftdi_service", 8192, nullptr, SERVICE_TASK_PRIORITY, nullptr, USB_CORE) == pdPASS &&
            xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, nullptr, USB_TASK_PRIORITY, nullptr, USB_CORE) == pdPASS &&
            xTaskCreatePinnedToCore(usbClientTask, "usb_client", 4096, nullptr, USB_TASK_PRIORITY, nullptr, USB_CORE) == pdPASS;
    }
}


namespace SerialBridge
{

void begin()
{
    currentBaud = Config::loadSerialBaud();

    if (!createSyncObjects())
    {
        Serial.println("ERROR: no memory for the FT232R driver");
        return;
    }

    usb_host_config_t hostConfig = {};
    hostConfig.intr_flags = ESP_INTR_FLAG_LOWMED;
#if CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
    hostConfig.enum_filter_cb = acceptDevice;
#endif
    esp_err_t err = usb_host_install(&hostConfig);
    if (err != ESP_OK)
    {
        recordError("host install", err);
        Serial.printf("ERROR: USB host failed to start: %s\n", esp_err_to_name(err));
        return;
    }

    if (!allocateTransfers())
    {
        recordError("transfer alloc", 0);
        Serial.println("ERROR: no memory for USB transfers");
        return;
    }

    usb_host_client_config_t clientConfig = {};
    clientConfig.is_synchronous = false;
    clientConfig.max_num_event_msg = 8;
    clientConfig.async.client_event_callback = clientEvent;
    clientConfig.async.callback_arg = nullptr;
    err = usb_host_client_register(&clientConfig, &client);
    if (err != ESP_OK)
    {
        recordError("client register", err);
        Serial.printf("ERROR: USB client failed to register: %s\n", esp_err_to_name(err));
        return;
    }

    if (!startTasks())
    {
        recordError("task create", 0);
        Serial.println("ERROR: USB tasks failed to start");
        return;
    }

    Serial.println("USB host started");
}


bool isConnected()
{
    return ftdiConnected;
}


size_t pendingBytes()
{
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(rxRing, nullptr, nullptr, nullptr, nullptr, &waiting);
    return waiting;
}


uint32_t getBaud()
{
    return currentBaud;
}


void setBaud(uint32_t baud)
{
    currentBaud = baud;

    Config::saveSerialBaud(baud);

    // Also covers an attach already in progress at the old rate.
    hardwareFlowControl = false;
    notifyService(EVENT_RECONFIGURE);
}


bool hardwareFlowControlEnabled()
{
    return ftdiConnected && hardwareFlowControl;
}


bool sendRaw(const uint8_t *data, size_t length)
{
    if (!ftdiConnected)
        return false;

    bool ok = true;
    xSemaphoreTake(txLock, portMAX_DELAY);

    while (ok && length > 0)
    {
        uint8_t slot = 0;
        if (xQueueReceive(txFreeSlots, &slot, pdMS_TO_TICKS(TX_SLOT_WAIT_MS)) != pdTRUE)
        {
            ok = false;
            break;
        }

        usb_transfer_t *transfer = txTransfers[slot];
        size_t n = length < TX_TRANSFER_BYTES ? length : TX_TRANSFER_BYTES;
        memcpy(transfer->data_buffer, data, n);
        transfer->num_bytes = n;

        xSemaphoreTake(submitLock, portMAX_DELAY);
        ok = txEnabled && usb_host_transfer_submit(transfer) == ESP_OK;
        xSemaphoreGive(submitLock);

        if (!ok)
            xQueueSend(txFreeSlots, &slot, 0);

        data += n;
        length -= n;
    }

    xSemaphoreGive(txLock);
    return ok;
}


bool flush(uint32_t timeoutMs)
{
    if (!ftdiConnected)
        return false;

    return waitUntil(txIdle, timeoutMs);
}


uint32_t droppedRxBytes()
{
    return rxDropCount + overrunCount;
}


uint32_t rxOverruns()
{
    return overrunCount;
}


uint32_t rxFramingErrors()
{
    return framingCount;
}


uint32_t rxBreaks()
{
    return breakCount;
}


String usbStatus()
{
    usb_host_lib_info_t info = {};
    bool running = client && usb_host_lib_info(&info) == ESP_OK;

    char text[160];
    snprintf(
        text,
        sizeof(text),
        "host=%s devices=%d new_device_events=%" PRIu32 " scanned=%" PRIu32 " last_vid=%04X last_error=%s:%d",
        running ? "up" : "down",
        info.num_devices,
        newDeviceEvents.load(),
        scannedDevices.load(),
        lastVid.load(),
        lastErrorStage.load(),
        lastErrorCode.load()
    );
    return text;
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
