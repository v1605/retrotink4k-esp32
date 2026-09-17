#include "remote_endpoints.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <set>

#include "osd_endpoints.h"
#include "web_util.h"
#include "../rt4k/link.h"

namespace
{
    constexpr int QUEUE_LEN = 32;
    constexpr uint32_t WORKER_STACK_BYTES = 8192;

    // Names accepted by "remote <button>". "axu6" (AUX6) is the device's spelling.
    const std::set<String> VALID_BUTTONS = {
        "pwr", "menu", "up", "down", "left", "right", "ok", "back",
        "diag", "stat", "input", "output", "scaler", "sfx", "adc", "col", "aud",
        "prof", "prof1", "prof2", "prof3", "prof4", "prof5", "prof6",
        "prof7", "prof8", "prof9", "prof10", "prof11", "prof12",
        "gain", "phase", "pause", "safe", "genlock", "buffer",
        "res4k", "res1080p", "res1440p", "res480p", "res1", "res2", "res3", "res4",
        "aux1", "aux2", "aux3", "aux4", "aux5", "axu6", "aux7", "aux8",
    };

    QueueHandle_t buttonQueue = nullptr;

    // Runs every queued button press strictly one at a time, in order.
    void buttonWorkerTask(void * /*arg*/)
    {
        for (;;)
        {
            String *button = nullptr;
            if (xQueueReceive(buttonQueue, &button, portMAX_DELAY) != pdTRUE)
                continue;

            Rt4kLink::Result result = Rt4kLink::sendRemoteButton(*button);
            if (!result.ok)
                Serial.println("Remote button send failed: " + result.error);

            // OSD viewers refresh once the last queued press has gone out.
            if (uxQueueMessagesWaiting(buttonQueue) == 0)
                OsdEndpoints::notifyRemoteDone(*button, result.ok);

            delete button;
        }
    }


    void handleRemoteButton(AsyncWebServerRequest *request)
    {
        String button;
        if (!WebUtil::requireParam(request, "button", true, button))
            return;

        if (VALID_BUTTONS.find(button) == VALID_BUTTONS.end())
        {
            WebUtil::sendError(request, 400, "Unknown button: " + button);
            return;
        }

        auto *queued = new String(button);
        if (xQueueSend(buttonQueue, &queued, 0) != pdTRUE)
        {
            delete queued;
            WebUtil::sendError(request, 503, "Button queue full");
            return;
        }

        request->send(200, "application/json", WebUtil::jsonOk());
    }
}


namespace RemoteEndpoints
{

void begin(AsyncWebServer &server)
{
    buttonQueue = xQueueCreate(QUEUE_LEN, sizeof(String *));
    xTaskCreate(buttonWorkerTask, "rt4k_button_worker", WORKER_STACK_BYTES, nullptr, 1, nullptr);

    server.on("/api/remote", HTTP_POST, handleRemoteButton);
}

} // namespace RemoteEndpoints
