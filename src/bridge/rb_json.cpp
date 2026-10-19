#include "rb_internal.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "../serial_bridge.h"
#include "../rt4k/link.h"
#include "../web/web_server.h"
#include "../wifi_manager.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

namespace
{
    using namespace RetroBridge;

    constexpr uint32_t FLOW_CONTROL_BAUD = 2000000;
    constexpr uint16_t FTDI_VID = 0x0403;
    constexpr uint16_t FT232R_PID = 0x6001;
    constexpr uint32_t STATUS_CACHE_MS = 250;

    // Also while disconnected, from the baud it will use.
    bool flowControlExpected()
    {
        if (SerialBridge::isConnected())
            return SerialBridge::hardwareFlowControlEnabled();
        return SerialBridge::getBaud() == FLOW_CONTROL_BAUD;
    }


    String deviceId()
    {
        uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);

        char hex[13];
        snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return String(hex);
    }


    String buildInfo(bool flowControl)
    {
        JsonDocument doc;
        doc["name"] = "Retro-Bridge";
        doc["firmware"] = String("tinkesp32-") + FIRMWARE_VERSION;
        doc["api_version"] = 1;
        doc["hostname"] = WifiManager::getHostname();
        doc["device_id"] = deviceId();

        JsonArray caps = doc["capabilities"].to<JsonArray>();
        for (const char *cap : {"transactions", "binary_leases", "no_ack_transfers", "lease_write_wait",
                 "queued_lease_cancel", "lease_cursor_64", "http_keep_alive", "rt4k_wake",
                 "rt4k_wake_readiness_probe", "usb_preferred", "mdns"})
            caps.add(cap);
        if (flowControl)
            caps.add("ftdi_rts_cts");

        JsonObject serial = doc["serial"].to<JsonObject>();
        serial["transport"] = "automatic";
        serial["preferred_transport"] = "usb_ftdi";
        serial["available_transports"].to<JsonArray>().add("usb_ftdi");
        serial["maximum_baud_configurable"] = false;
        serial["build_maximum_baud"] = 2000000;
        JsonArray bauds = serial["maximum_baud_options"].to<JsonArray>();
        bauds.add(115200);
        bauds.add(2000000);
        serial["upload_throttle_configurable"] = false;
        serial["upload_throttle_baud_minimum"] = 0;
        serial["upload_throttle_baud_maximum"] = 0;
        serial["upload_throttle_baud_off"] = 0;

        JsonObject limits = doc["limits"].to<JsonObject>();
        limits["clients"] = MAX_CLIENTS;
        limits["queue_per_client"] = QUEUE_PER_CLIENT;
        limits["queued_jobs"] = QUEUED_JOBS;
        limits["transaction_bytes"] = TRANSACTION_BYTES;
        limits["response_bytes"] = RESPONSE_BYTES;
        limits["lease_receive_bytes"] = LEASE_RECEIVE_BYTES;
        limits["lease_read_bytes"] = LEASE_READ_BYTES;
        limits["lease_write_bytes"] = LEASE_WRITE_BYTES;
        limits["lease_transmit_bytes"] = LEASE_TRANSMIT_BYTES;
        limits["lease_write_wait_ms"] = LEASE_WRITE_WAIT_MS;
        limits["lease_lifetime_maximum_ms"] = LEASE_LIFETIME_MAX_MS;
        limits["http_keep_alive_idle_ms"] = WebServer::KEEP_ALIVE_IDLE_MS;
        limits["http_connections"] = WebServer::MAX_OPEN_SOCKETS;
        limits["raw_tcp_port"] = 0;
        limits["raw_tcp_connections"] = 0;

        String out;
        serializeJson(doc, out);
        return out;
    }


    size_t openConnections()
    {
        httpd_handle_t server = WebServer::handle();
        if (!server)
            return 0;

        size_t count = CONFIG_LWIP_MAX_SOCKETS;
        int fds[CONFIG_LWIP_MAX_SOCKETS];
        if (httpd_get_client_list(server, &count, fds) != ESP_OK)
            return 0;
        return count;
    }


    String buildStatus()
    {
        size_t clients = 0;
        size_t queued = 0;
        bool leaseActive = false;
        uint32_t txErrors = 0;
        {
            Guard guard;
            for (const BridgeClient &client : state.clients)
            {
                if (client.id != 0)
                    clients++;
            }
            queued = queuedJobs();
            leaseActive = state.owner == Owner::Lease;
            txErrors = state.txErrors;
        }

        String owner = Rt4kLink::currentOwner();
        bool connected = SerialBridge::isConnected();
        const char *serialState = connected ? "ready" : "disconnected";
        uint32_t baud = SerialBridge::getBaud();
        bool flowControl = SerialBridge::hardwareFlowControlEnabled();
        bool station = WifiManager::getMode() == WifiManager::Mode::STATION;

        JsonDocument doc;
        JsonObject wifi = doc["wifi"].to<JsonObject>();
        wifi["mode"] = station ? "station" : "provisioning";
        wifi["connected"] = station && WiFi.isConnected();
        wifi["ssid"] = WifiManager::getSSID();
        wifi["ip"] = WifiManager::getIP().toString();
        wifi["hostname"] = WifiManager::getHostname();
        wifi["rssi"] = station ? WiFi.RSSI() : 0;

        JsonObject serial = doc["serial"].to<JsonObject>();
        serial["transport"] = "usb_ftdi";
        serial["state"] = serialState;
        serial["baud"] = baud;
        serial["hardware_flow_control"] = flowControl;
        serial["errors"] = txErrors;
        serial["rx_overflows"] = SerialBridge::droppedRxBytes();

        JsonObject links = doc["serial_links"].to<JsonObject>();
        links["preferred"] = "usb_ftdi";
        links["active"] = "usb_ftdi";
        links["enabled"] = true;
        links["parallel_available"] = false;
        JsonObject usbLink = links["usb"].to<JsonObject>();
        usbLink["enabled"] = true;
        usbLink["state"] = serialState;
        usbLink["selected"] = true;
        usbLink["ready"] = connected;
        usbLink["owner"] = owner;
        usbLink["baud"] = baud;
        usbLink["hardware_flow_control"] = flowControl;
        JsonObject hd15 = links["hd15"].to<JsonObject>();
        hd15["enabled"] = false;
        hd15["state"] = "disconnected";
        hd15["selected"] = false;
        hd15["ready"] = false;
        hd15["owner"] = "";

        JsonObject usb = doc["usb"].to<JsonObject>();
        usb["state"] = serialState;
        usb["vid"] = connected ? FTDI_VID : 0;
        usb["pid"] = connected ? FT232R_PID : 0;
        usb["baud"] = baud;
        usb["hardware_flow_control"] = flowControl;

        // Without RTS/CTS, streamed transfers can overrun.
        if (!flowControlExpected())
        {
            JsonObject bulk = doc["bulk_transfer"].to<JsonObject>();
            bulk["recommended_mode"] = "acknowledged";
            bulk["streaming_reliable"] = false;
        }

        JsonObject scheduler = doc["scheduler"].to<JsonObject>();
        scheduler["clients"] = clients;
        scheduler["queued"] = queued;
        scheduler["owner"] = owner;
        scheduler["owner_transport"] = owner.length() > 0 ? "usb_ftdi" : "";
        scheduler["secondary_owner"] = "";
        scheduler["secondary_owner_transport"] = "";
        scheduler["binary_transfer_active"] = leaseActive;
        scheduler["maintenance_active"] = false;

        JsonObject http = doc["http"].to<JsonObject>();
        http["connections"] = openConnections();
        http["maximum_connections"] = WebServer::MAX_OPEN_SOCKETS;

        String out;
        serializeJson(doc, out);
        return out;
    }
}


