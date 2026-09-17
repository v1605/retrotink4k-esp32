#include "osd_endpoints.h"

#include <ArduinoJson.h>
#include <base64.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "web_util.h"
#include "../rt4k/link.h"

namespace
{
    constexpr int SEND_QUEUE_LEN = 16;
    // Client ids start at 1.
    constexpr uint32_t BROADCAST = 0;

    AsyncWebSocket osdWs("/ws/osd");
    QueueHandle_t sendQueue = nullptr;

    struct OutgoingMessage
    {
        uint32_t clientId;
        String text;
        std::vector<uint8_t> binaryPayload;
        bool hasBinary = false;
    };

    // Requests run on separate tasks, but a client may only be written from
    // one, so all sends go through this queue.
    void sendWorkerTask(void * /*arg*/)
    {
        for (;;)
        {
            OutgoingMessage *msg = nullptr;
            if (xQueueReceive(sendQueue, &msg, portMAX_DELAY) != pdTRUE)
                continue;

            if (msg->clientId == BROADCAST)
            {
                osdWs.textAll(msg->text);
            }
            else if (AsyncWebSocketClient *client = osdWs.client(msg->clientId))
            {
                client->text(msg->text);
                if (msg->hasBinary)
                    client->binary(msg->binaryPayload.data(), msg->binaryPayload.size());
            }

            delete msg;
        }
    }

    void enqueueSend(uint32_t clientId, const String &text, const std::vector<uint8_t> *binaryPayload = nullptr)
    {
        auto *msg = new OutgoingMessage();
        msg->clientId = clientId;
        msg->text = text;

        if (binaryPayload)
        {
            msg->binaryPayload = *binaryPayload;
            msg->hasBinary = true;
        }

        if (xQueueSend(sendQueue, &msg, 0) != pdTRUE)
            delete msg;
    }

    void sendToClient(uint32_t clientId, JsonDocument &doc)
    {
        enqueueSend(clientId, WebUtil::toJson(doc));
    }


    // Answers a request whose worker task couldn't be created.
    void sendStartFailure(uint32_t clientId, int requestId, const char *type)
    {
        JsonDocument doc;
        doc["id"] = requestId;
        doc["type"] = type;
        doc["ok"] = false;
        doc["error"] = "Out of memory";
        sendToClient(clientId, doc);
    }


    void handleFontRequest(uint32_t clientId, int requestId)
    {
        Rt4kLink::OsdFont font;
        Rt4kLink::Result result = Rt4kLink::downloadOsdFont(font);

        JsonDocument doc;
        doc["id"] = requestId;
        doc["type"] = "font";

        if (!result.ok)
        {
            doc["ok"] = false;
            doc["error"] = result.error;
        }
        else
        {
            doc["ok"] = true;
            doc["data"] = base64::encode(font.data.data(), font.data.size());
        }

        sendToClient(clientId, doc);
    }


    void handlePlaneRequest(uint32_t clientId, int requestId, int plane)
    {
        Rt4kLink::OsdPlane osdPlane;
        Rt4kLink::Result result = Rt4kLink::downloadOsdPlane(plane, osdPlane);

        JsonDocument doc;
        doc["id"] = requestId;
        doc["type"] = "plane";

        if (!result.ok)
        {
            doc["ok"] = false;
            doc["error"] = result.error;
            sendToClient(clientId, doc);
            return;
        }

        doc["ok"] = true;
        doc["available"] = osdPlane.available;

        if (osdPlane.available)
        {
            doc["on"] = osdPlane.on;
            doc["osk"] = osdPlane.osk;
            doc["rows"] = osdPlane.rows;
            doc["width"] = osdPlane.width;
            doc["stride"] = osdPlane.stride;
            doc["cells"] = osdPlane.cells;
            doc["text"] = base64::encode(osdPlane.text.data(), osdPlane.text.size());
            doc["color"] = base64::encode(osdPlane.color.data(), osdPlane.color.size());
        }

        sendToClient(clientId, doc);
    }


    // The image goes as a binary frame, not base64 JSON, to save RAM. The
    // JSON metadata (hasData: true) is sent first.
    void handleBannerRequest(uint32_t clientId, int requestId, const String &knownPath)
    {
        Rt4kLink::BannerInfo info;
        std::vector<uint8_t> bytes;
        bool unchanged = false;
        Rt4kLink::Result result = Rt4kLink::downloadBanner(info, bytes, knownPath, unchanged);

        JsonDocument doc;
        doc["id"] = requestId;
        doc["type"] = "banner";

        if (!result.ok)
        {
            doc["ok"] = false;
            doc["error"] = result.error;
            sendToClient(clientId, doc);
            return;
        }

        doc["ok"] = true;
        doc["available"] = info.present;

        if (!info.present)
        {
            sendToClient(clientId, doc);
            return;
        }

        doc["unchanged"] = unchanged;
        doc["path"] = info.path;

        if (unchanged)
        {
            sendToClient(clientId, doc);
            return;
        }

        doc["hasData"] = true;
        enqueueSend(clientId, WebUtil::toJson(doc), &bytes);
    }


    void handleWsMessage(uint32_t clientId, const String &text)
    {
        JsonDocument request;
        if (deserializeJson(request, text) != DeserializationError::Ok)
            return;

        String type = request["type"] | "";
        int requestId = request["id"] | 0;

        if (type == "font")
        {
            if (!WebUtil::runInBackground([clientId, requestId]() { handleFontRequest(clientId, requestId); }))
                sendStartFailure(clientId, requestId, "font");
        }
        else if (type == "plane")
        {
            int plane = request["plane"] | 1;
            if (!WebUtil::runInBackground([clientId, requestId, plane]() { handlePlaneRequest(clientId, requestId, plane); }))
                sendStartFailure(clientId, requestId, "plane");
        }
        else if (type == "banner")
        {
            String knownPath = request["knownPath"] | "";
            if (!WebUtil::runInBackground([clientId, requestId, knownPath]() { handleBannerRequest(clientId, requestId, knownPath); }))
                sendStartFailure(clientId, requestId, "banner");
        }
    }


    void osdWsEvent(
        AsyncWebSocket * /*server*/,
        AsyncWebSocketClient *client,
        AwsEventType type,
        void *arg,
        uint8_t *data,
        size_t len)
    {
        if (type != WS_EVT_DATA)
            return;

        AwsFrameInfo *info = reinterpret_cast<AwsFrameInfo *>(arg);
        if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT)
            return;

        String text(data, len);
        handleWsMessage(client->id(), text);
    }
}


namespace OsdEndpoints
{

void registerRoutes(AsyncWebServer &server)
{
    sendQueue = xQueueCreate(SEND_QUEUE_LEN, sizeof(OutgoingMessage *));
    xTaskCreate(sendWorkerTask, "osd_ws_send", 4096, nullptr, 1, nullptr);

    osdWs.onEvent(osdWsEvent);
    server.addHandler(&osdWs);
}


void notifyRemoteDone(const String &button, bool ok)
{
    if (!sendQueue)
        return;

    JsonDocument doc;
    doc["type"] = "remote";
    doc["button"] = button;
    doc["ok"] = ok;
    enqueueSend(BROADCAST, WebUtil::toJson(doc));
}


void cleanupClients()
{
    osdWs.cleanupClients();
}

} // namespace OsdEndpoints
