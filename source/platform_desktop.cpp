// Desktop (macOS/Linux) backend used for developing the UI without a console.
// Mouse emulates a single finger, ctrl+wheel emulates pinch, keyboard maps to buttons.
#ifndef __SWITCH__
#include <ifaddrs.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <SDL.h>
#include <sys/stat.h>

#include <sys/resource.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gl.h"
#include "nanovg.h"
#include "platform.hpp"
#include "netfs.hpp"

namespace platform {
namespace {
SDL_Window* g_window = nullptr;
SDL_GLContext g_ctx = nullptr;
bool g_quit = false;
float g_backlight = 1.0f;
bool g_mouseDown = false;

std::string homeDir() {
    const char* h = getenv("HOME");
    return h ? std::string(h) + "/" : std::string("./");
}
}  // namespace

bool init() {
    mkdir(dataDir().c_str(), 0777);
    return true;
}

void shutdown() {}

bool running() { return !g_quit; }

bool createWindow() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) return false;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("SwitchMX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) return false;
    g_ctx = SDL_GL_CreateContext(g_window);
    if (!g_ctx) return false;
    SDL_GL_SetSwapInterval(1);
    return true;
}

void destroyWindow() {
    if (g_ctx) SDL_GL_DeleteContext(g_ctx);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

void swapBuffers() { SDL_GL_SwapWindow(g_window); }

void drawableSize(int& w, int& h) { SDL_GL_GetDrawableSize(g_window, &w, &h); }

void* glProcAddress(const char* name) { return SDL_GL_GetProcAddress(name); }

void pollInput(RawInput& raw) {
    raw = RawInput{};
    int ww, wh;
    SDL_GetWindowSize(g_window, &ww, &wh);
    float scale = wh / 720.0f;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: g_quit = true; break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) g_mouseDown = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) g_mouseDown = false;
                break;
            case SDL_MOUSEWHEEL:
                if (SDL_GetModState() & (KMOD_CTRL | KMOD_GUI))
                    raw.pinchWheel += e.wheel.preciseY;
                else
                    raw.wheel += e.wheel.preciseY;
                break;
        }
    }
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    struct { SDL_Scancode sc; uint32_t btn; } map[] = {
        {SDL_SCANCODE_RETURN, BTN_A}, {SDL_SCANCODE_SPACE, BTN_A},      {SDL_SCANCODE_BACKSPACE, BTN_B},
        {SDL_SCANCODE_ESCAPE, BTN_B}, {SDL_SCANCODE_X, BTN_X},          {SDL_SCANCODE_Y, BTN_Y},
        {SDL_SCANCODE_Q, BTN_L},      {SDL_SCANCODE_E, BTN_R},          {SDL_SCANCODE_1, BTN_ZL},
        {SDL_SCANCODE_3, BTN_ZR},     {SDL_SCANCODE_P, BTN_PLUS},       {SDL_SCANCODE_TAB, BTN_PLUS},
        {SDL_SCANCODE_M, BTN_MINUS},  {SDL_SCANCODE_UP, BTN_UP},        {SDL_SCANCODE_DOWN, BTN_DOWN},
        {SDL_SCANCODE_LEFT, BTN_LEFT}, {SDL_SCANCODE_RIGHT, BTN_RIGHT}, {SDL_SCANCODE_W, BTN_UP},
        {SDL_SCANCODE_S, BTN_DOWN},   {SDL_SCANCODE_A, BTN_LEFT},       {SDL_SCANCODE_D, BTN_RIGHT},
    };
    for (auto& m : map)
        if (ks[m.sc]) raw.buttons |= m.btn;

    // Dev aid: SWITCHMX_SCRIPT="60:X,90:D,120:A" holds a button for a few frames at a frame number.
    // Letters: A B X Y L R l(ZL) r(ZR) P(+) M(-) U D < > (d-pad).
    static const char* script = getenv("SWITCHMX_SCRIPT");
    static int scriptFrame = 0;
    scriptFrame++;
    bool scriptTouch = false;
    float scriptX = 0, scriptY = 0;
    if (script) {
        for (const char* p = script; *p;) {
            int f = atoi(p);
            const char* colon = strchr(p, ':');
            if (!colon) break;
            if (scriptFrame >= f && scriptFrame < f + 3) {
                switch (colon[1]) {
                    case 'A': raw.buttons |= BTN_A; break;
                    case 'B': raw.buttons |= BTN_B; break;
                    case 'X': raw.buttons |= BTN_X; break;
                    case 'Y': raw.buttons |= BTN_Y; break;
                    case 'L': raw.buttons |= BTN_L; break;
                    case 'R': raw.buttons |= BTN_R; break;
                    case 'l': raw.buttons |= BTN_ZL; break;
                    case 'r': raw.buttons |= BTN_ZR; break;
                    case 'P': raw.buttons |= BTN_PLUS; break;
                    case 'M': raw.buttons |= BTN_MINUS; break;
                    case 'U': raw.buttons |= BTN_UP; break;
                    case 'D': raw.buttons |= BTN_DOWN; break;
                    case '<': raw.buttons |= BTN_LEFT; break;
                    case '>': raw.buttons |= BTN_RIGHT; break;
                    case 'T':  // T<x>/<y>: a finger tap at logical 1280x720 coordinates
                        scriptTouch = true;
                        scriptX = (float)atof(colon + 2);
                        if (const char* sl = strchr(colon, '/')) scriptY = (float)atof(sl + 1);
                        break;
                }
            }
            const char* comma = strchr(colon, ',');
            if (!comma) break;
            p = comma + 1;
        }
    }

    if (scriptTouch) {
        raw.touches = 1;
        raw.tx[0] = scriptX;
        raw.ty[0] = scriptY;
        raw.quit = g_quit;
        return;
    }
    int mx, my;
    Uint32 mb = SDL_GetMouseState(&mx, &my);
    if (g_mouseDown && (mb & SDL_BUTTON_LMASK)) {
        raw.touches = 1;
        raw.tx[0] = mx / scale;
        raw.ty[0] = my / scale;
        // Shift + drag emulates a two-finger pinch around the window centre.
        if (SDL_GetModState() & KMOD_SHIFT) {
            float cx = ww / scale * 0.5f, cy = 360;
            raw.touches = 2;
            raw.tx[1] = 2 * cx - raw.tx[0];
            raw.ty[1] = 2 * cy - raw.ty[0];
        }
    } else {
        g_mouseDown = false;
    }
    raw.quit = g_quit;
}

