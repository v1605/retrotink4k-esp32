#include "link.h"
#include "link_internal.h"

#include "protocol.h"

using namespace Rt4kProtocol;


namespace Rt4kLink
{

namespace
{
    // 256 glyphs x 16 bytes.
    constexpr uint32_t OSD_FONT_BYTES = 4096;
    // 2048 character codes, then 2048 attribute bytes.
    constexpr size_t OSD_PLANE_HALF_BYTES = 2048;
    constexpr size_t OSD_PLANE_BYTES = 2 * OSD_PLANE_HALF_BYTES;
}


Result downloadOsdFont(OsdFont &out)
{
    Session session;
    if (!session)
        return failure(session.error());

    pendingReadyKeyword = "font ready";

    Result sent = sendLine("font");
    if (!sent.ok)
        return sent;

    std::vector<String> lines;
    if (!collectLinesUntil([](const String &line) { return line.startsWith("font ready"); }, lines, REPLY_TIMEOUT_MS))
        return failure("Timed out waiting for RT4K to start the font transfer" + describeCapturedLines(lines));

    const String &ready = lines.back();
    uint32_t size = 0;
    uint16_t nonce = 0;
    if (!parseUnsigned(fieldValue(ready, "size"), size) || !parseNonce(ready, nonce))
        return failure("Malformed ready reply: " + ready);

    if (size != OSD_FONT_BYTES)
        return failure("RT4K reported an unsupported font size");

    out.data.clear();
    out.data.reserve(size);

    Result transfer = receiveFrameTransfer(nonce, size, out.data, "font");
    if (!transfer.ok)
        return transfer;

    // Font transfers end with "get done", like file downloads.
    return waitForCompletion("get done", "get ", "RT4K did not confirm the font transfer");
}


Result downloadOsdPlane(int plane, OsdPlane &out)
{
    if (plane != 1 && plane != 2)
        return failure("OSD plane must be 1 or 2");

    Session session;
    if (!session)
        return failure(session.error());

    const char *command = plane == 1 ? "osd" : "osd2";
    const char *readyPrefix = plane == 1 ? "osd ready" : "osd2 ready";

    pendingReadyKeyword = readyPrefix;

    Result sent = sendLine(command);
    if (!sent.ok)
        return sent;

    std::vector<String> lines;
    bool replied = collectLinesUntil(
        [readyPrefix](const String &line)
        {
            return line.startsWith(readyPrefix) || line.indexOf("nothing shown") >= 0;
        },
        lines, REPLY_TIMEOUT_MS);

    if (!replied)
        return failure("Timed out waiting for RT4K to start the OSD transfer" + describeCapturedLines(lines));

    const String &ready = lines.back();
    if (!ready.startsWith(readyPrefix))
    {
        out.available = false;
        return success();
    }

    // Plane 1 calls it "width", plane 2 "cols".
    uint16_t nonce = 0;
    if (!parseUnsigned(fieldValue(ready, "rows"), out.rows) ||
        !parseUnsigned(fieldValue(ready, "stride"), out.stride) ||
        !parseUnsigned(fieldValue(ready, plane == 1 ? "width" : "cols"), out.width) ||
        !parseUnsigned(fieldValue(ready, "cells"), out.cells) ||
        !parseNonce(ready, nonce))
        return failure("Malformed ready reply: " + ready);

    if (plane == 2)
    {
        parseUnsigned(fieldValue(ready, "on"), out.on);
        parseUnsigned(fieldValue(ready, "osk"), out.osk);
    }

    std::vector<uint8_t> raw;
    raw.reserve(OSD_PLANE_BYTES);

    Result transfer = receiveFrameTransfer(nonce, OSD_PLANE_BYTES, raw, "OSD");
    if (!transfer.ok)
        return transfer;

    // Plane 2 finishes with "osd done" as well, not "osd2 done".
    Result completion = waitForCompletion("osd done", "osd ", "RT4K did not confirm the OSD transfer");
    if (!completion.ok)
        return completion;

    out.available = true;
    out.text.assign(raw.begin(), raw.begin() + OSD_PLANE_HALF_BYTES);
    out.color.assign(raw.begin() + OSD_PLANE_HALF_BYTES, raw.end());
    return success();
}

} // namespace Rt4kLink
