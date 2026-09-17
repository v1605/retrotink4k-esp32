#pragma once

#include <ESPAsyncWebServer.h>

// /api/profiles* -- Rt4kLink calls here block for seconds.

namespace ProfileEndpoints
{
    void registerRoutes(AsyncWebServer &server);
}
