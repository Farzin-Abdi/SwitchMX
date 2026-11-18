#ifdef __SWITCH__
#include <SDL.h>
#include <switch.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <usbhsfs.h>
#include <pthread.h>
#include <netdb.h>
#include <cerrno>
#include <new>
#include <string>

#include <atomic>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "gl.h"
#include "nanovg.h"
#include "platform.hpp"
#include "netfs.hpp"

static bool g_ssl = false;  // the system SSL service is open (see init())
extern "C" char* fake_heap_start;  // newlib's heap, as libnx set it up
extern "C" char* fake_heap_end;
extern "C" u32 __nx_applet_exit_mode;  // libnx: how the program ends (see shutdown())

// Linked with -Wl,--wrap=__syscall_thread_create. libnx creates every pthread with ideal core 0,
// so mpv's demuxer, decoder, audio and render threads all queue on one core first; give each
// new thread the next core in turn (it may still migrate to any allowed core).
extern "C" int __real___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg,
                                              void* stack_addr, size_t stack_size);
// Every created thread is remembered so per-thread CPU time can be logged for diagnostics.
namespace {
struct TrackedThread {
    Handle handle;
    uintptr_t func;
    u64 lastTicks;
};
TrackedThread g_threads[96];
int g_threadCount = 0;
}  // namespace

extern "C" int __wrap___syscall_thread_create(struct __pthread_t** thread, void* (*func)(void*), void* arg,
                                              void* stack_addr, size_t stack_size) {
    int rc = __real___syscall_thread_create(thread, func, arg, stack_addr, stack_size);
    if (rc != 0 || !thread || !*thread) return rc;
    if (g_threadCount < 96) g_threads[g_threadCount++] = {*(Handle*)(*thread), (uintptr_t)func, 0};
    static u64 mask = 0;
    static int next = 0;
    if (!mask && R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) return rc;
    for (int tries = 0; tries < 4; tries++) {
        int core = next++ % 4;
        if (mask & (1ull << core)) {
            // The pthread object starts with libnx's Thread, whose first field is the handle.
            Handle h = *(Handle*)(*thread);
            svcSetThreadCoreMask(h, core, (u32)mask);
            break;
        }
    }
    return rc;
}

