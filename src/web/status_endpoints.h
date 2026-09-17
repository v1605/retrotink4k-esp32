#pragma once

#include <ESPAsyncWebServer.h>

// /api/status, /api/serial, /api/command

namespace StatusEndpoints
{
    void registerRoutes(AsyncWebServer &server);
}
