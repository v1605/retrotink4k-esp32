#include "web_server.h"

#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "firmware_endpoints.h"
#include "osd_endpoints.h"
#include "ota_endpoints.h"
#include "profile_endpoints.h"
#include "remote_endpoints.h"
#include "status_endpoints.h"
#include "terminal_endpoints.h"
#include "wifi_endpoints.h"

namespace
{
    AsyncWebServer server(80);
}


namespace WebServer
{

void begin()
{
    TerminalEndpoints::begin(server);
    RemoteEndpoints::begin(server);
    StatusEndpoints::registerRoutes(server);
    WifiEndpoints::registerRoutes(server);
    ProfileEndpoints::registerRoutes(server);
    OsdEndpoints::registerRoutes(server);
    OtaEndpoints::registerRoutes(server);
    FirmwareEndpoints::registerRoutes(server);

    server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html");

    server.begin();

    Serial.println("Async web server started");
}


void loop()
{
    TerminalEndpoints::cleanupClients();
    OsdEndpoints::cleanupClients();
    FirmwareEndpoints::cleanupClients();
}

}
