#include "rb_internal.h"

#include <esp_timer.h>
#include <freertos/queue.h>

#include <algorithm>

#include "../serial_bridge.h"

// rb_tx: the only writer while the bridge owns the link.

namespace
{
    using namespace RetroBridge;

    constexpr UBaseType_t TX_TASK_PRIORITY = 5;
    constexpr uint32_t TX_TASK_STACK_BYTES = 4096;
    constexpr size_t COMMAND_QUEUE_LEN = 8;
    constexpr uint32_t FLUSH_TIMEOUT_MS = 1000;
    constexpr TickType_t IDLE_WAIT_TICKS = pdMS_TO_TICKS(50);

    struct Command
    {
        uint32_t token;
        std::vector<uint8_t> data;
    };

    TaskHandle_t txTask = nullptr;
    QueueHandle_t commands = nullptr;
    std::atomic<uint32_t> nextToken{1};
    uint8_t piece[TX_PIECE_BYTES];


    bool writePieces(const uint8_t *data, size_t length)
    {
        while (length > 0)
        {
            size_t n = std::min(length, TX_PIECE_BYTES);
            if (!SerialBridge::sendRaw(data, n))
                return false;
            data += n;
            length -= n;
        }
        return true;
    }


    void runCommand(Command &command)
    {
        bool ok = writePieces(command.data.data(), command.data.size());
        // Fails without the write queue; the bytes still went out.
        if (ok && !SerialBridge::flush(FLUSH_TIMEOUT_MS))
            ok = SerialBridge::isConnected();

        int64_t now = esp_timer_get_time();
        {
            Guard guard;
            state.txPending--;
            if (!ok)
                state.txErrors++;
            onCommandSent(command.token, ok, now);
        }
        notifyScheduler();
    }


    // False when there was nothing to send.
    bool drainLease()
    {
        size_t length = 0;
        uint32_t serial = 0;
        {
            Guard guard;
            Lease *lease = state.owner == Owner::Lease ? state.lease : nullptr;
            if (!lease || lease->txCount == 0 || lease->txFailed)
                return false;

            length = leaseTxTake(*lease, piece, sizeof(piece));
            serial = lease->serial;
            state.txPending++;
        }

        bool ok = SerialBridge::sendRaw(piece, length);

        {
            Guard guard;
            state.txPending--;
            Lease *lease = state.owner == Owner::Lease ? state.lease : nullptr;
            if (!ok && lease && lease->serial == serial)
                lease->txFailed = true;
            if (!ok)
                state.txErrors++;
        }
        notifyScheduler();
        return true;
    }


    void txTaskMain(void * /*arg*/)
    {
        for (;;)
        {
            ulTaskNotifyTake(pdTRUE, IDLE_WAIT_TICKS);

            Command *command = nullptr;
            while (xQueueReceive(commands, &command, 0) == pdTRUE)
            {
                runCommand(*command);
                delete command;
            }

            while (drainLease())
            {
            }
        }
    }
}


namespace RetroBridge
{

void startTx()
{
    commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(Command *));

    BaseType_t created = xTaskCreatePinnedToCore(
        txTaskMain, "rb_tx", TX_TASK_STACK_BYTES, nullptr, TX_TASK_PRIORITY, &txTask, 1);
    if (!commands || created != pdPASS)
        Serial.println("ERROR: Retro-Bridge TX task failed to start");
}


void notifyTx()
{
    if (txTask)
        xTaskNotifyGive(txTask);
}


uint32_t queueCommand(const uint8_t *data, size_t length)
{
    uint32_t token = nextToken++;
    if (token == 0)
        token = nextToken++;

    auto *command = new Command{token, std::vector<uint8_t>(data, data + length)};
    if (xQueueSend(commands, &command, 0) != pdTRUE)
    {
        delete command;
        return 0;
    }

    state.txPending++;
    notifyTx();
    return token;
}

} // namespace RetroBridge
