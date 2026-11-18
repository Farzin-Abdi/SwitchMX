#pragma once
#include <functional>
#include <string>
#include <vector>

#include "input.hpp"

struct NVGcontext;

namespace platform {

bool init();
void shutdown();
bool running();  // appletMainLoop on Switch

bool createWindow();
void destroyWindow();
void swapBuffers();
void drawableSize(int& w, int& h);
void* glProcAddress(const char* name);

void pollInput(RawInput& raw);

// Filesystem roots
std::string rootDir();  // where the file browser starts ("sdmc:/" on Switch)
std::string dataDir();  // app data folder, with trailing slash
std::string displayPath(const std::string& path);  // pretty path for UI

struct StorageRoot {
    std::string path;   // e.g. "sdmc:/", "ums0:/"
    std::string label;  // "SD Card", drive product name
    std::string detail; // "exFAT · 420 GB free of 931 GB" (only with withSpace)
    bool removable = false;
};
std::vector<StorageRoot> storageRoots(bool withSpace);
bool drivesChanged();  // true once after a drive was plugged in or pulled out
bool ejectDrive(const std::string& path);

// Writes the system font into dir/name if missing (used for libass). Returns path or "".
std::string exportSubtitleFont(const std::string& dir);
// Adds system fonts as fallbacks to the "sans" and "sans-bold" faces.
// Adds the platform's own fonts (CJK and symbols on the Switch) as fallbacks behind each of the
// app's UI fonts, so any language still renders whichever font the user picked.
void loadFallbackFonts(NVGcontext* vg, const std::vector<int>& fontIds);
// The console's own font for Chinese (0 traditional, 1 simplified), Japanese (2) or Korean (3), for
// documents that don't carry their own. False where there is none (the desktop build).
bool systemCjkFont(int which, const void*& data, size_t& size);

// Device status
int batteryPercent();  // -1 when unknown
bool charging();
bool isAppletMode();
std::string systemVersion();  // "18.0.1 (Atmosphère)" on the console
bool isDocked();  // on a TV
bool supportsBacklight();
float backlight();  // 0..1
void setBacklight(float v);
// The screen's light off (music keeps playing) and back on. Off never outlasts the app being in
// front: screenOffTick() turns it back on if the HOME Menu or another applet takes over.
void setScreenOff(bool off);
bool screenOff();
void screenOffTick();
// Called (on the main thread) when the app goes to the background, e.g. the HOME button: it may be
// closed from there without warning, so this is the moment to save.
void onLeaving(std::function<void()> fn);
// Where sound goes: 0 unknown, 1 speaker, 2 headphones (wired), 3 TV, 4 USB audio, 5 Bluetooth.
// Read on a background thread; cheap to call every frame.
int audioOutput();
// Leaves at once, from any thread, when the app is stuck: the screen is lit again first.
[[noreturn]] void emergencyExit();
void setMediaPlayback(bool active);  // keeps the screen from dimming
std::string localAddress();          // this console's address on the network, "" when offline
int disconnectWirelessControllers();  // returns how many were turned off

// Returns false if cancelled or if the keyboard couldn't open; keyboardFailed() tells them apart.
bool keyboard(const std::string& header, const std::string& initial, std::string& out);
bool keyboardFailed();

// The largest file the drive holding `path` can store (4 GB - 1 on FAT32), or 0 for no known limit.
unsigned long long fileSizeLimit(const std::string& path);

double nowSeconds();

// For background work that must never compete with playback or the UI.
void lowerThreadPriority();

// Load of each CPU core (0..1) since the previous call; returns the number of cores, 0 if unknown.
int cpuLoad(float* out, int maxCores);

// Threads that used more than 5% of a core since the previous call (diagnostics).
std::string busyThreads();
// Threads the process has and may have (developer status); false where unknown.
bool threadUsage(long long& used, long long& limit);
// Ends the app with a crash report of every thread (developer mode); does nothing on the desktop.
void abortForReport();
// Sets the CPU clock in MHz (0: back to what the system had, below 0: only asks); returns the
// clock now, -1 if unknown.
int setCpuClock(int mhz);

// Process memory in bytes; false when unknown.
bool memoryUsage(unsigned long long& used, unsigned long long& total);
// A reserve set aside at start: when memory runs out it is given back instead of the app ending,
// and lowMemory() turns true so the player can let go of what it holds.
void installMemoryReserve();
bool lowMemory();
void clearLowMemory();
void rearmMemoryReserve();  // once memory is plentiful again
void releaseMemoryReserve();  // last resort (the crash handler, so it can still write its note)

}  // namespace platform