// When the app crashes, write where to the SD card before Atmosphere takes over, so the
// address can be matched to SwitchMX.elf: offset = pc - (runtime address of platform::init - its ELF address).
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}
namespace platform {
bool init();
}
static bool readable(u64 addr) {
    MemoryInfo mi;
    u32 pi;
    return addr && !(addr & 7) && R_SUCCEEDED(svcQueryMemory(&mi, &pi, addr)) && (mi.perm & Perm_R);
}
extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    FILE* f = fopen("sdmc:/switch/SwitchMX/crash.txt", "w");
    if (!f) return;
    fprintf(f, "SwitchMX crash\nerror_desc=%u\npc=%016llx\nlr=%016llx\nsp=%016llx\nfp=%016llx\nref platform::init=%016llx\n",
            ctx->error_desc, (unsigned long long)ctx->pc.x, (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x,
            (unsigned long long)ctx->fp.x, (unsigned long long)(uintptr_t)&platform::init);
    u64 fp = ctx->fp.x;
    for (int i = 0; i < 32 && readable(fp) && readable(fp + 8); i++) {
        u64* frame = (u64*)fp;
        fprintf(f, "frame %d: %016llx\n", i, (unsigned long long)frame[1]);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    fclose(f);
}

namespace platform {
static void stopCpuSamplers();
namespace {
SDL_Window* g_window = nullptr;
SDL_GLContext g_ctx = nullptr;
PadState g_pad;
bool g_psm = false, g_lbl = false, g_pl = false;
AppletOperationMode g_mode = AppletOperationMode_Handheld;
bool g_quit = false;
bool g_usb = false;

void applyResolution() {
    g_mode = appletGetOperationMode();
    if (g_mode == AppletOperationMode_Console)
        SDL_SetWindowSize(g_window, 1920, 1080);
    else
        SDL_SetWindowSize(g_window, 1280, 720);
}
}  // namespace

bool init() {
    // Larger TCP buffers than the default: Wi-Fi transfers and streams run several times faster.
    // Sockets start small and grow (up to 1 MB) while a transfer is fast. They share one pool
    // (about sb_efficiency x 2 MB): HLS streams open many connections at once, and with large
    // starting buffers the pool ran out ("No buffer space available") after about 20.
    SocketInitConfig sc = *socketGetDefaultInitConfig();
    sc.tcp_tx_buf_size = 0x10000;
    sc.tcp_rx_buf_size = 0x20000;
    sc.tcp_tx_buf_max_size = 0x100000;
    sc.tcp_rx_buf_max_size = 0x100000;
    {
        u64 total = 0;  // twice the pool when the app has a game's memory
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        sc.sb_efficiency = total > (1ull << 30) ? 8 : 4;
    }
    sc.num_bsd_sessions = 6;  // the transfer server, its connections and mpv can block at once
    if (R_FAILED(socketInitialize(&sc))) socketInitializeDefault();
    // HTTPS (streams, update checks, subtitles online) goes through FFmpeg's TLS, which uses the
    // system's SSL service: without this every secure connection stalled, and with it the player.
    g_ssl = R_SUCCEEDED(sslInitialize(3));
#ifdef DEBUG_NXLINK
    nxlinkStdio();
#endif
    g_pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
    g_psm = R_SUCCEEDED(psmInitialize());
    g_lbl = R_SUCCEEDED(lblInitialize());

    g_usb = R_SUCCEEDED(usbHsFsInitialize(0));

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();

    mkdir(dataDir().c_str(), 0777);
    return true;
}

// The audio-output watcher (see audioOutput()); stopped in shutdown().
std::atomic<int> g_audioOut{0};
std::atomic<bool> g_audioQuit{false};
pthread_t g_audioThread;
bool g_audioStarted = false;

namespace {
u32 g_cpuDefaultHz = 0;  // what the system had before SwitchMX changed the CPU clock
bool g_cpuChanged = false;
}  // namespace

void shutdown() {
    // Started from the HOME Menu icon, SwitchMX runs as a game. A game must tell the system it is
    // closing; one that just ends is reported as "The software was closed because an error
    // occurred". Exit mode 1 makes libnx do that when the program ends.
    {
        u64 id = 0;
        if (R_SUCCEEDED(svcGetInfo(&id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) &&
            id == 0x0100534D58000000ULL)
            __nx_applet_exit_mode = 1;
    }
    if (g_cpuChanged) setCpuClock(0);  // the system's own clock again, before anything else runs
    g_audioQuit = true;
    if (g_audioStarted) pthread_join(g_audioThread, nullptr);
    g_audioStarted = false;
    stopCpuSamplers();
    setMediaPlayback(false);
    if (g_usb) usbHsFsExit();
    if (g_lbl) {
        setScreenOff(false);
        lblExit();
    }
    if (g_psm) psmExit();
    if (g_pl) plExit();
    if (g_ssl) sslExit();
    g_ssl = false;
    socketExit();
}

bool running() { return !g_quit && appletMainLoop(); }

bool createWindow() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) return false;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow("SwitchMX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g_window) return false;
    g_ctx = SDL_GL_CreateContext(g_window);
    if (!g_ctx) return false;
    gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress);
    SDL_GL_SetSwapInterval(1);
    applyResolution();
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
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_quit = true;
    }
    if (appletGetOperationMode() != g_mode) applyResolution();

    padUpdate(&g_pad);
    u64 k = padGetButtons(&g_pad);
    struct { u64 hid; uint32_t btn; } map[] = {
        {HidNpadButton_A, BTN_A},         {HidNpadButton_B, BTN_B},       {HidNpadButton_X, BTN_X},
        {HidNpadButton_Y, BTN_Y},         {HidNpadButton_L, BTN_L},       {HidNpadButton_R, BTN_R},
        {HidNpadButton_ZL, BTN_ZL},       {HidNpadButton_ZR, BTN_ZR},     {HidNpadButton_Plus, BTN_PLUS},
        {HidNpadButton_Minus, BTN_MINUS}, {HidNpadButton_Up, BTN_UP},     {HidNpadButton_Down, BTN_DOWN},
        {HidNpadButton_Left, BTN_LEFT},   {HidNpadButton_Right, BTN_RIGHT}, {HidNpadButton_StickL, BTN_LSTICK},
        {HidNpadButton_StickR, BTN_RSTICK},
    };
    for (auto& m : map)
        if (k & m.hid) raw.buttons |= m.btn;

    HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
    raw.lx = l.x / (float)JOYSTICK_MAX;
    raw.ly = l.y / (float)JOYSTICK_MAX;
    raw.rx = r.x / (float)JOYSTICK_MAX;
    raw.ry = r.y / (float)JOYSTICK_MAX;

    HidTouchScreenState ts = {};
    if (hidGetTouchScreenStates(&ts, 1)) {
        raw.touches = ts.count > 4 ? 4 : ts.count;
        for (int i = 0; i < raw.touches; i++) {
            raw.tx[i] = (float)ts.touches[i].x;
            raw.ty[i] = (float)ts.touches[i].y;
        }
    }
    raw.quit = g_quit;
}

