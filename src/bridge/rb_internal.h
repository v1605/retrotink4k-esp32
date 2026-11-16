#pragma once

#include <Arduino.h>
#include <esp_http_server.h>

#include <atomic>
#include <deque>
#include <utility>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// State is guarded by `lock`; never hold it across I/O.

namespace RetroBridge
{
    // Reported in /api/v1/info "limits".
    constexpr uint8_t MAX_CLIENTS = 4;
    constexpr uint8_t QUEUE_PER_CLIENT = 4;
    constexpr uint8_t QUEUED_JOBS = 16;
    constexpr size_t TRANSACTION_BYTES = 1024;
    constexpr size_t RESPONSE_BYTES = 16384;
    constexpr size_t LEASE_RECEIVE_BYTES = 65536;
    constexpr size_t LEASE_READ_BYTES = 16384;
    constexpr size_t LEASE_WRITE_BYTES = 4096;
    constexpr size_t LEASE_TRANSMIT_BYTES = 16384;
    constexpr uint32_t LEASE_WRITE_WAIT_MS = 1000;
    constexpr uint32_t LEASE_LIFETIME_MAX_MS = 300000;
    constexpr uint32_t CLIENT_EXPIRY_MS = 300000;

    // Fits one SerialBridge TX transfer (2560 B).
    constexpr size_t TX_PIECE_BYTES = 2048;
    constexpr uint8_t MAX_LEASES = 8;
    // Finished transactions kept per client for request_id replays.
    constexpr uint8_t RECENT_TRANSACTIONS = 4;
    constexpr uint32_t READ_WAIT_MAX_MS = 10000;
    constexpr uint32_t BUSY_RETRY_MS = 250;

    struct Parked
    {
        httpd_req_t *req = nullptr;
        int fd = -1;
        bool dead = false; // socket closed: complete without sending

        explicit operator bool() const
        {
            return req != nullptr;
        }
    };

    // A reply built under the lock and sent after it's released.
    struct Reply
    {
        httpd_req_t *req = nullptr;
        bool async = false;
        bool skip = false;
        int code = 200;
        const char *type = "application/json";
        std::vector<uint8_t> body;
        std::vector<std::pair<const char *, String>> headers;
    };

    using Replies = std::vector<Reply>;

    enum class Completion : uint8_t
    {
        IdleGap,
        FirstReplyLine,
        NoReply,
    };

    enum class TxnState : uint8_t
    {
        Queued,
        Running,
        Done,
    };

    enum class TxnPhase : uint8_t
    {
        Draining, // stale RX before the write
        Sending,
        Collecting,
    };

    struct Transaction
    {
        uint32_t clientId = 0;
        uint32_t requestId = 0;
        uint32_t seq = 0;
        bool interactive = true;
        Completion completion = Completion::IdleGap;
        uint32_t idleMs = 0;
        uint32_t timeoutMs = 0;
        TxnState state = TxnState::Queued;
        std::vector<uint8_t> command;
        Parked waiter;

        TxnPhase phase = TxnPhase::Draining;
        bool cancelRequested = false;
        int64_t deadlineUs = 0; // timeout_ms from arrival, queued or running
        int64_t drainUntilUs = 0;
        int64_t txDoneUs = 0;
        int64_t lastRxUs = 0;
        uint32_t txToken = 0;
        bool txFailed = false;
        bool overflow = false;
        bool lineDone = false;
        bool replyStarted = false; // a reply byte outside "[MCU] " chatter
        uint16_t lineLength = 0;
        char linePrefix[7] = {};
        std::vector<uint8_t> reply;

        int status = 0;
        const char *result = nullptr;
    };

    struct BridgeClient
    {
        uint32_t id = 0; // 0: free slot
        uint32_t lastUsedMs = 0;
        std::deque<Transaction *> transactions; // oldest first
    };

    enum class LeaseState : uint8_t
    {
        Queued,
        Active,
    };

    enum class LeaseMode : uint8_t
    {
        Exclusive,
        NoAck,
    };

    struct PendingRead
    {
        Parked waiter;
        uint64_t cursor = 0;
        int64_t deadlineUs = 0;
    };

    struct PendingWrite
    {
        Parked waiter;
        std::vector<uint8_t> data;
        int64_t deadlineUs = 0;
    };

    struct Lease
    {
        uint32_t clientId = 0;
        uint32_t leaseId = 0;
        uint32_t seq = 0;
        uint32_t serial = 0; // unique per lease, for rb_tx
        LeaseMode mode = LeaseMode::Exclusive;
        LeaseState state = LeaseState::Queued;
        uint32_t lifetimeMs = 0;
        uint32_t idleMs = 0;
        uint32_t completionIdleMs = 0;
        uint32_t createdMs = 0;
        uint32_t activatedMs = 0;
        uint32_t lastActivityMs = 0;
        Parked createWaiter;

        // RX: absolute offsets; [rxStart, rxEnd) is still buffered.
        uint8_t *rx = nullptr;
        uint64_t rxStart = 0;
        uint64_t rxEnd = 0;
        bool overflow = false;
        uint32_t dropsAtStart = 0;
        int64_t lastRxUs = 0;
        bool rxOpen = false; // RX before the client's first write is stale
        bool rxSeen = false; // since the client's last write

