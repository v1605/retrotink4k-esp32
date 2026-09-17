#include "protocol.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <esp_rom_crc.h>
#include <mbedtls/sha256.h>

#include "../serial_bridge.h"

using namespace Rt4kProtocol;

namespace
{
    constexpr size_t LINE_QUEUE_LEN = 256;
    constexpr size_t FRAME_QUEUE_LEN = 8;
    constexpr size_t MAX_LINE_LENGTH = 512;
    constexpr size_t MAX_HUNT_LINE_LENGTH = 128;

    constexpr uint8_t FRAME_MAGIC_1 = 0xA5;
    constexpr uint8_t FRAME_MAGIC_2 = 0x5A;
    // Magic, nonce, length, type, sequence; the CRC covers all but the magic.
    constexpr size_t FRAME_HEADER_SIZE = 8;
    constexpr size_t FRAME_CRC_SIZE = 2;
    constexpr uint16_t CRC_INIT = 0xFFFF;

    enum class DecodeState
    {
        Magic1, // also collects text failure reports between frames
        Magic2,
        NonceLow,
        NonceHigh,
        LengthLow,
        LengthHigh,
        Type,
        Sequence,
        Payload,
        CrcLow,
        CrcHigh,
    };

    // Parser state. Only the RX task touches it; other tasks ask for a reset
    // through parserResetRequested.
    String textLineBuffer;
    // Printable bytes seen between frames while frames are expected.
    String huntLineBuffer;

    struct
    {
        DecodeState state = DecodeState::Magic1;
        uint16_t nonce = 0;
        uint16_t length = 0;
        uint8_t type = 0;
        uint8_t sequence = 0;
        uint8_t payload[MAX_PAYLOAD];
        uint16_t payloadIndex = 0;
        uint16_t receivedCrc = 0;
    } decoder;

    std::atomic<bool> parserResetRequested{false};

    QueueHandle_t lineQueue = nullptr;
    QueueHandle_t frameQueue = nullptr;


    // CRC-16/CCITT-FALSE continued over data. The ROM routine complements its
    // input and output, hence the ~ on both sides (see esp_rom_crc.h).
    uint16_t crc16Update(uint16_t crc, const uint8_t *data, size_t length)
    {
        if (length == 0)
            return crc;

        return static_cast<uint16_t>(~esp_rom_crc16_be(static_cast<uint16_t>(~crc), data, length));
    }


    size_t buildFrame(
        uint8_t *out,
        uint16_t nonce,
        uint8_t type,
        uint8_t sequence,
        const uint8_t *payload,
        uint16_t length)
    {
        out[0] = FRAME_MAGIC_1;
        out[1] = FRAME_MAGIC_2;
        out[2] = nonce & 0xFF;
        out[3] = (nonce >> 8) & 0xFF;
        out[4] = length & 0xFF;
        out[5] = (length >> 8) & 0xFF;
        out[6] = type;
        out[7] = sequence;

        if (length > 0)
            memcpy(out + FRAME_HEADER_SIZE, payload, length);

        uint16_t crc = crc16Update(CRC_INIT, out + 2, FRAME_HEADER_SIZE - 2 + length);
        out[FRAME_HEADER_SIZE + length] = crc & 0xFF;
        out[FRAME_HEADER_SIZE + length + 1] = (crc >> 8) & 0xFF;

        return FRAME_HEADER_SIZE + length + FRAME_CRC_SIZE;
    }


    void resetDecoder()
    {
        decoder.state = DecodeState::Magic1;
        decoder.payloadIndex = 0;
    }


    void resetParser()
    {
        textLineBuffer = "";
        huntLineBuffer = "";
        resetDecoder();
    }


    void queueLine(const String &reply)
    {
        String *linePtr = new String(reply);
        if (xQueueSend(lineQueue, &linePtr, 0) != pdTRUE)
            delete linePtr;
    }


    // Drops "[MCU] " chatter entirely; strips the "[COM] " tag off the rest.
    bool prepareLine(String &line)
    {
        if (line.startsWith("[MCU] "))
            return false;

        if (line.startsWith("[COM] "))
            line = line.substring(6);

        return true;
    }


    void feedTextByte(uint8_t b)
    {
        // \r or \n ends a line; empty lines (from \r\n) are dropped.
        if (b == '\r' || b == '\n')
        {
            String reply = textLineBuffer;
            textLineBuffer = "";

            if (!prepareLine(reply) || reply.length() == 0)
                return;

            const char *keyword = pendingReadyKeyword;
            if (keyword && reply.startsWith(keyword))
            {
                pendingReadyKeyword = nullptr;
                mode = Mode::AwaitingFrames;
                resetDecoder();
            }

            queueLine(reply);
            return;
        }

        if (textLineBuffer.length() < MAX_LINE_LENGTH)
            textLineBuffer += static_cast<char>(b);
    }


