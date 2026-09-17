#pragma once

#include <ESPAsyncWebServer.h>

// /ws -- live serial terminal relay between the browser and the FT232R.

namespace TerminalEndpoints
{
    void begin(AsyncWebServer &server);

    // Drops closed /ws clients.
    void cleanupClients();
}
