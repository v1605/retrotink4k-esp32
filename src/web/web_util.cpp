#include "web_util.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <vector>

#include "web_server.h"

namespace
{
    constexpr uint32_t BACKGROUND_TASK_STACK_BYTES = 8192;
    constexpr size_t MAX_FORM_BYTES = 4096;
    // Consecutive socket timeouts before a body read or send gives up.
    constexpr int MAX_RECV_TIMEOUTS = 3;

    void backgroundTaskTrampoline(void *arg)
    {
        auto *work = static_cast<std::function<void()> *>(arg);
        (*work)();
        delete work;
        vTaskDelete(nullptr);
    }


    int hexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }


    // %XX escapes, and '+' as space when plusIsSpace.
    String urlDecode(const char *text, size_t length, bool plusIsSpace)
    {
        String out;
        out.reserve(length);

        for (size_t i = 0; i < length; i++)
        {
            char c = text[i];
            if (c == '+' && plusIsSpace)
            {
                out += ' ';
            }
            else if (c == '%' && i + 2 < length && hexValue(text[i + 1]) >= 0 && hexValue(text[i + 2]) >= 0)
            {
                out += static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
                i += 2;
            }
            else
            {
                out += c;
            }
        }
        return out;
    }


    void parseFields(const char *text, size_t length, WebUtil::Params &out)
    {
        size_t start = 0;
        while (start < length)
        {
            const char *amp = static_cast<const char *>(memchr(text + start, '&', length - start));
            size_t end = amp ? static_cast<size_t>(amp - text) : length;

            if (end > start)
            {
                const char *eq = static_cast<const char *>(memchr(text + start, '=', end - start));
                size_t keyEnd = eq ? static_cast<size_t>(eq - text) : end;
                size_t valueStart = eq ? keyEnd + 1 : end;

                out.items.emplace_back(
                    urlDecode(text + start, keyEnd - start, true),
                    urlDecode(text + valueStart, end - valueStart, true));
            }
            start = end + 1;
        }
    }


    // Replies up to this size go out in one write with their head.
    constexpr size_t SINGLE_WRITE_BODY_BYTES = 8192;

    esp_err_t sendAll(httpd_req_t *req, const char *data, size_t length)
    {
        int timeouts = 0;
        while (length > 0)
        {
            int n = httpd_send(req, data, length);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS)
                continue;
            if (n <= 0)
                return ESP_FAIL;

            timeouts = 0;
            data += n;
            length -= n;
        }
        return ESP_OK;
    }


    const char *statusLine(int code)
    {
        switch (code)
        {
            case 200: return "200 OK";
            case 201: return "201 Created";
            case 202: return "202 Accepted";
            case 204: return "204 No Content";
            case 400: return "400 Bad Request";
            case 403: return "403 Forbidden";
            case 404: return "404 Not Found";
            case 409: return "409 Conflict";
            case 413: return "413 Content Too Large";
            case 429: return "429 Too Many Requests";
            case 500: return "500 Internal Server Error";
            case 503: return "503 Service Unavailable";
            case 504: return "504 Gateway Timeout";
            default: return "500 Internal Server Error";
        }
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


esp_err_t runDeferred(httpd_req_t *req, std::function<void(httpd_req_t *)> work)
{
    httpd_req_t *async = WebServer::beginAsync(req);
    if (!async)
        return sendError(req, 503, "Out of memory");

    bool started = runInBackground([async, work]()
    {
        work(async);
        WebServer::endAsync(async);
    });

    if (!started)
    {
        sendError(async, 503, "Out of memory");
        WebServer::endAsync(async);
    }
    return ESP_OK;
}


bool Params::has(const char *name) const
{
    for (const auto &item : items)
    {
        if (item.first == name)
            return true;
    }
    return false;
}


String Params::get(const char *name, const String &fallback) const
{
    for (const auto &item : items)
    {
        if (item.first == name)
            return item.second;
    }
    return fallback;
}


Params queryParams(httpd_req_t *req)
{
    Params params;
    size_t length = httpd_req_get_url_query_len(req);
    if (length == 0)
        return params;

    std::unique_ptr<char[]> query(new char[length + 1]);
    if (httpd_req_get_url_query_str(req, query.get(), length + 1) == ESP_OK)
        parseFields(query.get(), length, params);
    return params;
}


bool readForm(httpd_req_t *req, Params &out)
{
    size_t length = req->content_len;
    if (length > MAX_FORM_BYTES)
    {
        sendError(req, 400, "Request body too large");
        closeConnection(req);
        return false;
    }

    std::unique_ptr<char[]> body(new char[length + 1]);
    if (!readBody(req, reinterpret_cast<uint8_t *>(body.get()), length))
    {
        sendError(req, 400, "Couldn't read the request body");
        return false;
    }

    parseFields(body.get(), length, out);
    return true;
}


bool requireParam(httpd_req_t *req, const Params &params, const char *name, String &out)
{
    if (!params.has(name))
    {
        sendError(req, 400, String("Missing ") + name);
        return false;
    }

    out = params.get(name);
    return true;
}


String path(httpd_req_t *req)
{
    const char *uri = req->uri;
    const char *query = strchr(uri, '?');
    size_t length = query ? static_cast<size_t>(query - uri) : strlen(uri);
    return urlDecode(uri, length, false);
}


String header(httpd_req_t *req, const char *name)
{
    size_t length = httpd_req_get_hdr_value_len(req, name);
    if (length == 0)
        return String();

    std::unique_ptr<char[]> value(new char[length + 1]);
    if (httpd_req_get_hdr_value_str(req, name, value.get(), length + 1) != ESP_OK)
        return String();
    return String(value.get());
}


bool isOctetStream(httpd_req_t *req)
{
    return header(req, "Content-Type").startsWith("application/octet-stream");
}


bool readBody(httpd_req_t *req, uint8_t *out, size_t length)
{
    size_t received = 0;
    int timeouts = 0;

    while (received < length)
    {
        int n = httpd_req_recv(req, reinterpret_cast<char *>(out + received), length - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS)
            continue;
        if (n <= 0)
            return false;

        timeouts = 0;
        received += n;
    }
    return true;
}


void closeConnection(httpd_req_t *req)
{
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
}


Response &Response::header(const char *name, String value)
{
    headers.emplace_back(name, std::move(value));
    return *this;
}


// httpd_resp_send() writes each header in several small pieces, and with
// TCP_NODELAY every piece is its own packet. This assembles the head (and a
// small body) into one write instead.
esp_err_t Response::send(int code, const char *contentType, const void *body, size_t length)
{
    String head;
    head.reserve(128);
    head += "HTTP/1.1 ";
    head += statusLine(code);
    head += "\r\nContent-Type: ";
    head += contentType;
    head += "\r\nContent-Length: ";
    head += String(static_cast<unsigned long>(length));
    head += "\r\n";
    for (const auto &h : headers)
    {
        head += h.first;
        head += ": ";
        head += h.second;
        head += "\r\n";
    }
    head += "\r\n";

    esp_err_t result;
    if (length <= SINGLE_WRITE_BODY_BYTES)
    {
        std::vector<char> packet(head.c_str(), head.c_str() + head.length());
        packet.insert(packet.end(), static_cast<const char *>(body), static_cast<const char *>(body) + length);
        result = sendAll(req, packet.data(), packet.size());
    }
    else
    {
        result = sendAll(req, head.c_str(), head.length());
        if (result == ESP_OK)
            result = sendAll(req, static_cast<const char *>(body), length);
    }
    return result;
}


esp_err_t Response::sendJson(int code, const String &json)
{
    return send(code, "application/json", json.c_str(), json.length());
}


esp_err_t sendJson(httpd_req_t *req, int code, const String &json)
{
    return Response(req).sendJson(code, json);
}


esp_err_t sendError(httpd_req_t *req, int code, const String &error)
{
    return sendJson(req, code, jsonError(error));
}


esp_err_t sendOkOrError(httpd_req_t *req, bool ok, const String &error)
{
    return ok ? sendJson(req, 200, jsonOk()) : sendError(req, 503, error);
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

} // namespace WebUtil
