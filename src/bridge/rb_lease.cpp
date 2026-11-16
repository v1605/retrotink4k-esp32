#include "rb_internal.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "../ftdi/serial_bridge.h"

namespace
{
    using namespace RetroBridge;

    constexpr int64_t TERMINAL_GRACE_US = 5000;
    // Streamed replies arrive in pieces up to ~35 ms apart.
    constexpr int64_t STREAM_LULL_US = 40000;
    constexpr int64_t FRAME_LULL_US = 2000;

    constexpr uint8_t FRAME_MAGIC_1 = 0xA5;
    constexpr uint8_t FRAME_MAGIC_2 = 0x5A;
    constexpr uint16_t MAX_FRAME_PAYLOAD = 2048;

    enum ScanState : uint8_t
    {
        SCAN_TEXT,
        SCAN_MAGIC,
        SCAN_HEADER,
        SCAN_FRAME,
    };


    bool startsWithWord(const char *text, const char *word)
    {
        size_t n = strlen(word);
        return strncmp(text, word, n) == 0 && (text[n] == ' ' || text[n] == '\0');
    }


    // "<get|put|font|osd|osd2> <done|aborted|failed|error|err>".
    bool isTransferEnd(const char *line)
    {
        const char *rest = nullptr;
        for (const char *cmd : {"get", "put", "font", "osd", "osd2"})
        {
            size_t n = strlen(cmd);
            if (strncmp(line, cmd, n) == 0 && line[n] == ' ')
                rest = line + n + 1;
        }
        if (!rest)
            return false;

        for (const char *end : {"done", "aborted", "failed", "error", "err"})
        {
            if (startsWithWord(rest, end) || (strncmp(rest, end, strlen(end)) == 0 && rest[strlen(end)] == ':'))
                return true;
        }
        return false;
    }


    bool isRefusal(const char *line)
    {
        String lower(line);
        lower.toLowerCase();
        for (const char *word : {"busy", "bad command", "unknown command", "nothing shown", "need a path", "bad flags"})
        {
            if (lower.indexOf(word) >= 0)
                return true;
        }
        return false;
    }


    void endLine(Lease &lease)
    {
        if (lease.lineLength == 0)
            return;

        lease.line[lease.lineLength] = '\0';
        lease.lineLength = 0;

        const char *text = lease.line;
        if (strncmp(text, "[MCU] ", 6) == 0)
            return;
        if (strncmp(text, "[COM] ", 6) == 0)
            text += 6;

        bool first = !lease.firstLineSeen;
        lease.firstLineSeen = true;
        if (isTransferEnd(text) || (first && isRefusal(text)))
            lease.terminalSeen = true;
    }


    void scanText(Lease &lease, uint8_t b)
    {
        if (b == FRAME_MAGIC_1)
        {
            lease.scanState = SCAN_MAGIC;
            return;
        }
        if (b == '\r' || b == '\n')
        {
            endLine(lease);
            return;
        }
        if (lease.lineLength < sizeof(lease.line) - 1)
            lease.line[lease.lineLength++] = static_cast<char>(b);
    }


    void scanByte(Lease &lease, uint8_t b)
    {
        switch (lease.scanState)
        {
            case SCAN_TEXT:
                scanText(lease, b);
                return;

            case SCAN_MAGIC:
                if (b == FRAME_MAGIC_2)
                {
                    lease.scanState = SCAN_HEADER;
                    lease.frameHeaderLength = 0;
                }
                else
                {
                    lease.scanState = SCAN_TEXT;
                    scanText(lease, b);
                }
                return;

            case SCAN_HEADER:
            {
                lease.frameHeader[lease.frameHeaderLength++] = b;
                if (lease.frameHeaderLength < sizeof(lease.frameHeader))
                    return;

                // Nonce, length, type, sequence; then payload and CRC.
                uint16_t length = lease.frameHeader[2] | (lease.frameHeader[3] << 8);
                lease.scanState = length > MAX_FRAME_PAYLOAD ? SCAN_TEXT : SCAN_FRAME;
                lease.frameSkip = length + 2;
                return;
            }

            case SCAN_FRAME:
                if (--lease.frameSkip == 0)
                    lease.scanState = SCAN_TEXT;
                return;
        }
    }

    uint8_t *allocBuffer(size_t bytes)
    {
        auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
        if (!buffer)
            buffer = static_cast<uint8_t *>(malloc(bytes));
        return buffer;
    }


    void freeBuffers(Lease &lease)
    {
        free(lease.rx);
        free(lease.tx);
        lease.rx = nullptr;
        lease.tx = nullptr;
    }
}


