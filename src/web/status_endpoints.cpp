#include "status_endpoints.h"

#include <ArduinoJson.h>

#include "ota_endpoints.h"
#include "web_server.h"
#include "web_util.h"
#include "../bridge/retro_bridge.h"
#include "../rt4k/link.h"
#include "../ftdi/serial_bridge.h"
#include "../wifi_manager.h"

namespace
{
    esp_err_t handleStatus(httpd_req_t *req)
    {
        JsonDocument doc;
        doc["ftdi_connected"] = SerialBridge::isConnected();
        doc["baud"] = SerialBridge::getBaud();
        doc["rx_dropped"] = SerialBridge::droppedRxBytes();
        doc["rx_overruns"] = SerialBridge::rxOverruns();
        doc["rx_framing_errors"] = SerialBridge::rxFramingErrors();
        doc["rx_breaks"] = SerialBridge::rxBreaks();
        doc["usb_status"] = SerialBridge::usbStatus();
        doc["wifi_ssid"] = WifiManager::getSSID();
        doc["wifi_ip"] = WifiManager::getIP().toString();
        doc["ota_supported"] = OtaEndpoints::isEnabled();

        JsonObject bridge = doc["retro_bridge"].to<JsonObject>();
        bridge["clients"] = RetroBridge::clientCount();
        bridge["owner"] = RetroBridge::ownerLabel();

        return WebUtil::sendJson(req, 200, WebUtil::toJson(doc));
    }


    esp_err_t handleSerialPost(httpd_req_t *req)
    {
        WebUtil::Params form;
        String value;
        if (!WebUtil::readForm(req, form) || !WebUtil::requireParam(req, form, "baud", value))
            return ESP_OK;

        uint32_t baud = value.toInt();

        if (baud != 2000000 && baud != 115200)
            return WebUtil::sendError(req, 400, "Unsupported baud rate");

        SerialBridge::setBaud(baud);

        return WebUtil::sendJson(req, 200, WebUtil::jsonOk());
    }


    esp_err_t handleCommand(httpd_req_t *req)
    {
        WebUtil::Params form;
        String cmd;
        if (!WebUtil::readForm(req, form) || !WebUtil::requireParam(req, form, "command", cmd))
            return ESP_OK;

        // Via Rt4kLink so it can't interleave with other RT4K operations.
        return WebUtil::runDeferred(req, [cmd](httpd_req_t *req)
        {
            Rt4kLink::Result result = Rt4kLink::sendRawCommand(cmd);
            WebUtil::sendOkOrError(req, result.ok, result.error);
        });
    }
}


namespace StatusEndpoints
{

void registerRoutes()
{
    WebServer::on("/api/status", HTTP_GET, handleStatus);
    WebServer::on("/api/serial", HTTP_POST, handleSerialPost);
    WebServer::on("/api/command", HTTP_POST, handleCommand);
}

} // namespace StatusEndpoints
