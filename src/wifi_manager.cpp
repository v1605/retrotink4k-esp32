#include "wifi_manager.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <mdns.h>

#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

namespace
{
    // Fallback network
    constexpr char AP_SSID[] = "TinkEsp32";
    constexpr char AP_PASSWORD[] = "12345678";
    constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 15000;
    // From access point mode, a saved network is retried this often.
    constexpr uint32_t STA_RETRY_INTERVAL_MS = 60000;
    constexpr uint32_t MDNS_REFRESH_INTERVAL_MS = 60000;

    WifiManager::Mode currentMode = WifiManager::Mode::ACCESS_POINT;
    String currentHostname;
    WifiCredentials savedCreds;
    uint32_t lastMdnsRefreshMs = 0;
    uint32_t lastRetryMs = 0;
    bool retrying = false;
    WifiManager::ScanResult lastScan;
    SemaphoreHandle_t lastScanMutex = xSemaphoreCreateMutex();


    void setLastScan(const WifiManager::ScanResult &value)
    {
        xSemaphoreTake(lastScanMutex, portMAX_DELAY);
        lastScan = value;
        xSemaphoreGive(lastScanMutex);
    }


    WifiManager::ScanResult copyLastScan()
    {
        xSemaphoreTake(lastScanMutex, portMAX_DELAY);
        WifiManager::ScanResult copy = lastScan;
        xSemaphoreGive(lastScanMutex);
        return copy;
    }


    bool connectStation(
        const String &ssid,
        const String &password)
    {
        Serial.printf(
            "Connecting to WiFi \"%s\"...\n",
            ssid.c_str()
        );

        WiFi.setHostname(currentHostname.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.begin(ssid.c_str(), password.c_str());

        uint32_t start = millis();

        while (
            WiFi.status() != WL_CONNECTED &&
            millis() - start < STA_CONNECT_TIMEOUT_MS
        )
        {
            delay(250);
            Serial.print(".");
        }

        Serial.println();

        return WiFi.status() == WL_CONNECTED;
    }


    void startAccessPoint()
    {
        WiFi.setHostname(currentHostname.c_str());
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);

        currentMode = WifiManager::Mode::ACCESS_POINT;

        Serial.println();
        Serial.println("WiFi AP started (fallback)");
        Serial.printf("SSID: %s\n", AP_SSID);
        Serial.printf("Password: %s\n", AP_PASSWORD);
        Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
    }


    // Connects alongside the access point; AP clients may drop briefly while
    // the radio follows the station's channel.
    void startStationRetry()
    {
        Serial.printf("Retrying WiFi \"%s\" from access point mode...\n", savedCreds.ssid.c_str());
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(savedCreds.ssid.c_str(), savedCreds.password.c_str());
        retrying = true;
        lastRetryMs = millis();
    }


    void finishStationRetry(bool connected)
    {
        retrying = false;
        if (!connected)
        {
            WiFi.disconnect();
            WiFi.mode(WIFI_AP);
            Serial.println("WiFi retry failed, staying in access point mode");
            return;
        }

        WiFi.softAPdisconnect(true);
        currentMode = WifiManager::Mode::STATION;
        Serial.println();
        Serial.println("WiFi connected");
        Serial.printf("SSID: %s\n", savedCreds.ssid.c_str());
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        WifiManager::refreshMdns();
    }
}


namespace WifiManager
{

void refreshMdns()
{
    if (esp_netif_t *netif = WiFi.STA.netif())
        mdns_netif_action(netif, static_cast<mdns_event_actions_t>(MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ANNOUNCE_IP4));
}


void begin()
{
    currentHostname = Config::loadHostname();

    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { refreshMdns(); }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    savedCreds = Config::loadWifiCredentials();

    if (savedCreds.valid && connectStation(savedCreds.ssid, savedCreds.password))
    {
        currentMode = Mode::STATION;

        Serial.println();
        Serial.println("WiFi connected");
        Serial.printf("SSID: %s\n", savedCreds.ssid.c_str());
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    }
    else
    {
        if (savedCreds.valid)
        {
            Serial.println(
                "Failed to connect to saved WiFi network"
            );
        }
        scanNetworks();

        startAccessPoint();
        lastRetryMs = millis();
    }

    if (MDNS.begin(currentHostname.c_str()))
    {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS responder started: http://%s.local\n", currentHostname.c_str());
    }
    else
    {
        Serial.println("mDNS responder failed to start");
    }
}


void loop()
{
    uint32_t now = millis();

    if (currentMode == Mode::STATION)
    {
        if (now - lastMdnsRefreshMs >= MDNS_REFRESH_INTERVAL_MS)
        {
            lastMdnsRefreshMs = now;
            refreshMdns();
        }
        return;
    }

    if (!savedCreds.valid)
        return;

    if (retrying)
    {
        bool connected = WiFi.status() == WL_CONNECTED;
        if (connected || now - lastRetryMs >= STA_CONNECT_TIMEOUT_MS)
            finishStationRetry(connected);
    }
    else if (now - lastRetryMs >= STA_RETRY_INTERVAL_MS)
    {
        startStationRetry();
    }
}


Mode getMode()
{
    return currentMode;
}


String getModeName()
{
    return currentMode == Mode::STATION ? "station" : "access point";
}


String getHostname()
{
    return currentHostname;
}


IPAddress getIP()
{
    return currentMode == Mode::STATION
        ? WiFi.localIP()
        : WiFi.softAPIP();
}


String getSSID()
{
    return currentMode == Mode::STATION
        ? WiFi.SSID()
        : String(AP_SSID);
}


void applyCredentialsAndRestart(
    const String &ssid,
    const String &password)
{
    Config::saveWifiCredentials(ssid, password);

    Serial.println("WiFi credentials saved, restarting...");

    delay(500);
    ESP.restart();
}


void forgetCredentialsAndRestart()
{
    Config::clearWifiCredentials();

    Serial.println("WiFi credentials cleared, restarting...");

    delay(500);
    ESP.restart();
}


void applyHostnameAndRestart(const String &hostname)
{
    Config::saveHostname(hostname);

    Serial.println("Hostname saved, restarting...");

    delay(500);
    ESP.restart();
}


ScanResult scanNetworks()
{
    ScanResult result;

    // Scanning needs the station radio, which is off in AP-only mode.
    bool wasApOnly = (WiFi.getMode() == WIFI_AP);
    if (wasApOnly)
        WiFi.mode(WIFI_AP_STA);

    int count = WiFi.scanNetworks();

    if (count < 0)
    {
        result.ok = false;
        result.error = "WiFi.scanNetworks() failed (code " + String(count) + ")";
    }
    else
    {
        result.ok = true;

        for (int i = 0; i < count; i++)
        {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0)
                continue;

            int32_t rssi = WiFi.RSSI(i);
            bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

            auto existing = std::find_if(
                result.networks.begin(), result.networks.end(),
                [&](const ScannedNetwork &n) { return n.ssid == ssid; }
            );

            if (existing != result.networks.end())
            {
                if (rssi > existing->rssi)
                    *existing = {ssid, rssi, secure};
            }
            else
            {
                result.networks.push_back({ssid, rssi, secure});
            }
        }

        std::sort(
            result.networks.begin(), result.networks.end(),
            [](const ScannedNetwork &a, const ScannedNetwork &b) { return a.rssi > b.rssi; }
        );
    }

    WiFi.scanDelete();

    if (wasApOnly)
        WiFi.mode(WIFI_AP);

    setLastScan(result);
    return result;
}


ScanResult getLastScanResult()
{
    return copyLastScan();
}

} // namespace WifiManager
