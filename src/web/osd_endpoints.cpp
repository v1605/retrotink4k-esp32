#include "osd_endpoints.h"

#include <ArduinoJson.h>
#include <base64.h>

#include "web_server.h"
#include "web_util.h"
#include "ws_channel.h"
#include "../rt4k/link.h"

namespace
{
    constexpr int SEND_QUEUE_LEN = 16;
    // Room for a reply that is still being built when the queue is full.
    constexpr uint32_t REPLY_WAIT_MS = 100;

    // Requests run on separate tasks; the channel's sender task keeps each
    // client's messages in order.
    WsChannel osdWs("/ws/osd", "osd_ws_send", SEND_QUEUE_LEN);

    void sendToClient(int clientId, JsonDocument &doc)
    {
        osdWs.sendText(clientId, WebUtil::toJson(doc), REPLY_WAIT_MS);
    }


    // Answers a request whose worker task couldn't be created.
    void sendStartFailure(int clientId, int requestId, const char *type)
    {
        JsonDocument doc;
        doc["id"] = requestId;
        doc["type"] = type;
        doc["ok"] = false;
        doc["error"] = "Out of memory";
        sendToClient(clientId, doc);
    }


    void handleFontRequest(int clientId, int requestId)
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


    void handlePlaneRequest(int clientId, int requestId, int plane)
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
    void handleBannerRequest(int clientId, int requestId, const String &knownPath)
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
        osdWs.sendText(clientId, WebUtil::toJson(doc), REPLY_WAIT_MS);
        osdWs.sendBinary(clientId, bytes.data(), bytes.size(), REPLY_WAIT_MS);
    }


    void handleWsMessage(int clientId, const String &text)
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


    esp_err_t handleWs(httpd_req_t *req)
    {
        WsChannel::Frame frame;
        esp_err_t result;
        if (!osdWs.receive(req, frame, result) || !frame.text)
            return result;

        String text(reinterpret_cast<const char *>(frame.data.data()), frame.data.size());
        handleWsMessage(frame.fd, text);
        return ESP_OK;
    }
}


namespace OsdEndpoints
{

void registerRoutes()
{
    osdWs.begin();
    WebServer::onWebSocket("/ws/osd", handleWs);
}


void notifyRemoteDone(const String &button, bool ok)
{
    JsonDocument doc;
    doc["type"] = "remote";
    doc["button"] = button;
    doc["ok"] = ok;
    osdWs.broadcastText(WebUtil::toJson(doc));
}

} // namespace OsdEndpoints
