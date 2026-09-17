#include "profile_endpoints.h"

#include <ArduinoJson.h>

#include <memory>
#include <vector>

#include "web_util.h"
#include "../rt4k/link.h"

namespace
{
    // Every path here is relative to this folder on the SD card.
    constexpr char PROFILE_ROOT[] = "profile/";

    void handleProfileCurrent(AsyncWebServerRequest *request)
    {
        WebUtil::runDeferred(request, [](AsyncWebServerRequest *request)
        {
            Rt4kLink::Profile profile;
            Rt4kLink::Result result = Rt4kLink::getLoadedProfile(profile);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(request, false, result.error);
                return;
            }

            JsonDocument doc;
            doc["ok"] = true;
            doc["loaded"] = profile.loaded;

            if (profile.loaded)
                doc["path"] = profile.path;

            request->send(200, "application/json", WebUtil::toJson(doc));
        });
    }


    void handleProfileList(AsyncWebServerRequest *request)
    {
        String path = WebUtil::paramOr(request, "path", "profile");

        WebUtil::runDeferred(request, [path](AsyncWebServerRequest *request)
        {
            std::vector<Rt4kLink::Entry> entries;
            Rt4kLink::Result result = Rt4kLink::listProfiles(path, entries);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(request, false, result.error);
                return;
            }

            JsonDocument doc;
            doc["ok"] = true;
            JsonArray jsonEntries = doc["entries"].to<JsonArray>();

            for (const auto &entry : entries)
            {
                JsonObject e = jsonEntries.add<JsonObject>();
                e["name"] = entry.name;
                e["isDirectory"] = entry.isDirectory;
                e["size"] = entry.size;
            }

            request->send(200, "application/json", WebUtil::toJson(doc));
        });
    }


    // Loading replaces the live settings; there's no separate reset.
    void handleProfileLoad(AsyncWebServerRequest *request)
    {
        String name;
        if (!WebUtil::requireParam(request, "name", true, name))
            return;

        WebUtil::runDeferred(request, [name](AsyncWebServerRequest *request)
        {
            Rt4kLink::Result result = Rt4kLink::loadProfile(name);
            WebUtil::sendOkOrError(request, result.ok, result.error);
        });
    }


    void handleProfileDelete(AsyncWebServerRequest *request)
    {
        String name;
        if (!WebUtil::requireParam(request, "name", true, name))
            return;

        WebUtil::runDeferred(request, [name](AsyncWebServerRequest *request)
        {
            Rt4kLink::Result result = Rt4kLink::deleteFile(PROFILE_ROOT + name);
            WebUtil::sendOkOrError(request, result.ok, result.error);
        });
    }


    void handleProfileDownload(AsyncWebServerRequest *request)
    {
        String name;
        if (!WebUtil::requireParam(request, "name", false, name))
            return;

        WebUtil::runDeferred(request, [name](AsyncWebServerRequest *request)
        {
            std::vector<uint8_t> data;
            Rt4kLink::Result result = Rt4kLink::downloadFile(PROFILE_ROOT + name, data);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(request, false, result.error);
                return;
            }

            int slash = name.lastIndexOf('/');
            String baseName = slash >= 0 ? name.substring(slash + 1) : name;

            AsyncResponseStream *response =
                request->beginResponseStream("application/octet-stream");

            response->addHeader(
                "Content-Disposition",
                "attachment; filename=\"" + baseName + "\""
            );

            response->write(data.data(), data.size());

            request->send(response);
        });
    }


    // Body handler: called repeatedly with chunks until index + len == total.
    void handleProfileUploadBody(
        AsyncWebServerRequest *request,
        uint8_t *data,
        size_t len,
        size_t index,
        size_t total)
    {
        std::shared_ptr<std::vector<uint8_t>> body =
            WebUtil::collectBody(request, data, len, index, total, Rt4kLink::MAX_FILE_BYTES);
        if (!body)
            return;

        String name;
        if (!WebUtil::requireParam(request, "name", false, name))
            return;

        // Not runDeferred(): the write completes even if the client disconnects.
        AsyncWebServerRequestPtr weakRequest = request->pause();

        bool started = WebUtil::runInBackground([weakRequest, name, body]()
        {
            Rt4kLink::Result result = Rt4kLink::uploadFile(
                PROFILE_ROOT + name,
                body->data(),
                body->size()
            );

            auto paused = weakRequest.lock();
            if (paused)
                WebUtil::sendOkOrError(paused.get(), result.ok, result.error);
        });

        // send() resumes the paused request.
        if (!started)
            WebUtil::sendError(request, 503, "Out of memory");
    }
}


namespace ProfileEndpoints
{

void registerRoutes(AsyncWebServer &server)
{
    server.on("/api/profiles/current", HTTP_GET, handleProfileCurrent);
    // exact(): a plain path would also match /api/profiles/download.
    server.on(AsyncURIMatcher::exact("/api/profiles"), HTTP_GET, handleProfileList);
    server.on("/api/profiles/load", HTTP_POST, handleProfileLoad);
    server.on("/api/profiles/delete", HTTP_POST, handleProfileDelete);
    server.on("/api/profiles/download", HTTP_GET, handleProfileDownload);
    server.on(
        "/api/profiles/upload",
        HTTP_POST,
        [](AsyncWebServerRequest *request) {},
        nullptr,
        handleProfileUploadBody
    );
}

} // namespace ProfileEndpoints
