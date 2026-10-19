#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// The clients of one WebSocket endpoint. Sends from any task go through a
// queue to one sender task, so a slow client never blocks the caller.
class WsChannel
{
public:
    struct Frame
    {
        int fd = -1;
        bool text = false;
        std::vector<uint8_t> data;
    };

    // path: the endpoint's URI, which identifies its connections.
    WsChannel(const char *path, const char *taskName, size_t queueLength);

    void begin();

    // Call from the endpoint's WebSocket handler. Returns true with a
    // complete text or binary frame in out. result is what the handler
    // should return.
    bool receive(httpd_req_t *req, Frame &out, esp_err_t &result);

    // waitMs: how long to wait for queue space before dropping the message.
    void sendText(int fd, const String &text, uint32_t waitMs = 0);
    void sendBinary(int fd, const uint8_t *data, size_t length, uint32_t waitMs = 0);
    void broadcastText(const String &text, uint32_t waitMs = 0);
    void broadcastBinary(const uint8_t *data, size_t length, uint32_t waitMs = 0);

private:
    static constexpr int ALL = -1;
    static constexpr size_t MAX_RECEIVE_BYTES = 4096;

    struct Message
    {
        int fd;
        httpd_ws_type_t type;
        std::vector<uint8_t> data;
    };

    void enqueue(int fd, httpd_ws_type_t type, const uint8_t *data, size_t length, uint32_t waitMs);
    std::vector<int> targets(int fd);
    static void senderTask(void *arg);

    const char *path;
    const char *taskName;
    size_t queueLength;
    QueueHandle_t queue = nullptr;
};
