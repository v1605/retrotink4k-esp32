#include "web_server.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <vector>

#include <lwip/sockets.h>

#include "firmware_endpoints.h"
#include "osd_endpoints.h"
#include "ota_endpoints.h"
#include "profile_endpoints.h"
#include "remote_endpoints.h"
#include "status_endpoints.h"
#include "terminal_endpoints.h"
#include "web_util.h"
#include "wifi_endpoints.h"
#include "../bridge/retro_bridge.h"

namespace
{
    constexpr uint16_t MAX_URI_HANDLERS = 40;
    constexpr uint16_t MAX_RESP_HEADERS = 16;
    constexpr uint32_t SERVER_STACK_BYTES = 8192;
    constexpr size_t STATIC_CHUNK_BYTES = 4096;
    // A dead peer is dropped after about 5 + 3 x 5 = 20 s.
    constexpr int KEEP_ALIVE_PROBE_IDLE_S = 5;
    constexpr int KEEP_ALIVE_PROBE_INTERVAL_S = 5;
    constexpr int KEEP_ALIVE_PROBE_COUNT = 3;

    constexpr size_t MAX_PATH_BYTES = 32;

    httpd_handle_t server = nullptr;

    // Per-connection state: activity, for closing idle keep-alive
    // connections, and the first request's path, which tells WebSocket
    // endpoints their clients apart (the server doesn't run handlers for
    // the upgrade request).
    struct Connection
    {
        int fd = -1;
        uint32_t lastActiveMs = 0;
        int busy = 0; // handlers and async requests in progress
        bool pathKnown = false;
        char path[MAX_PATH_BYTES] = {};
    };

    Connection connections[CONFIG_LWIP_MAX_SOCKETS];
    portMUX_TYPE connectionsLock = portMUX_INITIALIZER_UNLOCKED;



    // Call with connectionsLock held.
    Connection *findConnection(int fd, bool create)
    {
        Connection *empty = nullptr;
        for (Connection &c : connections)
        {
            if (c.fd == fd)
                return &c;
            if (!empty && c.fd < 0)
                empty = &c;
        }

        if (create && empty)
        {
            empty->fd = fd;
            empty->busy = 0;
            empty->pathKnown = false;
            empty->path[0] = '\0';
        }
        return create ? empty : nullptr;
    }


    void markBusy(int fd, int delta)
    {
        portENTER_CRITICAL(&connectionsLock);
        if (Connection *c = findConnection(fd, true))
        {
            c->busy += delta;
            c->lastActiveMs = millis();
        }
        portEXIT_CRITICAL(&connectionsLock);
    }


    // Remembers the path of a connection's first request line.
    void notePath(int fd, const char *data, size_t length)
    {
        const char *space = static_cast<const char *>(memchr(data, ' ', length));
        if (!space)
            return;

        const char *start = space + 1;
        size_t n = 0;
        while (start + n < data + length && start[n] != ' ' && start[n] != '?' && n < MAX_PATH_BYTES - 1)
            n++;

        portENTER_CRITICAL(&connectionsLock);
        Connection *c = findConnection(fd, false);
        if (c && !c->pathKnown)
        {
            memcpy(c->path, start, n);
            c->path[n] = '\0';
            c->pathKnown = true;
        }
        portEXIT_CRITICAL(&connectionsLock);
    }


