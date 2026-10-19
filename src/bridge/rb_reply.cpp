#include "rb_internal.h"

#include <freertos/queue.h>

#include "../web/web_server.h"
#include "../web/web_util.h"

// Reply building, and rb_reply: sends parked replies, so a stalled socket
// never blocks rb_sched or the HTTP server task.

namespace
{
    using namespace RetroBridge;

    constexpr UBaseType_t REPLY_TASK_PRIORITY = 5;
    constexpr uint32_t REPLY_TASK_STACK_BYTES = 4096;
    // Each connection has at most one parked request.
    constexpr size_t REPLY_QUEUE_LEN = WebServer::MAX_OPEN_SOCKETS + 4;

    QueueHandle_t replyQueue = nullptr;


    void addCommonHeaders(WebUtil::Response &response)
    {
        response.header("Access-Control-Allow-Origin", "*");
        response.header("Access-Control-Allow-Private-Network", "true");
        response.header("Access-Control-Expose-Headers",
            "X-Retro-Bridge-Result, X-Retro-Bridge-Cursor, X-Retro-Bridge-Overflow, "
            "X-Retro-Bridge-Complete, X-Retro-Bridge-Transport, Retry-After");
        response.header("Cache-Control", "no-store");
    }


    void transmit(Reply &reply)
    {
        if (!reply.skip)
        {
            WebUtil::Response response(reply.req);
            addCommonHeaders(response);
            for (auto &header : reply.headers)
                response.header(header.first, header.second);
            response.send(reply.code, reply.type, reply.body.data(), reply.body.size());
        }

        if (reply.async)
            WebServer::endAsync(reply.req);
        reply.req = nullptr;
    }


    void replyTaskMain(void * /*arg*/)
    {
        for (;;)
        {
            Reply *reply = nullptr;
            if (xQueueReceive(replyQueue, &reply, portMAX_DELAY) != pdTRUE)
                continue;

            transmit(*reply);
            delete reply;
        }
    }
}


namespace RetroBridge
{

void startReplies()
{
    replyQueue = xQueueCreate(REPLY_QUEUE_LEN, sizeof(Reply *));
    BaseType_t created = xTaskCreate(
        replyTaskMain, "rb_reply", REPLY_TASK_STACK_BYTES, nullptr, REPLY_TASK_PRIORITY, nullptr);
    if (!replyQueue || created != pdPASS)
        Serial.println("ERROR: Retro-Bridge reply task failed to start");
}


Reply replyTo(httpd_req_t *req)
{
    Reply reply;
    reply.req = req;
    return reply;
}


Reply replyTo(Parked &parked)
{
    Reply reply;
    reply.req = parked.req;
    reply.async = true;
    reply.skip = parked.dead;
    parked = Parked();
    return reply;
}


void setJson(Reply &reply, int code, const String &json)
{
    reply.code = code;
    reply.type = "application/json";
    reply.body.assign(json.c_str(), json.c_str() + json.length());
}


void setError(Reply &reply, int code, const char *error, uint32_t retryAfterMs)
{
    String json = String("{\"error\":\"") + error + "\"";
    if (retryAfterMs > 0)
    {
        json += ",\"retry_after_ms\":" + String(retryAfterMs);
        reply.headers.emplace_back("Retry-After", String((retryAfterMs + 999) / 1000));
    }
    json += "}";
    setJson(reply, code, json);
}


void setNoContent(Reply &reply)
{
    reply.code = 204;
    reply.type = "text/plain";
    reply.body.clear();
}


void setWakeBusy(Reply &reply)
{
    setJson(reply, 409, "{\"error\":\"serial_busy\",\"state\":\"busy\",\"retry_after_ms\":" + String(BUSY_RETRY_MS) + "}");
    reply.headers.emplace_back("Retry-After", "1");
}


void setTransactionResult(Reply &reply, const Transaction &txn)
{
    // On 503, result holds the error code.
    if (txn.status == 503)
    {
        setError(reply, 503, txn.result, 1000);
        return;
    }

    reply.code = txn.status;
    reply.type = "application/octet-stream";
    reply.body = txn.reply;
    reply.headers.emplace_back("X-Retro-Bridge-Result", txn.result);
    reply.headers.emplace_back("X-Retro-Bridge-Transport", "usb_ftdi");
}


void send(Reply &reply)
{
    if (!reply.req)
        return;

    if (reply.async && replyQueue)
    {
        auto *queued = new Reply(std::move(reply));
        reply.req = nullptr;
        if (xQueueSend(replyQueue, &queued, 0) == pdTRUE)
            return;

        reply = std::move(*queued);
        delete queued;
    }

    transmit(reply);
}


void send(Replies &replies)
{
    for (Reply &reply : replies)
        send(reply);
    replies.clear();
}


bool park(Reply &reply, Parked &out)
{
    httpd_req_t *async = WebServer::beginAsync(reply.req);
    if (!async)
    {
        setError(reply, 503, "http_connections_full", 1000);
        return false;
    }

    out.req = async;
    out.fd = httpd_req_to_sockfd(async);
    out.dead = false;
    reply.req = nullptr;
    return true;
}

} // namespace RetroBridge