std::string rootDir() {
    const char* r = getenv("SWITCHMX_ROOT");
    return r ? std::string(r) : homeDir();
}

std::string dataDir() { return homeDir() + ".switchmx/"; }

std::string displayPath(const std::string& path) {
    if (netfs::isNetPath(path)) return netfs::displayPath(path);  // "Server name/Films/"
    std::string h = homeDir();
    if (getenv("SWITCHMX_ROOT") && path.rfind(getenv("SWITCHMX_ROOT"), 0) == 0)
        return "Root/" + path.substr(strlen(getenv("SWITCHMX_ROOT")));
    if (path.rfind(h, 0) == 0) return "Home/" + path.substr(h.size());
    return path;
}

std::string exportSubtitleFont(const std::string&) { return ""; }

std::vector<StorageRoot> storageRoots(bool withSpace) {
    std::vector<StorageRoot> out;
    StorageRoot home;
    home.path = rootDir();
    home.label = "Home";
    if (withSpace) home.detail = "Local disk";
    out.push_back(home);
    // Dev aid: SWITCHMX_FAKE_DRIVE=/path/ shows a second, removable drive.
    if (const char* fake = getenv("SWITCHMX_FAKE_DRIVE")) {
        StorageRoot r;
        r.path = fake;
        r.label = "Kingston DataTraveler";
        r.removable = true;
        if (withSpace) r.detail = "exFAT  ·  420 GB free of 931 GB";
        out.push_back(r);
    }
    return out;
}

bool drivesChanged() { return false; }

bool ejectDrive(const std::string&) { return false; }
int disconnectWirelessControllers() { return 0; }

std::string systemVersion() { return "desktop"; }
bool threadUsage(long long&, long long&) { return false; }
void abortForReport() {}
int setCpuClock(int) { return -1; }

bool systemCjkFont(int, const void*& data, size_t& size) {
    // Stands in for the console's font while testing: one font with all of Chinese, Japanese and Korean.
    static std::vector<char> font;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (FILE* f = fopen("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "rb")) {
            fseek(f, 0, SEEK_END);
            font.resize((size_t)ftell(f));
            fseek(f, 0, SEEK_SET);
            if (fread(font.data(), 1, font.size(), f) != font.size()) font.clear();
            fclose(f);
        }
    }
    if (font.empty()) return false;
    data = font.data();
    size = font.size();
    return true;
}

