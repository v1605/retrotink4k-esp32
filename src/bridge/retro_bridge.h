#pragma once

#include <Arduino.h>

// Retro-Bridge API v1 (/api/v1/*) over the FT232R link.

namespace RetroBridge
{
    // Call once from setup(), after Rt4kLink::begin().
    void begin();

    void registerRoutes();

    // While true, RX bytes belong to the bridge, not the Rt4kLink parser.
    bool ownsLink();

    // RX task only.
    void onSerialRx(const uint8_t *data, size_t length);

    // From the web server's close_fn.
    void onSocketClosed(int fd);

    // "lease:<c>/<l>", "transaction:<c>/<r>", "internal:<op>" or "".
    String ownerLabel();

    size_t clientCount();
}
