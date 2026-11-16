#include "firmware_endpoints.h"

#include <memory>
#include <new>
#include <vector>

#include "web_server.h"
#include "web_util.h"
#include "ws_channel.h"
#include "../rt4k/link.h"

namespace
{
    constexpr int STATUS_QUEUE_LEN = 32;

    // JSON status events for firmware uploads and installs.
    WsChannel statusWs("/ws/firmware", "fw_status_ws", STATUS_QUEUE_LEN);

    // Progress events are dropped when the queue is full; others wait briefly.
    void broadcastStatus(JsonDocument &doc, bool expendable = false)
    {
        statusWs.broadcastText(WebUtil::toJson(doc), expendable ? 0 : 100);
    }

    void sendStatus(const char *type, const String &file = "", const String &message = "")
    {
        JsonDocument doc;
        doc["type"] = type;
        if (file.length())
            doc["file"] = file;
        if (message.length())
            doc["message"] = message;
        broadcastStatus(doc);
    }


    // Update files must be at the SD card root.
    bool isFlatFilename(const String &name)
    {
        return name.length() > 0 && name.indexOf('/') < 0 && name.indexOf('\\') < 0;
    }

    esp_err_t handleFirmwareUpload(httpd_req_t *req)
    {
        String name;
        if (!WebUtil::requireParam(req, WebUtil::queryParams(req), "name", name))
        {
            WebUtil::closeConnection(req);
            return ESP_OK;
        }

        if (!isFlatFilename(name))
        {
            WebUtil::sendError(req, 400, "Invalid filename");
            WebUtil::closeConnection(req);
            return ESP_OK;
        }

        // The body arrives on the worker, so a large upload doesn't hold up
        // the server.
        return WebUtil::runDeferred(req, [name](httpd_req_t *req)
        {
            size_t length = req->content_len;
            if (!WebUtil::isOctetStream(req) || length == 0 || length > Rt4kLink::MAX_FILE_BYTES)
            {
                WebUtil::sendError(req, 400, WebUtil::isOctetStream(req)
                    ? "Invalid upload size" : "Content-Type must be application/octet-stream");
                WebUtil::closeConnection(req);
                return;
            }

            std::vector<uint8_t> body(length);
            std::unique_ptr<uint8_t[]> body(new (std::nothrow) uint8_t[length]);
            if (!body)
            {
                WebUtil::sendError(req, 503, "Not enough memory for the upload");
                WebUtil::closeConnection(req);
                return;
            }

            if (!WebUtil::readBody(req, body.get(), length))
                return;

            // Whole-percent steps; sent == total means the RT4K is verifying.
            int lastPercent = -1;
            Rt4kLink::Result result = Rt4kLink::uploadFile(
                name,
                body.get(),
                length,
                [&](size_t sent, size_t total)
                {
                    if (sent >= total)
                    {
                        sendStatus("verifying", name);
                        return;
                    }

                    int percent = static_cast<int>(sent * 100 / total);
                    if (percent == lastPercent)
                        return;
                    lastPercent = percent;

                    JsonDocument doc;
                    doc["type"] = "progress";
                    doc["file"] = name;
                    doc["sent"] = sent;
                    doc["total"] = total;
                    broadcastStatus(doc, true);
                }
            );

            if (result.ok)
                sendStatus("uploaded", name);
            else
                sendStatus("error", name, result.error);

            WebUtil::sendOkOrError(req, result.ok, result.error);
        });
    }


    esp_err_t handleFirmwareCheck(httpd_req_t *req)
    {
        return WebUtil::runDeferred(req, [](httpd_req_t *req)
        {
            sendStatus("checking");

            Rt4kLink::FirmwareUpdate update;
            Rt4kLink::Result result = Rt4kLink::checkFirmwareUpdate(update);

            if (!result.ok)
            {
                sendStatus("error", "", result.error);
                WebUtil::sendOkOrError(req, false, result.error);
                return;
            }

            JsonDocument event;
            event["type"] = "checked";
            event["version"] = update.version;
            broadcastStatus(event);

            JsonDocument doc;
            doc["ok"] = true;
            doc["version"] = update.version;
            doc["token"] = update.token;

            WebUtil::sendJson(req, 200, WebUtil::toJson(doc));
        });
    }


    esp_err_t handleFirmwareInstall(httpd_req_t *req)
    {
        WebUtil::Params form;
        String token;
        if (!WebUtil::readForm(req, form) || !WebUtil::requireParam(req, form, "token", token))
            return ESP_OK;

        return WebUtil::runDeferred(req, [token](httpd_req_t *req)
        {
            sendStatus("installing");
            Rt4kLink::Result result = Rt4kLink::startFirmwareUpdate(token);
            sendStatus(result.ok ? "flashing" : "error", "", result.error);
            WebUtil::sendOkOrError(req, result.ok, result.error);
        });
    }


    esp_err_t handleFirmwareDevice(httpd_req_t *req)
    {
        return WebUtil::runDeferred(req, [](httpd_req_t *req)
        {
            Rt4kLink::DeviceInfo info;
            Rt4kLink::Result result = Rt4kLink::getDeviceInfo(info);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(req, false, result.error);
                return;
            }

            JsonDocument doc;
            doc["ok"] = true;
            doc["modelId"] = info.modelId;
            doc["model"] = info.model;
            doc["version"] = info.version;

            WebUtil::sendJson(req, 200, WebUtil::toJson(doc));
        });
    }

    esp_err_t handleStatusWs(httpd_req_t *req)
    {
        // Events only flow out; incoming frames are ignored.
        WsChannel::Frame frame;
        esp_err_t result;
        statusWs.receive(req, frame, result);
        return result;
    }
}


namespace FirmwareEndpoints
{

void registerRoutes()
{
    statusWs.begin();
    WebServer::onWebSocket("/ws/firmware", handleStatusWs);

    WebServer::on("/api/firmware/upload", HTTP_POST, handleFirmwareUpload);
    WebServer::on("/api/firmware/check", HTTP_GET, handleFirmwareCheck);
    WebServer::on("/api/firmware/install", HTTP_POST, handleFirmwareInstall);
    WebServer::on("/api/firmware/device", HTTP_GET, handleFirmwareDevice);
}

} // namespace FirmwareEndpoints
