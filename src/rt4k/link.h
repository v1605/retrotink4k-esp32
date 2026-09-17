#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

// RT4K console and file-transfer API. Calls block and are serialized by a mutex.

namespace Rt4kLink
{
    // Largest file uploadFile() sends or downloadFile() accepts.
    constexpr size_t MAX_FILE_BYTES = 8 * 1024 * 1024;

    struct Profile
    {
        bool loaded = false;
        String path; // valid only when loaded is true
    };

    struct Entry
    {
        String name;
        bool isDirectory = false;
        uint32_t size = 0;
    };

    struct Result
    {
        bool ok = false;
        String error;
    };

    struct OsdFont
    {
        std::vector<uint8_t> data; // 4096 bytes: 256 glyphs x 16 bytes, 8x16 1bpp, row-major
    };

    struct OsdPlane
    {
        bool available = false; // false if the RT4K currently has nothing to show
        uint32_t on = 0;        // plane 2 only
        uint32_t osk = 0;       // plane 2 only -- on-screen keyboard visible
        uint32_t rows = 0;
        uint32_t width = 0;
        uint32_t stride = 0;
        uint32_t cells = 0;
        std::vector<uint8_t> text;  // 2048 bytes: character code per cell
        std::vector<uint8_t> color; // 2048 bytes: attribute byte per cell
    };

    struct DeviceInfo
    {
        int modelId = -1;
        String model;   // e.g. "RT4K_Pro"
        String version; // running firmware, e.g. "1.80.1"
    };

    // Reports bytes handed to the link so far; the last call has sent == total.
    using ProgressCallback = std::function<void(size_t sent, size_t total)>;

    struct FirmwareUpdate
    {
        String version; // of the rt4kup.bin on the SD card
        String token;   // hand back to startFirmwareUpdate()
    };

    struct BannerInfo
    {
        bool present = false;
        String path; // SD-root-relative, including the filename
        String file; // last segment of path
    };

    // Call once from setup(), after SerialBridge::begin().
    void begin();

    // Feeds FT232R bytes to the parser. textOut, if given, receives the bytes
    // parsed as text rather than as binary frames.
    void feed(const uint8_t *data, size_t length, std::vector<uint8_t> *textOut = nullptr);

    // Sends one line without waiting for a reply; serialized via opMutex.
    Result sendRawCommand(const String &text);

    // Sends "remote <button>" and waits briefly for the RT4K to confirm it.
    Result sendRemoteButton(const String &button);

    Result getLoadedProfile(Profile &out);
    Result loadProfile(const String &name);
    Result deleteFile(const String &path);
    Result listProfiles(const String &path, std::vector<Entry> &out);
    Result downloadFile(const String &path, std::vector<uint8_t> &out);
    Result uploadFile(const String &path, const uint8_t *data, size_t length, const ProgressCallback &onProgress = nullptr);

    // OSD snapshot: the font plus per-plane character and attribute buffers
    // (plane 1 = OSD, 2 = menu).
    Result downloadOsdFont(OsdFont &out);
    Result downloadOsdPlane(int plane, OsdPlane &out); // plane must be 1 or 2

    // "fwup check" reports the SD card's rt4kup.bin version and a token;
    // "fwup go <token>" flashes it and reboots the RT4K.
    Result checkFirmwareUpdate(FirmwareUpdate &out);
    Result startFirmwareUpdate(const String &token);

    // From the "model" and "ver" replies.
    Result getDeviceInfo(DeviceInfo &out);

    Result getBannerInfo(BannerInfo &out);

    // Banner info plus its bytes in one locked operation. Skips the download
    // (unchanged = true) when the RT4K reports knownPath.
    Result downloadBanner(BannerInfo &info, std::vector<uint8_t> &out, const String &knownPath, bool &unchanged);
}
