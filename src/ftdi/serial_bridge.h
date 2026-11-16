#pragma once

#include <Arduino.h>

// USB host <-> FT232R serial bridge. Implementation in ftdi_*.cpp.
namespace SerialBridge
{
    using DataCallback = void (*)(const uint8_t *data, size_t length);
    using StatusCallback = void (*)(bool connected, const String &message);

    void begin();

    bool isConnected();

    // Unread bytes in the FT232R RX buffer, used to detect a backlog.
    size_t pendingBytes();

    uint32_t getBaud();

    // Persists and applies immediately if already connected.
    void setBaud(uint32_t baud);

    // RTS/CTS is active (set up automatically at 2 Mbaud).
    bool hardwareFlowControlEnabled();

    // Sends bytes exactly as given, no framing added. Blocks up to 1s per
    // 2560 B while the TX queue is full.
    bool sendRaw(const uint8_t *data, size_t length);

    // Not from the USB client task.
    bool flush(uint32_t timeoutMs);

    // RX bytes lost since boot: full buffer, plus one per FT232R overrun.
    uint32_t droppedRxBytes();

    // FT232R line status counts since boot.
    uint32_t rxOverruns();
    uint32_t rxFramingErrors();
    uint32_t rxBreaks();

    // USB host state and the last driver error, for /api/status.
    String usbStatus();

    // Only one subscriber is supported; the web server is the sole consumer.
    void onData(DataCallback callback);
    void onStatus(StatusCallback callback);
}
