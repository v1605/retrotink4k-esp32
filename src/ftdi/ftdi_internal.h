#pragma once

#include <Arduino.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <usb/usb_host.h>

#include "serial_bridge.h"

// Shared by the ftdi_*.cpp files. Three tasks touch this state: usb_client
// (transfer callbacks and host events), ftdi_service (attach, detach,
// recovery, RX forwarding) and whichever task calls the public API.

namespace Ftdi
{
    constexpr uint16_t FTDI_VID = 0x0403;
    constexpr uint16_t FTDI_LATENCY_TIMER_MS = 1;
    constexpr uint32_t HARDWARE_FLOW_CONTROL_BAUD = 2000000;

    // Every IN packet starts with a modem status and a line status byte.
    constexpr size_t FTDI_STATUS_BYTES = 2;

    // Queued IN transfers keep the controller polling the FT232R's 256-byte
    // FIFO while completions are handled in software.
    constexpr size_t RX_TRANSFER_COUNT = 4;
    constexpr size_t RX_TRANSFER_BYTES = 1024;

    constexpr size_t TX_TRANSFER_COUNT = 4;
    constexpr size_t TX_TRANSFER_BYTES = 2560;
    constexpr uint32_t TX_SLOT_WAIT_MS = 1000;

    constexpr uint32_t CONTROL_TIMEOUT_MS = 500;
    constexpr uint32_t IDLE_WAIT_MS = 1000;

    // Core 1 keeps USB away from WiFi; usb_host_install() runs there too,
    // so the USB interrupt does as well.
    constexpr BaseType_t USB_CORE = 1;
    constexpr UBaseType_t USB_TASK_PRIORITY = 10;
    constexpr UBaseType_t SERVICE_TASK_PRIORITY = 6;

    constexpr uint32_t EVENT_NEW_DEVICE = 1 << 0;
    constexpr uint32_t EVENT_DEVICE_GONE = 1 << 1;
    constexpr uint32_t EVENT_RECONFIGURE = 1 << 2;
    constexpr uint32_t EVENT_RX_FAILED = 1 << 3;
    constexpr uint32_t EVENT_TX_FAILED = 1 << 4;

    // ---- State, defined in serial_bridge.cpp ----

    extern RingbufHandle_t rxRing;
    extern usb_host_client_handle_t client;
    extern QueueHandle_t newDevices;

    // The service task sleeps on serviceSet: the RX ring and serviceWake, a
    // one-slot queue that says pendingEvents has bits to handle.
    extern QueueSetHandle_t serviceSet;
    extern QueueHandle_t serviceWake;
    extern std::atomic<uint32_t> pendingEvents;

    // Allocated once in begin() and reused for every connection.
    extern usb_transfer_t *rxTransfers[RX_TRANSFER_COUNT];
    extern usb_transfer_t *txTransfers[TX_TRANSFER_COUNT];
    extern usb_transfer_t *controlTransfer;

    // Set by the service task while a device is attached.
    extern std::atomic<usb_device_handle_t> openDevice;
    extern uint8_t interfaceNumber;
    extern uint8_t inEndpoint;
    extern uint8_t outEndpoint;
    extern uint16_t inPacketSize;
    // A transfer never came back, so the buffers can't be reused.
    extern bool transfersStuck;

    // Held around every bulk submit; rxEnabled/txEnabled only change under it.
    extern SemaphoreHandle_t submitLock;
    extern bool rxEnabled;
    extern bool txEnabled;

    extern std::atomic<uint8_t> rxInFlight;
    // Free txTransfers indices; one writer at a time holds txLock.
    extern QueueHandle_t txFreeSlots;
    extern SemaphoreHandle_t txLock;

    // Given once per submitted control transfer; controlOwed (service task
    // only) says the give for the last submit hasn't been taken yet.
    extern SemaphoreHandle_t controlDone;
    extern bool controlOwed;

    extern std::atomic<bool> ftdiConnected;
    extern std::atomic<uint32_t> currentBaud;
    extern std::atomic<bool> hardwareFlowControl;

    extern std::atomic<uint32_t> rxDropCount;
    extern std::atomic<uint32_t> overrunCount;
    extern std::atomic<uint32_t> framingCount;
    extern std::atomic<uint32_t> breakCount;
    extern bool rxInBreak; // USB client task only

    // Reported by usbStatus().
    extern std::atomic<uint32_t> newDeviceEvents;
    extern std::atomic<uint32_t> scannedDevices;
    extern std::atomic<uint16_t> lastVid;
    extern std::atomic<const char *> lastErrorStage;
    extern std::atomic<int> lastErrorCode;

    extern std::atomic<SerialBridge::DataCallback> dataCallback;
    extern std::atomic<SerialBridge::StatusCallback> statusCallback;

    // ---- serial_bridge.cpp ----
    void notifyStatus(bool connected, const String &message);
    void recordError(const char *stage, int code);

    // ---- ftdi_transfers.cpp: callbacks on the USB tasks, submits, idle checks ----
    bool allocateTransfers();
    void prepareTransfers(usb_device_handle_t device);
    bool startRx();
    // Callers stop new submits first; the transfers then drain.
    void cancelEndpoint(usb_device_handle_t device, uint8_t endpoint);
    void clientEvent(const usb_host_client_event_msg_t *event, void *arg);
    void usbLibTask(void *arg);
    void usbClientTask(void *arg);
    bool rxIdle();
    bool txIdle();
    bool controlIdle();
    bool allIdle();
    void setEnabled(bool &flag, bool value);
    bool waitUntil(bool (*done)(), uint32_t timeoutMs);

    // ---- ftdi_control.cpp: FT232R vendor requests (service task) ----
    bool configureLink(uint32_t baud, bool reset);

    // ---- ftdi_device.cpp: attach, detach and recovery (service task) ----
    void attach(uint8_t address);
    void scanDevices();
    void detach();
    void reconfigure();
    void recoverRx();
    void recoverTx();

    // ---- ftdi_service.cpp ----
    void notifyService(uint32_t events);
    void serviceTaskMain(void *arg);
}
