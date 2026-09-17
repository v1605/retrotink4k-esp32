#pragma once

#include <ESPAsyncWebServer.h>

// /api/wifi, /api/wifi/forget, /api/wifi/hostname, /api/wifi/scan

namespace WifiEndpoints
{
    void registerRoutes(AsyncWebServer &server);
}
