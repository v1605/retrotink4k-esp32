#include "rb_internal.h"

#include <esp_timer.h>

#include <algorithm>

#include "../rt4k/link.h"
#include "../serial_bridge.h"

// rb_sched: link ownership, transactions, wake, lease timers, parked requests.

namespace
{
    using namespace RetroBridge;

    constexpr UBaseType_t SCHED_TASK_PRIORITY = 5;
    constexpr uint32_t SCHED_TASK_STACK_BYTES = 8192;
    constexpr int64_t IDLE_WAIT_US = 100000;
    constexpr int64_t ACQUIRE_RETRY_US = 10000;
    // Before writing, a transaction waits for this much RX quiet (a previous
    // reply's tail), up to DRAIN_MAX_US.
    constexpr int64_t DRAIN_QUIET_US = 3000;
    constexpr int64_t DRAIN_MAX_US = 20000;
    constexpr int64_t DRAIN_POLL_US = 1000;
    constexpr int64_t WAKE_FIRST_PROBE_US = 500000;
    constexpr int64_t WAKE_PROBE_INTERVAL_US = 2000000;

    TaskHandle_t schedTask = nullptr;

    struct Tick
    {
        int64_t now;
        int64_t next;

        void at(int64_t whenUs)
        {
            next = std::min(next, whenUs);
        }
    };


    bool acquireLink()
    {
        if (state.linkHeld)
            return true;
        if (!Rt4kLink::acquireExternal(0, ""))
            return false;

        state.linkHeld = true;
        linkOwned = true;
        return true;
    }


    // Kept until rb_tx is idle, so nothing interleaves with a write.
    void releaseLink()
    {
        if (!state.linkHeld || state.txPending > 0)
            return;

        linkOwned = false;
        state.linkHeld = false;
        Rt4kLink::releaseExternal();
    }


    void sendLine(const char *line)
    {
        queueCommand(reinterpret_cast<const uint8_t *>(line), strlen(line));
    }


    void expireClients(Replies &out)
    {
        uint32_t now = millis();
        for (BridgeClient &client : state.clients)
        {
            if (client.id != 0 && now - client.lastUsedMs >= CLIENT_EXPIRY_MS)
            {
                Serial.printf("Retro-Bridge: client %lu expired\n", static_cast<unsigned long>(client.id));
                removeClient(client, out);
            }
        }
    }


    // Dead, or queued past timeout_ms: never sent.
    void expireQueued(Tick &t, Replies &out)
    {
        std::vector<Transaction *> expired;
        for (Transaction *txn : state.queue)
        {
            if (txn->waiter.dead || t.now >= txn->deadlineUs)
                expired.push_back(txn);
            else
                t.at(txn->deadlineUs);
        }

        for (Transaction *txn : expired)
        {
            if (txn->waiter.dead)
            {
                cancelTransaction(*txn, out);
                continue;
            }
            state.queue.erase(std::remove(state.queue.begin(), state.queue.end(), txn), state.queue.end());
            finishTransaction(*txn, 200, "timeout", out);
        }
    }


    void serviceTransaction(Tick &t, Replies &out)
    {
        Transaction *txn = state.owner == Owner::Transaction ? state.txn : nullptr;
        if (!txn)
            return;

        if (txn->cancelRequested)
        {
            finishTransaction(*txn, 200, "canceled", out);
            return;
        }

        if (txn->phase == TxnPhase::Draining)
        {
            bool quiet = t.now - state.lastRxUs >= DRAIN_QUIET_US && SerialBridge::pendingBytes() == 0;
            if (!quiet && t.now < txn->drainUntilUs)
            {
                t.at(t.now + DRAIN_POLL_US);
                return;
            }

            txn->phase = TxnPhase::Sending;
            txn->txToken = queueCommand(txn->command.data(), txn->command.size());
            if (txn->txToken == 0)
            {
                finishTransaction(*txn, 503, "write_failed", out);
                return;
            }
        }

        if (txn->txFailed)
        {
            finishTransaction(*txn, 503, "write_failed", out);
            return;
        }
        if (txn->overflow)
        {
            finishTransaction(*txn, 413, "overflow", out);
            return;
        }

        if (txn->phase == TxnPhase::Collecting)
        {
            bool done = false;
            switch (txn->completion)
            {
                case Completion::NoReply:
                    done = true;
                    break;

                case Completion::FirstReplyLine:
                    done = txn->lineDone;
                    break;

                case Completion::IdleGap:
                {
                    // From the first reply byte: fwup check is silent for seconds.
                    if (!txn->replyStarted)
                        break;
                    int64_t due = txn->lastRxUs + static_cast<int64_t>(txn->idleMs) * 1000;
                    done = t.now >= due;
                    if (!done)
                        t.at(due);
                    break;
                }
            }

            if (done)
            {
                finishTransaction(*txn, 200, "complete", out);
                return;
            }
        }

        if (t.now >= txn->deadlineUs)
            finishTransaction(*txn, 200, "timeout", out);
        else
            t.at(txn->deadlineUs);
    }