    void feedDecoderByte(uint8_t b)
    {
        switch (decoder.state)
        {
            case DecodeState::Magic1:
                if (b == FRAME_MAGIC_1)
                {
                    decoder.state = DecodeState::Magic2;
                    return;
                }

                // A text failure report while frames are expected (e.g. "put fail:
                // crc") means the RT4K is back in text mode.
                if (b == '\r' || b == '\n')
                {
                    String line = huntLineBuffer;
                    huntLineBuffer = "";

                    if (prepareLine(line) && line.startsWith("put ") && line != "put done")
                    {
                        transferFailed = true;
                        mode = Mode::AwaitingLines;
                        queueLine(line);
                    }
                }
                else if (b >= 0x20 && b <= 0x7e && huntLineBuffer.length() < MAX_HUNT_LINE_LENGTH)
                {
                    huntLineBuffer += static_cast<char>(b);
                }
                return;

            case DecodeState::Magic2:
                if (b == FRAME_MAGIC_2)
                    decoder.state = DecodeState::NonceLow;
                else if (b != FRAME_MAGIC_1)
                    decoder.state = DecodeState::Magic1;
                return;

            case DecodeState::NonceLow:
                decoder.nonce = b;
                decoder.state = DecodeState::NonceHigh;
                return;

            case DecodeState::NonceHigh:
                decoder.nonce |= static_cast<uint16_t>(b) << 8;
                decoder.state = DecodeState::LengthLow;
                return;

            case DecodeState::LengthLow:
                decoder.length = b;
                decoder.state = DecodeState::LengthHigh;
                return;

            case DecodeState::LengthHigh:
                decoder.length |= static_cast<uint16_t>(b) << 8;
                if (decoder.length > MAX_PAYLOAD)
                {
                    decoder.state = (b == FRAME_MAGIC_1) ? DecodeState::Magic2 : DecodeState::Magic1;
                    return;
                }
                decoder.state = DecodeState::Type;
                return;

            case DecodeState::Type:
                decoder.type = b;
                decoder.state = DecodeState::Sequence;
                return;

            case DecodeState::Sequence:
                decoder.sequence = b;
                decoder.payloadIndex = 0;
                decoder.state = (decoder.length == 0) ? DecodeState::CrcLow : DecodeState::Payload;
                return;

            case DecodeState::Payload:
                decoder.payload[decoder.payloadIndex++] = b;
                if (decoder.payloadIndex == decoder.length)
                    decoder.state = DecodeState::CrcLow;
                return;

            case DecodeState::CrcLow:
                decoder.receivedCrc = b;
                decoder.state = DecodeState::CrcHigh;
                return;

            case DecodeState::CrcHigh:
            {
                decoder.receivedCrc |= static_cast<uint16_t>(b) << 8;

                const uint8_t header[] = {
                    static_cast<uint8_t>(decoder.nonce & 0xFF),
                    static_cast<uint8_t>((decoder.nonce >> 8) & 0xFF),
                    static_cast<uint8_t>(decoder.length & 0xFF),
                    static_cast<uint8_t>((decoder.length >> 8) & 0xFF),
                    decoder.type,
                    decoder.sequence,
                };

                uint16_t crc = crc16Update(CRC_INIT, header, sizeof(header));
                crc = crc16Update(crc, decoder.payload, decoder.length);

                auto *frame = new Frame();
                frame->nonce = decoder.nonce;
                frame->type = decoder.type;
                frame->sequence = decoder.sequence;
                frame->crcValid = (crc == decoder.receivedCrc);
                frame->payload.assign(decoder.payload, decoder.payload + decoder.length);

                resetDecoder();

                // Back to line mode now, so a trailing "get done" is read as text.
                if (frame->type == FRAME_RESPONSE || frame->type == FRAME_NAK || frame->type == FRAME_ABORT)
                    mode = Mode::AwaitingLines;

                if (frame->type == FRAME_ACK && frame->sequence == finalAckSequence)
                {
                    finalAckSequence = -1;
                    mode = Mode::AwaitingLines;
                }

                if (xQueueSend(frameQueue, &frame, 0) != pdTRUE)
                    delete frame;
                return;
            }
        }
    }


    // Index just past "key=" where key starts the line or follows a space,
    // or -1.
    int findField(const String &line, const char *key)
    {
        String prefix = String(key) + "=";
        for (int at = line.indexOf(prefix); at >= 0; at = line.indexOf(prefix, at + 1))
        {
            if (at == 0 || line.charAt(at - 1) == ' ')
                return at + static_cast<int>(prefix.length());
        }
        return -1;
    }
}


namespace Rt4kProtocol
{

std::atomic<Mode> mode{Mode::AwaitingLines};
std::atomic<const char *> pendingReadyKeyword{nullptr};
std::atomic<int> finalAckSequence{-1};
std::atomic<bool> transferFailed{false};
SemaphoreHandle_t opMutex = nullptr;


void begin()
{
    lineQueue = xQueueCreate(LINE_QUEUE_LEN, sizeof(String *));
    frameQueue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(Frame *));
    opMutex = xSemaphoreCreateMutex();

