#include "retro_bridge.h"
#include "rb_internal.h"

#include <esp_timer.h>

#include <algorithm>
#include <cstring>

#include "../rt4k/link.h"

namespace
{
    // Idle clients give way to new ones after this.
    constexpr uint32_t CLIENT_EVICT_IDLE_MS = 30000;

    using namespace RetroBridge;

    bool clientBusy(const BridgeClient &client)
    {
        for (const Transaction *txn : client.transactions)
        {
            if (txn->state != TxnState::Done)
                return true;
        }
        for (const Lease *lease : state.leases)
        {
            if (lease->clientId == client.id)
                return true;
        }
        return state.wake.clientId == client.id && (state.wake.pending || state.wake.running);
    }


    void appendTransactionRx(Transaction &txn, const uint8_t *data, size_t length, int64_t nowUs)
    {
        txn.lastRxUs = nowUs;

        size_t room = RESPONSE_BYTES - std::min(txn.reply.size(), RESPONSE_BYTES);
        size_t kept = std::min(room, length);
        txn.reply.insert(txn.reply.end(), data, data + kept);
        if (kept < length)
            txn.overflow = true;

        if (txn.lineDone)
            return;

        // The first non-empty line, ignoring "[MCU] " chatter.
        for (size_t i = 0; i < length && !txn.lineDone; i++)
        {
            uint8_t b = data[i];
            bool chatter = txn.lineLength >= 6 && memcmp(txn.linePrefix, "[MCU] ", 6) == 0;
            if (b == '\r' || b == '\n')
            {
                if (txn.lineLength > 0 && !chatter)
                {
                    txn.replyStarted = true;
                    txn.lineDone = true;
                }
                txn.lineLength = 0;
                continue;
            }

            if (txn.lineLength < sizeof(txn.linePrefix))
                txn.linePrefix[txn.lineLength] = static_cast<char>(b);
            if (txn.lineLength < UINT16_MAX)
                txn.lineLength++;

            if (txn.lineLength >= 6 && memcmp(txn.linePrefix, "[MCU] ", 6) != 0)
                txn.replyStarted = true;
        }
    }


    void appendWakeRx(Wake &wake, const uint8_t *data, size_t length)
    {
        constexpr size_t MAX_LINE = 160;

        for (size_t i = 0; i < length; i++)
        {
            char c = static_cast<char>(data[i]);
            if (c != '\r' && c != '\n')
            {
                if (wake.line.length() < MAX_LINE)
                    wake.line += c;
                continue;
            }

            String line = wake.line;
            wake.line = "";
            if (line.startsWith("[COM] "))
                line = line.substring(6);

            if (line.indexOf("Power On Requested") >= 0 || line.startsWith("Bad Command: pwr"))
                wake.acknowledged = true;
            else if (line.indexOf("FW Version:") >= 0 && wake.response.length() == 0)
                wake.response = line;
        }
    }


    void markDead(Parked &parked, int fd)
    {
        if (parked && parked.fd == fd)
            parked.dead = true;
    }
}


