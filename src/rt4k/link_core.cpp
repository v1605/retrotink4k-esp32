#include "link.h"
#include "link_internal.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "protocol.h"

using namespace Rt4kProtocol;


namespace Rt4kLink
{

namespace
{
    // Lets the RT4K start on a raw command before the next one goes out.
    constexpr uint32_t RAW_COMMAND_SETTLE_MS = 30;
}


void begin()
{
    Rt4kProtocol::begin();
}


void feed(const uint8_t *data, size_t length, std::vector<uint8_t> *textOut)
{
    Rt4kProtocol::feed(data, length, textOut);
}


Result sendRawCommand(const String &text)
{
    Session session;
    if (!session)
        return failure(session.error());

    Result sent = sendLine(text);
    if (sent.ok)
        vTaskDelay(pdMS_TO_TICKS(RAW_COMMAND_SETTLE_MS));

    return sent;
}


// Holds the link until the RT4K confirms, so the next command doesn't arrive
// while it's still processing this one.
Result sendRemoteButton(const String &button)
{
    Session session;
    if (!session)
        return failure(session.error());

    Result sent = sendLine("remote " + button);
    if (!sent.ok)
        return sent;

    String expected = "Serial Remote: " + button;
    std::vector<String> lines;
    collectLinesUntil([&](const String &line) { return line == expected; }, lines, COMMAND_ACK_TIMEOUT_MS);

    vTaskDelay(pdMS_TO_TICKS(REMOTE_BUTTON_SETTLE_MS));
    return success();
}

} // namespace Rt4kLink
