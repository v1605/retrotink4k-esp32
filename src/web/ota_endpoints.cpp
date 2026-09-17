#include "ota_endpoints.h"

#include "web_util.h"

#ifdef OTA_ENABLED

#include <Arduino.h>
#include <Update.h>

#include <algorithm>
#include <cstring>

namespace
{
    constexpr char COMBINED_MAGIC[4] = {'R', 'T', '4', 'O'};
    constexpr uint8_t COMBINED_VERSION = 1;
    constexpr size_t COMBINED_HEADER_SIZE = 4 + 1 + 4 + 4;

    uint32_t readLE32(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) |
            (static_cast<uint32_t>(p[1]) << 8) |
            (static_cast<uint32_t>(p[2]) << 16) |
            (static_cast<uint32_t>(p[3]) << 24);
    }

    struct CombinedUpload
    {
        enum class Phase { Header, Filesystem, Firmware };

        Phase phase = Phase::Header;
        uint8_t header[COMBINED_HEADER_SIZE];
        size_t headerFilled = 0;
        uint32_t filesystemLength = 0;
        uint32_t firmwareLength = 0;
        uint32_t filesystemWritten = 0;
        uint32_t firmwareWritten = 0;
    };

    // Drops an unfinished upload and any Update session it opened.
    void abandonCombined(AsyncWebServerRequest *request)
    {
        auto *state = static_cast<CombinedUpload *>(request->_tempObject);
        if (!state)
            return;

        if (state->phase != CombinedUpload::Phase::Header)
            Update.abort();

        delete state;
        request->_tempObject = nullptr;
    }

    void failCombined(AsyncWebServerRequest *request, const String &msg)
    {
        abandonCombined(request);
        WebUtil::sendError(request, 500, msg);
    }

    void rejectCombined(AsyncWebServerRequest *request, const String &msg)
    {
        abandonCombined(request);
        WebUtil::sendError(request, 400, msg);
    }

    // Streams each segment into its own Update session. A chunk can span
    // segment boundaries, so each phase takes its share and falls through.
    void handleCombinedUploadBody(
        AsyncWebServerRequest *request,
        uint8_t *data,
        size_t len,
        size_t index,
        size_t total)
    {
        auto *state = static_cast<CombinedUpload *>(request->_tempObject);

        if (!state)
        {
            // Rejected or failed with an earlier chunk.
            if (index > 0)
                return;

            // Only application/octet-stream. Form-encoded bodies never reach
            // this handler: ESPAsyncWebServer buffers them into a String and
            // runs out of memory, so clients must set the header.
            if (request->contentType() != "application/octet-stream")
            {
                WebUtil::sendError(request, 400, "Content-Type must be application/octet-stream");
                return;
            }

            if (total <= COMBINED_HEADER_SIZE)
            {
                WebUtil::sendError(request, 400, "Invalid upload size");
                return;
            }

            state = new CombinedUpload();
            request->_tempObject = state;

            // The library free()s a leftover _tempObject, which would skip
            // Update.abort().
            request->onDisconnect([request]() { abandonCombined(request); });
        }

        size_t offset = 0;

        if (state->phase == CombinedUpload::Phase::Header)
        {
            size_t take = std::min(COMBINED_HEADER_SIZE - state->headerFilled, len);
            memcpy(state->header + state->headerFilled, data, take);
            state->headerFilled += take;
            offset += take;

            if (state->headerFilled < COMBINED_HEADER_SIZE)
                return;

            if (memcmp(state->header, COMBINED_MAGIC, 4) != 0 || state->header[4] != COMBINED_VERSION)
            {
                rejectCombined(request, "Not a valid combined OTA file");
                return;
            }

            state->filesystemLength = readLE32(state->header + 5);
            state->firmwareLength = readLE32(state->header + 9);

            uint64_t expectedTotal = static_cast<uint64_t>(COMBINED_HEADER_SIZE) +
                state->filesystemLength + state->firmwareLength;

            if (state->filesystemLength == 0 || state->firmwareLength == 0 || expectedTotal != total)
            {
                rejectCombined(request, "Combined OTA file size mismatch");
                return;
            }

            if (!Update.begin(state->filesystemLength, U_SPIFFS))
            {
                failCombined(request, Update.errorString());
                return;
            }

            state->phase = CombinedUpload::Phase::Filesystem;
        }

        if (state->phase == CombinedUpload::Phase::Filesystem)
        {
            size_t take = std::min<size_t>(state->filesystemLength - state->filesystemWritten, len - offset);

            if (take > 0 && Update.write(data + offset, take) != take)
            {
                failCombined(request, Update.errorString());
                return;
            }

            state->filesystemWritten += take;
            offset += take;

            if (state->filesystemWritten < state->filesystemLength)
                return;

            if (!Update.end(true))
            {
                failCombined(request, Update.errorString());
                return;
            }

            if (!Update.begin(state->firmwareLength, U_FLASH))
            {
                failCombined(request, Update.errorString());
                return;
            }

            state->phase = CombinedUpload::Phase::Firmware;
        }

        if (state->phase == CombinedUpload::Phase::Firmware)
        {
            size_t take = len - offset;

            if (take > 0 && Update.write(data + offset, take) != take)
            {
                failCombined(request, Update.errorString());
                return;
            }

            state->firmwareWritten += take;
        }

        if (index + len < total)
            return;

        delete state;
        request->_tempObject = nullptr;

        bool ok = Update.end(true);
        WebUtil::sendOkOrError(request, ok, ok ? "" : Update.errorString());

        if (!ok)
            return;

        // Delayed so the response goes out first.
        bool started = WebUtil::runInBackground([]()
        {
            delay(1000);
            ESP.restart();
        });

        if (!started)
            Serial.println("OTA update applied; restart the device to boot it");
    }
}


namespace OtaEndpoints
{

void registerRoutes(AsyncWebServer &server)
{
    server.on(
        "/api/ota/upload",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        nullptr,
        handleCombinedUploadBody
    );
}


bool isEnabled()
{
    return true;
}

} // namespace OtaEndpoints

#else

namespace OtaEndpoints
{

void registerRoutes(AsyncWebServer & /*server*/)
{
}


bool isEnabled()
{
    return false;
}

} // namespace OtaEndpoints

#endif
