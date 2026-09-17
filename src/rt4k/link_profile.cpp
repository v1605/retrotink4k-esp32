#include "link.h"
#include "link_internal.h"

#include "protocol.h"

using namespace Rt4kProtocol;


namespace Rt4kLink
{

namespace
{
    // Can be slow, especially if it triggers an HDMI resync.
    constexpr uint32_t PROFILE_LOAD_TIMEOUT_MS = 30000;

    String timeoutError(const std::vector<String> &lines)
    {
        return "Timed out waiting for RT4K" + describeCapturedLines(lines);
    }
}


Result getLoadedProfile(Profile &out)
{
    Session session;
    if (!session)
        return failure(session.error());

    Result sent = sendLine("prof get");
    if (!sent.ok)
        return sent;

    std::vector<String> lines;
    if (!collectLinesUntil([](const String &line) { return line.startsWith("prof loaded="); }, lines, QUERY_TIMEOUT_MS))
        return failure("No response from RT4K (requires firmware 1.80 or newer)" + describeCapturedLines(lines));

    const String &last = lines.back();

    if (last == "prof loaded=0")
    {
        out.loaded = false;
        return success();
    }

    String file = fieldRest(last, "file");
    if (last.startsWith("prof loaded=1") && file.length() > 0)
    {
        out.loaded = true;
        out.path = file;
        return success();
    }

    return failure(last);
}


Result loadProfile(const String &name)
{
    if (name.length() == 0 || name.startsWith("/") || name.endsWith("/") ||
        name.indexOf("..") >= 0 || name.indexOf('\\') >= 0)
        return failure("Invalid profile name");

    Session session;
    if (!session)
        return failure(session.error());

    Result sent = sendLine("prof load " + name);
    if (!sent.ok)
        return sent;

    return waitForCompletion("prof load ok", "prof load ", "", "prof:", PROFILE_LOAD_TIMEOUT_MS, timeoutError);
}


Result deleteFile(const String &path)
{
    if (path.length() == 0 || path.indexOf("..") >= 0)
        return failure("Invalid path");

    Session session;
    if (!session)
        return failure(session.error());

    Result sent = sendLine("rm " + stripSlashes(path));
    if (!sent.ok)
        return sent;

    return waitForCompletion("rm ok", "rm ", "", "rm:", REPLY_TIMEOUT_MS, timeoutError);
}


Result listProfiles(const String &path, std::vector<Entry> &out)
{
    if (path.indexOf("..") >= 0)
        return failure("Invalid path");

    Session session;
    if (!session)
        return failure(session.error());

    String cleanPath = stripSlashes(path);
    Result sent = sendLine(cleanPath.length() > 0 ? ("ls " + cleanPath) : String("ls"));
    if (!sent.ok)
        return sent;

    std::vector<String> lines;
    bool replied = collectLinesUntil(
        [](const String &line)
        {
            return line.startsWith("ls end ") || line.startsWith("ls:") || line.startsWith("ls err=");
        },
        lines, REPLY_TIMEOUT_MS);

    if (!replied)
        return failure(timeoutError(lines));

    const String &last = lines.back();
    if (!last.startsWith("ls end "))
        return failure(last);

    // "ent t=<D|F> sz=<bytes> mt=<unixtime> nm=<name>"; nm runs to the end.
    for (const String &line : lines)
    {
        if (!line.startsWith("ent "))
            continue;

        Entry entry;
        entry.name = fieldRest(line, "nm");
        if (entry.name.length() == 0 || !parseUnsigned(fieldValue(line, "sz"), entry.size))
            continue;

        entry.isDirectory = fieldValue(line, "t") == "D";
        out.push_back(entry);
    }

    return success();
}

} // namespace Rt4kLink
