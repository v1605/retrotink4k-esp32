#include "retro_bridge.h"
#include "rb_internal.h"

#include <esp_timer.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <memory>

#include "../web/web_server.h"
#include "../web/web_util.h"

// /api/v1/* handlers; slow routes park an async copy for rb_sched.

namespace
{
    using namespace RetroBridge;

    constexpr char PREFIX[] = "/api/v1/";
    constexpr size_t MAX_SEGMENTS = 6;
    constexpr uint32_t DEFAULT_IDLE_MS = 180;
    constexpr uint32_t MAX_IDLE_MS = 60000;
    constexpr uint32_t DEFAULT_TIMEOUT_MS = 15000;
    constexpr uint32_t MAX_TIMEOUT_MS = 300000;
    constexpr uint32_t DEFAULT_LEASE_LIFETIME_MS = 120000;
    constexpr uint32_t DEFAULT_LEASE_IDLE_MS = 15000;
    constexpr uint32_t DEFAULT_COMPLETION_IDLE_MS = 1000;
    constexpr uint32_t DEFAULT_WAKE_TIMEOUT_MS = 30000;
    constexpr uint32_t MAX_WAKE_TIMEOUT_MS = 120000;

    struct Route
    {
        String parts[MAX_SEGMENTS];
        size_t count = 0;

        bool is(size_t n, const char *a, const char *b = nullptr, const char *c = nullptr, const char *d = nullptr) const
        {
            const char *want[] = {a, b, c, d};
            if (count != n)
                return false;
            for (size_t i = 0; i < n && i < 4; i++)
            {
                if (want[i] && parts[i] != want[i])
                    return false;
            }
            return true;
        }
    };


    bool parseRoute(httpd_req_t *req, Route &out)
    {
        String path = WebUtil::path(req);
        if (!path.startsWith(PREFIX))
            return false;

        int start = sizeof(PREFIX) - 1;
        while (start <= static_cast<int>(path.length()))
        {
            int slash = path.indexOf('/', start);
            int end = slash < 0 ? path.length() : slash;
            if (out.count == MAX_SEGMENTS)
                return false;
            out.parts[out.count++] = path.substring(start, end);
            if (slash < 0)
                break;
            start = slash + 1;
        }
        return true;
    }


    bool parseU64(const String &text, uint64_t &out)
    {
        if (text.length() == 0 || text.length() > 20)
            return false;
        for (size_t i = 0; i < text.length(); i++)
        {
            if (!isdigit(static_cast<unsigned char>(text[i])))
                return false;
        }

        errno = 0;
        char *end = nullptr;
        unsigned long long value = strtoull(text.c_str(), &end, 10);
        if (errno != 0 || *end != '\0')
            return false;
        out = value;
        return true;
    }


    bool parseU32(const String &text, uint32_t &out)
    {
        uint64_t value = 0;
        if (!parseU64(text, value) || value > UINT32_MAX)
            return false;
        out = static_cast<uint32_t>(value);
        return true;
    }


    // False only when present and invalid.
    bool optionalU32(const WebUtil::Params &params, const char *name, uint32_t fallback, uint32_t min, uint32_t max, uint32_t &out)
    {
        if (!params.has(name))
        {
            out = fallback;
            return true;
        }
        return parseU32(params.get(name), out) && out >= min && out <= max;
    }


    esp_err_t reply(httpd_req_t *req, int code, const String &json)
    {
        Reply r = replyTo(req);
        setJson(r, code, json);
        send(r);
        return ESP_OK;
    }


    esp_err_t error(httpd_req_t *req, int code, const char *err, uint32_t retryAfterMs = 0)
    {
        Reply r = replyTo(req);
        setError(r, code, err, retryAfterMs);
        send(r);
        return ESP_OK;
    }


    esp_err_t noContent(httpd_req_t *req)
    {
        Reply r = replyTo(req);
        setNoContent(r);
        send(r);
        return ESP_OK;
    }


    // Sends r, or wakes rb_sched if park() took the request.
    esp_err_t answer(Reply &r)
    {
        if (r.req)
            send(r);
        else
            notifyScheduler();
        return ESP_OK;
    }


    // Answers 400 itself on failure.
    bool readBody(httpd_req_t *req, size_t limit, std::vector<uint8_t> &out)
    {
        size_t length = req->content_len;
        if (length > limit)
        {
            error(req, 400, "invalid_request");
            WebUtil::closeConnection(req);
            return false;
        }

        out.resize(length);
        if (length > 0 && !WebUtil::readBody(req, out.data(), length))
        {
            error(req, 400, "malformed_request");
            WebUtil::closeConnection(req);
            return false;
        }
        return true;
    }


