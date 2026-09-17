#include "config.h"

#include <Preferences.h>

namespace
{
    constexpr char WIFI_NAMESPACE[] = "wifi";
    constexpr char KEY_SSID[] = "ssid";
    constexpr char KEY_PASSWORD[] = "password";
    constexpr char KEY_HOSTNAME[] = "hostname";

    constexpr char SERIAL_NAMESPACE[] = "serial";
    constexpr char KEY_BAUD[] = "baud";
}

namespace Config
{

WifiCredentials loadWifiCredentials()
{
    WifiCredentials creds;

    Preferences prefs;
    prefs.begin(WIFI_NAMESPACE, true);

    creds.ssid = prefs.getString(KEY_SSID, "");
    creds.password = prefs.getString(KEY_PASSWORD, "");

    prefs.end();

    creds.valid = creds.ssid.length() > 0;

    return creds;
}


void saveWifiCredentials(
    const String &ssid,
    const String &password)
{
    Preferences prefs;
    prefs.begin(WIFI_NAMESPACE, false);

    prefs.putString(KEY_SSID, ssid);
    prefs.putString(KEY_PASSWORD, password);

    prefs.end();
}


void clearWifiCredentials()
{
    Preferences prefs;
    prefs.begin(WIFI_NAMESPACE, false);

    prefs.clear();

    prefs.end();
}


uint32_t loadSerialBaud()
{
    Preferences prefs;
    prefs.begin(SERIAL_NAMESPACE, true);

    uint32_t baud = prefs.getUInt(KEY_BAUD, DEFAULT_SERIAL_BAUD);

    prefs.end();

    return baud;
}


void saveSerialBaud(uint32_t baud)
{
    Preferences prefs;
    prefs.begin(SERIAL_NAMESPACE, false);

    prefs.putUInt(KEY_BAUD, baud);

    prefs.end();
}


String loadHostname()
{
    Preferences prefs;
    prefs.begin(WIFI_NAMESPACE, true);

    String hostname = prefs.getString(KEY_HOSTNAME, DEFAULT_HOSTNAME);

    prefs.end();

    return hostname;
}


void saveHostname(const String &hostname)
{
    Preferences prefs;
    prefs.begin(WIFI_NAMESPACE, false);

    prefs.putString(KEY_HOSTNAME, hostname);

    prefs.end();
}

} // namespace Config