namespace RetroBridge
{

State state;
SemaphoreHandle_t lock = nullptr;
std::atomic<bool> linkOwned{false};


void begin()
{
    lock = xSemaphoreCreateMutex();
    if (!lock)
    {
        Serial.println("ERROR: Retro-Bridge lock unavailable");
        return;
    }

    startReplies();
    startTx();
    startScheduler();
}


bool ownsLink()
{
    return linkOwned;
}


void onSerialRx(const uint8_t *data, size_t length)
{
    int64_t now = esp_timer_get_time();
    bool wake = false;
    {
        Guard guard;
        state.lastRxUs = now;
        switch (state.owner)
        {
            case Owner::Transaction:
                if (state.txn->phase != TxnPhase::Draining)
                    appendTransactionRx(*state.txn, data, length, now);
                wake = true;
                break;

            case Owner::Lease:
                leaseRxPush(*state.lease, data, length, now);
                // Only parked reads depend on lease RX.
                wake = !state.lease->reads.empty();
                break;

            case Owner::Wake:
                appendWakeRx(state.wake, data, length);
                wake = true;
                break;

            case Owner::None:
                break;
        }
    }
    if (wake)
        notifyScheduler();
}


void onSocketClosed(int fd)
{
    if (!lock)
        return;

    {
        Guard guard;
        for (Transaction *txn : state.queue)
            markDead(txn->waiter, fd);
        if (state.txn)
            markDead(state.txn->waiter, fd);
        for (Lease *lease : state.leases)
        {
            markDead(lease->createWaiter, fd);
            for (PendingRead &read : lease->reads)
                markDead(read.waiter, fd);
            for (PendingWrite &write : lease->writes)
                markDead(write.waiter, fd);
        }
        markDead(state.wake.waiter, fd);
    }
    notifyScheduler();
}


String ownerLabel()
{
    return Rt4kLink::currentOwner();
}


size_t clientCount()
{
    if (!lock)
        return 0;

    Guard guard;
    size_t count = 0;
    for (const BridgeClient &client : state.clients)
    {
        if (client.id != 0)
            count++;
    }
    return count;
}


BridgeClient *findClient(uint32_t id)
{
    if (id == 0)
        return nullptr;

    for (BridgeClient &client : state.clients)
    {
        if (client.id == id)
        {
            client.lastUsedMs = millis();
            return &client;
        }
    }
    return nullptr;
}


BridgeClient *createClient()
{
    BridgeClient *slot = nullptr;
    for (BridgeClient &client : state.clients)
    {
        if (client.id == 0)
        {
            slot = &client;
            break;
        }
    }

    if (!slot)
    {
        uint32_t now = millis();
        for (BridgeClient &client : state.clients)
        {
            if (now - client.lastUsedMs < CLIENT_EVICT_IDLE_MS || clientBusy(client))
                continue;
            if (!slot || client.lastUsedMs < slot->lastUsedMs)
                slot = &client;
        }
        if (!slot)
            return nullptr;

        Replies none;
        removeClient(*slot, none);
    }

    slot->id = state.nextClientId++;
    if (state.nextClientId == 0)
        state.nextClientId = 1;
    slot->lastUsedMs = millis();
    return slot;
}


void removeClient(BridgeClient &client, Replies &out)
{
    uint32_t id = client.id;

    for (Transaction *txn : client.transactions)
    {
        if (txn->state != TxnState::Done)
            cancelTransaction(*txn, out);
    }

    std::vector<Lease *> owned;
    for (Lease *lease : state.leases)
    {
        if (lease->clientId == id)
            owned.push_back(lease);
    }
    for (Lease *lease : owned)
        endLease(lease, out);

    // A running transaction still references its record until it finishes.
    for (Transaction *txn : client.transactions)
    {
        if (txn != state.txn)
            delete txn;
        else
            txn->clientId = 0;
    }
    client.transactions.clear();
    client.id = 0;
}


Lease *findLease(uint32_t clientId, uint32_t leaseId)
{
    for (Lease *lease : state.leases)
    {
        if (lease->clientId == clientId && lease->leaseId == leaseId)
            return lease;
    }
    return nullptr;
}


Transaction *findTransaction(BridgeClient &client, uint32_t requestId)
{
    for (Transaction *txn : client.transactions)
    {
        if (txn->requestId == requestId)
            return txn;
    }
    return nullptr;
}


void finishTransaction(Transaction &txn, int status, const char *result, Replies &out)
{
    txn.state = TxnState::Done;
    txn.status = status;
    txn.result = result;

    if (txn.waiter)
    {
        Reply reply = replyTo(txn.waiter);
        setTransactionResult(reply, txn);
        out.push_back(std::move(reply));
    }

    if (state.txn == &txn)
    {
        state.txn = nullptr;
        state.owner = Owner::None;
        updateOwner();

        // Its client was removed while it ran.
        if (txn.clientId == 0)
            delete &txn;
    }
}


void cancelTransaction(Transaction &txn, Replies &out)
{
    if (txn.state == TxnState::Queued)
    {
        state.queue.erase(std::remove(state.queue.begin(), state.queue.end(), &txn), state.queue.end());
        finishTransaction(txn, 200, "canceled", out);
    }
    else if (txn.state == TxnState::Running)
    {
        txn.cancelRequested = true;
        notifyScheduler();
    }
}


void updateOwner()
{
    char label[48] = "";
    switch (state.owner)
    {
        case Owner::Transaction:
            snprintf(label, sizeof(label), "transaction:%lu/%lu",
                static_cast<unsigned long>(state.txn->clientId), static_cast<unsigned long>(state.txn->requestId));
            break;
        case Owner::Lease:
            snprintf(label, sizeof(label), "lease:%lu/%lu",
                static_cast<unsigned long>(state.lease->clientId), static_cast<unsigned long>(state.lease->leaseId));
            break;
        case Owner::Wake:
            snprintf(label, sizeof(label), "wake:%lu", static_cast<unsigned long>(state.wake.clientId));
            break;
        case Owner::None:
            break;
    }

    if (state.linkHeld)
        Rt4kLink::setExternalOwner(label, state.owner == Owner::Lease);
}


size_t queuedJobs()
{
    size_t count = state.queue.size();
    for (const Lease *lease : state.leases)
    {
        if (lease->state == LeaseState::Queued)
            count++;
    }
    return count;
}

} // namespace RetroBridge