    // An unread body would be parsed as the next request.
    bool discardBody(httpd_req_t *req)
    {
        std::vector<uint8_t> ignored;
        return req->content_len == 0 || readBody(req, LEASE_WRITE_BYTES, ignored);
    }


    esp_err_t createClient(httpd_req_t *req)
    {
        uint32_t id = 0;
        {
            Guard guard;
            if (BridgeClient *client = RetroBridge::createClient())
                id = client->id;
        }

        if (id == 0)
            return error(req, 429, "client_limit");
        return reply(req, 200, "{\"client_id\":" + String(id) + ",\"expires_ms\":" + String(CLIENT_EXPIRY_MS) + "}");
    }


    esp_err_t deleteClient(httpd_req_t *req, uint32_t clientId)
    {
        Replies out;
        bool found = false;
        {
            Guard guard;
            if (BridgeClient *client = findClient(clientId))
            {
                removeClient(*client, out);
                found = true;
            }
        }
        if (!found)
            return error(req, 404, "no_client");

        send(out);
        notifyScheduler();
        return noContent(req);
    }


    void trimRecent(BridgeClient &client)
    {
        size_t done = 0;
        for (Transaction *txn : client.transactions)
        {
            if (txn->state == TxnState::Done)
                done++;
        }

        for (auto it = client.transactions.begin(); it != client.transactions.end() && done > RECENT_TRANSACTIONS;)
        {
            if ((*it)->state == TxnState::Done)
            {
                delete *it;
                it = client.transactions.erase(it);
                done--;
            }
            else
            {
                ++it;
            }
        }
    }


    size_t queuedFor(const BridgeClient &client)
    {
        size_t count = 0;
        for (const Transaction *txn : client.transactions)
        {
            if (txn->state == TxnState::Queued)
                count++;
        }
        return count;
    }


    // The query's transaction settings; returns the error code, if any.
    const char *parseTransaction(const WebUtil::Params &params, Transaction &txn)
    {
        if (!parseU32(params.get("request_id"), txn.requestId))
            return "invalid_request";

        String completion = params.get("completion", "idle_gap");
        if (completion == "first_reply_line")
            txn.completion = Completion::FirstReplyLine;
        else if (completion == "no_reply")
            txn.completion = Completion::NoReply;
        else if (completion != "idle_gap")
            return "invalid_completion";

        if (!optionalU32(params, "idle_ms", DEFAULT_IDLE_MS, 1, MAX_IDLE_MS, txn.idleMs))
            return "invalid_request";
        if (!optionalU32(params, "timeout_ms", DEFAULT_TIMEOUT_MS, 1, MAX_TIMEOUT_MS, txn.timeoutMs))
            return "invalid_timeout_ms";

        String priority = params.get("priority", "interactive");
        if (priority != "interactive" && priority != "background")
            return "invalid_request";
        txn.interactive = priority == "interactive";
        return nullptr;
    }


    // Lock held.
    void enqueueTransaction(BridgeClient &client, Transaction *txn)
    {
        txn->clientId = client.id;
        txn->seq = state.nextSeq++;
        txn->deadlineUs = esp_timer_get_time() + static_cast<int64_t>(txn->timeoutMs) * 1000;
        client.transactions.push_back(txn);
        trimRecent(client);
        state.queue.push_back(txn);
    }


    esp_err_t postTransaction(httpd_req_t *req, uint32_t clientId)
    {
        auto txn = std::make_unique<Transaction>();
        if (const char *err = parseTransaction(WebUtil::queryParams(req), *txn))
            return error(req, 400, err);
        if (!readBody(req, TRANSACTION_BYTES, txn->command))
            return ESP_OK;

        Reply r = replyTo(req);
        {
            Guard guard;
            BridgeClient *client = findClient(clientId);
            Transaction *previous = client ? findTransaction(*client, txn->requestId) : nullptr;

            if (!client)
                setError(r, 404, "no_client");
            else if (previous && previous->state == TxnState::Done)
                setTransactionResult(r, *previous); // never sent twice
            else if (previous)
                setError(r, 409, "duplicate_request", BUSY_RETRY_MS);
            else if (state.owner == Owner::Lease)
                setError(r, 409, "serial_reserved", BUSY_RETRY_MS);
            else if (queuedFor(*client) >= QUEUE_PER_CLIENT || queuedJobs() >= QUEUED_JOBS)
                setError(r, 429, "queue_full", BUSY_RETRY_MS);
            else if (park(r, txn->waiter))
                enqueueTransaction(*client, txn.release());
        }
        return answer(r);
    }


