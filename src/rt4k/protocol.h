#pragma once

#include <Arduino.h>

#include <atomic>
#include <memory>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace Rt4kProtocol
{
    constexpr uint16_t MAX_PAYLOAD = 2048;
    constexpr uint32_t FRAME_IDLE_TIMEOUT_MS = 7000;
    constexpr uint32_t OP_MUTEX_TIMEOUT_MS = 20000;
    // Short queries: "prof get", "banner", "model", "ver".
    constexpr uint32_t QUERY_TIMEOUT_MS = 3000;
    // Ready lines, listings and completion lines.
    constexpr uint32_t REPLY_TIMEOUT_MS = 10000;
    constexpr uint32_t COMMAND_ACK_TIMEOUT_MS = 400;
    constexpr uint32_t REMOTE_BUTTON_SETTLE_MS = 60;
    // Resend an unacked upload frame after this; the RT4K gives up within ~5s.
    constexpr uint32_t UPLOAD_ACK_TIMEOUT_MS = 1000;
    constexpr int UPLOAD_FRAME_ATTEMPTS = 4;

    enum class Mode
    {
        AwaitingLines,
        AwaitingFrames,
    };

    enum FrameType : uint8_t
    {
        FRAME_COMMAND = 1,
        FRAME_RESPONSE = 2,
        FRAME_DATA = 3,
        FRAME_ACK = 4,
        FRAME_NAK = 5,
        FRAME_ABORT = 6,
        FRAME_PING = 7,
    };

    struct Frame
    {
        uint16_t nonce = 0;
        uint8_t type = 0;
        uint8_t sequence = 0;
        bool crcValid = false;
        std::vector<uint8_t> payload;
    };

    using FramePtr = std::unique_ptr<Frame>;

    // Time left on a timeout that spans several waits. FreeRTOS's timeout
    // tracking handles tick-count overflow.
    class Deadline
    {
    public:
        explicit Deadline(uint32_t timeoutMs)
        {
            restart(timeoutMs);
        }

        void restart(uint32_t timeoutMs)
        {
            ticksLeft = pdMS_TO_TICKS(timeoutMs);
            vTaskSetTimeOutState(&timeOut);
        }

        // 0 once expired.
        TickType_t remaining()
        {
            if (ticksLeft > 0 && xTaskCheckForTimeOut(&timeOut, &ticksLeft) != pdFALSE)
                ticksLeft = 0;
            return ticksLeft;
        }

    private:
        TimeOut_t timeOut;
        TickType_t ticksLeft = 0;
    };

    // Shared by the RX task (the parser) and the task running the current
    // operation; opMutex allows one operation at a time.
    extern std::atomic<Mode> mode;
    // Prefix (a string literal) of the reply line that switches the parser to
    // frames, or nullptr.
    extern std::atomic<const char *> pendingReadyKeyword;
    // Sequence of the final upload frame awaiting its ack, or -1. The parser
    // switches to line mode on that ack so the "put done" after it is text.
    extern std::atomic<int> finalAckSequence;
    // Set (and the line queued) when the RT4K reports an upload failure as
    // text while frames are expected. Cleared by drainQueues().
    extern std::atomic<bool> transferFailed;
    extern SemaphoreHandle_t opMutex;

    void begin();

    // Parses FT232R bytes; RX task only. textOut, if given, receives the
    // printable bytes parsed as text rather than as frames.
    void feed(const uint8_t *data, size_t length, std::vector<uint8_t> *textOut);

    // Writes command followed by "\r\n".
    bool sendCommand(const String &command);
    bool sendFrame(
        uint16_t nonce,
        uint8_t type,
        uint8_t sequence,
        const uint8_t *payload = nullptr,
        uint16_t length = 0);

    // Drops queued replies and has the RX task reset its parser before its
    // next byte.
    void drainQueues();
    bool waitForLine(TickType_t ticks, String &outLine);
    bool waitForFrame(TickType_t ticks, FramePtr &outFrame);

    String stripSlashes(const String &path);

    // Value of "key=value" in a space-separated reply line, or "".
    String fieldValue(const String &line, const char *key);
    // Everything after "key=" to the end of the line, for names that can
    // contain spaces, or "".
    String fieldRest(const String &line, const char *key);
    // strtoul that rejects signs, spaces, trailing junk and overflow.
    bool parseUnsigned(const String &text, uint32_t &out, int base = 10);
    // The "nonce=0x<hex>" field of a ready line.
    bool parseNonce(const String &readyLine, uint16_t &out);

    // Appended to timeout errors so failures are self-diagnosing.
    String describeCapturedLines(const std::vector<String> &lines);

    void sha256(const uint8_t *data, size_t length, uint8_t digest[32]);
    String sha256Hex(const uint8_t *data, size_t length);
}