    void finishWake(const char *wakeState, int code, Replies &out)
    {
        Wake &wake = state.wake;
        if (wake.waiter)
        {
            Reply reply = replyTo(wake.waiter);
            setJson(reply, code, wakeJson(wake, wakeState, millis()));
            out.push_back(std::move(reply));
        }

        Serial.printf("Retro-Bridge: wake %s after %lu ms\n", wakeState,
            static_cast<unsigned long>(millis() - wake.startedMs));

        wake.running = false;
        state.owner = Owner::None;
        updateOwner();
    }


    void serviceWake(Tick &t, Replies &out)
    {
        Wake &wake = state.wake;
        if (state.owner != Owner::Wake || !wake.running)
            return;

        if (wake.response.length() > 0)
        {
            finishWake("ready", 200, out);
            return;
        }

        // The FT232R can re-enumerate while the RT4K powers up.
        bool connected = SerialBridge::isConnected();
        if (connected && !wake.connectedBefore && !wake.acknowledged)
            wake.pwrOnSent = false;
        wake.connectedBefore = connected;
        if (connected)
            wake.everConnected = true;

        if (connected && !wake.pwrOnSent && !wake.acknowledged)
        {
            wake.txToken = queueCommand(reinterpret_cast<const uint8_t *>("pwr on\r\n"), 8);
            wake.pwrOnSent = wake.txToken != 0;
            wake.wakeCommands++;
            wake.nextProbeUs = t.now + WAKE_FIRST_PROBE_US;
        }

        // Already awake: probe at once.
        if (wake.acknowledged && wake.probes == 0)
            wake.nextProbeUs = std::min(wake.nextProbeUs, t.now);

        if (connected && (wake.pwrOnSent || wake.acknowledged) && t.now >= wake.nextProbeUs)
        {
            sendLine("ver\r\n");
            wake.probes++;
            wake.nextProbeUs = t.now + WAKE_PROBE_INTERVAL_US;
        }
        if (connected)
            t.at(wake.nextProbeUs);

        if (t.now >= wake.deadlineUs)
        {
            if (wake.everConnected)
                finishWake("timeout", 504, out);
            else
                finishWake("unavailable", 503, out);
            return;
        }
        t.at(wake.deadlineUs);
    }


    // True when answered.
    bool serviceRead(Lease &lease, PendingRead &read, Tick &t, Replies &out)
    {
        bool answer = read.waiter.dead || lease.overflow || t.now >= read.deadlineUs;

        if (!answer && read.cursor < lease.rxEnd)
        {
            int64_t settleAt = lease.lastRxUs + leaseReadLullUs(lease);
            answer = lease.rxEnd - read.cursor >= LEASE_READ_BYTES / 2 || t.now >= settleAt;
            if (!answer)
                t.at(settleAt);
        }
        else if (!answer)
        {
            answer = leaseComplete(lease, read.cursor, t.now);
            if (!answer && lease.terminalSeen)
                t.at(lease.lastRxUs + leaseReadLullUs(lease));
            else if (!answer && lease.mode == LeaseMode::NoAck && lease.rxSeen)
                t.at(lease.lastRxUs + static_cast<int64_t>(lease.completionIdleMs) * 1000);
        }

        if (!answer)
        {
            t.at(read.deadlineUs);
            return false;
        }

        Reply reply = replyTo(read.waiter);
        if (read.cursor < lease.rxStart)
            setError(reply, 400, "invalid_cursor");
        else if (!reply.skip)
            setReadResult(reply, lease, read.cursor, t.now);
        out.push_back(std::move(reply));
        return true;
    }


