#include "terminal_endpoints.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <vector>

#include "../rt4k/link.h"
#include "../serial_bridge.h"

namespace
{
    // Skip the WS broadcast while this much RX data is waiting.
    constexpr size_t BROADCAST_BACKLOG_THRESHOLD = 512;

    constexpr int COMMAND_QUEUE_LEN = 32;
    constexpr uint32_t COMMAND_WORKER_STACK_BYTES = 8192;

    AsyncWebSocket ws("/ws");

    struct QueuedCommand
    {
        uint32_t clientId;
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

                AsyncWebSocketClient *client = ws.client(item->clientId);
                if (client)
                    client->text("[TX failed - " + result.error + "]\r\n");
            }

            delete item;
        }
    }


    // FT232R -> all WebSocket clients
    void handleSerialBridgeData(const uint8_t *data, size_t length)
    {
        // Parse first so feed() keeps up; skip the slower broadcast under a
        // backlog.
        std::vector<uint8_t> textBytes;
        Rt4kLink::feed(data, length, &textBytes);

        if (!textBytes.empty() && SerialBridge::pendingBytes() < BROADCAST_BACKLOG_THRESHOLD)
            ws.binaryAll(textBytes.data(), textBytes.size());
    }


    void handleSerialBridgeStatus(bool /*connected*/, const String &message)
    {
        ws.textAll(message);
    }


    void webSocketEvent(
        AsyncWebSocket *server,
        AsyncWebSocketClient *client,
        AwsEventType type,
        void *arg,
        uint8_t *data,
        size_t len)
    {
        switch (type)
        {
            case WS_EVT_CONNECT:
            {
                Serial.printf("Web client connected: %u\n", client->id());
                break;
            }


            case WS_EVT_DISCONNECT:
            {
                Serial.printf("Web client disconnected: %u\n", client->id());
                break;
            }


            case WS_EVT_DATA:
            {
                AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);

                // Make sure we received one complete frame.
                if (!info->final || info->index != 0 || info->len != len)
                    return;

                // Queued for commandWorkerTask to keep sends in order.
                if (info->opcode == WS_TEXT || info->opcode == WS_BINARY)
                {
                    String text(data, len);
                    while (text.endsWith("\r") || text.endsWith("\n"))
                        text.remove(text.length() - 1);

                    auto *item = new QueuedCommand{client->id(), text};
                    if (xQueueSend(commandQueue, &item, 0) != pdTRUE)
                        delete item;
                }

                break;
            }


            default:
                break;
        }
    }
}


namespace TerminalEndpoints
{

void begin(AsyncWebServer &server)
{
    commandQueue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(QueuedCommand *));
    xTaskCreate(commandWorkerTask, "rt4k_cmd_worker", COMMAND_WORKER_STACK_BYTES, nullptr, 1, nullptr);

    SerialBridge::onData(handleSerialBridgeData);
    SerialBridge::onStatus(handleSerialBridgeStatus);

    ws.onEvent(webSocketEvent);
    server.addHandler(&ws);
}


void cleanupClients()
{
    ws.cleanupClients();
}

} // namespace TerminalEndpoints
