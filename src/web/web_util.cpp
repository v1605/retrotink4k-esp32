#include "web_util.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace
{
    constexpr uint32_t BACKGROUND_TASK_STACK_BYTES = 8192;

    void backgroundTaskTrampoline(void *arg)
    {
        auto *work = static_cast<std::function<void()> *>(arg);
        (*work)();
        delete work;
        vTaskDelete(nullptr);
    }
}


namespace WebUtil
{

bool runInBackground(std::function<void()> work)
{
    auto *heapWork = new std::function<void()>(std::move(work));

    BaseType_t created = xTaskCreate(
        backgroundTaskTrampoline,
        "esp32_bg_task",
        BACKGROUND_TASK_STACK_BYTES,
        heapWork,
        1,
        nullptr
    );

    if (created != pdPASS)
    {
        delete heapWork;
        return false;
    }

    return true;
}


void runDeferred(
    AsyncWebServerRequest *request,
    std::function<void(AsyncWebServerRequest *)> work)
{
    AsyncWebServerRequestPtr weakRequest = request->pause();

    bool started = runInBackground([weakRequest, work]()
    {
        auto paused = weakRequest.lock();
        if (!paused)
            return;

        work(paused.get());
    });

    // send() resumes the paused request.
    if (!started)
        sendError(request, 503, "Out of memory");
}


std::shared_ptr<std::vector<uint8_t>> collectBody(
    AsyncWebServerRequest *request,
    uint8_t *data,
    size_t len,
    size_t index,
    size_t total,
    size_t maxBytes)
{
    auto *body = static_cast<std::vector<uint8_t> *>(request->_tempObject);

    if (!body)
    {
        // Rejected with an earlier chunk.
        if (index > 0)
            return nullptr;

        // Form-encoded bodies never reach a body handler: ESPAsyncWebServer
        // buffers them into a String and runs out of memory.
        if (request->contentType() != "application/octet-stream")
        {
            sendError(request, 400, "Content-Type must be application/octet-stream");
            return nullptr;
        }

        if (total == 0 || total > maxBytes)
        {
            sendError(request, 400, "Invalid upload size");
            return nullptr;
        }

        body = new std::vector<uint8_t>();
        body->reserve(total);
        request->_tempObject = body;
        
        request->onDisconnect([request]()
        {
            delete static_cast<std::vector<uint8_t> *>(request->_tempObject);
            request->_tempObject = nullptr;
        });
    }

    body->insert(body->end(), data, data + len);

    if (index + len < total)
        return nullptr;

    request->_tempObject = nullptr;
    return std::shared_ptr<std::vector<uint8_t>>(body);
}


bool requireParam(
    AsyncWebServerRequest *request,
    const char *name,
    bool fromBody,
    String &out)
{
    if (!request->hasParam(name, fromBody))
    {
        sendError(request, 400, String("Missing ") + name);
        return false;
    }

    out = request->getParam(name, fromBody)->value();
    return true;
}


String paramOr(AsyncWebServerRequest *request, const char *name, const String &fallback)
{
    return request->hasParam(name) ? request->getParam(name)->value() : fallback;
}


void sendError(AsyncWebServerRequest *request, int code, const String &error)
{
    request->send(code, "application/json", jsonError(error));
}


void sendOkOrError(AsyncWebServerRequest *request, bool ok, const String &error)
{
    if (ok)
        request->send(200, "application/json", jsonOk());
    else
        sendError(request, 503, error);
}


String toJson(JsonDocument &doc)
{
    String out;
    serializeJson(doc, out);
    return out;
}


String jsonOk()
{
    JsonDocument doc;
    doc["ok"] = true;
    return toJson(doc);
}


String jsonError(const String &error)
{
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = error;
    return toJson(doc);
}

}