namespace RetroBridge
{

bool activateLease(Lease &lease)
{
    lease.rx = allocBuffer(LEASE_RECEIVE_BYTES);
    lease.tx = allocBuffer(LEASE_TRANSMIT_BYTES);
    if (!lease.rx || !lease.tx)
    {
        freeBuffers(lease);
        return false;
    }

    lease.state = LeaseState::Active;
    lease.activatedMs = millis();
    lease.lastActivityMs = lease.activatedMs;
    lease.dropsAtStart = SerialBridge::droppedRxBytes();

    state.owner = Owner::Lease;
    state.lease = &lease;
    updateOwner();
    return true;
}


void endLease(Lease *lease, Replies &out)
{
    if (lease->createWaiter)
    {
        Reply reply = replyTo(lease->createWaiter);
        setError(reply, 409, "lease_not_active");
        out.push_back(std::move(reply));
    }
    for (PendingRead &read : lease->reads)
    {
        Reply reply = replyTo(read.waiter);
        setError(reply, 409, "lease_not_active");
        out.push_back(std::move(reply));
    }
    for (PendingWrite &write : lease->writes)
    {
        Reply reply = replyTo(write.waiter);
        setError(reply, 409, "lease_not_active");
        out.push_back(std::move(reply));
    }

    if (state.lease == lease)
    {
        // Bytes the client was told were written still go out.
        if (lease->txCount > 0)
        {
            std::vector<uint8_t> unsent(lease->txCount);
            leaseTxTake(*lease, unsent.data(), unsent.size());
            if (queueCommand(unsent.data(), unsent.size()) == 0)
                Serial.println("WARNING: Retro-Bridge dropped unsent lease bytes");
        }

        state.lease = nullptr;
        state.owner = Owner::None;
        updateOwner();
        notifyScheduler();
    }

    state.leases.erase(std::remove(state.leases.begin(), state.leases.end(), lease), state.leases.end());
    freeBuffers(*lease);
    delete lease;
}


size_t leaseTxFree(const Lease &lease)
{
    return LEASE_TRANSMIT_BYTES - lease.txCount;
}


void leaseTxPush(Lease &lease, const uint8_t *data, size_t length)
{
    size_t tail = (lease.txHead + lease.txCount) % LEASE_TRANSMIT_BYTES;
    size_t first = std::min(length, LEASE_TRANSMIT_BYTES - tail);
    memcpy(lease.tx + tail, data, first);
    memcpy(lease.tx, data + first, length - first);
    lease.txCount += length;
}


size_t leaseTxTake(Lease &lease, uint8_t *out, size_t max)
{
    size_t length = std::min(max, lease.txCount);
    size_t first = std::min(length, LEASE_TRANSMIT_BYTES - lease.txHead);
    memcpy(out, lease.tx + lease.txHead, first);
    memcpy(out + first, lease.tx, length - first);
    lease.txHead = (lease.txHead + length) % LEASE_TRANSMIT_BYTES;
    lease.txCount -= length;
    return length;
}


void leaseRxPush(Lease &lease, const uint8_t *data, size_t length, int64_t nowUs)
{
    if (!lease.rxOpen)
        return;

    lease.lastRxUs = nowUs;
    lease.rxSeen = true;
    if (lease.overflow)
        return;

    if (lease.rxEnd - lease.rxStart + length > LEASE_RECEIVE_BYTES)
    {
        lease.overflow = true;
        return;
    }

    size_t at = lease.rxEnd % LEASE_RECEIVE_BYTES;
    size_t first = std::min(length, LEASE_RECEIVE_BYTES - at);
    memcpy(lease.rx + at, data, first);
    memcpy(lease.rx, data + first, length - first);
    lease.rxEnd += length;

    if (lease.mode == LeaseMode::NoAck)
    {
        for (size_t i = 0; i < length; i++)
            scanByte(lease, data[i]);
    }
}


// RTL1 frames (ACKs, upload data) don't restart end-of-transfer detection.
void leaseNoteWrite(Lease &lease, const uint8_t *data, size_t length)
{
    lease.rxOpen = true;
    lease.rxSeen = false;
    if (length >= 2 && data[0] == FRAME_MAGIC_1 && data[1] == FRAME_MAGIC_2)
    {
        lease.clientFrames = true;
        return;
    }

    lease.terminalSeen = false;
    lease.firstLineSeen = false;
}


int64_t leaseReadLullUs(const Lease &lease)
{
    if (lease.terminalSeen)
        return TERMINAL_GRACE_US;
    return lease.mode == LeaseMode::NoAck && !lease.clientFrames ? STREAM_LULL_US : FRAME_LULL_US;
}


// Shortly after the end-of-transfer line, else after completion_idle_ms quiet.
bool leaseComplete(const Lease &lease, uint64_t cursor, int64_t nowUs)
{
    if (lease.mode != LeaseMode::NoAck || cursor != lease.rxEnd || lease.scanState != SCAN_TEXT)
        return false;

    int64_t quietUs = nowUs - lease.lastRxUs;
    if (lease.terminalSeen)
        return quietUs >= TERMINAL_GRACE_US && lease.txCount == 0;
    return lease.rxSeen && quietUs >= static_cast<int64_t>(lease.completionIdleMs) * 1000;
}


// Everything before cursor counts as consumed.
void setReadResult(Reply &reply, Lease &lease, uint64_t cursor, int64_t nowUs)
{
    // Upstream RX loss during the lease leaves a gap in its stream.
    if (SerialBridge::droppedRxBytes() != lease.dropsAtStart)
        lease.overflow = true;

    size_t length = static_cast<size_t>(std::min<uint64_t>(lease.rxEnd - cursor, LEASE_READ_BYTES));
    reply.code = 200;
    reply.type = "application/octet-stream";
    reply.body.resize(length);

    size_t at = cursor % LEASE_RECEIVE_BYTES;
    size_t first = std::min(length, LEASE_RECEIVE_BYTES - at);
    memcpy(reply.body.data(), lease.rx + at, first);
    memcpy(reply.body.data() + first, lease.rx, length - first);

    lease.rxStart = std::max(lease.rxStart, cursor);
    lease.lastActivityMs = millis();

    uint64_t next = cursor + length;
    char cursorText[24];
    snprintf(cursorText, sizeof(cursorText), "%" PRIu64, next);

    reply.headers.emplace_back("X-Retro-Bridge-Cursor", cursorText);
    reply.headers.emplace_back("X-Retro-Bridge-Overflow", lease.overflow ? "true" : "false");
    reply.headers.emplace_back("X-Retro-Bridge-Complete", leaseComplete(lease, next, nowUs) ? "true" : "false");
}

} // namespace RetroBridge
