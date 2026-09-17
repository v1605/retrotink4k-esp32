#include "link.h"
#include "link_internal.h"

#include "protocol.h"

using namespace Rt4kProtocol;


namespace Rt4kLink
{

Result getBannerInfoLocked(BannerInfo &out)
{
    Result sent = sendLine("banner");
    if (!sent.ok)
        return sent;

    std::vector<String> lines;
    if (!collectLinesUntil([](const String &line) { return line.startsWith("banner="); }, lines, QUERY_TIMEOUT_MS))
        return failure("Timed out waiting for RT4K" + describeCapturedLines(lines));

    const String &last = lines.back();

    if (last.startsWith("banner=0"))
    {
        out.present = false;
        return success();
    }

    if (!last.startsWith("banner=1"))
        return failure(last);

    // "banner=1 path=<dir> file=<name>"; path ends at the space before "file=".
    int pathIdx = last.indexOf("path=");
    int fileIdx = last.indexOf(" file=");
    if (pathIdx < 0 || fileIdx <= pathIdx)
        return failure("Malformed banner reply: " + last);

    out.present = true;
    out.path = last.substring(pathIdx + 5, fileIdx);
    out.file = last.substring(fileIdx + 6);
    return success();
}


Result getBannerInfo(BannerInfo &out)
{
    Session session;
    if (!session)
        return failure(session.error());

    return getBannerInfoLocked(out);
}


Result downloadBanner(BannerInfo &info, std::vector<uint8_t> &out, const String &knownPath, bool &unchanged)
{
    unchanged = false;

    // One session, so no command lands between the query and the download.
    Session session;
    if (!session)
        return failure(session.error());

    Result result = getBannerInfoLocked(info);
    if (!result.ok || !info.present)
        return result;

    // Skip the multi-second download when the caller already has this path.
    if (knownPath.length() > 0 && info.path == knownPath)
    {
        unchanged = true;
        return success();
    }

    return downloadFileLocked(stripSlashes(info.path), out);
}

} // namespace Rt4kLink
