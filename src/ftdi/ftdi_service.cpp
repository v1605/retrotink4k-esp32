#include "ftdi_internal.h"

#include <cinttypes>

// The service task: runs device events, the periodic scan, error reports
// and RX forwarding to the data callback.

namespace
{
    using namespace Ftdi;

    constexpr size_t FORWARD_CHUNK_CAPACITY = 512;
    // Events get a turn between passes of a long RX stream.
    constexpr size_t FORWARD_CHUNKS_PER_PASS = 64;
    constexpr uint32_t SCAN_INTERVAL_MS = 1000;
    constexpr uint32_t REPORT_INTERVAL_MS = 1000;


    void handleEvents(uint32_t events)
    {
        if (events & EVENT_DEVICE_GONE)
            detach();

        if (events & EVENT_NEW_DEVICE)
        {
            uint8_t address = 0;
            while (xQueueReceive(newDevices, &address, 0) == pdTRUE)
                attach(address);
        }

        if (events & EVENT_RECONFIGURE)
            reconfigure();
        if (events & EVENT_RX_FAILED)
            recoverRx();
        if (events & EVENT_TX_FAILED)
            recoverTx();
    }


    void scanIfDue()
    {
        static uint32_t lastScanMs = 0;
        if (!openDevice && !transfersStuck && millis() - lastScanMs >= SCAN_INTERVAL_MS)
        {
            lastScanMs = millis();
            scanDevices();
        }
    }


    void reportErrors()
    {
        static uint32_t reportedDrops = 0;
        static uint32_t reportedLineErrors = 0;
        static uint32_t lastReportMs = 0;

        if (millis() - lastReportMs < REPORT_INTERVAL_MS)
            return;

        uint32_t drops = rxDropCount;
        uint32_t overruns = overrunCount;
        uint32_t framing = framingCount;
        uint32_t breaks = breakCount;
        uint32_t lineErrors = overruns + framing + breaks;

        if (drops != reportedDrops)
        {
            Serial.printf("WARNING: RX buffer full, %" PRIu32 " bytes dropped since boot\n", drops);
            reportedDrops = drops;
            lastReportMs = millis();
        }

        // Framing errors and breaks are normal while the RT4K powers up or down.
        if (lineErrors != reportedLineErrors)
        {
            Serial.printf(
                "FT232R line status since boot: %" PRIu32 " overrun, %" PRIu32 " framing, %" PRIu32 " break\n",
                overruns,
                framing,
                breaks
            );
            reportedLineErrors = lineErrors;
            lastReportMs = millis();
        }
    }


    // Returns true when the pass filled up and more data may be waiting.
    bool forwardRx()
    {
        size_t chunks = 0;
        for (; chunks < FORWARD_CHUNKS_PER_PASS; chunks++)
        {
            size_t length = 0;
            auto *chunk = static_cast<uint8_t *>(xRingbufferReceiveUpTo(rxRing, &length, 0, FORWARD_CHUNK_CAPACITY));
            if (!chunk)
                break;

            SerialBridge::DataCallback callback = dataCallback;
            if (callback)
                callback(chunk, length);

            vRingbufferReturnItem(rxRing, chunk);
        }
        return chunks == FORWARD_CHUNKS_PER_PASS;
    }
}


namespace Ftdi
{

void notifyService(uint32_t events)
{
    pendingEvents |= events;

    // Full means a wake is already pending.
    uint8_t token = 0;
    if (serviceWake)
        xQueueSend(serviceWake, &token, 0);
}


void serviceTaskMain(void * /*arg*/)
{
    constexpr TickType_t IDLE_TICKS = pdMS_TO_TICKS(SCAN_INTERVAL_MS);

    bool rxBacklog = false;
    for (;;)
    {
        // Wakes on RX data or an event; the timeout keeps the scan going.
        if (!rxBacklog)
            xQueueSelectFromSet(serviceSet, IDLE_TICKS);

        uint8_t token = 0;
        xQueueReceive(serviceWake, &token, 0);
        handleEvents(pendingEvents.exchange(0));

        scanIfDue();
        reportErrors();
        rxBacklog = forwardRx();
    }
}

} // namespace Ftdi
