#include "link.h"
#include "link_internal.h"

#include "protocol.h"

#include <cstring>

#include <esp_heap_caps.h>

using namespace Rt4kProtocol;

namespace
{
    constexpr char DEFAULT_BANNER_PATH[] = "image/default.bmp";
    constexpr char DEFAULT_BANNER_FILE[] = "default.bmp";

    String cachedPath;
    uint8_t *cachedData = nullptr;
    size_t cachedSize = 0;

    void cacheBanner(const String &path, const std::vector<uint8_t> &bytes)
    {
        heap_caps_free(cachedData);
        cachedData = nullptr;
        cachedSize = 0;
        cachedPath = "";

        if (bytes.empty())
            return;

        cachedData = static_cast<uint8_t *>(heap_caps_malloc(bytes.size(), MALLOC_CAP_SPIRAM));
        if (!cachedData)
            return;

        memcpy(cachedData, bytes.data(), bytes.size());
        cachedSize = bytes.size();
        cachedPath = path;
    }
}


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
    Session session("banner");
    if (!session)
        return failure(session.error());

    return getBannerInfoLocked(out);
}


Result downloadBanner(BannerInfo &info, std::vector<uint8_t> &out, const String &knownPath, bool &unchanged)
{
    unchanged = false;

    // One session, so no command lands between the query and the download.
    Session session("banner");
    if (!session)
        return failure(session.error());

    Result result = getBannerInfoLocked(info);
    if (!result.ok)
        return result;

    bool usingDefault = !info.present;
    if (usingDefault)
    {
        info.present = true;
        info.path = DEFAULT_BANNER_PATH;
        info.file = DEFAULT_BANNER_FILE;
    }

    // Skip the multi-second download when the caller already has this path.
    if (knownPath.length() > 0 && info.path == knownPath)
    {
        unchanged = true;
        return success();
    }

    if (cachedData && info.path == cachedPath)
    {
        out.assign(cachedData, cachedData + cachedSize);
        return success();
    }

    Result download = downloadFileLocked(stripSlashes(info.path), out);
    if (download.ok)
        cacheBanner(info.path, out);

    if (!download.ok && usingDefault)
    {
        info.present = false;
        out.clear();
        return success();
    }

    return download;
}

} // namespace Rt4kLink
