#include "wifi_endpoints.h"

#include <ArduinoJson.h>

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


    void handleWifiGet(AsyncWebServerRequest *request)
    {
        JsonDocument doc;
        doc["mode"] = WifiManager::getModeName();
        doc["ssid"] = WifiManager::getSSID();
        doc["ip"] = WifiManager::getIP().toString();
        doc["hostname"] = WifiManager::getHostname();
        writeScanResult(doc["scan"].to<JsonObject>(), WifiManager::getLastScanResult());

        request->send(200, "application/json", WebUtil::toJson(doc));
    }


    void handleWifiPost(AsyncWebServerRequest *request)
    {
        String ssid;
        if (!WebUtil::requireParam(request, "ssid", true, ssid))
            return;

        String password =
            request->hasParam("password", true)
                ? request->getParam("password", true)->value()
                : "";

        if (ssid.length() == 0)
        {
            WebUtil::sendError(request, 400, "ssid must not be empty");
            return;
        }

        request->send(200, "application/json", WebUtil::jsonOk());

        WifiManager::applyCredentialsAndRestart(ssid, password);
    }


    void handleWifiForget(AsyncWebServerRequest *request)
    {
        request->send(200, "application/json", WebUtil::jsonOk());

        WifiManager::forgetCredentialsAndRestart();
    }


    void handleHostnamePost(AsyncWebServerRequest *request)
    {
        String hostname;
        if (!WebUtil::requireParam(request, "hostname", true, hostname))
            return;

        if (!isValidHostname(hostname))
        {
            WebUtil::sendError(request, 400, "Invalid hostname (letters, digits, hyphens only, 1-32 chars)");
            return;
        }

        request->send(200, "application/json", WebUtil::jsonOk());

        WifiManager::applyHostnameAndRestart(hostname);
    }


    void handleWifiScan(AsyncWebServerRequest *request)
    {
        // In AP-only mode scanning can drop this connection; the result is
        // still cached for /api/wifi.
        WebUtil::runDeferred(request, [](AsyncWebServerRequest *request)
        {
            WifiManager::ScanResult scan = WifiManager::scanNetworks();
            request->send(200, "application/json", scanResultJson(scan));
        });
    }
}


namespace WifiEndpoints
{

void registerRoutes(AsyncWebServer &server)
{
    // exact(): a plain path would also match /api/wifi/scan and the rest.
    server.on(AsyncURIMatcher::exact("/api/wifi"), HTTP_GET, handleWifiGet);
    server.on(AsyncURIMatcher::exact("/api/wifi"), HTTP_POST, handleWifiPost);
    server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
    server.on("/api/wifi/hostname", HTTP_POST, handleHostnamePost);
    server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
}

} // namespace WifiEndpoints
