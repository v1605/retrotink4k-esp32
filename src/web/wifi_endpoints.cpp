#include "wifi_endpoints.h"

#include <ArduinoJson.h>

#include "web_server.h"
#include "web_util.h"
#include "../wifi_manager.h"

namespace
{
    bool isValidHostname(const String &hostname)
    {
        if (hostname.length() == 0 || hostname.length() > 32)
            return false;

        for (size_t i = 0; i < hostname.length(); i++)
        {
            char c = hostname.charAt(i);
            if (!isalnum(static_cast<unsigned char>(c)) && c != '-')
                return false;
        }

        return hostname.charAt(0) != '-' && hostname.charAt(hostname.length() - 1) != '-';
    }


    void writeScanResult(JsonObject target, const WifiManager::ScanResult &scan)
    {
        target["ok"] = scan.ok;

        if (!scan.ok)
        {
            target["error"] = scan.error;
            return;
        }

        JsonArray networks = target["networks"].to<JsonArray>();

        for (const auto &network : scan.networks)
        {
            JsonObject n = networks.add<JsonObject>();
            n["ssid"] = network.ssid;
            n["rssi"] = network.rssi;
            n["secure"] = network.secure;
        }
    }


    String scanResultJson(const WifiManager::ScanResult &scan)
    {
        JsonDocument doc;
        writeScanResult(doc.to<JsonObject>(), scan);
        return WebUtil::toJson(doc);
    }


    esp_err_t handleWifiGet(httpd_req_t *req)
    {
        JsonDocument doc;
        doc["mode"] = WifiManager::getModeName();
        doc["ssid"] = WifiManager::getSSID();
        doc["ip"] = WifiManager::getIP().toString();
        doc["hostname"] = WifiManager::getHostname();
        writeScanResult(doc["scan"].to<JsonObject>(), WifiManager::getLastScanResult());

        return WebUtil::sendJson(req, 200, WebUtil::toJson(doc));
    }


    esp_err_t handleWifiPost(httpd_req_t *req)
    {
        WebUtil::Params form;
        String ssid;
        if (!WebUtil::readForm(req, form) || !WebUtil::requireParam(req, form, "ssid", ssid))
            return ESP_OK;

        String password = form.get("password");

        if (ssid.length() == 0)
            return WebUtil::sendError(req, 400, "ssid must not be empty");

        WebUtil::sendJson(req, 200, WebUtil::jsonOk());

        WifiManager::applyCredentialsAndRestart(ssid, password);
        return ESP_OK;
    }


    esp_err_t handleWifiForget(httpd_req_t *req)
    {
        WebUtil::sendJson(req, 200, WebUtil::jsonOk());

        WifiManager::forgetCredentialsAndRestart();
        return ESP_OK;
    }


    esp_err_t handleHostnamePost(httpd_req_t *req)
    {
        WebUtil::Params form;
        String hostname;
        if (!WebUtil::readForm(req, form) || !WebUtil::requireParam(req, form, "hostname", hostname))
            return ESP_OK;

        if (!isValidHostname(hostname))
            return WebUtil::sendError(req, 400, "Invalid hostname (letters, digits, hyphens only, 1-32 chars)");

        WebUtil::sendJson(req, 200, WebUtil::jsonOk());

        WifiManager::applyHostnameAndRestart(hostname);
        return ESP_OK;
    }


    esp_err_t handleWifiScan(httpd_req_t *req)
    {
        // In AP-only mode scanning can drop this connection; the result is
        // still cached for /api/wifi.
        return WebUtil::runDeferred(req, [](httpd_req_t *req)
        {
            WifiManager::ScanResult scan = WifiManager::scanNetworks();
            WebUtil::sendJson(req, 200, scanResultJson(scan));
        });
    }
}


namespace WifiEndpoints
{

void registerRoutes()
{
    WebServer::on("/api/wifi", HTTP_GET, handleWifiGet);
    WebServer::on("/api/wifi", HTTP_POST, handleWifiPost);
    WebServer::on("/api/wifi/forget", HTTP_POST, handleWifiForget);
    WebServer::on("/api/wifi/hostname", HTTP_POST, handleHostnamePost);
    WebServer::on("/api/wifi/scan", HTTP_GET, handleWifiScan);
}

} // namespace WifiEndpoints