    // In order, each once its bytes fit.
    void serviceWrites(Lease &lease, Tick &t, Replies &out)
    {
        while (!lease.writes.empty())
        {
            PendingWrite &write = lease.writes.front();
            Reply reply;

            if (write.waiter.dead)
            {
                reply = replyTo(write.waiter);
            }
            else if (lease.txFailed)
            {
                reply = replyTo(write.waiter);
                setError(reply, 503, "write_failed");
            }
            else if (leaseTxFree(lease) >= write.data.size())
            {
                leaseTxPush(lease, write.data.data(), write.data.size());
                leaseNoteWrite(lease, write.data.data(), write.data.size());
                notifyTx();
                reply = replyTo(write.waiter);
                setJson(reply, 200, "{\"written\":" + String(write.data.size()) + "}");
            }
            else if (t.now >= write.deadlineUs)
            {
                reply = replyTo(write.waiter);
                setError(reply, 429, "lease_not_writable");
            }
            else
            {
                t.at(write.deadlineUs);
                return;
            }

            lease.lastActivityMs = millis();
            out.push_back(std::move(reply));
            lease.writes.pop_front();
        }
    }


    void serviceLeases(Tick &t, Replies &out)
    {
        uint32_t nowMs = millis();
        std::vector<Lease *> leases = state.leases;

        for (Lease *lease : leases)
        {
            if (lease->state == LeaseState::Queued)
            {
                // Abandoned: the client stopped polling it.
                if (nowMs - lease->lastActivityMs >= lease->idleMs)
                    endLease(lease, out);
                continue;
            }

            if (SerialBridge::droppedRxBytes() != lease->dropsAtStart)
                lease->overflow = true;

            if (!lease->reads.empty() || !lease->writes.empty())
                lease->lastActivityMs = nowMs;

            const char *reason = nullptr;
            if (nowMs - lease->activatedMs >= lease->lifetimeMs)
                reason = "lifetime";
            else if (nowMs - lease->lastActivityMs >= lease->idleMs)
                reason = "idle";
            if (reason)
            {
                Serial.printf("Retro-Bridge: lease %lu/%lu released (%s)\n",
                    static_cast<unsigned long>(lease->clientId), static_cast<unsigned long>(lease->leaseId), reason);
                endLease(lease, out);
                continue;
            }

            for (auto it = lease->reads.begin(); it != lease->reads.end();)
            {
                if (serviceRead(*lease, *it, t, out))
                    it = lease->reads.erase(it);
                else
                    ++it;
            }
            serviceWrites(*lease, t, out);
        }
    }


    void startWake(Tick &t, Replies &out)
    {
        Wake &wake = state.wake;
        wake.pending = false;

        if (queuedJobs() > 0 || !acquireLink())
        {
            Reply reply = replyTo(wake.waiter);
            setWakeBusy(reply);
            out.push_back(std::move(reply));
            return;
        }

        wake.running = true;
        wake.generation++;
        wake.startedMs = millis();
        wake.deadlineUs = t.now + static_cast<int64_t>(wake.timeoutMs) * 1000;
        wake.nextProbeUs = wake.deadlineUs;
        wake.connectedBefore = SerialBridge::isConnected();
        wake.everConnected = false;
        wake.pwrOnSent = false;
        wake.acknowledged = false;
        wake.wakeCommands = 0;
        wake.probes = 0;
        wake.txToken = 0;
        wake.line = "";
        wake.response = "";

        state.owner = Owner::Wake;
        updateOwner();
    }


