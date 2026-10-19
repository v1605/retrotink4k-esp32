#pragma once

#include <esp_http_server.h>

#include <vector>

// HTTP + WebSocket front end (esp_http_server, port 80, keep-alive): web UI,
// REST API, and the /ws relays.

namespace WebServer
{
    using Handler = esp_err_t (*)(httpd_req_t *req);

    // lwIP has 16 sockets: 3 for the server's own use, 1 for the network
    // watchdog's ping.
    constexpr uint16_t MAX_OPEN_SOCKETS = 12;

    // Idle keep-alive connections are closed after this. Short, because only
    // 12 connections fit and a full server evicts the least recently used,
    // which a browser then has to reopen.
    constexpr uint32_t KEEP_ALIVE_IDLE_MS = 2000;

    void begin();

    // Closes idle keep-alive connections; call from loop() about once a
    // second.
    void loop();

    // For the modules' registerRoutes(), which begin() calls. Patterns follow
    // httpd_uri_match_wildcard: "*" matches any rest, "?" makes the previous
    // character optional.
    void on(const char *uri, httpd_method_t method, Handler handler);
    void onWebSocket(const char *uri, Handler handler);

    // Open WebSocket connections to the endpoint at path.
    std::vector<int> webSockets(const char *path);

    // Takes a request off the server task (httpd_req_async_handler_begin);
    // the connection counts as busy until endAsync(). nullptr on failure.
    httpd_req_t *beginAsync(httpd_req_t *req);
    void endAsync(httpd_req_t *async);

    httpd_handle_t handle();
}
