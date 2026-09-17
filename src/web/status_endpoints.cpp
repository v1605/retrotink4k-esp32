#include "status_endpoints.h"

#include <ArduinoJson.h>

#include "ota_endpoints.h"
#include "web_util.h"
#include "../rt4k/link.h"
#include "../serial_bridge.h"
#include "../wifi_manager.h"

namespace
{
    void handleStatus(AsyncWebServerRequest *request)
    {
        JsonDocument doc;
        doc["ftdi_connected"] = SerialBridge::isConnected();
        doc["baud"] = SerialBridge::getBaud();
        doc["wifi_ssid"] = WifiManager::getSSID();
        doc["wifi_ip"] = WifiManager::getIP().toString();
        doc["ota_supported"] = OtaEndpoints::isEnabled();

        request->send(200, "application/json", WebUtil::toJson(doc));
    }


    void handleSerialPost(AsyncWebServerRequest *request)
    {
        String value;
        if (!WebUtil::requireParam(request, "baud", true, value))
            return;

        uint32_t baud = value.toInt();

        if (baud != 2000000 && baud != 115200)
        {
            WebUtil::sendError(request, 400, "Unsupported baud rate");
            return;
        }

        SerialBridge::setBaud(baud);

        request->send(200, "application/json", WebUtil::jsonOk());
    }


    void handleCommand(AsyncWebServerRequest *request)
    {
        String cmd;
        if (!WebUtil::requireParam(request, "command", true, cmd))
            return;

        // Via Rt4kLink so it can't interleave with other RT4K operations.
        WebUtil::runDeferred(request, [cmd](AsyncWebServerRequest *request)
        {
            Rt4kLink::Result result = Rt4kLink::sendRawCommand(cmd);
            WebUtil::sendOkOrError(request, result.ok, result.error);
        });
    }
}


namespace StatusEndpoints
{

void registerRoutes(AsyncWebServer &server)
{
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/serial", HTTP_POST, handleSerialPost);
    server.on("/api/command", HTTP_POST, handleCommand);
}

} // namespace StatusEndpoints
