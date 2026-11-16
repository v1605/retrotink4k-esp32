#include "terminal_endpoints.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <vector>

#include "web_server.h"
#include "ws_channel.h"
#include "../bridge/retro_bridge.h"
#include "../rt4k/link.h"
#include "../ftdi/serial_bridge.h"

namespace
{
    // Skip the WS broadcast while this much RX data is waiting.
    constexpr size_t BROADCAST_BACKLOG_THRESHOLD = 512;

    constexpr int COMMAND_QUEUE_LEN = 32;
    constexpr uint32_t COMMAND_WORKER_STACK_BYTES = 8192;
    constexpr size_t SEND_QUEUE_LEN = 64;

    WsChannel ws("/ws", "terminal_ws", SEND_QUEUE_LEN);

    struct QueuedCommand
    {
        int fd;
        String text;
    };

    QueueHandle_t commandQueue = nullptr;

    // Runs typed commands one at a time, in arrival order.
    void commandWorkerTask(void * /*arg*/)
    {
        for (;;)
        {
            QueuedCommand *item = nullptr;
            if (xQueueReceive(commandQueue, &item, portMAX_DELAY) != pdTRUE)
                continue;

            Rt4kLink::Result result = Rt4kLink::sendRawCommand(item->text);

            if (!result.ok)
            {
                Serial.println("FTDI TX failed or not ready");
                ws.sendText(item->fd, "[TX failed - " + result.error + "]\r\n");
            }

            delete item;
        }
    }


    // FT232R -> all WebSocket clients
    void handleSerialBridgeData(const uint8_t *data, size_t length)
    {
        // No parser or terminal mirror for bridge traffic.
        if (RetroBridge::ownsLink())
        {
            RetroBridge::onSerialRx(data, length);
            return;
        }

        // Parse first so feed() keeps up; skip the slower broadcast under a
        // backlog.
        std::vector<uint8_t> textBytes;
        textBytes.reserve(length);
        Rt4kLink::feed(data, length, &textBytes);

        if (!textBytes.empty() && SerialBridge::pendingBytes() < BROADCAST_BACKLOG_THRESHOLD)
            ws.broadcastBinary(textBytes.data(), textBytes.size());
    }


    void handleSerialBridgeStatus(bool /*connected*/, const String &message)
    {
        ws.broadcastText(message);
    }


    esp_err_t handleWs(httpd_req_t *req)
    {
        WsChannel::Frame frame;
        esp_err_t result;
        if (!ws.receive(req, frame, result))
            return result;

        // Queued for commandWorkerTask to keep sends in order.
        String text(reinterpret_cast<const char *>(frame.data.data()), frame.data.size());
        while (text.endsWith("\r") || text.endsWith("\n"))
            text.remove(text.length() - 1);

        auto *item = new QueuedCommand{frame.fd, text};
        if (xQueueSend(commandQueue, &item, 0) != pdTRUE)
            delete item;

        return ESP_OK;
    }
}


namespace TerminalEndpoints
{

void registerRoutes()
{
    commandQueue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(QueuedCommand *));
    xTaskCreate(commandWorkerTask, "rt4k_cmd_worker", COMMAND_WORKER_STACK_BYTES, nullptr, 1, nullptr);

    ws.begin();
    SerialBridge::onData(handleSerialBridgeData);
    SerialBridge::onStatus(handleSerialBridgeStatus);

    WebServer::onWebSocket("/ws", handleWs);
}

} // namespace TerminalEndpoints
