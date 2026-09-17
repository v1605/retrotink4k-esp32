#pragma once

#include <Arduino.h>

// USB host <-> FT232R serial bridge.
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

    // Sends bytes exactly as given, no framing added.
    bool sendRaw(const uint8_t *data, size_t length);

    // Only one subscriber is supported; the web server is the sole consumer.
    void onData(DataCallback callback);
    void onStatus(StatusCallback callback);
}
