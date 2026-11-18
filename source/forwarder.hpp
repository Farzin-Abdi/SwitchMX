// Installs a HOME Menu icon that starts this .nro. The icon's content is finished on the console with
// its own keys, so neither the keys nor a ready-made package ever ship with the app.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace forwarder {

constexpr uint64_t kTitleId = 0x0100534D58000000ull;

struct Keys {
    uint8_t header[32];
    uint8_t keyArea[16];
};
// False with `error` set when the file is missing or lacks the two keys.
bool loadKeys(const std::string& path, Keys& out, std::string& error);

struct Content {
    std::vector<uint8_t> data;
    uint8_t id[16];
    uint8_t hash[32];
    int type;  // NcmContentType: 0 meta, 1 program, 3 control
};
// In install order: program, control, meta. `legacy`: the launcher for firmware before 19.0.0.
std::vector<Content> build(const Keys& keys, bool legacy = false);
// True on firmware before 19.0.0, which needs the legacy launcher.
bool legacyFirmware();

bool supported();
bool installed();
// True when this process was started by the icon itself, which can't be removed while in use.
bool runningFromIcon();
// Where the keys are, or "" when they aren't on the SD card.
std::string findKeys();
bool install(std::string& error);
bool remove(std::string& error);

}  // namespace forwarder
