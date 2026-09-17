#pragma once

#include <ESPAsyncWebServer.h>

// RT4K firmware updates:
//   /api/firmware/upload  -- write a file to the SD card root
//   /api/firmware/check   -- version of rt4kup.bin ("fwup check")
//   /api/firmware/install -- flash it ("fwup go")
//   /api/firmware/device  -- connected model and running version
//   /ws/firmware          -- status events

namespace FirmwareEndpoints
{
    void registerRoutes(AsyncWebServer &server);

    // Drops closed /ws/firmware clients.
    void cleanupClients();
}