    esp_err_t cancelTransaction(httpd_req_t *req, uint32_t clientId, const String &requestText)
    {
        uint32_t requestId = 0;
        if (!parseU32(requestText, requestId))
            return error(req, 400, "invalid_request");

        Replies out;
        const char *err = nullptr;
        {
            Guard guard;
            BridgeClient *client = findClient(clientId);
            Transaction *txn = client ? findTransaction(*client, requestId) : nullptr;
            if (!client)
                err = "no_client";
            else if (!txn || txn->state == TxnState::Done)
                err = "transaction_not_found";
            else
                RetroBridge::cancelTransaction(*txn, out);
        }
        if (err)
            return error(req, 404, err);

        send(out);
        return noContent(req);
    }


    // The query's lease settings; returns the error code, if any.
    const char *parseLease(const WebUtil::Params &params, Lease &lease)
    {
        if (!parseU32(params.get("lease_id"), lease.leaseId))
            return "invalid_request";

        String mode = params.get("mode", "exclusive");
        if (mode == "no_ack")
            lease.mode = LeaseMode::NoAck;
        else if (mode != "exclusive")
            return "invalid_lease_mode";

        if (!optionalU32(params, "lifetime_ms", DEFAULT_LEASE_LIFETIME_MS, 1, UINT32_MAX, lease.lifetimeMs) ||
            !optionalU32(params, "idle_ms", DEFAULT_LEASE_IDLE_MS, 1, LEASE_LIFETIME_MAX_MS, lease.idleMs) ||
            !optionalU32(params, "completion_idle_ms", DEFAULT_COMPLETION_IDLE_MS, 1, LEASE_LIFETIME_MAX_MS, lease.completionIdleMs))
            return "invalid_request";

        lease.lifetimeMs = std::min(lease.lifetimeMs, LEASE_LIFETIME_MAX_MS);
        return nullptr;
    }


    // Lock held. rb_sched activates it, or answers "queued".
    void enqueueLease(uint32_t clientId, Lease *lease)
    {
        lease->clientId = clientId;
        lease->seq = state.nextSeq++;
        lease->serial = state.nextLeaseSerial++;
        lease->createdMs = millis();
        lease->lastActivityMs = lease->createdMs;
        state.leases.push_back(lease);
    }


    esp_err_t createLease(httpd_req_t *req, uint32_t clientId)
    {
        auto lease = std::make_unique<Lease>();
        if (const char *err = parseLease(WebUtil::queryParams(req), *lease))
            return error(req, 400, err);

        Reply r = replyTo(req);
        {
            Guard guard;
            BridgeClient *client = findClient(clientId);
            Lease *existing = client ? findLease(clientId, lease->leaseId) : nullptr;

            if (!client)
            {
                setError(r, 404, "no_client");
            }
            else if (existing)
            {
                // A retried create.
                existing->lastActivityMs = millis();
                setJson(r, 200, leaseJson(*existing, false));
            }
            else if (state.leases.size() >= MAX_LEASES || queuedJobs() >= QUEUED_JOBS)
            {
                setError(r, 429, "queue_full", BUSY_RETRY_MS);
            }
            else if (park(r, lease->createWaiter))
            {
                enqueueLease(clientId, lease.release());
            }
        }
        return answer(r);
    }


    // Lock held. On nullptr, r holds the error.
    Lease *lookupLease(Reply &r, uint32_t clientId, uint32_t leaseId, bool mustBeActive = false)
    {
        if (!findClient(clientId))
        {
            setError(r, 404, "no_client");
            return nullptr;
        }

        Lease *lease = findLease(clientId, leaseId);
        if (!lease)
        {
            setError(r, 404, "lease_not_found");
            return nullptr;
        }
        if (mustBeActive && lease->state != LeaseState::Active)
        {
            setError(r, 409, "lease_not_active");
            return nullptr;
        }
        return lease;
    }