namespace RetroBridge
{

// HTTP server task only.
String infoJson()
{
    static String cached[2];
    bool flowControl = flowControlExpected();
    String &info = cached[flowControl ? 1 : 0];
    if (info.length() == 0)
        info = buildInfo(flowControl);
    return info;
}


// HTTP server task only.
String statusJson()
{
    static String cached;
    static uint32_t builtMs = 0;

    if (cached.length() == 0 || millis() - builtMs >= STATUS_CACHE_MS)
    {
        cached = buildStatus();
        builtMs = millis();
    }
    return cached;
}


String leaseJson(const Lease &lease, bool full)
{
    JsonDocument doc;
    doc["lease_id"] = lease.leaseId;
    doc["state"] = lease.state == LeaseState::Active ? "active" : "queued";
    doc["mode"] = lease.mode == LeaseMode::NoAck ? "no_ack" : "exclusive";
    doc["transport"] = "usb_ftdi";
    doc["automatic_release"] = true;

    if (full)
    {
        bool blocked = lease.state == LeaseState::Queued;
        uint32_t blockingClient = 0;
        uint32_t blockingId = 0;
        const char *reason = "";

        if (blocked)
        {
            reason = "serial_busy";
            if (state.owner == Owner::Lease)
            {
                reason = "serial_reserved";
                blockingClient = state.lease->clientId;
                blockingId = state.lease->leaseId;
            }
            else if (state.owner == Owner::Transaction)
            {
                blockingClient = state.txn->clientId;
                blockingId = state.txn->requestId;
            }
        }

        doc["bridge_paced_download"] = false;
        doc["blocked"] = blocked;
        doc["wait_reason"] = reason;
        doc["blocking_client_id"] = blockingClient;
        doc["blocking_id"] = blockingId;
    }

    String out;
    serializeJson(doc, out);
    return out;
}


String wakeJson(const Wake &wake, const char *wakeState, uint32_t completedMs)
{
    bool ready = strcmp(wakeState, "ready") == 0;

    JsonDocument doc;
    doc["generation"] = wake.generation;
    doc["state"] = wakeState;
    doc["started_ms"] = wake.startedMs;
    doc["completed_ms"] = completedMs;
    doc["timeout_ms"] = wake.timeoutMs;
    doc["acknowledged"] = wake.acknowledged;
    doc["acknowledged_transport"] = wake.acknowledged ? "usb_ftdi" : "none";
    doc["ready_transport"] = ready ? "usb_ftdi" : "none";
    doc["ready_baud"] = ready ? SerialBridge::getBaud() : 0;
    if (wake.response.length() > 0)
        doc["rt4k_response"] = wake.response;
    else
        doc["rt4k_response"] = nullptr;

    JsonObject usb = doc["usb"].to<JsonObject>();
    usb["state"] = ready ? "ready" : (wake.everConnected ? "connected" : "disconnected");
    usb["wake_commands"] = wake.wakeCommands;
    usb["probes"] = wake.probes;
    usb["acknowledged"] = wake.acknowledged;

    JsonObject hd15 = doc["hd15"].to<JsonObject>();
    hd15["state"] = "disabled";
    hd15["wake_commands"] = 0;
    hd15["probes"] = 0;
    hd15["acknowledged"] = false;

    String out;
    serializeJson(doc, out);
    return out;
}

} // namespace RetroBridge