std::string rootDir() { return "sdmc:/"; }
std::string dataDir() { return "sdmc:/switch/SwitchMX/"; }

std::string displayPath(const std::string& path) {
    if (netfs::isNetPath(path)) return netfs::displayPath(path);  // "Server name/Films/"
    if (path.rfind("sdmc:/", 0) == 0) return "SD Card/" + path.substr(6);
    size_t colon = path.find(":/");
    if (path.rfind("ums", 0) == 0 && colon != std::string::npos) return "USB/" + path.substr(colon + 2);
    return path;
}

static std::string spaceString(const char* path) {
    struct statvfs st;
    if (statvfs(path, &st) != 0 || !st.f_blocks) return "";
    char buf[64];
    double unit = st.f_frsize ? st.f_frsize : st.f_bsize;
    snprintf(buf, sizeof buf, "%.0f GB free of %.0f GB", st.f_bavail * unit / 1e9, st.f_blocks * unit / 1e9);
    return buf;
}

std::vector<StorageRoot> storageRoots(bool withSpace) {
    std::vector<StorageRoot> out;
    StorageRoot sd;
    sd.path = "sdmc:/";
    sd.label = "SD Card";
    if (withSpace) sd.detail = spaceString("sdmc:/");
    out.push_back(sd);
    if (!g_usb) return out;
    u32 count = usbHsFsGetMountedDeviceCount();
    if (!count) return out;
    std::vector<UsbHsFsDevice> devices(count);
    count = usbHsFsListMountedDevices(devices.data(), count);
    for (u32 i = 0; i < count; i++) {
        const UsbHsFsDevice& d = devices[i];
        StorageRoot r;
        r.path = std::string(d.name) + "/";
        r.label = d.product_name[0] ? d.product_name : "USB drive";
        r.removable = true;
        if (withSpace) {
            r.detail = LIBUSBHSFS_FS_TYPE_STR(d.fs_type);
            std::string space = spaceString(r.path.c_str());
            if (!space.empty()) r.detail += "  ·  " + space;
        }
        out.push_back(r);
    }
    return out;
}

bool drivesChanged() {
    if (!g_usb) return false;
    UEvent* ev = usbHsFsGetStatusChangeUserEvent();
    if (!ev || R_FAILED(waitSingle(waiterForUEvent(ev), 0))) return false;
    ueventClear(ev);
    return true;
}

unsigned long long fileSizeLimit(const std::string& path) {
    if (!g_usb || path.rfind("ums", 0) != 0) return 0;
    u32 count = usbHsFsGetMountedDeviceCount();
    std::vector<UsbHsFsDevice> devices(count);
    count = count ? usbHsFsListMountedDevices(devices.data(), count) : 0;
    for (u32 i = 0; i < count; i++) {
        std::string root = std::string(devices[i].name) + "/";
        if (path.rfind(root, 0) != 0) continue;
        u8 t = devices[i].fs_type;
        bool fat = t == UsbHsFsDeviceFileSystemType_FAT12 || t == UsbHsFsDeviceFileSystemType_FAT16 ||
                   t == UsbHsFsDeviceFileSystemType_FAT32;
        return fat ? 0xFFFFFFFFull : 0;
    }
    return 0;
}

bool ejectDrive(const std::string& path) {
    if (!g_usb) return false;
    u32 count = usbHsFsGetMountedDeviceCount();
    if (!count) return false;
    std::vector<UsbHsFsDevice> devices(count);
    count = usbHsFsListMountedDevices(devices.data(), count);
    for (u32 i = 0; i < count; i++)
        if (path == std::string(devices[i].name) + "/") return usbHsFsUnmountDevice(&devices[i], true);
    return false;
}

int disconnectWirelessControllers() {
    // Handheld (attached Joy-Con) is a separate id and is left alone.
    int off = 0;
    for (int id = HidNpadIdType_No1; id <= HidNpadIdType_No8; id++) {
        if (hidGetNpadStyleSet((HidNpadIdType)id) == 0) continue;
        if (R_SUCCEEDED(hidDisconnectNpad((HidNpadIdType)id))) off++;
    }
    return off;
}

