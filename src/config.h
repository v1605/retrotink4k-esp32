#pragma once

#include <Arduino.h>

// Persistent config (NVS-backed via Preferences); survives reflashes.

struct WifiCredentials
{
    String ssid;
    String password;
    bool valid = false;
};

// RT4K defaults to 2000000 baud on firmware >= 1.75.0, else 115200.
constexpr uint32_t DEFAULT_SERIAL_BAUD = 2000000;

// Used as both the fallback AP SSID and the default DHCP/mDNS hostname.
constexpr char DEFAULT_HOSTNAME[] = "TinkEsp32";

namespace Config
{
    WifiCredentials loadWifiCredentials();

    void saveWifiCredentials(
        const String &ssid,
        const String &password);

    void clearWifiCredentials();

    uint32_t loadSerialBaud();
    void saveSerialBaud(uint32_t baud);

    String loadHostname();
    void saveHostname(const String &hostname);
}
