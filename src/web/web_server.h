#pragma once

// HTTP + WebSocket front end: terminal UI, /ws relay, and REST API.

namespace WebServer
{
    void begin();

    // Housekeeping (drops closed WebSocket clients); call from loop() about
    // once a second.
    void loop();
}
