#include "net_watchdog.h"

#include <WiFi.h>
#include <lwip/ip_addr.h>
#include <ping/ping_sock.h>

#include <atomic>

#include "wifi_manager.h"

namespace
{
    constexpr uint32_t PING_INTERVAL_MS = 5000;
    constexpr uint32_t PING_TIMEOUT_MS = 2000;
    constexpr uint32_t RECONNECT_AFTER_MS = 30000;
    constexpr uint32_t RESTART_AFTER_MS = 180000;

    esp_ping_handle_t session = nullptr;
    std::atomic<uint32_t> lastReplyMs{0};
    std::atomic<bool> armed{false};
    uint32_t lastReconnectMs = 0;

    void onReply(esp_ping_handle_t, void *)
    {
        lastReplyMs = millis();
        armed = true;
    }

    void startPing()
    {
        IPAddress gateway = WiFi.gatewayIP();
        if (gateway == IPAddress(0, 0, 0, 0))
            return;

        esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
        IP_ADDR4(&config.target_addr, gateway[0], gateway[1], gateway[2], gateway[3]);
        config.count = ESP_PING_COUNT_INFINITE;
        config.interval_ms = PING_INTERVAL_MS;
        config.timeout_ms = PING_TIMEOUT_MS;

        esp_ping_callbacks_t callbacks = {};
        callbacks.on_ping_success = onReply;

        if (esp_ping_new_session(&config, &callbacks, &session) != ESP_OK)
        {
            session = nullptr;
            Serial.println("Network watchdog: couldn't start pinging the gateway");
            return;
        }

        esp_ping_start(session);
        Serial.printf("Network watchdog: pinging gateway %s\n", gateway.toString().c_str());
    }
}


namespace NetWatchdog
{

void loop()
{
    if (WifiManager::getMode() != WifiManager::Mode::STATION)
        return;

    if (!session)
    {
        if (WiFi.isConnected())
            startPing();
        return;
    }

    if (!armed)
        return;

    uint32_t now = millis();
    uint32_t silentMs = now - lastReplyMs;

    if (silentMs >= RESTART_AFTER_MS)
    {
        Serial.println("Network watchdog: no gateway reply for 3 minutes, restarting");
        delay(100);
        ESP.restart();
    }

    if (silentMs >= RECONNECT_AFTER_MS && now - lastReconnectMs >= RECONNECT_AFTER_MS)
    {
        Serial.println("Network watchdog: no gateway reply, reconnecting WiFi");
        lastReconnectMs = now;
        WiFi.reconnect();
    }
}

} // namespace NetWatchdog