    // Lock held. Queues the bytes, parks until they fit, or refuses. True
    // when bytes were queued for rb_tx.
    bool writeOrPark(Reply &r, Lease &lease, std::vector<uint8_t> &body, uint32_t waitMs)
    {
        lease.lastActivityMs = millis();

        if (lease.txFailed)
        {
            setError(r, 503, "write_failed", 1000);
            return false;
        }

        if (lease.writes.empty() && leaseTxFree(lease) >= body.size())
        {
            leaseTxPush(lease, body.data(), body.size());
            leaseNoteWrite(lease, body.data(), body.size());
            setJson(r, 200, "{\"written\":" + String(body.size()) + "}");
            return true;
        }

        if (waitMs == 0)
        {
            setError(r, 429, "lease_not_writable");
            return false;
        }

        PendingWrite write;
        if (park(r, write.waiter))
        {
            write.data = std::move(body);
            write.deadlineUs = esp_timer_get_time() + static_cast<int64_t>(waitMs) * 1000;
            lease.writes.push_back(std::move(write));
        }
        return false;
    }


    // Lock held. Answers from the buffer, or parks until data, completion or
    // wait_ms.
    void readOrPark(Reply &r, Lease &lease, uint64_t cursor, uint32_t waitMs, int64_t nowUs)
    {
        if (cursor > lease.rxEnd || cursor < lease.rxStart)
        {
            setError(r, 400, "invalid_cursor");
            return;
        }

        if (cursor < lease.rxEnd || lease.overflow || waitMs == 0 || leaseComplete(lease, cursor, nowUs))
        {
            setReadResult(r, lease, cursor, nowUs);
            return;
        }

        PendingRead read;
        if (park(r, read.waiter))
        {
            lease.lastActivityMs = millis();
            read.cursor = cursor;
            read.deadlineUs = nowUs + static_cast<int64_t>(waitMs) * 1000;
            lease.reads.push_back(std::move(read));
        }
    }


    esp_err_t getLease(httpd_req_t *req, uint32_t clientId, uint32_t leaseId)
    {
        Reply r = replyTo(req);
        {
            Guard guard;
            if (Lease *lease = lookupLease(r, clientId, leaseId))
            {
                lease->lastActivityMs = millis();
                setJson(r, 200, leaseJson(*lease, true));
            }
        }
        send(r);
        return ESP_OK;
    }


    esp_err_t writeLease(httpd_req_t *req, uint32_t clientId, uint32_t leaseId)
    {
        WebUtil::Params params = WebUtil::queryParams(req);
        uint32_t waitMs = 0;
        if (!optionalU32(params, "wait_ms", 0, 0, UINT32_MAX, waitMs))
            return error(req, 400, "invalid_request");
        waitMs = std::min(waitMs, LEASE_WRITE_WAIT_MS);

        std::vector<uint8_t> body;
        if (!readBody(req, LEASE_WRITE_BYTES, body))
            return ESP_OK;

        bool queuedTx = false;
        Reply r = replyTo(req);
        {
            Guard guard;
            if (Lease *lease = lookupLease(r, clientId, leaseId, true))
                queuedTx = writeOrPark(r, *lease, body, waitMs);
        }

        if (queuedTx)
            notifyTx();
        return answer(r);
    }


    esp_err_t readLease(httpd_req_t *req, uint32_t clientId, uint32_t leaseId)
    {
        WebUtil::Params params = WebUtil::queryParams(req);
        uint64_t cursor = 0;
        if (!parseU64(params.get("cursor"), cursor))
            return error(req, 400, "invalid_cursor");

        uint32_t waitMs = 0;
        if (!optionalU32(params, "wait_ms", 0, 0, UINT32_MAX, waitMs))
            return error(req, 400, "invalid_request");
        waitMs = std::min(waitMs, READ_WAIT_MAX_MS);

        int64_t now = esp_timer_get_time();
        Reply r = replyTo(req);
        {
            Guard guard;
            if (Lease *lease = lookupLease(r, clientId, leaseId, true))
                readOrPark(r, *lease, cursor, waitMs, now);
        }
        return answer(r);
    }


    // cancel also ends an active lease.
    esp_err_t endLease(httpd_req_t *req, uint32_t clientId, uint32_t leaseId, bool cancel)
    {
        Replies out;
        Reply r = replyTo(req);
        {
            Guard guard;
            if (Lease *lease = lookupLease(r, clientId, leaseId, !cancel))
            {
                RetroBridge::endLease(lease, out);
                setNoContent(r);
            }
        }

        send(out);
        send(r);
        notifyScheduler();
        return ESP_OK;
    }