    // The server's default receive, plus notePath() on the first data.
    int receiveHook(httpd_handle_t /*hd*/, int fd, char *buf, size_t length, int flags)
    {
        if (!buf)
            return HTTPD_SOCK_ERR_INVALID;

        int n = recv(fd, buf, length, flags);
        if (n < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;

        if (n > 0)
            notePath(fd, buf, n);
        return n;
    }


    esp_err_t handleSocketOpen(httpd_handle_t hd, int fd)
    {
        // Small replies would otherwise wait on Nagle and delayed ACKs.
        int noDelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

        markBusy(fd, 0);
        httpd_sess_set_recv_override(hd, fd, receiveHook);
        return ESP_OK;
    }


    void handleSocketClose(httpd_handle_t /*hd*/, int fd)
    {
        portENTER_CRITICAL(&connectionsLock);
        if (Connection *c = findConnection(fd, false))
            c->fd = -1;
        portEXIT_CRITICAL(&connectionsLock);

        RetroBridge::onSocketClosed(fd);
        close(fd);
    }


    // Every route goes through here; user_ctx holds the real handler.
    esp_err_t dispatch(httpd_req_t *req)
    {
        auto handler = reinterpret_cast<WebServer::Handler>(req->user_ctx);
        int fd = httpd_req_to_sockfd(req);

        markBusy(fd, 1);
        esp_err_t result = handler(req);
        markBusy(fd, -1);
        return result;
    }


    const char *contentTypeFor(const String &path)
    {
        if (path.endsWith(".html"))
            return "text/html";
        if (path.endsWith(".js"))
            return "text/javascript";
        if (path.endsWith(".css"))
            return "text/css";
        if (path.endsWith(".svg"))
            return "image/svg+xml";
        if (path.endsWith(".json"))
            return "application/json";
        if (path.endsWith(".png"))
            return "image/png";
        if (path.endsWith(".ico"))
            return "image/x-icon";
        if (path.endsWith(".avif"))
            return "image/avif";
        if (path.endsWith(".woff2"))
            return "font/woff2";
        return "application/octet-stream";
    }


    esp_err_t sendNotFound(httpd_req_t *req)
    {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Not found");
    }


    // Files from LittleFS, preferring the gzipped copy the build produces.
    esp_err_t handleStatic(httpd_req_t *req)
    {
        String path = WebUtil::path(req);
        if (path.endsWith("/"))
            path += "index.html";
        if (path.indexOf("..") >= 0)
            return sendNotFound(req);

        String gzPath = path + ".gz";
        bool gzipped = LittleFS.exists(gzPath);
        if (!gzipped && !LittleFS.exists(path))
            return sendNotFound(req);

        File file = LittleFS.open(gzipped ? gzPath : path, "r");
        if (!file || file.isDirectory())
            return sendNotFound(req);

        httpd_resp_set_type(req, contentTypeFor(path));
        if (gzipped)
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

        httpd_resp_set_hdr(req, "Cache-Control",
            path.startsWith("/assets/") ? "public, max-age=31536000, immutable" : "no-cache");

        std::unique_ptr<char[]> chunk(new char[STATIC_CHUNK_BYTES]);
        for (;;)
        {
            size_t n = file.read(reinterpret_cast<uint8_t *>(chunk.get()), STATIC_CHUNK_BYTES);
            if (n == 0)
                break;
            if (httpd_resp_send_chunk(req, chunk.get(), n) != ESP_OK)
                return ESP_FAIL;
        }
        return httpd_resp_send_chunk(req, nullptr, 0);
    }


    void registerHandler(const char *uri, httpd_method_t method, WebServer::Handler handler, bool websocket)
    {
        httpd_uri_t route = {};
        route.uri = uri;
        route.method = method;
        route.handler = dispatch;
        route.user_ctx = reinterpret_cast<void *>(handler);
        route.is_websocket = websocket;

        if (httpd_register_uri_handler(server, &route) != ESP_OK)
            Serial.printf("ERROR: couldn't register %s\n", uri);
    }
}


namespace WebServer
{

void begin()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = MAX_OPEN_SOCKETS;
    config.max_uri_handlers = MAX_URI_HANDLERS;
    config.max_resp_headers = MAX_RESP_HEADERS;
    config.stack_size = SERVER_STACK_BYTES;
    // Idle keep-alive connections give way to new ones.
    config.lru_purge_enable = true;
    // Drops vanished peers, whose WebSockets would otherwise hold a slot.
    config.keep_alive_enable = true;
    config.keep_alive_idle = KEEP_ALIVE_PROBE_IDLE_S;
    config.keep_alive_interval = KEEP_ALIVE_PROBE_INTERVAL_S;
    config.keep_alive_count = KEEP_ALIVE_PROBE_COUNT;
    config.open_fn = handleSocketOpen;
    config.close_fn = handleSocketClose;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) != ESP_OK)
    {
        Serial.println("ERROR: web server failed to start");
        return;
    }

    // Matched in registration order; the static files catch the rest.
    TerminalEndpoints::registerRoutes();
    RemoteEndpoints::registerRoutes();
    StatusEndpoints::registerRoutes();
    WifiEndpoints::registerRoutes();
    ProfileEndpoints::registerRoutes();
    OsdEndpoints::registerRoutes();
    OtaEndpoints::registerRoutes();
    FirmwareEndpoints::registerRoutes();
    RetroBridge::registerRoutes();
    on("/*", HTTP_GET, handleStatic);

    Serial.println("Web server started");
}


void loop()
{
    if (!server)
        return;

    uint32_t now = millis();
    int idle[CONFIG_LWIP_MAX_SOCKETS];
    size_t count = 0;

    portENTER_CRITICAL(&connectionsLock);
    for (const Connection &c : connections)
    {
        if (c.fd >= 0 && c.busy <= 0 && now - c.lastActiveMs >= KEEP_ALIVE_IDLE_MS)
            idle[count++] = c.fd;
    }
    portEXIT_CRITICAL(&connectionsLock);

    for (size_t i = 0; i < count; i++)
    {
        if (httpd_ws_get_fd_info(server, idle[i]) == HTTPD_WS_CLIENT_HTTP)
            httpd_sess_trigger_close(server, idle[i]);
    }
}


httpd_req_t *beginAsync(httpd_req_t *req)
{
    httpd_req_t *async = nullptr;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK)
        return nullptr;

    markBusy(httpd_req_to_sockfd(async), 1);
    return async;
}


void endAsync(httpd_req_t *async)
{
    markBusy(httpd_req_to_sockfd(async), -1);
    httpd_req_async_handler_complete(async);
}


void on(const char *uri, httpd_method_t method, Handler handler)
{
    registerHandler(uri, method, handler, false);
}


void onWebSocket(const char *uri, Handler handler)
{
    registerHandler(uri, HTTP_GET, handler, true);
}


std::vector<int> webSockets(const char *path)
{
    std::vector<int> candidates;

    portENTER_CRITICAL(&connectionsLock);
    for (const Connection &c : connections)
    {
        if (c.fd >= 0 && c.pathKnown && strcmp(c.path, path) == 0)
            candidates.push_back(c.fd);
    }
    portEXIT_CRITICAL(&connectionsLock);

    std::vector<int> out;
    for (int fd : candidates)
    {
        if (server && httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET)
            out.push_back(fd);
    }
    return out;
}


httpd_handle_t handle()
{
    return server;
}

} // namespace WebServer