void loadFallbackFonts(NVGcontext* vg, const std::vector<int>& fontIds) {
    const char* fallbacks[] = {"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf",
                               "/System/Library/Fonts/Supplemental/Arial.ttf",
                               "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf"};
    for (auto p : fallbacks) {
        int id = nvgCreateFont(vg, "fallback", p);
        if (id < 0) continue;
        for (int f : fontIds)
            if (f >= 0) nvgAddFallbackFontId(vg, f, id);
        break;
    }
}

int batteryPercent() {
    int pct = -1;
    SDL_GetPowerInfo(nullptr, &pct);
    return pct;
}

bool charging() {
    // Dev aid: SWITCHMX_CHARGE_AT=<seconds> pretends the charger is plugged in that long after start.
    static const char* at = getenv("SWITCHMX_CHARGE_AT");
    if (at) {
        static double t0 = nowSeconds();
        return nowSeconds() - t0 > atof(at);
    }
    SDL_PowerState s = SDL_GetPowerInfo(nullptr, nullptr);
    return s == SDL_POWERSTATE_CHARGING || s == SDL_POWERSTATE_CHARGED;
}

std::string localAddress() {
    std::string out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a && out.empty(); a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)a->ifa_addr)->sin_addr, buf, sizeof buf);
        if (strncmp(buf, "127.", 4) != 0) out = buf;
    }
    freeifaddrs(list);
    return out;
}

bool isDocked() { return true; }
bool isAppletMode() { return false; }
bool supportsBacklight() { return false; }
float backlight() { return g_backlight; }
void setBacklight(float v) { g_backlight = v; }
bool g_screenOff = false;
void setScreenOff(bool off) { g_screenOff = off; }
bool screenOff() { return g_screenOff; }
int audioOutput() {
    // Dev aid: SWITCHMX_AUDIO_UNPLUG_AT=<s> pretends headphones were unplugged that long after start.
    static const char* at = getenv("SWITCHMX_AUDIO_UNPLUG_AT");
    if (!at) return 0;
    static double t0 = nowSeconds();
    return nowSeconds() - t0 > atof(at) ? 1 : 2;
}
void emergencyExit() { _exit(3); }
void onLeaving(std::function<void()>) {}
void screenOffTick() {}
void setMediaPlayback(bool) {}

bool keyboardFailed() { return false; }
unsigned long long fileSizeLimit(const std::string&) { return 0; }

bool keyboard(const std::string& header, const std::string& initial, std::string& out) {
    // Dev aid: scripted answers for automated UI tests ("first|second|...").
    static std::string scripted = getenv("SWITCHMX_KEYBOARD") ? getenv("SWITCHMX_KEYBOARD") : "";
    if (!scripted.empty()) {
        size_t bar = scripted.find('|');
        out = scripted.substr(0, bar);
        scripted = bar == std::string::npos ? "" : scripted.substr(bar + 1);
        return true;
    }
#ifdef __APPLE__
    auto esc = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '"' || c == '\\') r += '\\';
            if (c == '\'') { r += "'\\''"; continue; }
            r += c;
        }
        return r;
    };
    std::string cmd = "osascript -e 'text returned of (display dialog \"" + esc(header) +
                      "\" default answer \"" + esc(initial) + "\")' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[2048];
    std::string res;
    while (fgets(buf, sizeof buf, p)) res += buf;
    int rc = pclose(p);
    if (rc != 0) return false;
    while (!res.empty() && (res.back() == '\n' || res.back() == '\r')) res.pop_back();
    out = res;
    return true;
#else
    printf("%s [%s]: ", header.c_str(), initial.c_str());
    fflush(stdout);
    char buf[2048];
    if (!fgets(buf, sizeof buf, stdin)) return false;
    out = buf;
    while (!out.empty() && out.back() == '\n') out.pop_back();
    if (out.empty()) out = initial;
    return true;
#endif
}

int cpuLoad(float*, int) { return 0; }
std::string busyThreads() { return ""; }

bool memoryUsage(unsigned long long& used, unsigned long long& total) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return false;
#ifdef __APPLE__
    used = (unsigned long long)ru.ru_maxrss;  // bytes on macOS (peak)
#else
    used = (unsigned long long)ru.ru_maxrss * 1024;
#endif
    total = 0;
    return true;
}

void lowerThreadPriority() {}

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace platform
#endif
