#include "link.h"
#include "link_internal.h"

#include <cctype>

#include "protocol.h"

using namespace Rt4kProtocol;


namespace Rt4kLink
{

namespace
{
    constexpr uint32_t FWUP_CHECK_TIMEOUT_MS = 15000;
    constexpr uint32_t FWUP_GO_TIMEOUT_MS = 10000;

    // Sends a fwup command; on success lines.back() is its "fwup" reply line.
    Result runFwupCommand(const String &command, uint32_t timeoutMs, std::vector<String> &lines)
    {
        Session session;
        if (!session)
            return failure(session.error());

        Result sent = sendLine(command);
        if (!sent.ok)
            return sent;

        if (!collectLinesUntil([](const String &line) { return line.startsWith("fwup"); }, lines, timeoutMs))
            return failure("No response from RT4K" + describeCapturedLines(lines));

        return success();
    }
}


Result getDeviceInfo(DeviceInfo &out)
{
    Session session;
    if (!session)
        return failure(session.error());

    // e.g. "model=0 RT4K_Pro", then "RT4KPRO, FW Version: 1.80.1"
    std::vector<String> modelLines;
    std::vector<String> verLines;
    bool ok =
        sendLine("model").ok &&
        collectLinesUntil([](const String &line) { return line.startsWith("model="); }, modelLines, QUERY_TIMEOUT_MS) &&
        sendLine("ver").ok &&
        collectLinesUntil([](const String &line) { return line.indexOf("FW Version:") >= 0; }, verLines, QUERY_TIMEOUT_MS);

    if (!ok)
        return failure("No response from RT4K (requires firmware 1.80 or newer)" +
            describeCapturedLines(verLines.empty() ? modelLines : verLines));

    const String &modelLine = modelLines.back();
    uint32_t modelId = 0;
    if (parseUnsigned(fieldValue(modelLine, "model"), modelId))
        out.modelId = static_cast<int>(modelId);

    int space = modelLine.indexOf(' ');
    out.model = space < 0 ? String() : modelLine.substring(space + 1);

    const String &verLine = verLines.back();
    out.version = verLine.substring(verLine.indexOf("FW Version:") + 11);
    out.version.trim();

    return success();
}


Result checkFirmwareUpdate(FirmwareUpdate &out)
{
    std::vector<String> lines;
    Result result = runFwupCommand("fwup check", FWUP_CHECK_TIMEOUT_MS, lines);
    if (!result.ok)
        return result;

    // e.g. "fwup ok version=1.75.0 token=B3A1CD49"
    const String &last = lines.back();
    if (!last.startsWith("fwup ok"))
        return failure(last);

    out.version = fieldValue(last, "version");
    out.token = fieldValue(last, "token");

    if (out.version.length() == 0 || out.token.length() == 0)
        return failure("Malformed reply: " + last);

    return success();
}


Result startFirmwareUpdate(const String &token)
{
    // Goes into a command line, so hex only.
    bool valid = token.length() > 0 && token.length() <= 32;
    for (size_t i = 0; valid && i < token.length(); i++)
        valid = isxdigit(static_cast<unsigned char>(token.charAt(i)));

    if (!valid)
        return failure("Invalid token");

    std::vector<String> lines;
    Result result = runFwupCommand("fwup go " + token, FWUP_GO_TIMEOUT_MS, lines);
    if (!result.ok)
        return failure(result.error + " -- if the RT4K's LED is flashing pink or blue, the install started anyway");

    // Any other reply is returned as the error.
    const String &last = lines.back();
    if (!last.startsWith("fwup: flashing"))
        return failure(last);

    return success();
}

} // namespace Rt4kLink
