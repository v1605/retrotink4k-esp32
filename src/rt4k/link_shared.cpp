#include "link_internal.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "protocol.h"
#include "../serial_bridge.h"

using namespace Rt4kProtocol;


namespace Rt4kLink
{

Session::Session()
{
    if (!SerialBridge::isConnected())
    {
        failureReason = "FT232R not connected";
        return;
    }

    if (xSemaphoreTake(opMutex, pdMS_TO_TICKS(OP_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        failureReason = "RT4K link busy";
        return;
    }

    locked = true;
    drainQueues();
}


Session::~Session()
{
    if (!locked)
        return;

    // A failed operation can leave the parser expecting frames.
    pendingReadyKeyword = nullptr;
    finalAckSequence = -1;
    mode = Mode::AwaitingLines;

    xSemaphoreGive(opMutex);
}


Result sendLine(const String &command)
{
    // A line break would end the command early and start another.
    if (command.indexOf('\r') >= 0 || command.indexOf('\n') >= 0)
        return failure("Command contains a line break");

    if (!sendCommand(command))
        return failure("FT232R not ready");

    return success();
}


Result receiveFrameTransfer(uint16_t nonce, size_t total, std::vector<uint8_t> &out, const String &label)
{
    uint8_t nextSequence = 0;

    // Tells the RT4K to stop sending; anything it sends afterwards is cleared
    // by the next drainQueues().
    auto failTransfer = [&](const String &msg) -> Result
    {
        sendFrame(nonce, FRAME_ABORT, nextSequence);
        mode = Mode::AwaitingLines;
        return failure(msg);
    };

    bool gotResponse = false;
    Deadline idle(FRAME_IDLE_TIMEOUT_MS);

    while (out.size() < total || !gotResponse)
    {
        TickType_t left = idle.remaining();
        if (left == 0)
            return failTransfer("RT4K " + label + " transfer timed out");

        FramePtr frame;
        if (!waitForFrame(left, frame))
            continue;

        idle.restart(FRAME_IDLE_TIMEOUT_MS);
        nextSequence = frame->sequence + 1;

        if (!frame->crcValid)
            return failTransfer("RTL1 frame CRC mismatch");
        if (frame->nonce != nonce)
            return failTransfer("RTL1 frame for the wrong session");

        if (frame->type == FRAME_DATA)
        {
            if (out.size() + frame->payload.size() > total)
                return failTransfer("RT4K sent more data than declared");
            out.insert(out.end(), frame->payload.begin(), frame->payload.end());
        }
        else if (frame->type == FRAME_RESPONSE)
        {
            uint8_t digest[32];
            if (out.size() != total || frame->payload.size() != sizeof(digest))
                return failTransfer("Incomplete " + label + " transfer");

            sha256(out.data(), out.size(), digest);
            if (memcmp(digest, frame->payload.data(), sizeof(digest)) != 0)
                return failTransfer("SHA-256 verification failed");

            gotResponse = true;
        }
        else if (frame->type == FRAME_NAK)
        {
            // No abort needed -- the device ended the transfer itself.
            mode = Mode::AwaitingLines;
            return failure("RT4K rejected the " + label + " transfer");
        }
    }

    mode = Mode::AwaitingLines;
    return success();
}


bool collectLinesUntil(
    const std::function<bool(const String &)> &isTerminal,
    std::vector<String> &lines,
    uint32_t timeoutMs)
{
    Deadline deadline(timeoutMs);

    for (TickType_t left = deadline.remaining(); left > 0; left = deadline.remaining())
    {
        String line;
        if (!waitForLine(left, line))
            return false;

        lines.push_back(line);
        if (isTerminal(line))
            return true;
    }
    return false;
}


Result waitForCompletion(
    const String &doneLine,
    const String &prefix,
    const String &noReplyError,
    const String &altPrefix,
    uint32_t timeoutMs,
    const std::function<String(const std::vector<String> &)> &formatError)
{
    std::vector<String> lines;
    bool matched = collectLinesUntil(
        [&](const String &line)
        {
            return line == doneLine || line.startsWith(prefix) ||
                (altPrefix.length() > 0 && line.startsWith(altPrefix));
        },
        lines, timeoutMs);

    if (matched && lines.back() == doneLine)
        return success();

    return failure(formatError ? formatError(lines) : (lines.empty() ? noReplyError : lines.back()));
}

} // namespace Rt4kLink
