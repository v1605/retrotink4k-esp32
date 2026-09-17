#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#include <vector>


namespace WifiManager
{
    enum class Mode
    {
        STATION,
        ACCESS_POINT,
    };

    struct ScannedNetwork
    {
        String ssid;
        int32_t rssi = 0;
        bool secure = false;
    };

    struct ScanResult
    {
        bool ok = false;
        String error;
        std::vector<ScannedNetwork> networks;
    };

    void begin();

    Mode getMode();
    String getModeName(); // "station" or "access point"
    IPAddress getIP();
    String getSSID();

    // Hostname applied at boot (DHCP and mDNS: http://<hostname>.local).
    String getHostname();

    // Saves new station credentials and reboots to apply them.
    void applyCredentialsAndRestart(
        const String &ssid,
        const String &password);

    // Clears saved credentials and reboots into access point mode.
    void forgetCredentialsAndRestart();

    // Saves a new hostname and reboots to apply it.
    void applyHostnameAndRestart(const String &hostname);

    // Scans and caches the result. In AP-only mode this can drop clients
    // connected to the AP.
    ScanResult scanNetworks();

    // Result of the last scanNetworks().
    ScanResult getLastScanResult();
}
