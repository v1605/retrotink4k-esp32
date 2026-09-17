#pragma once

#include <ESPAsyncWebServer.h>

// /api/remote: "remote <button>" presses.
namespace RemoteEndpoints
{
    void begin(AsyncWebServer &server);
}
