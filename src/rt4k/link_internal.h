#pragma once

#include "link.h"
#include "protocol.h"

#include <functional>

// Helpers shared by the link_*.cpp files.
namespace Rt4kLink
{
    inline Result success()
    {
        return {true, String()};
    }

    inline Result failure(const String &error)
    {
        return {false, error};
    }

    // One RT4K operation: checks the FT232R, takes opMutex and drops stale
    // replies. Destruction returns the parser to line mode and releases the
    // mutex. Test it before use; error() says why it failed.
    class Session
    {
    public:
        Session();
        ~Session();

        Session(const Session &) = delete;
        Session &operator=(const Session &) = delete;

        explicit operator bool() const
        {
            return locked;
        }

        const String &error() const
        {
            return failureReason;
        }

    private:
        bool locked = false;
        String failureReason;
    };

    // Sends one command line; refuses one containing a line break.
    Result sendLine(const String &command);

    // Caller holds a Session; cleanPath is already validated.
    Result downloadFileLocked(const String &cleanPath, std::vector<uint8_t> &out);

    // Caller holds a Session.
    Result getBannerInfoLocked(BannerInfo &out);

    // Receives `total` bytes of data frames plus the closing SHA-256 response.
    // Resets `mode`, and aborts the transfer on failure. `label` names it in
    // errors.
    Result receiveFrameTransfer(uint16_t nonce, size_t total, std::vector<uint8_t> &out, const String &label);

    // Collects lines until one satisfies isTerminal or the timeout passes.
    // Returns whether the terminal line arrived.
    bool collectLinesUntil(
        const std::function<bool(const String &)> &isTerminal,
        std::vector<String> &lines,
        uint32_t timeoutMs);

    // Succeeds on doneLine; fails on a line starting with prefix or altPrefix.
    // The error is formatError(lines), else the last line, else noReplyError.
    Result waitForCompletion(
        const String &doneLine,
        const String &prefix,
        const String &noReplyError,
        const String &altPrefix = "",
        uint32_t timeoutMs = Rt4kProtocol::REPLY_TIMEOUT_MS,
        const std::function<String(const std::vector<String> &)> &formatError = nullptr);
}
