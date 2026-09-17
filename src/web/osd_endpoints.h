#pragma once

#include <ESPAsyncWebServer.h>

// "/ws/osd" -- JSON request/response socket for font/plane/banner data, plus
// pushed "remote" events. See osd_endpoints.cpp for the message protocol.

namespace OsdEndpoints
{
    void registerRoutes(AsyncWebServer &server);

    // Tells every client the button queue has drained.
    void notifyRemoteDone(const String &button, bool ok);

    // Drops closed /ws/osd clients.
    void cleanupClients();
}
