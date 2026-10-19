#include "profile_endpoints.h"

#include <ArduinoJson.h>

#include <vector>

#include "web_server.h"
#include "web_util.h"
#include "../rt4k/link.h"

namespace
{
    // Every path here is relative to this folder on the SD card.
    constexpr char PROFILE_ROOT[] = "profile/";

    esp_err_t handleProfileCurrent(httpd_req_t *req)
    {
        return WebUtil::runDeferred(req, [](httpd_req_t *req)
        {
            Rt4kLink::Profile profile;
            Rt4kLink::Result result = Rt4kLink::getLoadedProfile(profile);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(req, false, result.error);
                return;
            }

            JsonDocument doc;
            doc["ok"] = true;
            doc["loaded"] = profile.loaded;

            if (profile.loaded)
                doc["path"] = profile.path;

            WebUtil::sendJson(req, 200, WebUtil::toJson(doc));
        });
    }


    esp_err_t handleProfileList(httpd_req_t *req)
    {
        String path = WebUtil::queryParams(req).get("path", "profile");

        return WebUtil::runDeferred(req, [path](httpd_req_t *req)
        {
            std::vector<Rt4kLink::Entry> entries;
            Rt4kLink::Result result = Rt4kLink::listProfiles(path, entries);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(req, false, result.error);
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

            WebUtil::sendJson(req, 200, WebUtil::toJson(doc));
        });
    }


    // Loading replaces the live settings; there's no separate reset.
    esp_err_t handleProfileLoad(httpd_req_t *req)
    {
        WebUtil::Params form;
        String name;
        if (!WebUtil::readForm(req, form) || !WebUtil::requireParam(req, form, "name", name))
            return ESP_OK;

        return WebUtil::runDeferred(req, [name](httpd_req_t *req)
        {
            Rt4kLink::Result result = Rt4kLink::loadProfile(name);
            WebUtil::sendOkOrError(req, result.ok, result.error);
        });
    }


    esp_err_t handleProfileDelete(httpd_req_t *req)
    {
        WebUtil::Params form;
        String name;
        if (!WebUtil::readForm(req, form) || !WebUtil::requireParam(req, form, "name", name))
            return ESP_OK;

        return WebUtil::runDeferred(req, [name](httpd_req_t *req)
        {
            Rt4kLink::Result result = Rt4kLink::deleteFile(PROFILE_ROOT + name);
            WebUtil::sendOkOrError(req, result.ok, result.error);
        });
    }


    esp_err_t handleProfileDownload(httpd_req_t *req)
    {
        String name;
        if (!WebUtil::requireParam(req, WebUtil::queryParams(req), "name", name))
            return ESP_OK;

        return WebUtil::runDeferred(req, [name](httpd_req_t *req)
        {
            std::vector<uint8_t> data;
            Rt4kLink::Result result = Rt4kLink::downloadFile(PROFILE_ROOT + name, data);

            if (!result.ok)
            {
                WebUtil::sendOkOrError(req, false, result.error);
                return;
            }

            int slash = name.lastIndexOf('/');
            String baseName = slash >= 0 ? name.substring(slash + 1) : name;

            WebUtil::Response(req)
                .header("Content-Disposition", "attachment; filename=\"" + baseName + "\"")
                .send(200, "application/octet-stream", data.data(), data.size());
        });
    }


    esp_err_t handleProfileUpload(httpd_req_t *req)
    {
        String name;
        if (!WebUtil::requireParam(req, WebUtil::queryParams(req), "name", name))
        {
            WebUtil::closeConnection(req);
            return ESP_OK;
        }

        // The body arrives on the worker, so a large upload doesn't hold up
        // the server.
        return WebUtil::runDeferred(req, [name](httpd_req_t *req)
        {
            size_t total = req->content_len;
            if (!WebUtil::isOctetStream(req) || total == 0 || total > Rt4kLink::MAX_FILE_BYTES)
            {
                WebUtil::sendError(req, 400, WebUtil::isOctetStream(req)
                    ? "Invalid upload size" : "Content-Type must be application/octet-stream");
                WebUtil::closeConnection(req);
                return;
            }

            std::vector<uint8_t> body(total);
            if (!WebUtil::readBody(req, body.data(), total))
                return;

            // Completes even if the client has gone by now.
            Rt4kLink::Result result = Rt4kLink::uploadFile(PROFILE_ROOT + name, body.data(), body.size());
            WebUtil::sendOkOrError(req, result.ok, result.error);
        });
    }
}


namespace ProfileEndpoints
{

void registerRoutes()
{
    WebServer::on("/api/profiles/current", HTTP_GET, handleProfileCurrent);
    WebServer::on("/api/profiles", HTTP_GET, handleProfileList);
    WebServer::on("/api/profiles/load", HTTP_POST, handleProfileLoad);
    WebServer::on("/api/profiles/delete", HTTP_POST, handleProfileDelete);
    WebServer::on("/api/profiles/download", HTTP_GET, handleProfileDownload);
    WebServer::on("/api/profiles/upload", HTTP_POST, handleProfileUpload);
}

} // namespace ProfileEndpoints
