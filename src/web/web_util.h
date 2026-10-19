#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>

#include <functional>
#include <utility>
#include <vector>

// Small helpers shared across the web/ endpoint modules.

namespace WebUtil
{
    // Runs work on a short-lived FreeRTOS task, off the HTTP server's task.
    // Returns false, without running it, if the task couldn't be created.
    bool runInBackground(std::function<void()> work);

    // For blocking handlers: hands the request to a new task, where work()
    // answers it. Answers 503 itself if that isn't possible.
    esp_err_t runDeferred(httpd_req_t *req, std::function<void(httpd_req_t *)> work);

    // Query string or form fields, URL-decoded.
    struct Params
    {
        std::vector<std::pair<String, String>> items;

        bool has(const char *name) const;
        String get(const char *name, const String &fallback = "") const;
    };

    Params queryParams(httpd_req_t *req);

    // Reads an application/x-www-form-urlencoded body. Answers 400 itself
    // and returns false on failure.
    bool readForm(httpd_req_t *req, Params &out);

    // Reads a required parameter, or answers 400 and returns false.
    bool requireParam(httpd_req_t *req, const Params &params, const char *name, String &out);

    // Request path without the query string, URL-decoded.
    String path(httpd_req_t *req);

    // Header value, or "" if absent.
    String header(httpd_req_t *req, const char *name);

    // Whether the body is declared application/octet-stream.
    bool isOctetStream(httpd_req_t *req);

    // Reads length bytes of the body. False if the connection failed.
    bool readBody(httpd_req_t *req, uint8_t *out, size_t length);

    // Has the server close the connection after the reply, for errors with
    // an unread body.
    void closeConnection(httpd_req_t *req);

    // A reply with extra headers; their values are kept until sent.
    class Response
    {
    public:
        explicit Response(httpd_req_t *req) : req(req) {}

        Response &header(const char *name, String value);
        esp_err_t send(int code, const char *contentType, const void *body, size_t length);
        esp_err_t sendJson(int code, const String &json);

    private:
        httpd_req_t *req;
        std::vector<std::pair<const char *, String>> headers;
    };

    esp_err_t sendJson(httpd_req_t *req, int code, const String &json);

    // code + {"ok":false,"error":...}.
    esp_err_t sendError(httpd_req_t *req, int code, const String &error);

    // 200 + {"ok":true}, or 503 + {"ok":false,"error":...}.
    esp_err_t sendOkOrError(httpd_req_t *req, bool ok, const String &error);

    String toJson(JsonDocument &doc);
    String jsonOk();
    String jsonError(const String &error);
}
