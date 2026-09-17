#pragma once

#include <ESPAsyncWebServer.h>

// /api/ota/upload: firmware and web UI from one combined file (see
// scripts/release.py). Only with OTA_ENABLED; needs ota_0, ota_1 and otadata
// partitions.

namespace OtaEndpoints
{
    void registerRoutes(AsyncWebServer &server);

    // Whether OTA_ENABLED is compiled in (reported by /api/status).
    bool isEnabled();
}