std::string exportSubtitleFont(const std::string& dir) {
    std::string path = dir + "subfont.ttf";
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) return path;
    if (!g_pl) return "";
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) return "";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    fwrite(font.address, 1, font.size, f);
    fclose(f);
    return path;
}

bool systemCjkFont(int which, const void*& data, size_t& size) {
    if (!g_pl) return false;
    static const PlSharedFontType types[] = {PlSharedFontType_ChineseTraditional, PlSharedFontType_ChineseSimplified,
                                             PlSharedFontType_Standard, PlSharedFontType_KO};
    PlFontData font;
    if (which < 0 || which > 3 || R_FAILED(plGetSharedFontByType(&font, types[which]))) return false;
    data = font.address;
    size = font.size;
    return true;
}

void loadFallbackFonts(NVGcontext* vg, const std::vector<int>& fontIds) {
    if (!g_pl) return;
    PlSharedFontType types[] = {PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified,
                                PlSharedFontType_ExtChineseSimplified, PlSharedFontType_ChineseTraditional,
                                PlSharedFontType_KO};
    const char* names[] = {"nx-standard", "cjk-sc", "cjk-sc-ext", "cjk-tc", "ko"};
    for (int i = 0; i < 5; i++) {
        PlFontData font;
        if (R_FAILED(plGetSharedFontByType(&font, types[i]))) continue;
        int id = nvgCreateFontMem(vg, names[i], (unsigned char*)font.address, font.size, 0);
        if (id < 0) continue;
        for (int f : fontIds)
            if (f >= 0) nvgAddFallbackFontId(vg, f, id);
    }
}

// Drawn every frame, but each read is a call into a system service. These change slowly.
namespace {
int g_batteryPct = -1;
bool g_batteryCharging = false;
double g_batteryRead = -100, g_chargerRead = -100;

void refreshBattery() {
    // The charger is checked twice a second so plugging in shows at once; the percentage only
    // changes slowly, so it's read every few seconds, and straight away when the charger changes.
    double now = nowSeconds();
    bool changed = false;
    if (now - g_chargerRead >= 0.5) {
        g_chargerRead = now;
        PsmChargerType t = PsmChargerType_Unconnected;
        if (g_psm) psmGetChargerType(&t);
        bool c = t != PsmChargerType_Unconnected;
        changed = c != g_batteryCharging;
        g_batteryCharging = c;
    }
    if (!changed && now - g_batteryRead < 5.0) return;
    g_batteryRead = now;
    u32 p = 0;
    g_batteryPct = g_psm && R_SUCCEEDED(psmGetBatteryChargePercentage(&p)) ? (int)p : -1;
}
}  // namespace

int batteryPercent() {
    refreshBattery();
    return g_batteryPct;
}

bool charging() {
    refreshBattery();
    return g_batteryCharging;
}

bool isDocked() { return appletGetOperationMode() == AppletOperationMode_Console; }

bool isAppletMode() {
    AppletType t = appletGetAppletType();
    return t != AppletType_Application && t != AppletType_SystemApplication;
}

std::string systemVersion() {
    u32 v = hosversionGet();
    char b[64];
    snprintf(b, sizeof b, "%u.%u.%u%s", HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v),
             hosversionIsAtmosphere() ? " (Atmosphère)" : "");
    return b;
}

bool supportsBacklight() { return g_lbl; }

float backlight() {
    float v = 0.5f;
    if (g_lbl) lblGetCurrentBrightnessSetting(&v);
    return v;
}

bool g_screenOff = false;
void setScreenOff(bool off) {
    if (off == g_screenOff || !g_lbl) return;
    g_screenOff = off;
    if (off) lblSwitchBacklightOff(300000000ULL);  // a 0.3 s fade
    else lblSwitchBacklightOn(150000000ULL);
}
bool screenOff() { return g_screenOff; }

