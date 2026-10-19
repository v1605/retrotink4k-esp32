#include "ota_endpoints.h"

#include "web_server.h"
#include "web_util.h"

#ifdef OTA_ENABLED

#include <Arduino.h>
#include <Update.h>

#include <algorithm>
#include <cstring>
#include <memory>

namespace
{
    constexpr char COMBINED_MAGIC[4] = {'R', 'T', '4', 'O'};
    constexpr uint8_t COMBINED_VERSION = 1;
    constexpr size_t COMBINED_HEADER_SIZE = 4 + 1 + 4 + 4;
    constexpr size_t RECEIVE_CHUNK_BYTES = 4096;

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

    // Writes one received chunk. A chunk can span segment boundaries, so each
    // phase takes its share and falls through. Returns an error, or "".
    String writeChunk(CombinedUpload &state, const uint8_t *data, size_t len, size_t total)
    {
        size_t offset = 0;

        if (state.phase == CombinedUpload::Phase::Header)
        {
            size_t take = std::min(COMBINED_HEADER_SIZE - state.headerFilled, len);
            memcpy(state.header + state.headerFilled, data, take);
            state.headerFilled += take;
            offset += take;

            if (state.headerFilled < COMBINED_HEADER_SIZE)
                return "";

            if (memcmp(state.header, COMBINED_MAGIC, 4) != 0 || state.header[4] != COMBINED_VERSION)
                return "Not a valid combined OTA file";

            state.filesystemLength = readLE32(state.header + 5);
            state.firmwareLength = readLE32(state.header + 9);

            uint64_t expectedTotal = static_cast<uint64_t>(COMBINED_HEADER_SIZE) +
                state.filesystemLength + state.firmwareLength;

            if (state.filesystemLength == 0 || state.firmwareLength == 0 || expectedTotal != total)
                return "Combined OTA file size mismatch";

            if (!Update.begin(state.filesystemLength, U_SPIFFS))
                return Update.errorString();

            state.phase = CombinedUpload::Phase::Filesystem;
        }

        if (state.phase == CombinedUpload::Phase::Filesystem)
        {
            size_t take = std::min<size_t>(state.filesystemLength - state.filesystemWritten, len - offset);

            if (take > 0 && Update.write(const_cast<uint8_t *>(data + offset), take) != take)
                return Update.errorString();

            state.filesystemWritten += take;
            offset += take;

            if (state.filesystemWritten < state.filesystemLength)
                return "";

            if (!Update.end(true))
                return Update.errorString();

            if (!Update.begin(state.firmwareLength, U_FLASH))
                return Update.errorString();

            state.phase = CombinedUpload::Phase::Firmware;
        }

        if (state.phase == CombinedUpload::Phase::Firmware)
        {
            size_t take = len - offset;

            if (take > 0 && Update.write(const_cast<uint8_t *>(data + offset), take) != take)
                return Update.errorString();

            state.firmwareWritten += take;
        }

        return "";
    }


    // The filesystem goes first and the firmware second, each into its own
    // Update session, streamed as the body arrives.
    void receiveCombined(httpd_req_t *req)
    {
        size_t total = req->content_len;

        // Only application/octet-stream, as before this server; scripts and
        // the web UI set it.
        if (!WebUtil::isOctetStream(req))
        {
            WebUtil::sendError(req, 400, "Content-Type must be application/octet-stream");
            WebUtil::closeConnection(req);
            return;
        }

        if (total <= COMBINED_HEADER_SIZE)
        {
            WebUtil::sendError(req, 400, "Invalid upload size");
            WebUtil::closeConnection(req);
            return;
        }

        CombinedUpload state;
        std::unique_ptr<uint8_t[]> chunk(new uint8_t[RECEIVE_CHUNK_BYTES]);
        size_t received = 0;

        while (received < total)
        {
            size_t want = std::min(RECEIVE_CHUNK_BYTES, total - received);
            if (!WebUtil::readBody(req, chunk.get(), want))
            {
                if (state.phase != CombinedUpload::Phase::Header)
                    Update.abort();
                return;
            }
            received += want;

            String error = writeChunk(state, chunk.get(), want, total);
            if (error.length())
            {
                bool rejected = state.phase == CombinedUpload::Phase::Header;
                if (!rejected)
                    Update.abort();
                WebUtil::sendError(req, rejected ? 400 : 500, error);
                WebUtil::closeConnection(req);
                return;
            }
        }

        bool ok = Update.end(true);
        WebUtil::sendOkOrError(req, ok, ok ? "" : Update.errorString());

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


    esp_err_t handleCombinedUpload(httpd_req_t *req)
    {
        return WebUtil::runDeferred(req, receiveCombined);
    }
}


namespace OtaEndpoints
{

void registerRoutes()
{
    WebServer::on("/api/ota/upload", HTTP_POST, handleCombinedUpload);
}


bool isEnabled()
{
    return true;
}

} // namespace OtaEndpoints

#else

namespace OtaEndpoints
{

void registerRoutes()
{
}


bool isEnabled()
{
    return false;
}

} // namespace OtaEndpoints

#endif