    // Interactive first, then oldest; leases count as interactive.
    void startNext(Tick &t, Replies &out)
    {
        if (state.owner != Owner::None)
            return;

        // The firmware's own UI goes first, at the gap between bridge jobs.
        if (Rt4kLink::internalWaiting())
        {
            releaseLink();
            t.at(t.now + ACQUIRE_RETRY_US);
            return;
        }

        if (state.wake.pending)
        {
            startWake(t, out);
            if (state.owner != Owner::None)
                return;
        }

        Transaction *bestTxn = nullptr;
        Lease *bestLease = nullptr;
        auto better = [&](bool interactive, uint32_t seq)
        {
            bool bestInteractive = bestLease ? true : (bestTxn && bestTxn->interactive);
            uint32_t bestSeq = bestLease ? bestLease->seq : (bestTxn ? bestTxn->seq : UINT32_MAX);
            if (!bestTxn && !bestLease)
                return true;
            if (interactive != bestInteractive)
                return interactive;
            return seq < bestSeq;
        };

        for (Transaction *txn : state.queue)
        {
            if (better(txn->interactive, txn->seq))
            {
                bestTxn = txn;
                bestLease = nullptr;
            }
        }
        for (Lease *lease : state.leases)
        {
            if (lease->state == LeaseState::Queued && better(true, lease->seq))
            {
                bestLease = lease;
                bestTxn = nullptr;
            }
        }

        if (!bestTxn && !bestLease)
        {
            // Lets internal operations in between bridge jobs.
            releaseLink();
            return;
        }

        if (!SerialBridge::isConnected())
        {
            // Transactions fail at once; leases wait for the FT232R.
            std::vector<Transaction *> queued;
            queued.swap(state.queue);
            for (Transaction *txn : queued)
                finishTransaction(*txn, 503, "serial_unavailable", out);
            releaseLink();
            return;
        }

        if (!acquireLink())
        {
            t.at(t.now + ACQUIRE_RETRY_US);
            return;
        }

        if (bestTxn)
        {
            state.queue.erase(std::remove(state.queue.begin(), state.queue.end(), bestTxn), state.queue.end());
            bestTxn->state = TxnState::Running;
            bestTxn->phase = TxnPhase::Draining;
            bestTxn->drainUntilUs = t.now + DRAIN_MAX_US;
            state.txn = bestTxn;
            state.owner = Owner::Transaction;
            updateOwner();
            return;
        }

        if (!activateLease(*bestLease))
        {
            Reply reply = replyTo(bestLease->createWaiter);
            setError(reply, 503, "out_of_memory");
            out.push_back(std::move(reply));
            endLease(bestLease, out);
            return;
        }

        if (bestLease->createWaiter)
        {
            Reply reply = replyTo(bestLease->createWaiter);
            setJson(reply, 200, leaseJson(*bestLease, false));
            out.push_back(std::move(reply));
        }
    }


    void answerQueuedCreates(Replies &out)
    {
        for (Lease *lease : state.leases)
        {
            if (lease->state == LeaseState::Queued && lease->createWaiter)
            {
                Reply reply = replyTo(lease->createWaiter);
                setJson(reply, 200, leaseJson(*lease, false));
                out.push_back(std::move(reply));
            }
        }
    }


    void schedTaskMain(void * /*arg*/)
    {
        TickType_t waitTicks = pdMS_TO_TICKS(IDLE_WAIT_US / 1000);

        for (;;)
        {
            ulTaskNotifyTake(pdTRUE, waitTicks);

            Replies out;
            int64_t next;
            {
                Guard guard;
                int64_t now = esp_timer_get_time();
                Tick t{now, now + IDLE_WAIT_US};

                expireClients(out);
                expireQueued(t, out);
                serviceTransaction(t, out);
                serviceWake(t, out);
                serviceLeases(t, out);
                startNext(t, out);
                serviceTransaction(t, out);
                serviceWake(t, out);
                answerQueuedCreates(out);
                next = t.next;
            }

            send(out);

            int64_t waitUs = next - esp_timer_get_time();
            // At least a tick, so an overdue deadline can't starve core 1.
            waitTicks = std::max<TickType_t>(1, pdMS_TO_TICKS(std::max<int64_t>(0, (waitUs + 999) / 1000)));
        }
    }
}


namespace RetroBridge
{

void startScheduler()
{
    BaseType_t created = xTaskCreatePinnedToCore(
        schedTaskMain, "rb_sched", SCHED_TASK_STACK_BYTES, nullptr, SCHED_TASK_PRIORITY, &schedTask, 1);
    if (created != pdPASS)
        Serial.println("ERROR: Retro-Bridge scheduler failed to start");
}


void notifyScheduler()
{
    if (schedTask)
        xTaskNotifyGive(schedTask);
}


void onCommandSent(uint32_t token, bool ok, int64_t nowUs)
{
    if (state.owner == Owner::Transaction && state.txn->txToken == token)
    {
        if (ok)
        {
            state.txn->txDoneUs = nowUs;
            state.txn->phase = TxnPhase::Collecting;
        }
        else
        {
            state.txn->txFailed = true;
        }
    }
    else if (state.owner == Owner::Wake && state.wake.txToken == token && !ok)
    {
        // Resent once the FT232R is back.
        state.wake.pwrOnSent = false;
    }
}

} // namespace RetroBridge