void* audioWatch(void*) {
    if (R_FAILED(audctlInitialize())) return nullptr;
    while (!g_audioQuit) {
        AudioTarget t = AudioTarget_Invalid;
        if (R_SUCCEEDED(audctlGetActiveOutputTarget(&t))) g_audioOut = (int)t;
        svcSleepThread(250000000ULL);  // 4 times a second
    }
    audctlExit();
    return nullptr;
}
int audioOutput() {
    if (!g_audioStarted && !g_audioQuit) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 32 * 1024);
        g_audioStarted = pthread_create(&g_audioThread, &a, audioWatch, nullptr) == 0;
        pthread_attr_destroy(&a);
        if (!g_audioStarted) g_audioQuit = true;  // don't keep trying
    }
    return g_audioOut;
}
void emergencyExit() {
    if (g_screenOff && g_lbl) lblSwitchBacklightOn(0);
    u64 id = 0;
    if (R_SUCCEEDED(svcGetInfo(&id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) &&
        id == 0x0100534D58000000ULL) {
        __nx_applet_exit_mode = 1;  // tell the system the game is closing (see shutdown())
        appletExit();
    }
    svcExitProcess();
    __builtin_unreachable();
}
static std::function<void()> g_onLeaving;
static AppletHookCookie g_focusCookie;
static void focusHook(AppletHookType type, void*) {
    if (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus && g_onLeaving)
        g_onLeaving();
}
void onLeaving(std::function<void()> fn) {
    static bool hooked = false;
    g_onLeaving = std::move(fn);
    if (!hooked) {
        appletHook(&g_focusCookie, focusHook, nullptr);
        hooked = true;
    }
}
void screenOffTick() {
    if (g_screenOff && appletGetFocusState() != AppletFocusState_InFocus) setScreenOff(false);
}

void setBacklight(float v) {
    if (!g_lbl) return;
    if (v < 0.02f) v = 0.02f;
    if (v > 1) v = 1;
    lblSetCurrentBrightnessSetting(v);
}

std::string localAddress() {
    static bool nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    u32 ip = 0;
    if (!nifm || R_FAILED(nifmGetCurrentIpAddress(&ip)) || !ip) return "";
    char buf[32];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
    return buf;
}

void setMediaPlayback(bool active) {
    static bool cur = false;
    if (cur == active) return;
    cur = active;
    appletSetMediaPlaybackState(active);
}

static bool g_keyboardFailed = false;
bool keyboardFailed() { return g_keyboardFailed; }

bool keyboard(const std::string& header, const std::string& initial, std::string& out) {
    SwkbdConfig kbd;
    g_keyboardFailed = false;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        g_keyboardFailed = true;
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetGuideText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 1000);
    char buf[1024] = {};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    // Cancelling comes back as a "bad exit" of the applet; anything else means it didn't run.
    if (R_FAILED(rc)) {
        g_keyboardFailed = rc != MAKERESULT(Module_Libnx, LibnxError_LibAppletBadExit);
        return false;
    }
    out = buf;
    return true;
}

void lowerThreadPriority() { svcSetThreadPriority(threadGetCurHandle(), 0x3B); }

double nowSeconds() { return armTicksToNs(armGetSystemTick()) / 1e9; }

// Idle ticks can only be read for the core the caller runs on, so one tiny sampler thread is
// pinned to each core and publishes its core's idle counter twice a second.
namespace {
struct CoreSample {
    volatile float load = 0;
};
CoreSample g_cores[4];
Thread g_samplers[4];
bool g_samplerRunning[4] = {};
bool g_samplersStarted = false;
volatile bool g_samplersQuit = false;

void samplerMain(void* arg) {
    CoreSample* cs = (CoreSample*)arg;
    u64 lastIdle = 0, lastTick = 0;
    while (!g_samplersQuit) {
        u64 idle = 0;
        svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, -1);
        u64 tick = armGetSystemTick();
        if (lastTick && tick > lastTick) {
            float load = 1.0f - (float)(idle - lastIdle) / (float)(tick - lastTick);
            cs->load = load < 0 ? 0 : (load > 1 ? 1 : load);
        }
        lastIdle = idle;
        lastTick = tick;
        svcSleepThread(500000000ull);
    }
}
}  // namespace

static void stopCpuSamplers() {
    g_samplersQuit = true;
    for (int c = 0; c < 4; c++) {
        if (!g_samplerRunning[c]) continue;
        threadWaitForExit(&g_samplers[c]);
        threadClose(&g_samplers[c]);
        g_samplerRunning[c] = false;
    }
}

int cpuLoad(float* out, int maxCores) {
    u64 mask = 0;
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    if (!g_samplersStarted) {
        g_samplersStarted = true;
        for (int c = 0; c < 4; c++)
            if ((mask & (1ull << c)) && R_SUCCEEDED(threadCreate(&g_samplers[c], samplerMain, &g_cores[c], nullptr,
                                                                  0x4000, 0x2C, c)))
                g_samplerRunning[c] = R_SUCCEEDED(threadStart(&g_samplers[c]));
    }
    int n = 0;
    for (int c = 0; c < 4 && n < maxCores; c++)
        if (mask & (1ull << c)) out[n++] = g_cores[c].load;
    return n;
}

