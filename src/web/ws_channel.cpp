#include "ws_channel.h"

#include <algorithm>

#include <freertos/task.h>

#include "web_server.h"

namespace
{
    constexpr uint32_t SENDER_STACK_BYTES = 4096;
}


WsChannel::WsChannel(const char *path, const char *taskName, size_t queueLength)
    : path(path), taskName(taskName), queueLength(queueLength)
{
}


void WsChannel::begin()
{
    queue = xQueueCreate(queueLength, sizeof(Message *));
    xTaskCreate(senderTask, taskName, SENDER_STACK_BYTES, this, 1, nullptr);
}


bool WsChannel::receive(httpd_req_t *req, Frame &out, esp_err_t &result)
{
    result = ESP_OK;

    // Some ESP-IDF versions also run the handler for the upgrade request.
    if (req->method == HTTP_GET)
        return false;

    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len > MAX_RECEIVE_BYTES)
    {
        result = ESP_FAIL;
        return false;
    }

    out.data.resize(frame.len);
    frame.payload = out.data.data();
    if (frame.len > 0 && httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK)
    {
        result = ESP_FAIL;
        return false;
    }

    // Fragmented messages aren't used by the web UI.
    if (!frame.final || (frame.type != HTTPD_WS_TYPE_TEXT && frame.type != HTTPD_WS_TYPE_BINARY))
        return false;

    out.fd = httpd_req_to_sockfd(req);
    out.text = frame.type == HTTPD_WS_TYPE_TEXT;
    return true;
}


void WsChannel::sendText(int fd, const String &text, uint32_t waitMs)
{
    enqueue(fd, HTTPD_WS_TYPE_TEXT, reinterpret_cast<const uint8_t *>(text.c_str()), text.length(), waitMs);
}


void WsChannel::sendBinary(int fd, const uint8_t *data, size_t length, uint32_t waitMs)
{
    enqueue(fd, HTTPD_WS_TYPE_BINARY, data, length, waitMs);
}


void WsChannel::broadcastText(const String &text, uint32_t waitMs)
{
    sendText(ALL, text, waitMs);
}


void WsChannel::broadcastBinary(const uint8_t *data, size_t length, uint32_t waitMs)
{
    sendBinary(ALL, data, length, waitMs);
}


void WsChannel::enqueue(int fd, httpd_ws_type_t type, const uint8_t *data, size_t length, uint32_t waitMs)
{
    if (!queue)
        return;

    // No clients, nothing to copy.
    if (fd == ALL && WebServer::webSockets(path).empty())
        return;

    auto *msg = new Message{fd, type, std::vector<uint8_t>(data, data + length)};
    if (xQueueSend(queue, &msg, pdMS_TO_TICKS(waitMs)) != pdTRUE)
        delete msg;
}


std::vector<int> WsChannel::targets(int fd)
{
    std::vector<int> clients = WebServer::webSockets(path);
    if (fd == ALL)
        return clients;

    bool open = std::find(clients.begin(), clients.end(), fd) != clients.end();
    return open ? std::vector<int>{fd} : std::vector<int>();
}


void WsChannel::senderTask(void *arg)
{
    auto *self = static_cast<WsChannel *>(arg);

    for (;;)
    {
        Message *msg = nullptr;
        if (xQueueReceive(self->queue, &msg, portMAX_DELAY) != pdTRUE)
            continue;

        httpd_handle_t server = WebServer::handle();
        for (int fd : self->targets(msg->fd))
        {
            httpd_ws_frame_t frame = {};
            frame.final = true;
            frame.type = msg->type;
            frame.payload = msg->data.data();
            frame.len = msg->data.size();

            // Runs on the server task, so it never interleaves with its own
            // writes to the socket.
            if (httpd_ws_send_data(server, fd, &frame) != ESP_OK)
                httpd_sess_trigger_close(server, fd);
        }

        delete msg;
    }
}