        // no_ack end-of-transfer scanner.
        uint8_t scanState = 0;
        uint8_t frameHeader[6] = {};
        uint8_t frameHeaderLength = 0;
        uint32_t frameSkip = 0;
        char line[40] = {};
        uint8_t lineLength = 0;
        bool firstLineSeen = false;
        bool terminalSeen = false;
        bool clientFrames = false; // client sends RTL1 ACKs

        uint8_t *tx = nullptr;
        size_t txHead = 0;
        size_t txCount = 0;
        bool txFailed = false;

        std::deque<PendingRead> reads;
        std::deque<PendingWrite> writes;
    };

    struct Wake
    {
        bool pending = false; // parked, not started
        bool running = false;
        uint32_t generation = 0;
        uint32_t clientId = 0;
        Parked waiter;
        uint32_t startedMs = 0;
        uint32_t timeoutMs = 0;
        int64_t deadlineUs = 0;
        int64_t nextProbeUs = 0;
        bool connectedBefore = false;
        bool everConnected = false;
        bool pwrOnSent = false;
        bool acknowledged = false;
        uint32_t wakeCommands = 0;
        uint32_t probes = 0;
        uint32_t txToken = 0;
        String line;
        String response;
    };

    enum class Owner : uint8_t
    {
        None,
        Transaction,
        Lease,
        Wake,
    };

    struct State
    {
        BridgeClient clients[MAX_CLIENTS];
        std::vector<Transaction *> queue; // queued transactions
        std::vector<Lease *> leases;      // queued and active
        Owner owner = Owner::None;
        Transaction *txn = nullptr;
        Lease *lease = nullptr;
        Wake wake;
        bool linkHeld = false;
        int64_t lastRxUs = 0;   // any RX while the bridge holds the link
        uint32_t txPending = 0; // rb_tx work queued or in flight

        uint32_t nextClientId = 1;
        uint32_t nextSeq = 1;
        uint32_t nextLeaseSerial = 1;
        uint32_t txErrors = 0;
    };

    extern State state;
    extern SemaphoreHandle_t lock;
    extern std::atomic<bool> linkOwned;

    class Guard
    {
    public:
        Guard()
        {
            xSemaphoreTake(lock, portMAX_DELAY);
        }

        ~Guard()
        {
            xSemaphoreGive(lock);
        }

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
    };

    // retro_bridge.cpp (lock held)
    BridgeClient *findClient(uint32_t id);
    BridgeClient *createClient();
    void removeClient(BridgeClient &client, Replies &out);
    Lease *findLease(uint32_t clientId, uint32_t leaseId);
    Transaction *findTransaction(BridgeClient &client, uint32_t requestId);
    void finishTransaction(Transaction &txn, int status, const char *result, Replies &out);
    void cancelTransaction(Transaction &txn, Replies &out);
    void updateOwner();
    size_t queuedJobs();

    // rb_reply.cpp (no lock needed)
    void startReplies();
    Reply replyTo(httpd_req_t *req);
    Reply replyTo(Parked &parked); // clears parked
    void setJson(Reply &reply, int code, const String &json);
    void setError(Reply &reply, int code, const char *error, uint32_t retryAfterMs = 0);
    void setNoContent(Reply &reply);
    // 409 for a wake refused while the link is busy.
    void setWakeBusy(Reply &reply);
    void setTransactionResult(Reply &reply, const Transaction &txn);
    // Parked replies go out on rb_reply, so no caller blocks on a socket.
    void send(Reply &reply);
    void send(Replies &replies);
    // Parks reply's request for rb_sched and clears reply; on failure reply
    // holds a 503.
    bool park(Reply &reply, Parked &out);

    // rb_lease.cpp (lock held)
    bool activateLease(Lease &lease);
    void endLease(Lease *lease, Replies &out);
    size_t leaseTxFree(const Lease &lease);
    void leaseTxPush(Lease &lease, const uint8_t *data, size_t length);
    size_t leaseTxTake(Lease &lease, uint8_t *out, size_t max);
    void leaseRxPush(Lease &lease, const uint8_t *data, size_t length, int64_t nowUs);
    void leaseNoteWrite(Lease &lease, const uint8_t *data, size_t length);
    bool leaseComplete(const Lease &lease, uint64_t cursor, int64_t nowUs);
    int64_t leaseReadLullUs(const Lease &lease);
    void setReadResult(Reply &reply, Lease &lease, uint64_t cursor, int64_t nowUs);

    // rb_sched.cpp
    void startScheduler();
    void notifyScheduler();
    void onCommandSent(uint32_t token, bool ok, int64_t nowUs); // lock held

    // rb_tx.cpp
    void startTx();
    void notifyTx();
    // Lock held. Written and flushed by rb_tx, then onCommandSent(). 0 if full.
    uint32_t queueCommand(const uint8_t *data, size_t length);

    // rb_json.cpp
    String infoJson();
    String statusJson();
    String leaseJson(const Lease &lease, bool full);
    String wakeJson(const Wake &wake, const char *wakeState, uint32_t completedMs);
}
