#include <Arduino.h>
#include <LittleFS.h>

#include "net_watchdog.h"
#include "rt4k/link.h"
#include "serial_bridge.h"
#include "web/web_server.h"
#include "wifi_manager.h"

void setup()
{
    Serial.begin(115200);

    delay(500);

    Serial.println("ESP32-S3 USB Host");

    if (!LittleFS.begin(true))
        Serial.println("ERROR: LittleFS mount failed");

    WifiManager::begin();
    SerialBridge::begin();
    Rt4kLink::begin();
    WebServer::begin();

    Serial.println();
    Serial.println("Waiting for Retrotink...");
}


void loop()
{
    WebServer::loop();
    NetWatchdog::loop();

    static uint32_t lastMdnsRefresh = 0;
    if (millis() - lastMdnsRefresh >= 60000)
    {
        lastMdnsRefresh = millis();
        WifiManager::refreshMdns();
    }

    delay(1000);
}
