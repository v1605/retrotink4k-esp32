#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <functional>
#include <memory>
#include <vector>

// Small helpers shared across the web/ endpoint modules.

namespace WebUtil
{
    // Runs work on a short-lived FreeRTOS task, off AsyncTCP's shared task.
    // Returns false, without running it, if the task couldn't be created.
    bool runInBackground(std::function<void()> work);

    // Same, for blocking handlers: pauses the request, and skips the work if
    // the client has disconnected. Answers 503 if no task could be created.
    void runDeferred(
        AsyncWebServerRequest *request,
        std::function<void(AsyncWebServerRequest *)> work);

    // For raw-upload body handlers (application/octet-stream, 1 to maxBytes).
    // Returns the whole body with the last chunk, else nullptr; answers 400
    // itself for a bad upload.
    std::shared_ptr<std::vector<uint8_t>> collectBody(
        AsyncWebServerRequest *request,
        uint8_t *data,
        size_t len,
        size_t index,
        size_t total,
        size_t maxBytes);

    // Reads a required parameter, or answers 400 and returns false. fromBody
    // reads POST fields instead of the query string.
    bool requireParam(
        AsyncWebServerRequest *request,
        const char *name,
        bool fromBody,
        String &out);

    // Reads an optional query parameter.
    String paramOr(AsyncWebServerRequest *request, const char *name, const String &fallback);

    // code + {"ok":false,"error":...}.
    void sendError(AsyncWebServerRequest *request, int code, const String &error);

    // 200 + {"ok":true}, or 503 + {"ok":false,"error":...}.
    void sendOkOrError(AsyncWebServerRequest *request, bool ok, const String &error);

    String toJson(JsonDocument &doc);
    String jsonOk();
    String jsonError(const String &error);
}