    esp_err_t postWake(httpd_req_t *req, uint32_t clientId)
    {
        WebUtil::Params params = WebUtil::queryParams(req);
        uint32_t timeoutMs = 0;
        if (!optionalU32(params, "timeout_ms", DEFAULT_WAKE_TIMEOUT_MS, 1, MAX_WAKE_TIMEOUT_MS, timeoutMs))
            return error(req, 400, "invalid_timeout_ms");

        Reply r = replyTo(req);
        {
            Guard guard;
            Wake &wake = state.wake;
            if (!findClient(clientId))
            {
                setError(r, 404, "no_client");
            }
            else if (wake.pending || wake.running || state.owner != Owner::None)
            {
                setWakeBusy(r);
            }
            else if (park(r, wake.waiter))
            {
                wake.pending = true;
                wake.clientId = clientId;
                wake.timeoutMs = timeoutMs;
            }
        }
        return answer(r);
    }


    esp_err_t notFound(httpd_req_t *req)
    {
        return error(req, 404, "not_found");
    }


    esp_err_t handleOptions(httpd_req_t *req)
    {
        Reply r = replyTo(req);
        setNoContent(r);
        r.headers.emplace_back("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        r.headers.emplace_back("Access-Control-Allow-Headers", "Content-Type, X-Request-ID");
        r.headers.emplace_back("Access-Control-Max-Age", "600");
        send(r);
        return ESP_OK;
    }


    esp_err_t handleGet(httpd_req_t *req)
    {
        Route route;
        if (!parseRoute(req, route))
            return notFound(req);

        if (route.is(1, "info"))
            return reply(req, 200, infoJson());
        if (route.is(1, "status"))
            return reply(req, 200, statusJson());

        uint32_t clientId = 0;
        uint32_t leaseId = 0;
        if (route.count >= 4 && route.parts[0] == "clients" && route.parts[2] == "leases")
        {
            if (!parseU32(route.parts[1], clientId))
                return error(req, 404, "no_client");
            if (!parseU32(route.parts[3], leaseId))
                return error(req, 404, "lease_not_found");

            if (route.count == 4)
                return getLease(req, clientId, leaseId);
            if (route.count == 5 && route.parts[4] == "read")
                return readLease(req, clientId, leaseId);
        }
        return notFound(req);
    }


    esp_err_t handlePost(httpd_req_t *req)
    {
        Route route;
        if (!parseRoute(req, route))
            return notFound(req);

        bool takesBody = route.count == 3 && route.parts[2] == "transactions";
        takesBody |= route.count == 5 && route.parts[2] == "leases" && route.parts[4] == "write";
        if (!takesBody && !discardBody(req))
            return ESP_OK;

        if (route.is(1, "clients"))
            return createClient(req);

        uint32_t clientId = 0;
        if (route.count < 3 || route.parts[0] != "clients")
            return notFound(req);
        if (!parseU32(route.parts[1], clientId))
            return error(req, 404, "no_client");

        const String &kind = route.parts[2];
        if (kind == "transactions")
        {
            if (route.count == 3)
                return postTransaction(req, clientId);
            if (route.count == 5 && route.parts[4] == "cancel")
                return cancelTransaction(req, clientId, route.parts[3]);
        }
        else if (kind == "leases")
        {
            if (route.count == 3)
                return createLease(req, clientId);

            uint32_t leaseId = 0;
            if (route.count == 5 && !parseU32(route.parts[3], leaseId))
                return error(req, 404, "lease_not_found");
            if (route.count == 5 && route.parts[4] == "write")
                return writeLease(req, clientId, leaseId);
            if (route.count == 5 && route.parts[4] == "release")
                return endLease(req, clientId, leaseId, false);
            if (route.count == 5 && route.parts[4] == "cancel")
                return endLease(req, clientId, leaseId, true);
        }
        else if (kind == "wake" && route.count == 3)
        {
            return postWake(req, clientId);
        }
        return notFound(req);
    }


    esp_err_t handleDelete(httpd_req_t *req)
    {
        Route route;
        uint32_t clientId = 0;
        if (!parseRoute(req, route) || !route.is(2, "clients"))
            return notFound(req);
        if (!discardBody(req))
            return ESP_OK;
        if (!parseU32(route.parts[1], clientId))
            return error(req, 404, "no_client");
        return deleteClient(req, clientId);
    }
}


namespace RetroBridge
{

void registerRoutes()
{
    WebServer::on("/api/v1/*", HTTP_GET, handleGet);
    WebServer::on("/api/v1/*", HTTP_POST, handlePost);
    WebServer::on("/api/v1/*", HTTP_DELETE, handleDelete);
    // Without it the server answers preflights 405.
    WebServer::on("/api/v1/*", HTTP_OPTIONS, handleOptions);
}

} // namespace RetroBridge
