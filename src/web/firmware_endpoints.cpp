#include "firmware_endpoints.h"

#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "web_util.h"
#include "../rt4k/link.h"

namespace
{
    constexpr int STATUS_QUEUE_LEN = 32;

    // JSON status events for firmware uploads and installs.
    AsyncWebSocket statusWs("/ws/firmware");
    QueueHandle_t statusQueue = nullptr;

    // Events come from several tasks, but a client may only be written from
    // one (see osd_endpoints.cpp), so this task sends them all.
    void statusSendTask(void * /*arg*/)
    {
        for (;;)
        {
            String *text = nullptr;
            if (xQueueReceive(statusQueue, &text, portMAX_DELAY) != pdTRUE)
                continue;

            statusWs.textAll(*text);
            delete text;
        }
    }

    // Progress events are dropped when the queue is full; others wait briefly.
    void broadcastStatus(JsonDocument &doc, bool expendable = false)
    {
        auto *text = new String(WebUtil::toJson(doc));
        if (xQueueSend(statusQueue, &text, pdMS_TO_TICKS(expendable ? 0 : 100)) != pdTRUE)
            delete text;
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

    // Body handler: called repeatedly with chunks until index + len == total.
    void handleFirmwareUploadBody(
        AsyncWebServerRequest *request,
        uint8_t *data,
        size_t len,
        size_t index,
        size_t total)
    {
        std::shared_ptr<std::vector<uint8_t>> body =
            WebUtil::collectBody(request, data, len, index, total, Rt4kLink::MAX_FILE_BYTES);
        if (!body)
            return;

        String name;
        if (!WebUtil::requireParam(request, "name", false, name))
            return;

        if (!isFlatFilename(name))
        {
            WebUtil::sendError(request, 400, "Invalid filename");
            return;
        }

        // Not runDeferred(): the write completes even if the client disconnects.
        AsyncWebServerRequestPtr weakRequest = request->pause();

        bool started = WebUtil::runInBackground([weakRequest, name, body]()
        {
            // Whole-percent steps; sent == total means the RT4K is verifying.
            int lastPercent = -1;
            Rt4kLink::Result result = Rt4kLink::uploadFile(
                name,
                body->data(),
                body->size(),
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

            auto paused = weakRequest.lock();
            if (paused)
                WebUtil::sendOkOrError(paused.get(), result.ok, result.error);
        });

        // send() resumes the paused request.
        if (!started)
            WebUtil::sendError(request, 503, "Out of memory");
    }


    void handleFirmwareCheck(AsyncWebServerRequest *request)
    {
        WebUtil::runDeferred(request, [](AsyncWebServerRequest *request)
        {
            sendStatus("checking");

            Rt4kLink::FirmwareUpdate update;
            Rt4kLink::Result result = Rt4kLink::checkFirmwareUpdate(update);

            if (!result.ok)
            {
                sendStatus("error", "", result.error);
                WebUtil::sendOkOrError(request, false, result.error);
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

            request->send(200, "application/json", WebUtil::toJson(doc));
        });
    }


    void handleFirmwareInstall(AsyncWebServerRequest *request)
    {
        String token;
        if (!WebUtil::requireParam(request, "token", true, token))
            return;

        WebUtil::runDeferred(request, [token](AsyncWebServerRequest *request)
        {
            sendStatus("installing");
            Rt4kLink::Result result = Rt4kLink::startFirmwareUpdate(token);
            sendStatus(result.ok ? "flashing" : "error", "", result.error);
            WebUtil::sendOkOrError(request, result.ok, result.error);
        });
    }


    void handleFirmwareDevice(AsyncWebServerRequest *request)
    {
        WebUtil::runDeferred(request, [](AsyncWebServerRequest *request)
        {
            Rt4kLink::DeviceInfo info;
            Rt4kLink::Result result = Rt4kLink::getDeviceInfo(info);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(request, false, result.error);
                return;
            }

            JsonDocument doc;
            doc["ok"] = true;
            doc["modelId"] = info.modelId;
            doc["model"] = info.model;
            doc["version"] = info.version;

            request->send(200, "application/json", WebUtil::toJson(doc));
        });
    }
}


namespace FirmwareEndpoints
{

void registerRoutes(AsyncWebServer &server)
{
    statusQueue = xQueueCreate(STATUS_QUEUE_LEN, sizeof(String *));
    xTaskCreate(statusSendTask, "fw_status_ws", 4096, nullptr, 1, nullptr);
    server.addHandler(&statusWs);

    server.on(
        "/api/firmware/upload",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        nullptr,
        handleFirmwareUploadBody
    );
    server.on("/api/firmware/check", HTTP_GET, handleFirmwareCheck);
    server.on("/api/firmware/install", HTTP_POST, handleFirmwareInstall);
    server.on("/api/firmware/device", HTTP_GET, handleFirmwareDevice);
}


void cleanupClients()
{
    statusWs.cleanupClients();
}

} // namespace FirmwareEndpoints