    if (!lineQueue || !frameQueue || !opMutex)
        Serial.println("ERROR: RT4K link queues unavailable");
}


void feed(const uint8_t *data, size_t length, std::vector<uint8_t> *textOut)
{
    for (size_t i = 0; i < length; i++)
    {
        if (parserResetRequested.load() && parserResetRequested.exchange(false))
            resetParser();

        uint8_t b = data[i];

        if (mode == Mode::AwaitingFrames)
        {
            feedDecoderByte(b);
            continue;
        }

        // Terminal mirror: printable ASCII and whitespace only.
        if (textOut && (b == '\t' || b == '\r' || b == '\n' || (b >= 0x20 && b <= 0x7e)))
            textOut->push_back(b);
        feedTextByte(b);
    }
}


bool sendCommand(const String &command)
{
    String cmd = command + "\r\n";
    return SerialBridge::sendRaw(
        reinterpret_cast<const uint8_t *>(cmd.c_str()),
        cmd.length()
    );
}


bool sendFrame(
    uint16_t nonce,
    uint8_t type,
    uint8_t sequence,
    const uint8_t *payload,
    uint16_t length)
{
    if (length > MAX_PAYLOAD)
        return false;

    uint8_t buf[FRAME_HEADER_SIZE + MAX_PAYLOAD + FRAME_CRC_SIZE];
    size_t total = buildFrame(buf, nonce, type, sequence, payload, length);
    return SerialBridge::sendRaw(buf, total);
}


void drainQueues()
{
    String *linePtr = nullptr;
    while (xQueueReceive(lineQueue, &linePtr, 0) == pdTRUE)
        delete linePtr;

    Frame *framePtr = nullptr;
    while (xQueueReceive(frameQueue, &framePtr, 0) == pdTRUE)
        delete framePtr;

    mode = Mode::AwaitingLines;
    pendingReadyKeyword = nullptr;
    finalAckSequence = -1;
    transferFailed = false;
    parserResetRequested = true;
}


bool waitForLine(TickType_t ticks, String &outLine)
{
    String *linePtr = nullptr;
    if (xQueueReceive(lineQueue, &linePtr, ticks) != pdTRUE)
        return false;

    std::unique_ptr<String> owned(linePtr);
    outLine = std::move(*owned);
    return true;
}


bool waitForFrame(TickType_t ticks, FramePtr &outFrame)
{
    Frame *framePtr = nullptr;
    if (xQueueReceive(frameQueue, &framePtr, ticks) != pdTRUE)
        return false;

    outFrame.reset(framePtr);
    return true;
}


String stripSlashes(const String &path)
{
    int start = 0;
    int end = path.length();
    while (start < end && path.charAt(start) == '/')
        start++;
    while (end > start && path.charAt(end - 1) == '/')
        end--;
    return path.substring(start, end);
}


String fieldValue(const String &line, const char *key)
{
    int start = findField(line, key);
    if (start < 0)
        return "";

    int end = line.indexOf(' ', start);
    return line.substring(start, end < 0 ? line.length() : end);
}


String fieldRest(const String &line, const char *key)
{
    int start = findField(line, key);
    return start < 0 ? String() : line.substring(start);
}


bool parseUnsigned(const String &text, uint32_t &out, int base)
{
    // strtoul would also take leading spaces and a sign.
    if (text.length() == 0 || !isxdigit(static_cast<unsigned char>(text.charAt(0))))
        return false;

    char *end = nullptr;
    errno = 0;
    unsigned long value = strtoul(text.c_str(), &end, base);
    if (errno != 0 || *end != '\0')
        return false;

    out = value;
    return true;
}


bool parseNonce(const String &readyLine, uint16_t &out)
{
    // Base 16 also accepts the "0x" prefix.
    uint32_t value = 0;
    if (!parseUnsigned(fieldValue(readyLine, "nonce"), value, 16) || value > 0xFFFF)
        return false;

    out = static_cast<uint16_t>(value);
    return true;
}


String describeCapturedLines(const std::vector<String> &lines)
{
    if (lines.empty())
        return " (saw no reply lines at all)";

    String last = lines.back();
    if (last.length() > 120)
        last = last.substring(0, 120) + "...";

    return " (saw " + String(lines.size()) + " line(s); last: \"" + last + "\")";
}


void sha256(const uint8_t *data, size_t length, uint8_t digest[32])
{
    mbedtls_sha256(data, length, digest, 0);
}


String sha256Hex(const uint8_t *data, size_t length)
{
    uint8_t digest[32];
    sha256(data, length, digest);

    char hex[65];
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);

    return String(hex);
}

} // namespace Rt4kProtocol