std::string busyThreads() {
    // Function addresses are logged relative to the thread hook so they can be symbolized with the ELF.
    const uintptr_t base = (uintptr_t)&__wrap___syscall_thread_create;
    static u64 lastTick = 0, mainLast = 0;
    u64 now = armGetSystemTick();
    u64 dt = lastTick ? now - lastTick : 0;
    lastTick = now;
    std::string out;
    char buf[64];
    u64 t = 0;
    if (R_SUCCEEDED(svcGetInfo(&t, InfoType_ThreadTickCount, threadGetCurHandle(), (u64)-1))) {
        if (dt && t - mainLast > dt / 20) {
            snprintf(buf, sizeof buf, " main=%.0f%%", 100.0 * (t - mainLast) / dt);
            out += buf;
        }
        mainLast = t;
    }
    for (int i = 0; i < g_threadCount; i++) {
        TrackedThread& th = g_threads[i];
        if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, th.handle, (u64)-1))) continue;
        if (dt && th.lastTicks && t - th.lastTicks > dt / 20) {
            snprintf(buf, sizeof buf, " f%+lld=%.0f%%", (long long)(th.func - base),
                     100.0 * (t - th.lastTicks) / dt);
            out += buf;
        }
        th.lastTicks = t;
    }
    return out.empty() ? " none" : out;
}

void abortForReport() { diagAbortWithResult(MAKERESULT(Module_HomebrewLoader, 999)); }

int setCpuClock(int mhz) {
    u32 hz = 0;
    if (hosversionAtLeast(8, 0, 0)) {
        if (R_FAILED(clkrstInitialize())) return -1;
        ClkrstSession s;
        if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_CpuBus, 3))) {
            if (!g_cpuChanged) clkrstGetClockRate(&s, &g_cpuDefaultHz);
            u32 target = mhz > 0 ? (u32)mhz * 1000000u : g_cpuDefaultHz;
            if (mhz >= 0 && target) clkrstSetClockRate(&s, target);
            if (mhz >= 0) g_cpuChanged = mhz > 0;
            clkrstGetClockRate(&s, &hz);
            clkrstCloseSession(&s);
        }
        clkrstExit();
    } else {
        if (R_FAILED(pcvInitialize())) return -1;
        if (!g_cpuChanged) pcvGetClockRate(PcvModule_CpuBus, &g_cpuDefaultHz);
        u32 target = mhz > 0 ? (u32)mhz * 1000000u : g_cpuDefaultHz;
        if (mhz >= 0 && target) pcvSetClockRate(PcvModule_CpuBus, target);
        if (mhz >= 0) g_cpuChanged = mhz > 0;
        pcvGetClockRate(PcvModule_CpuBus, &hz);
        pcvExit();
    }
    return (int)(hz / 1000000);
}

bool threadUsage(long long& used, long long& limit) {
    // The kernel's count for this process (its resource limit), so leaks show.
    Handle rl = 0;
    u64 h = 0;
    if (R_FAILED(svcGetInfo(&h, InfoType_ResourceLimit, INVALID_HANDLE, 0))) return false;
    rl = (Handle)h;
    s64 cur = 0, max = 0;
    bool ok = R_SUCCEEDED(svcGetResourceLimitCurrentValue(&cur, rl, LimitableResource_Threads)) &&
              R_SUCCEEDED(svcGetResourceLimitLimitValue(&max, rl, LimitableResource_Threads));
    svcCloseHandle(rl);
    used = cur;
    limit = max;
    return ok;
}

bool memoryUsage(unsigned long long& used, unsigned long long& total) {
    // libnx maps all free memory as heap at startup, so the kernel's "used" figure is always
    // ~100%. Report what malloc has actually handed out instead.
    // Measured against what malloc can actually hand out: the heap newlib was given, not the
    // process's whole share (which also holds the app itself, its stacks and the system's parts).
    struct mallinfo mi = mallinfo();
    u64 t = 0;
    if (fake_heap_end > fake_heap_start) t = (u64)(fake_heap_end - fake_heap_start);
    else if (R_FAILED(svcGetInfo(&t, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) return false;
    used = (unsigned long long)mi.uordblks;
    total = t;
    return true;
}

}  // namespace platform
#endif
