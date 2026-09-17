#include "link.h"
#include "link_internal.h"

#include <algorithm>

#include "protocol.h"
#include "../serial_bridge.h"

using namespace Rt4kProtocol;


namespace Rt4kLink
{

namespace
{
    constexpr uint32_t RESYNC_TIMEOUT_MS = 1500;
    constexpr uint32_t ACK_POLL_MS = 50;

    bool isValidPath(const String &cleanPath)
    {
        return cleanPath.length() > 0 && cleanPath.indexOf("..") < 0;
    }

    // Error for transferFailed, with the RT4K's line if it was queued.
    String uploadEndedError()
    {
        String line;
        return waitForLine(0, line) ? "RT4K ended the upload: " + line : String("RT4K ended the upload");
    }
}


Result downloadFileLocked(const String &cleanPath, std::vector<uint8_t> &out)
{
    drainQueues();
    pendingReadyKeyword = "get ready";

    Result sent = sendLine("get " + cleanPath);
    if (!sent.ok)
        return sent;

    std::vector<String> lines;
    bool replied = collectLinesUntil(
        [](const String &line)
        {
            return line.startsWith("get ready") || line.startsWith("get:") || line.startsWith("get err:");
        },
        lines, REPLY_TIMEOUT_MS);

    if (!replied)
        return failure("Timed out waiting for RT4K to start the download" + describeCapturedLines(lines));

    const String &ready = lines.back();
    if (!ready.startsWith("get ready"))
        return failure(ready);

    uint32_t total = 0;
    uint16_t nonce = 0;
    if (!parseUnsigned(fieldValue(ready, "total"), total) || !parseNonce(ready, nonce))
        return failure("Malformed ready reply: " + ready);

    if (total > MAX_FILE_BYTES)
        return failure("RT4K reported an implausible file size");

    out.reserve(total);

    Result transfer = receiveFrameTransfer(nonce, total, out, "file");
    if (!transfer.ok)
        return transfer;

    return waitForCompletion("get done", "get ", "RT4K did not confirm the download");
}


Result downloadFile(const String &path, std::vector<uint8_t> &out)
{
    String cleanPath = stripSlashes(path);
    if (!isValidPath(cleanPath))
        return failure("Invalid path");

    Session session;
    if (!session)
        return failure(session.error());

    return downloadFileLocked(cleanPath, out);
}


Result uploadFile(const String &path, const uint8_t *data, size_t length, const ProgressCallback &onProgress)
{
    String cleanPath = stripSlashes(path);
    if (!isValidPath(cleanPath))
        return failure("Invalid path");

    if (length > MAX_FILE_BYTES)
        return failure("File too large");

    Session session;
    if (!session)
        return failure(session.error());

    String hash = sha256Hex(data, length);

    const bool streaming = SerialBridge::hardwareFlowControlEnabled();

    pendingReadyKeyword = "put ready";

    Result sent = sendLine(
        String(streaming ? "put " : "put -a ") + String(static_cast<unsigned long>(length)) + " " + hash + " " + cleanPath);
    if (!sent.ok)
        return sent;

    std::vector<String> lines;
    bool replied = collectLinesUntil(
        [](const String &line)
        {
            return line.startsWith("put ready") || line.startsWith("put:") || line.startsWith("put err:");
        },
        lines, REPLY_TIMEOUT_MS);

    if (!replied)
        return failure("Timed out waiting for RT4K to accept the upload" + describeCapturedLines(lines));

    const String &ready = lines.back();
    if (!ready.startsWith("put ready"))
        return failure(ready);

    uint16_t nonce = 0;
    if (!parseNonce(ready, nonce))
        return failure("Malformed ready reply: " + ready);

    uint8_t sequence = 0;
    size_t offset = 0;

    // On failure: abort the transfer and resync the RT4K's text parser.
    auto failDuringTransfer = [&](const String &msg) -> Result
    {
        // If the RT4K already gave up it's in text mode; skip the abort frame.
        if (!transferFailed)
            sendFrame(nonce, FRAME_ABORT, sequence);

        // Leftover frame bytes can sit in the RT4K's line buffer and corrupt
        // the next command. A bare newline flushes them ("Bad Command" reply).
        mode = Mode::AwaitingLines;
        sendCommand("");
        std::vector<String> drained;
        collectLinesUntil(
            [](const String &line) { return line.startsWith("Bad Command"); },
            drained, RESYNC_TIMEOUT_MS);

        return failure(msg);
    };

    auto sendFrameAcked = [&](const uint8_t *payload, uint16_t len, String &error) -> bool
    {
        for (int attempt = 0; attempt < UPLOAD_FRAME_ATTEMPTS; attempt++)
        {
            mode = Mode::AwaitingFrames;

            if (!sendFrame(nonce, FRAME_DATA, sequence, payload, len))
            {
                error = "Failed to write to the FTDI serial port";
                return false;
            }

            Deadline ackDeadline(UPLOAD_ACK_TIMEOUT_MS);

            for (TickType_t left = ackDeadline.remaining(); left > 0; left = ackDeadline.remaining())
            {
                if (transferFailed)
                {
                    error = uploadEndedError();
                    return false;
                }

                FramePtr frame;
                if (!waitForFrame(std::min<TickType_t>(left, pdMS_TO_TICKS(ACK_POLL_MS)), frame))
                    continue;

                if (!frame->crcValid || frame->nonce != nonce)
                    continue;

                if (frame->type == FRAME_ACK && frame->sequence == sequence)
                    return true;

                // Resend.
                if (frame->type == FRAME_NAK)
                    break;

                if (frame->type == FRAME_ABORT)
                {
                    error = "RT4K aborted the upload";
                    return false;
                }
            }
        }

        error = "RT4K didn't acknowledge upload frame " + String(sequence) +
            " after " + String(UPLOAD_FRAME_ATTEMPTS) + " attempts";
        return false;
    };

    if (streaming)
    {
        // Frames back to back, paced by the USB queue and the RT4K's CTS.
        // Stops at the first NAK or failure report.
        auto streamFailure = [&]() -> String
        {
            if (transferFailed)
                return uploadEndedError();

            FramePtr frame;
            while (waitForFrame(0, frame))
            {
                if (frame->type == FRAME_NAK || frame->type == FRAME_ABORT)
                    return "RT4K rejected upload frame " + String(frame->sequence);
            }
            return "";
        };

        while (offset < length)
        {
            String problem = streamFailure();
            if (problem.length())
                return failDuringTransfer(problem);

            size_t chunk = std::min<size_t>(MAX_PAYLOAD, length - offset);
            if (!sendFrame(nonce, FRAME_DATA, sequence, data + offset, static_cast<uint16_t>(chunk)))
                return failDuringTransfer("Failed to write to the FTDI serial port");

            offset += chunk;
            sequence++;

            if (onProgress)
                onProgress(offset, length);
        }

        // Final empty frame signals end-of-file.
        if (!sendFrame(nonce, FRAME_DATA, sequence))
            return failDuringTransfer("Failed to write to the FTDI serial port");
    }
    else
    {
        while (offset < length)
        {
            size_t chunk = std::min<size_t>(MAX_PAYLOAD, length - offset);
            String error;
            if (!sendFrameAcked(data + offset, static_cast<uint16_t>(chunk), error))
                return failDuringTransfer(error);

            offset += chunk;
            sequence++;

            if (onProgress)
                onProgress(offset, length);
        }

        // Final empty frame signals end-of-file.
        finalAckSequence = sequence;
        String error;
        if (!sendFrameAcked(nullptr, 0, error))
        {
            finalAckSequence = -1;
            return failDuringTransfer(error);
        }
    }

    mode = Mode::AwaitingLines;
    return waitForCompletion("put done", "put ", "RT4K did not confirm the upload", "put:");
}

} // namespace Rt4kLink
