#include "app.hpp"
#include "netfs.hpp"

#include <unwind.h>

#include "forwarder.hpp"
#include "reader.hpp"
#include "i18n.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>

#include <atomic>
#include <mutex>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gl.h"
#include "nanovg.h"
#define NANOVG_GL3
#include "nanovg_gl.h"
#include "platform.hpp"
#include "readahead.hpp"

#include <ctime>
#include <exception>
#include <new>
#include <typeinfo>

#ifdef __SWITCH__
extern "C" char __start__;  // where the app is loaded (libnx's linker script)
static const uintptr_t kModuleBase = (uintptr_t)&__start__;
#else
static const uintptr_t kModuleBase = 0;
#endif
extern "C" const unsigned char g_fontVazirRegular[], g_fontVazirBold[], g_fontDisplay[], g_fontVazirMedium[];
extern "C" const unsigned int g_fontVazirRegular_size, g_fontVazirBold_size, g_fontDisplay_size, g_fontVazirMedium_size;
#define FONT_DECL(n) extern "C" const unsigned char g_font##n##Regular[], g_font##n##Bold[]; \
                     extern "C" const unsigned int g_font##n##Regular_size, g_font##n##Bold_size;
FONT_DECL(Inter) FONT_DECL(Roboto) FONT_DECL(Sahel) FONT_DECL(PlexAr)
FONT_DECL(Samim) FONT_DECL(Shabnam) FONT_DECL(Parastoo)
FONT_DECL(Cairo) FONT_DECL(Almarai) FONT_DECL(Tajawal) FONT_DECL(NotoAr)
FONT_DECL(OpenSans) FONT_DECL(Lato) FONT_DECL(Montserrat) FONT_DECL(PtSans)
#undef FONT_DECL
#define FONT_ENTRY(label, n) {label, g_font##n##Regular, &g_font##n##Regular_size, g_font##n##Bold, &g_font##n##Bold_size}

// Every bundled typeface. Each is free to redistribute (SIL Open Font Licence, Roboto under
// Apache 2.0) and each was checked for missing or blank glyphs before being added.
const UiFontDef kUiFonts[] = {
    {"Vazirmatn", g_fontVazirRegular, &g_fontVazirRegular_size, g_fontVazirBold, &g_fontVazirBold_size},  // 0
    FONT_ENTRY("Sahel", Sahel),              // 1
    FONT_ENTRY("Samim", Samim),              // 2
    FONT_ENTRY("Shabnam", Shabnam),          // 3
    FONT_ENTRY("Parastoo", Parastoo),        // 4
    FONT_ENTRY("Cairo", Cairo),              // 5
    FONT_ENTRY("Almarai", Almarai),          // 6
    FONT_ENTRY("Tajawal", Tajawal),          // 7
    FONT_ENTRY("IBM Plex Sans Arabic", PlexAr),  // 8
    FONT_ENTRY("Noto Sans Arabic", NotoAr),  // 9
    FONT_ENTRY("Inter", Inter),              // 10
    FONT_ENTRY("Roboto", Roboto),            // 11
    FONT_ENTRY("Open Sans", OpenSans),       // 12
    FONT_ENTRY("Lato", Lato),                // 13
    FONT_ENTRY("Montserrat", Montserrat),    // 14
    FONT_ENTRY("PT Sans", PtSans),           // 15
    // Not offered by name: "Automatic" uses it for Persian and Arabic, where a medium weight reads
    // far better than a light one against a dark screen.
    {"Vazirmatn Medium", g_fontVazirMedium, &g_fontVazirMedium_size, g_fontVazirBold, &g_fontVazirBold_size},  // 16
};
#undef FONT_ENTRY
const int kUiFontCount = (int)(sizeof(kUiFonts) / sizeof(kUiFonts[0]));
const int kFontVazirMedium = 16;

// The five typefaces offered for each language, and the one "Automatic" picks. A font only has to
// suit the script the menus are in: anything it lacks (a file name in another alphabet, Japanese
// under a Latin font) still comes from Vazirmatn and the console's own fonts behind it.
struct LangFonts {
    const char* code;    // "" is the fallback set, used by every Latin language
    int shown[5];
    int automatic;
};
const LangFonts kLangFonts[] = {
    {"fa", {0, 1, 2, 3, 4}, kFontVazirMedium},       // Vazirmatn, Sahel, Samim, Shabnam, Parastoo
    {"ar", {5, 6, 7, 8, 9}, 5},                      // Cairo, Almarai, Tajawal, IBM Plex, Noto Sans Arabic
    {"ru", {10, 11, 12, 15, 14}, 10},                // Inter, Roboto, Open Sans, PT Sans, Montserrat
    {"", {10, 11, 12, 13, 14}, 10},                  // Inter, Roboto, Open Sans, Lato, Montserrat
};

const LangFonts& fontsForLanguage() {
    const auto& langs = i18n::languages();
    int li = std::clamp(i18n::language(), 0, (int)langs.size() - 1);
    std::string code = langs[li].code;
    for (auto& l : kLangFonts)
        if (code == l.code) return l;
    return kLangFonts[(sizeof(kLangFonts) / sizeof(kLangFonts[0])) - 1];
}

namespace {
std::string g_eventLog;

// ---- Watchdog: if the screen stops updating, the event log says where the app was stuck, and
// after 40 s it closes instead of leaving a frozen screen. Time inside system dialogs or while the
// app is in the background doesn't count.
std::atomic<double> g_beat{0};
std::atomic<const char*> g_phase{"starting"};
std::atomic<int> g_systemWait{0};
std::mutex g_ipMutex;
std::string g_ip;  // this console's network address, looked up off the main thread
void appendEvent(const char* what);

void phase(const char* p) {
    g_phase = p;
    g_beat = platform::nowSeconds();
}

struct SystemWait {
    SystemWait() { g_systemWait++; }
    ~SystemWait() {
        g_systemWait--;
        g_beat = platform::nowSeconds();
    }
};

// Background threads must all have finished before the app returns to the loader: it unmaps the
// app's code, and a thread still running it crashes the console (the error seen on Exit).
std::atomic<bool> g_quitting{false};
std::atomic<int> g_bgThreads{0};

void* watchdogMain(void*) {
    double lastLoop = platform::nowSeconds();
    bool reported = false;
    while (!g_quitting) {
        usleep(500000);
        if (g_quitting) break;
        double now = platform::nowSeconds();
        if (now - lastLoop > 3 || g_systemWait > 0) {  // the console slept, or a system dialog is up
            g_beat = now;
            reported = false;
        }
        lastLoop = now;
        double stuck = now - g_beat;
        if (stuck < 2) reported = false;
        if (stuck > 5 && !reported) {
            appendEvent((std::string("hang: the screen stopped updating for 5 s (in ") + g_phase.load() + ")").c_str());
            reported = true;
        }
        if (stuck > 15) {
            appendEvent((std::string("hang: closing after 15 s stuck (in ") + g_phase.load() + ")").c_str());
            platform::emergencyExit();
        }
    }
    g_bgThreads--;
    return nullptr;
}

void* addressMain(void*) {
    while (!g_quitting) {
        std::string ip = platform::localAddress();
        {
            std::lock_guard<std::mutex> lk(g_ipMutex);
            g_ip = ip;
        }
        for (int i = 0; i < 30 && !g_quitting; i++) usleep(100000);
    }
    g_bgThreads--;
    return nullptr;
}

void startBackgroundThread(void* (*fn)(void*)) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_t t;
    g_bgThreads++;
    if (pthread_create(&t, &attr, fn, nullptr) == 0) pthread_detach(t);
    else g_bgThreads--;
    pthread_attr_destroy(&attr);
}

void appendEvent(const char* what) {
    DevTools::log("event", what);
    if (g_eventLog.empty()) return;
    FILE* f = fopen(g_eventLog.c_str(), "a");
    if (!f) return;
    time_t t = time(nullptr);
    char ts[32] = "";
    if (struct tm* tm = localtime(&t)) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "%s  %s\n", ts, what);
    fclose(f);
}

// How the last session ended, from its final line: 0 cleanly ("exit ..."), 1 closed from the HOME
// Menu (it was saving as it went to the background), 2 something else: a crash or a hang.
int lastSessionEnd(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return 0;
    char line[512], last[512] = "";
    while (fgets(line, sizeof line, f)) memcpy(last, line, sizeof line);
    fclose(f);
    if (last[0] == 0 || strstr(last, "  exit")) return 0;
    if (strstr(last, "left for the HOME Menu")) return 1;
    return 2;
}

// Writes a bundled font to disk for libass unless an identical-size copy exists.
void installFont(const std::string& path, const unsigned char* data, unsigned int size) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size == (off_t)size) return;
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}
}  // namespace

#ifndef __SWITCH__
#include <cstdlib>
#include <vector>

// Desktop dev aid: SWITCHMX_SNAPSHOT=out.bmp saves frame N (SWITCHMX_SNAPSHOT_FRAME, default 90) and quits.
static void writeSnapshot(const char* path, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px.data());
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int row = (w * 3 + 3) & ~3, size = 54 + row * h;
    unsigned char hdr[54] = {'B', 'M'};
    auto put = [&](int off, int v) { for (int i = 0; i < 4; i++) hdr[off + i] = (v >> (8 * i)) & 255; };
    put(2, size); put(10, 54); put(14, 40); put(18, w); put(22, h);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> pad(row - w * 3);
    for (int y = 0; y < h; y++) {
        fwrite(&px[(size_t)y * w * 3], 1, w * 3, f);
        fwrite(pad.data(), 1, pad.size(), f);
    }
    fclose(f);
}
#endif

bool App::init() {
    if (!platform::init()) return false;
    dataDir = platform::dataDir();
    mkdir((dataDir + "mpv").c_str(), 0777);
    mkdir((dataDir + "screenshots").c_str(), 0777);
    mkdir((dataDir + "subcache").c_str(), 0777);
    // Converted subtitles are regenerated on demand; don't let the cache grow forever.
    if (DIR* d = opendir((dataDir + "subcache").c_str())) {
        while (struct dirent* e = readdir(d))
            if (e->d_name[0] != '.') unlink((dataDir + "subcache/" + e->d_name).c_str());
        closedir(d);
    }
    mkdir((dataDir + "mpv/fonts").c_str(), 0777);
    installFont(dataDir + "mpv/fonts/Vazirmatn-Regular.ttf", g_fontVazirRegular, g_fontVazirRegular_size);
    installFont(dataDir + "mpv/fonts/Vazirmatn-Bold.ttf", g_fontVazirBold, g_fontVazirBold_size);
    g_eventLog = dataDir + "events.log";
    struct stat logSt;
    if (stat(g_eventLog.c_str(), &logSt) == 0 && logSt.st_size > 64 * 1024) {
        unlink((dataDir + "events.old.log").c_str());
        rename(g_eventLog.c_str(), (dataDir + "events.old.log").c_str());
    }
    int endedBefore = lastSessionEnd(g_eventLog);
    appendEvent(platform::isAppletMode() ? "start " SWITCHMX_VERSION " (applet mode)" : "start " SWITCHMX_VERSION);
    if (endedBefore == 1) appendEvent("note: the previous session was closed from the HOME Menu");
    // Closing the app from the HOME Menu kills it without the focus hook ever running, so this
    // can't tell that apart from a crash. A real crash leaves crash.txt beside this log; say so
    // rather than claiming one happened.
    if (endedBefore == 2)
        appendEvent("note: the previous session didn't shut down cleanly (closed from the HOME Menu, "
                    "or a crash or hang - crash.txt is written if it crashed)");
    {
        unsigned long long used = 0, total = 0;
        if (platform::memoryUsage(used, total) && total)
            appendEvent(("memory: " + std::to_string(total >> 20) + " MB for the app" +
                         (total < (1ull << 30) ? " (the small applet share: buffers are kept small)" : ""))
                            .c_str());
    }
    // The log isn't held open (the Switch then refuses to let anything else read it, the web page
    // included); a crash note gets memory by letting go of the reserve first.
    platform::installMemoryReserve();
    std::set_terminate([] {
        platform::releaseMemoryReserve();  // room for the note and the details below
        appendEvent("--  crash: the app stopped on an error (details on the next line if any)");
        std::string what = "unknown";
        if (auto e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                what = std::string(typeid(ex).name()) + ": " + ex.what();
            } catch (...) {
            }
        }
        appendEvent(("crash: uncaught C++ exception (" + what + ")").c_str());
        // Where it happened: return addresses from the app's start, for addr2line on the ELF.
        std::string bt = "crash: stack";
        _Unwind_Backtrace(
            [](_Unwind_Context* c, void* arg) -> _Unwind_Reason_Code {
                auto* s = (std::string*)arg;
                uintptr_t pc = _Unwind_GetIP(c);
                if (!pc || s->size() > 600) return _URC_END_OF_STACK;
                char b[24];
                snprintf(b, sizeof b, " %lx", (unsigned long)(pc - kModuleBase));
                *s += b;
                return _URC_NO_REASON;
            },
            &bt);
        appendEvent(bt.c_str());
        abort();
    });
    cfg.load(dataDir + "config.ini");
    i18n::setLanguage(cfg.s.language);
    lib.load(dataDir);
    netfs::load(dataDir);  // network places
    driveRoots_ = platform::storageRoots(false);

    if (!platform::createWindow()) return false;
    vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg) return false;
    // Vazirmatn covers Latin, Persian and Arabic and has a real bold weight (the Switch system
    // font doesn't); platform fonts are added as fallbacks for CJK and symbols.
    nvgCreateFontMem(vg, "sans", (unsigned char*)g_fontVazirRegular, g_fontVazirRegular_size, 0);
    nvgCreateFontMem(vg, "sans-bold", (unsigned char*)g_fontVazirBold, g_fontVazirBold_size, 0);
    // The name in the logo and on the splash: wide, geometric capitals (Orbitron Bold).
    nvgCreateFontMem(vg, "display", (unsigned char*)g_fontDisplay, g_fontDisplay_size, 0);
    // Every choice is loaded once here, so switching font is instant and can't fail later.
    std::vector<int> fontIds;
    for (int i = 0; i < kUiFontCount; i++) fontRegular_[i] = fontBold_[i] = -1;
    for (int i = 0; i < kUiFontCount; i++) {
        std::string reg = "ui" + std::to_string(i), bold = reg + "b";
        fontRegular_[i] = i == 0 ? nvgFindFont(vg, "sans")
                                 : nvgCreateFontMem(vg, reg.c_str(), (unsigned char*)kUiFonts[i].regular,
                                                    *kUiFonts[i].regularSize, 0);
        fontBold_[i] = i == 0 ? nvgFindFont(vg, "sans-bold")
                              : nvgCreateFontMem(vg, bold.c_str(), (unsigned char*)kUiFonts[i].bold,
                                                 *kUiFonts[i].boldSize, 0);
        fontIds.push_back(fontRegular_[i]);
        fontIds.push_back(fontBold_[i]);
        // Vazirmatn behind the rest: it has Latin, Persian and Arabic, so nothing goes blank.
        if (i > 0) {
            if (fontRegular_[i] >= 0) nvgAddFallbackFontId(vg, fontRegular_[i], fontRegular_[0]);
            if (fontBold_[i] >= 0) nvgAddFallbackFontId(vg, fontBold_[i], fontBold_[0]);
        }
    }
    platform::loadFallbackFonts(vg, fontIds);
    applyUiFont();
    ui.init(vg, &in);
    ui.applyTheme(cfg);
    covers.start(dataDir);

    std::string mpvDir = dataDir + "mpv/";
    platform::exportSubtitleFont(mpvDir);
    playerReady = player.init(cfg, mpvDir, dataDir + "screenshots/") && player.initRender();
    if (playerReady) player.setVolume(cfg.s.defaultVolume);

    home = new HomeScreen(*this);
    playerScreen = new PlayerScreen(*this);
    reader = new ReaderScreen(*this);
    switchTo(home);
    if (cfg.s.webAutostart) webStart();
    // HOME pressed: the app may be closed from there without being asked, so save now.
    platform::onLeaving([this] {
        if (erased) return;
        if (screen == playerScreen) playerScreen->saveProgress();
        playerScreen->endHeavyVideo();  // no faster clock while in the background (back on return)
        cfg.save();
        lib.save();
        appendEvent("left for the HOME Menu: saved");
    });
#ifndef __SWITCH__
    if (getenv("SWITCHMX_NOSPLASH")) splashDone_ = true;
    if (const char* sec = getenv("SWITCHMX_SECTION")) home->setSection((HomeScreen::Section)atoi(sec));
    if (const char* tab = getenv("SWITCHMX_TAB")) home->setSettingsTab(atoi(tab));
    if (const char* f = getenv("SWITCHMX_PLAY")) playPath(f);
#endif

    if (!playerReady) ui.toast("Video engine failed to start", Icon::Info);
    else if (platform::isAppletMode()) ui.toast("Applet mode: hold R while opening a game for full memory", Icon::Info);
    return true;
}

void App::logEvent(const std::string& what) { appendEvent(what.c_str()); }

std::string App::nroPath() const {
#ifdef __SWITCH__
    auto endsNro = [](const std::string& p) {
        return p.size() > 4 && strcasecmp(p.c_str() + p.size() - 4, ".nro") == 0;
    };
    if (selfPath.rfind("sdmc:/", 0) == 0 && endsNro(selfPath)) return selfPath;
    return "sdmc:/switch/SwitchMX/SwitchMX.nro";
#else
    return dataDir + "SwitchMX-update.nro";  // the desktop build never replaces itself
#endif
}

// A stream from the Streams list: played with its name as the title (a live channel has none).
void App::playStream(const std::string& url, const std::string& name) {
    if (!name.empty()) player.titles[url] = name;
    playFiles({url}, 0);
}

std::string App::streamName(const std::string& url) const {
    for (auto& s : lib.streams)
        if (s.url == url) return s.name;
    return "";
}

bool App::askText(const std::string& header, const std::string& initial, std::string& out) {
    bool ok;
    {
        SystemWait wait;  // the keyboard is a system dialog: waiting for it isn't a hang
        ok = platform::keyboard(tr(header), initial, out);  // the system keyboard shows it as is
    }
    if (ok) return true;
    if (platform::keyboardFailed()) {
        logEvent("keyboard failed to open");
        ui.toast("The keyboard couldn't open. Start SwitchMX from a game (hold R) and try again", Icon::Info);
    }
    return false;
}

void App::shutdown() {
    // Each step is noted, so if closing ever goes wrong the log says where.
    auto step = [this](const char* what) {
        if (!erased) appendEvent((std::string("exit: ") + what).c_str());
    };
    if (!erased) {
        appendEvent("exit");
        if (screen == playerScreen) playerScreen->exit();
        if (screen == reader) reader->saveProgress();
        cfg.save();
        lib.stopBackgroundSaves();
        lib.save();
    }
    step("saved; stopping cover art");
    covers.stop();
    step("stopping the player");
    player.shutdown();
    step("stopping Wi-Fi transfer");
    web.stop();
    if (home) home->cancelJob();
    delete home;
    home = nullptr;
    delete playerScreen;
    playerScreen = nullptr;
    delete reader;  // its pages are textures: before the drawing context goes
    reader = nullptr;
    if (vg) nvgDeleteGL3(vg);
    vg = nullptr;
    platform::destroyWindow();
    // Stop the app's own background threads and wait for them.
    step("waiting for background threads");
    g_quitting = true;
    updater.cancel();
    auto busy = [&] {
        return g_bgThreads > 0 || web.aliveThreads() > 0 || vis::Analyzer::running() > 0 || SubSearch::running() > 0 ||
               Updater::running() > 0;
    };
    // Up to 3 s: the watchdog and the address lookup wake every half second to notice.
    for (int i = 0; i < 150 && busy(); i++) usleep(20000);
    bool stragglers = busy();
    if (stragglers)
        appendEvent(("exit: still running: " + std::to_string(g_bgThreads.load()) + " app, " +
                     std::to_string(web.aliveThreads()) + " transfer, " + std::to_string(vis::Analyzer::running()) +
                     " visualizer, " + std::to_string(SubSearch::running()) + " subtitle threads")
                        .c_str());
    step("closing system services");
    platform::shutdown();
    // Something still running (a connection stuck on the network, a drive that stopped answering):
    // returning to the loader would crash on it, so end the process cleanly instead.
    if (stragglers) {
        appendEvent("exit: closing the process");
        platform::emergencyExit();
    }
    step("finished cleanly");
}

void App::switchTo(Screen* s) {
    if (screen) savedFocus_[screen] = ui.focus;
    screen = s;
    auto it = savedFocus_.find(s);
    if (it != savedFocus_.end()) ui.focus = it->second;
    ui.value(uid("app.fade")) = 0;
    s->enter();
}

void App::playPath(const std::string& path) {
    std::vector<std::string> files;
    int idx = 0;
    MediaKind k = kindForExtension(fileExtension(path));
    if (k == MediaKind::Document && !isUrl(path)) {
        openDocument(path);
        return;
    }
    bool gallery = k == MediaKind::Image;  // pictures always browse their folder
    if ((cfg.s.autoplayNext || gallery) && !isUrl(path) && k != MediaKind::Playlist) {
        idx = lib.siblings(path, files);
    } else {
        files.push_back(path);
    }
    playFiles(files, idx);
}

void App::playFiles(const std::vector<std::string>& files, int index) {
    if (!playerReady) {
        ui.toast("Video engine is not available", Icon::Info);
        return;
    }
    if (files.empty()) return;
    switchTo(playerScreen);
    playerScreen->play(files, index);
}

void App::applyUiFont() {
    // 0 is Automatic; 1..5 are the five typefaces offered for the language on screen.
    const LangFonts& lf = fontsForLanguage();
    int i = cfg.s.uiFont == 0 ? lf.automatic : lf.shown[std::clamp(cfg.s.uiFont - 1, 0, 4)];
    if (i < 0 || i >= kUiFontCount || fontRegular_[i] < 0 || fontBold_[i] < 0) i = 0;
    ui.fontId = fontRegular_[i];
    ui.fontBoldId = fontBold_[i];
}

// The names shown in Settings: the five for the language in use.
const char* App::uiFontName(int i) { return kUiFonts[fontsForLanguage().shown[std::clamp(i, 0, 4)]].name; }
int App::uiFontCount() { return 5; }

void App::settingsChanged() {
    // Saved about a second after the last change (a slider being dragged changes every frame): the
    // app may be closed from the HOME Menu, which never runs the save on the way out.
    cfgSaveIn_ = 1.0f;
    i18n::setLanguage(cfg.s.language);
    applyUiFont();  // after the language: Automatic follows whatever is now on screen
    ui.applyTheme(cfg);
    if (playerReady) player.applySettings(cfg);
}

void App::openMenu(const std::string& title, const std::vector<MenuItem>& items, std::function<void(int)> cb,
                   const std::string& message) {
    if (!modal_.open) modal_.savedFocus = ui.focus;
    modal_.open = true;
    modal_.closing = false;
    modal_.title = title;
    modal_.message = message;
    modal_.items = items;
    modal_.cb = std::move(cb);
    modal_.scroll.reset();
    modal_.scroll.id = uid("modal.scroll");
    ui.setFocus(uid("modal.item", 0));
}

void App::confirm(const std::string& title, const std::string& message, const std::string& okLabel,
                  std::function<void()> onOk, bool danger) {
    std::vector<MenuItem> items = {{okLabel, danger ? Icon::Trash : Icon::Check, "", danger},
                                   {"Cancel", Icon::Close}};
    openMenu(title, items, [onOk](int i) {
        if (i == 0) onOk();
    }, message);
}

void App::drawModal() {
    float target = modal_.open ? 1.0f : 0.0f;
    modal_.anim = ui.smooth(uid("modal.anim"), target, 16);
    if (modal_.anim < 0.01f && !modal_.open) return;
    float a = modal_.anim;
    bool interactive = modal_.open;

    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, ui.W, ui.H);
    nvgFillColor(vg, hex(0, 0.55f * a));
    nvgFill(vg);

    const float rowH = 60;
    float w = std::min(540.0f, ui.W - 80);
    float headerH = modal_.title.empty() ? 16 : 74;
    float msgH = 0;
    if (!modal_.message.empty()) {
        float b[4];
        ui.setFont(false);
        nvgFontSize(vg, 18);
        nvgTextLineHeight(vg, 1.25f);
        nvgTextBoxBounds(vg, 0, 0, w - 56, modal_.message.c_str(), nullptr, b);
        msgH = b[3] - b[1] + 18;
    }
    float listH = modal_.items.size() * rowH;
    float h = std::min(ui.H - 80, headerH + msgH + listH + 18);
    float s = 0.94f + 0.06f * a;
    Rect card{ui.W / 2 - w * s / 2, ui.H / 2 - h * s / 2 + 20 * (1 - a), w * s, h * s};

    nvgSave(vg);
    ui.pushAlpha(a);
    ui.panel(card, 1, ui.t.radius + 8);
    // Long titles (file names) glide instead of running off the card.
    if (!modal_.title.empty())
        ui.marquee(uid("modal.title"), card.x + 28, card.y + 40, card.w - 56, modal_.title, 24, ui.t.text, LEFT, true);
    if (msgH > 0) ui.textBox(card.x + 28, card.y + headerH - 6, card.w - 56, modal_.message, 18, ui.t.textDim);

    Rect listR{card.x + 12, card.y + headerH + msgH, card.w - 24, card.h - headerH - msgH - 12};
    modal_.scroll.update(ui, listR, listH);
    modal_.scroll.begin(ui);
    int chosen = -1;
    for (size_t i = 0; i < modal_.items.size(); i++) {
        const MenuItem& it = modal_.items[i];
        Rect r{listR.x, listR.y + i * rowH - modal_.scroll.offset, listR.w, rowH - 4};
        uint32_t id = uid("modal.item", (int)i);
        if (interactive) ui.focusable(id, r);
        bool foc = ui.focused(id);
        if (foc && ui.focusMoved) modal_.scroll.ensureVisible(i * rowH, rowH);
        // Archive listings can have hundreds of rows; only those in view need drawing.
        if (r.bottom() < listR.y || r.y > listR.bottom()) {
            if (interactive && foc && ui.confirm()) chosen = (int)i;
            continue;
        }
        float hv = ui.smooth(id ^ 0x1234, (foc && ui.showFocus()) || ui.pressedVisual(r) ? 1.0f : 0.0f, 18);
        if (hv > 0.01f) ui.fillRect(r, withAlpha(ui.t.surfaceHi, hv), ui.t.radius);
        if (foc && ui.showFocus()) ui.fillRect({r.x + 2, r.y + 14, 4, r.h - 28}, ui.t.accent, 2);
        NVGcolor fg = it.danger ? ui.t.danger : ui.t.text;
        float x = r.x + 18;
        if (it.icon != Icon::None) {
            ui.icon(it.icon, x + 12, r.cy(), 24, it.danger ? ui.t.danger : (foc ? ui.t.accent : ui.t.textDim));
            x += 40;
        }
        // The detail on the right gets at most 40% of the row; the label the rest.
        std::string detail = it.detail.empty() ? "" : ui.ellipsize(it.detail, r.w * 0.4f, 17);
        float maxW = r.right() - x - 20 - (detail.empty() ? 0 : ui.textWidth(detail, 17) + 16) - (it.checked ? 30 : 0);
        // Measured in the weight it's drawn in; the highlighted row shows its whole name, gliding.
        if (foc && ui.showFocus()) ui.marquee(id ^ 0x77, x, r.cy(), maxW, it.label, 20, fg, LEFT, true);
        else ui.text(x, r.cy(), ui.ellipsize(it.label, maxW, 20, foc), 20, fg, LEFT, foc);
        float rx = r.right() - 18;
        if (it.checked) {
            ui.icon(Icon::Check, rx - 10, r.cy(), 22, ui.t.accent);
            rx -= 32;
        }
        if (!detail.empty()) ui.text(rx, r.cy(), detail, 17, ui.t.textFaint, RIGHT);
        if (interactive && (ui.tapped(r) || (foc && ui.confirm()))) chosen = (int)i;
    }
    modal_.scroll.end(ui);
    modal_.scroll.drawScrollbar(ui);
    ui.popAlpha();
    nvgRestore(vg);

    if (!interactive) return;
    bool cancel = ui.back() || (in.tap && !card.contains(in.tapX, in.tapY));
    if (cancel) in.consumeTap();
    if (chosen >= 0 || cancel) {
        auto cb = modal_.cb;
        modal_.open = false;
        ui.setFocus(modal_.savedFocus);
        if (cb) cb(cancel ? -1 : chosen);
    }
}

bool runningNow() {
    SystemWait wait;  // the system holds the app here while it's in the background
    return platform::running();
}

void App::run() {
    double last = platform::nowSeconds();
    startBackgroundThread(watchdogMain);
    startBackgroundThread(addressMain);
    while (running && runningNow()) {
        phase("input");
        double now = platform::nowSeconds();
        float dt = (float)std::min(0.1, now - last);
        last = now;
        time += dt;

        float scale = cfg.uiScaleValue();
        RawInput raw;
        platform::pollInput(raw);
        // Developer mode: scripted presses and touches, in screen pixels like the real ones (and the
        // screenshots), so they go in before both are turned into interface units.
        dev.applyInput(raw, dt);
        for (int i = 0; i < raw.touches; i++) {
            raw.tx[i] /= scale;
            raw.ty[i] /= scale;
        }
        in.update(raw, dt);
        if (in.quit) break;

        phase("player events");
        if (playerReady) playerScreen->handleEvents(player.poll());
        phase("drives");
        if (platform::drivesChanged()) driveTimer_ = 1.0f;  // check right away
        // Docking or undocking changes what the audio may be sent as.
        bool docked = platform::isDocked();
        if (docked != wasDocked_) {
            wasDocked_ = docked;
            if (playerReady && cfg.s.audioOut == 1) player.applySettings(cfg);
        }
        watchDrives(dt);
        phase("wifi transfer");
        updateWeb(dt);
        platform::screenOffTick();
        watchAudioOutput(dt);
        if (cfgSaveIn_ > 0 && (cfgSaveIn_ -= dt) <= 0 && !erased) cfg.save();
        // Screen off: keep the music and the wake-up check going, but draw nothing and never wait
        // for the display (with its light off the console may stop refreshing it).
        if (screen == playerScreen && playerScreen->dark()) {
            phase("screen off");
            if (playerScreen->darkTick()) {
                usleep(16000);
                continue;
            }
        }

        int fbw, fbh;
        platform::drawableSize(fbw, fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // The music player draws everything itself; mpv has no picture to give it.
        phase("video");
        if (screen == playerScreen && playerReady && !playerScreen->musicMode()) {
            player.render(0, fbw, fbh);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, fbw, fbh);
        }

        float logicalH = 720.0f / scale;
        float px = fbh / logicalH;
        float logicalW = fbw / px;
        // Never during video, including while it is opening; music leaves plenty of time to spare.
        bool coversBusy = screen == playerScreen && playerScreen->videoMode();
        phase("cover art");
        covers.frame(vg, dt, cfg.s.coverArt != 0, cfg.s.videoThumbs != 0, coversBusy);
        nvgBeginFrame(vg, logicalW, logicalH, px);
        ui.beginFrame(dt, logicalW, logicalH);
        if (screen == playerScreen && playerReady && !playerScreen->musicMode()) drawRotatedVideo(logicalW, logicalH);

        if (splashDone_ && !cfg.s.setupDone && screen == home && !modal_.open) home->openSetup();
        else if (splashDone_ && !forwarderAsked_ && screen == home && !modal_.open) askForwarderOnce();
        screenFade_ = ui.smooth(uid("app.fade"), 1.0f, 10);
        ui.pushAlpha(screenFade_);
        ui.blocked = modal_.open || !splashDone_;
        phase(screen == playerScreen ? "player screen" : "home screen");
        // Right-to-left languages: the home screen and menus are drawn mirrored, and touches and
        // left/right are mirrored to match (until the focus has moved, in endFrame). The video and
        // music controls stay as they are, as media players do everywhere.
        bool mirrorHome = i18n::rtl() && screen == home;
        auto drawMirrored = [&](bool on) {
            if (on) {
                nvgSave(vg);
                nvgTranslate(vg, logicalW, 0);
                nvgScale(vg, -1, 1);
            } else {
                nvgRestore(vg);
            }
            ui.mirror = on;
        };
        if (mirrorHome) {
            in.mirrorX(logicalW);
            drawMirrored(true);
        }
        // An error here used to end the app on the spot. Out of memory, the player lets go of the
        // film (the biggest holder) and the library comes back; anything else is noted and the next
        // frame carries on.
        try {
            screen->frame();
        } catch (const std::bad_alloc&) {
            platform::releaseMemoryReserve();
            appendEvent("error: out of memory while drawing; playback stopped");
            if (screen == playerScreen) playerScreen->exit();
            ui.toast("Playback stopped", Icon::Info);
        } catch (const std::exception& e) {
            appendEvent((std::string("error: ") + e.what()).c_str());
        }
#ifndef __SWITCH__
        // Dev aid: SWITCHMX_HANGTEST=<seconds> freezes the main loop once, to test the watchdog.
        static int hangFrames = 0;
        if (const char* h = getenv("SWITCHMX_HANGTEST"); h && ++hangFrames == 60) sleep(atoi(h));
#endif
        phase("drawing");
        ui.blocked = false;
        ui.popAlpha();
        bool mirrorModal = i18n::rtl() && !mirrorHome && (modal_.open || modal_.anim > 0.001f);
        if (mirrorModal) {
            in.mirrorX(logicalW);
            drawMirrored(true);
        }
        drawModal();
        if (mirrorModal) {
            drawMirrored(false);
            in.mirrorX(logicalW);
        }
        if (mirrorHome) drawMirrored(false);
        watchCharger(dt);
        drawChargeSplash();
        ui.drawToasts();
        if (!splashDone_) drawSplash(dt);
        frameTimes_[frameIndex_++ % kFrameHistory] = dt;
        if (cfg.s.debugLog && screen == playerScreen) writeDiagnostics(dt);
        if (cfg.s.showStats) drawDebugOverlay();
#ifndef __SWITCH__
        // Dev aids: SWITCHMX_ICONSHEET shows every icon; SWITCHMX_LOGOONLY draws the HOME Menu icon.
        if (getenv("SWITCHMX_ICONSHEET")) drawIconSheet();
        if (getenv("SWITCHMX_LOGOONLY")) {
            nvgBeginPath(vg);
            nvgRect(vg, 0, 0, logicalW, logicalH);
            nvgFillColor(vg, nvgRGB(0, 0, 0));
            nvgFill(vg);
            // The HOME Menu icon: the mark raised a little, the name underneath.
            float L = logicalH, lx = (logicalW - L) / 2;
            drawLogo(vg, lx, 0, L, false, -1, 0.405f, 0.27f);
            nvgFontFace(vg, "display");
            nvgFontSize(vg, L * 0.098f);
            nvgTextLetterSpacing(vg, L * 0.022f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
            nvgFontBlur(vg, L * 0.02f);
            nvgFillColor(vg, nvgRGBA(0, 0, 0, 160));
            nvgText(vg, lx + L / 2 + L * 0.011f, L * 0.895f, "SWITCHMX", nullptr);
            nvgFontBlur(vg, 0);
            nvgFillColor(vg, nvgRGB(255, 255, 255));
            nvgText(vg, lx + L / 2 + L * 0.011f, L * 0.89f, "SWITCHMX", nullptr);
            nvgTextLetterSpacing(vg, 0);
        }
#endif
        ui.endFrame();
        if (mirrorHome) in.mirrorX(logicalW);
        nvgEndFrame(vg);
        dev.endFrame(fbw, fbh);  // developer mode: answers that need this thread (screenshots too)

#ifndef __SWITCH__
        static const char* snap = getenv("SWITCHMX_SNAPSHOT");
        static int snapFrame = getenv("SWITCHMX_SNAPSHOT_FRAME") ? atoi(getenv("SWITCHMX_SNAPSHOT_FRAME")) : 90;
        if (snap && --snapFrame == 0) {
            writeSnapshot(snap, fbw, fbh);
            running = false;
        }
#endif
        phase("showing the frame");
        platform::swapBuffers();
        if (playerReady) player.reportSwap();
    }
}

void App::drawDebugOverlay() {
    NVGcontext* v = vg;
    float avg = 0, worst = 0;
    int n = std::min(frameIndex_, kFrameHistory);
    for (int i = 0; i < n; i++) {
        avg += frameTimes_[i];
        worst = std::max(worst, frameTimes_[i]);
    }
    avg = n ? avg / n : 0;

    std::vector<std::string> lines;
    char buf[200];
    snprintf(buf, sizeof buf, "UI  %.1f fps   frame %.1f ms   worst %.1f ms", avg > 0 ? 1 / avg : 0.0f, avg * 1000,
             worst * 1000);
    lines.push_back(buf);
    // CPU load is sampled twice a second so the numbers are readable.
    cpuTimer_ += ui.dt;
    if (cpuTimer_ > 0.5f) {
        cpuTimer_ = 0;
        cpuCores_ = platform::cpuLoad(cpu_, 4);
    }
    if (cpuCores_ > 0) {
        std::string l = "CPU";
        for (int c = 0; c < cpuCores_; c++) {
            snprintf(buf, sizeof buf, "   core%d %3.0f%%", c, cpu_[c] * 100);
            l += buf;
        }
        lines.push_back(l);
    }
    unsigned long long used = 0, total = 0;
    if (platform::memoryUsage(used, total)) {
        if (total) snprintf(buf, sizeof buf, "Memory  %.0f / %.0f MB", used / 1048576.0, total / 1048576.0);
        else snprintf(buf, sizeof buf, "Memory  %.0f MB peak", used / 1048576.0);
        lines.push_back(buf);
    }
    if (screen == playerScreen && player.loaded) {
        const Player& p = player;
        snprintf(buf, sizeof buf, "Video  %s  %lldx%lld  %.2f fps (file)  %.2f fps (shown)", p.videoCodec.c_str(),
                 (long long)p.width, (long long)p.height, p.fps, p.outputFps);
        lines.push_back(buf);
        bool software = p.hwdecCurrent.empty() || p.hwdecCurrent == "no";
        std::string dec = software ? "software (CPU)" : p.hwdecCurrent;
        snprintf(buf, sizeof buf, "Decoder  %s   dropped %lld (decoder %lld)", dec.c_str(),
                 (long long)p.droppedFrames, (long long)p.decoderDrops);
        lines.push_back(buf);
        snprintf(buf, sizeof buf, "Audio  %s   A/V sync %+.0f ms", p.audioCodec.c_str(), p.avsync * 1000);
        lines.push_back(buf);
        snprintf(buf, sizeof buf, "Bitrate  video %.1f Mb/s  audio %.0f kb/s", p.videoBitrate / 1e6,
                 p.audioBitrate / 1e3);
        lines.push_back(buf);
        snprintf(buf, sizeof buf, "Cache  %.1f s / %.0f KB ahead%s   speed %.2fx",
                 std::max(0.0, p.cacheTime - p.position), p.cacheBytes / 1024.0, p.buffering ? " (buffering)" : "",
                 p.speed);
        lines.push_back(buf);
        ReadAheadStats ra = readAheadStats();
        if (ra.active) {
            snprintf(buf, sizeof buf, "SD read  %.1f MB/s   RAM buffer %.1f MB   slowest read %.0f ms", ra.readMBps,
                     ra.bufferedMB, ra.slowestReadMs);
        } else {
            snprintf(buf, sizeof buf, "Network read  %.2f MB/s", p.readSpeed / 1048576.0);
        }
        lines.push_back(buf);
    }

    float w = 0;
    for (auto& l : lines) w = std::max(w, ui.textWidth(l, 14) + 28);
    float graphH = cfg.s.debugGraph ? 54 : 0;
    float h = lines.size() * 20 + 18 + graphH;
    int corner = cfg.s.debugCorner;
    float x = (corner == 1 || corner == 3) ? ui.W - w - 12 : 12;
    float y = corner >= 2 ? ui.H - h - 12 : 12;
    ui.fillRect({x, y, w, h}, hex(0, 0.72f), 10);
    for (size_t i = 0; i < lines.size(); i++) {
        // Software decoding of video is the usual cause of stutter on the Switch: flag it.
        bool warn = lines[i].find("software (CPU)") != std::string::npos ||
                    lines[i].find("(buffering)") != std::string::npos;
        ui.text(x + 14, y + 19 + i * 20, lines[i], 14, warn ? hex(0xFFB020) : hex(0xFFFFFF, 0.92f));
    }

    if (graphH > 0) {
        // Frame-time bars: green under 16.7 ms, amber under 33 ms, red above.
        float gx = x + 14, gy = y + h - 12, gw = w - 28, gh = graphH - 16;
        float barW = gw / kFrameHistory;
        nvgBeginPath(v);
        nvgRect(v, gx, gy - gh * (16.7f / 50), gw, 1);
        nvgFillColor(v, hex(0xFFFFFF, 0.25f));
        nvgFill(v);
        for (int i = 0; i < n; i++) {
            float ft = frameTimes_[(frameIndex_ - n + i) % kFrameHistory];
            float bh = std::min(1.0f, ft * 1000 / 50) * gh;
            NVGcolor c = ft < 0.0175f ? hex(0x2BD48A) : ft < 0.034f ? hex(0xFFB020) : hex(0xFF4D5E);
            nvgBeginPath(v);
            nvgRect(v, gx + i * barW, gy - bh, std::max(1.0f, barW - 0.5f), bh);
            nvgFillColor(v, c);
            nvgFill(v);
        }
    }
}

// Once a second during playback (with Settings > Debug > Save debug log): one line of
// measurements in switch/SwitchMX/switchmx-log.txt, so stutter can be analysed afterwards.
void App::writeDiagnostics(float dt) {
    diagTimer_ += dt;
    diagFrames_++;
    if (diagTimer_ < 1.0f) return;
    float cpu[4] = {};
    int cores = platform::cpuLoad(cpu, 4);
    // A line a second adds up: left switched on for a few long films the log would grow until the
    // SD card filled, so past 4 MB it starts again rather than appending.
    std::string logPath = dataDir + "switchmx-log.txt";
    struct stat lst;
    const char* mode = (stat(logPath.c_str(), &lst) == 0 && lst.st_size > 4 * 1024 * 1024) ? "w" : "a";
    FILE* f = fopen(logPath.c_str(), mode);
    if (f) {
        const Player& p = player;
        ReadAheadStats ra = readAheadStats();
        fprintf(f,
                "t=%.1f pos=%.2f cache=%.2fs buffering=%d paused=%d | reader=%d %.1fMB/s ram=%.1fMB slow=%.0fms | "
                "cpu",
                time, p.position, std::max(0.0, p.cacheTime - p.position), (int)p.buffering, (int)p.paused,
                (int)ra.active, ra.readMBps, ra.bufferedMB, ra.slowestReadMs);
        for (int c = 0; c < cores; c++) fprintf(f, " %.0f", cpu[c] * 100);
        fprintf(f, " | threads:%s", platform::busyThreads().c_str());
        fprintf(f, " | ui=%.0ffps dec=%s drops=%lld | %s\n",
                diagFrames_ / diagTimer_, p.hwdecCurrent.c_str(), (long long)p.droppedFrames,
                baseName(p.path).c_str());
        fclose(f);
    }
    diagTimer_ = 0;
    diagFrames_ = 0;
}

// USB drives come and go; check once a second and tell the user.
void App::watchDrives(float dt) {
    driveTimer_ += dt;
    if (driveTimer_ < 1.0f) return;
    driveTimer_ = 0;
    auto roots = platform::storageRoots(false);
    std::vector<std::string> paths;
    for (auto& r : roots) paths.push_back(r.path);
    if (paths == drivePaths_) return;
    bool first = drivePaths_.empty();
    for (auto& r : roots)
        if (!first && std::find(drivePaths_.begin(), drivePaths_.end(), r.path) == drivePaths_.end()) {
            ui.toast(trf("Connected: {0}", {r.label}), Icon::Drive);
            logEvent("drive connected: " + r.path + " " + r.label);
        }
    if (!first) {
        for (auto& old : drivePaths_) {
            if (std::find(paths.begin(), paths.end(), old) != paths.end()) continue;
            // Files still open on a drive that's gone must be closed before anything else touches them.
            logEvent("drive removed: " + old);
            if (screen == playerScreen && playerScreen->closeIfOn(old)) {
                openMenu("USB drive removed", {{"OK", Icon::Check}}, [](int) {},
                         "Playback stopped because the drive with this file was unplugged.");
            } else {
                ui.toast("USB drive removed", Icon::Drive);
            }
        }
    }
    drivePaths_ = paths;
    driveRoots_ = roots;
    if (!first) home->drivesChanged();
}

bool App::canCollect(const std::string& path) const {
    if (path.empty() || isUrl(path)) return false;
    MediaKind k = kindForExtension(fileExtension(path));
    return k == MediaKind::Video || k == MediaKind::Audio;
}

void App::toggleLike(const std::string& path) {
    bool now = lib.toggleLiked(path);
    ui.toast(now ? "Added to Liked" : "Removed from Liked", now ? Icon::HeartFilled : Icon::Heart);
}

void App::addToPlaylistMenu(const std::vector<std::string>& paths) {
    // Folders add the music and videos directly inside them.
    std::vector<std::string> files;
    for (auto& p : paths) {
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            std::string dir = p.back() == '/' ? p : p + "/";
            for (auto& e : lib.list(dir, false, true, SORT_NAME))
                if (e.kind == MediaKind::Video || e.kind == MediaKind::Audio) files.push_back(e.path);
        } else if (canCollect(p)) {
            files.push_back(p);
        }
    }
    if (files.empty()) {
        ui.toast("No music or videos to add", Icon::Info);
        return;
    }
    auto addTo = [this, files](int list) {
        size_t added = 0;
        for (auto& f : files)
            if (lib.addToPlaylist(list, f)) added++;
        const std::string& name = lib.playlists[list].name;
        if (added == 0) ui.toast(trf("Already in {0}", {name}), Icon::Info);
        else if (added == 1) ui.toast(trf("Added to {0}", {name}), Icon::Check);
        else ui.toast(trf("Added {0} files to {1}", {std::to_string(added), name}), Icon::Check);
    };
    std::vector<MenuItem> items = {{"New playlist", Icon::Plus}};
    for (auto& pl : lib.playlists) items.push_back({pl.name, Icon::Playlist, std::to_string(pl.items.size())});
    openMenu("Add to playlist", items, [this, addTo](int i) {
        if (i > 0) {
            addTo(i - 1);
        } else if (i == 0) {
            // The keyboard takes over the screen, so don't let a video run on behind it.
            bool resume = screen == playerScreen && !player.paused;
            if (resume) player.setPause(true);
            std::string name;
            bool ok = askText("Playlist name", "", name) && !name.empty();
            if (resume) player.setPause(false);
            if (ok) addTo(lib.createPlaylist(name));
        }
    });
}

bool App::forwarderAvailable() {
#ifndef __SWITCH__
    if (getenv("SWITCHMX_FORWARDER_DEMO")) return true;  // dev aid: shows the prompts on desktop
#endif
    return forwarder::supported();
}

bool App::forwarderRunningFromIcon() { return forwarder::runningFromIcon(); }

bool App::forwarderInstalled() {
    if (forwarderState_ < 0) forwarderState_ = forwarder::installed() ? 1 : 0;
    return forwarderState_ == 1;
}

bool App::forwarderPending() {
    struct stat st;
    return stat((dataDir + "forwarder_wanted").c_str(), &st) == 0;
}

void App::setForwarderPending(bool on) {
    std::string marker = dataDir + "forwarder_wanted";
    if (!on) {
        unlink(marker.c_str());
    } else if (FILE* f = fopen(marker.c_str(), "w")) {
        fclose(f);
    }
}

// The HOME Menu icon's launcher changes now and then (the first one couldn't start on newer
// firmware). An icon installed by an older SwitchMX is replaced by the current one by itself.
static const int kForwarderBuild = 8;  // 8: ring logo, Orbitron name. 7: ring logo. 6: started as an application (full memory), not an applet

// The marker records the build and which launcher went in ("5 legacy"), so an icon is also replaced
// when the console's firmware moves across 19.0.0.
static void writeForwarderMarker(const std::string& dataDir) {
    if (FILE* f = fopen((dataDir + "forwarder_build").c_str(), "w")) {
        fprintf(f, "%d %s\n", kForwarderBuild, forwarder::legacyFirmware() ? "legacy" : "current");
        fclose(f);
    }
}

void App::forwarderInstall(bool quiet) {
    std::string error;
    if (forwarder::findKeys().empty()) {
        // Nothing for the user to come back for: the next start with the keys present adds it.
        setForwarderPending(true);
        if (!quiet)
            openMenu("The icon will be added by itself", {{"OK", Icon::Check}}, [](int) {},
                     "It needs your console's keys, saved once by Lockpick_RCM:\n"
                     "1. Restart into hekate → Payloads → Lockpick_RCM.\n"
                     "2. Move with VOL+ / VOL−, choose with POWER: pick \"Dump from SysNAND\".\n"
                     "3. When it says the keys were saved, choose \"Reboot to hekate\" (or Power off).\n"
                     "4. Launch Atmosphère as usual and open SwitchMX: the icon appears by itself.");
        return;
    }
    if (forwarder::install(error)) {
        forwarderState_ = 1;
        setForwarderPending(false);
        writeForwarderMarker(dataDir);
        ui.toast("SwitchMX is on the HOME Menu", Icon::Check);
    } else {
        setForwarderPending(false);  // don't retry something that fails at every start
        openMenu("Couldn't add the icon", {{"OK", Icon::Check}}, [](int) {}, error);
    }
}

void App::forwarderRemove() {
    std::string error;
    setForwarderPending(false);
    if (forwarder::remove(error)) {
        forwarderState_ = 0;
        ui.toast("Icon removed from the HOME Menu", Icon::Check);
    } else {
        openMenu("Couldn't remove the icon", {{"OK", Icon::Check}}, [](int) {}, error);
    }
}

// Asks once, on the first start after the splash screen; a small file remembers that it did.
bool App::forwarderOutdated() {
    // Asked every frame while Settings shows the icon row: the answer is kept a few seconds.
    static float checkedAt = -10;
    static bool cached = false;
    if (time - checkedAt < 3) return cached;
    checkedAt = time;
    int build = 0;
    char kind[16] = "";
    if (FILE* f = fopen((dataDir + "forwarder_build").c_str(), "r")) {
        if (fscanf(f, "%d %15s", &build, kind) < 1) build = 0;
        fclose(f);
    }
    bool matches = !strcmp(kind, forwarder::legacyFirmware() ? "legacy" : "current");
    cached = !(build >= kForwarderBuild && matches) && !forwarder::findKeys().empty();
    return cached;
}

void App::askForwarderOnce() {
    forwarderAsked_ = true;
    // A console clock behind this build's date (a modded Switch that can't set it from the internet)
    // makes every secure site look invalid: streams and updates. Worth saying, but it isn't worth a
    // dialog on every launch — a toast says it once, and the log keeps the detail.
    {
        static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* d = __DATE__;  // "Nov 10 2026"
        int month = (int)((strstr(months, std::string(d, 3).c_str()) - months) / 3), day = atoi(d + 4), year = atoi(d + 7);
        time_t now = ::time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        long have = (t.tm_year + 1900) * 400L + t.tm_mon * 32 + t.tm_mday, built = year * 400L + month * 32 + day;
        if (have < built - 1) {
            char shown[32];
            strftime(shown, sizeof shown, "%Y-%m-%d", &t);
            logEvent(std::string("clock: the console says ") + shown +
                     ", before this app was built; https may fail (System Settings -> System -> Date and Time)");
            ui.toast(trf("The console's clock says {0}: streams may fail", {shown}), Icon::Clock);
        }
    }
    if (!forwarderAvailable()) return;
    // Asked for earlier while the keys were missing: do it now that they may be there.
    if (forwarderPending()) {
        if (forwarderInstalled()) setForwarderPending(false);
        else if (!forwarder::findKeys().empty()) forwarderInstall(true);
        return;
    }
    if (!forwarderInstalled()) return;
    if (!forwarderOutdated()) return;
    if (forwarderRunningFromIcon()) {  // it can't replace itself while it's the one running
        ui.toast("A new HOME Menu icon is ready: open SwitchMX once from the Album to install it", Icon::Home);
        return;
    }
    std::string error;
    if (forwarder::remove(error) && forwarder::install(error)) {
        forwarderState_ = 1;
        writeForwarderMarker(dataDir);
        logEvent("HOME Menu icon updated to build " + std::to_string(kForwarderBuild) +
                 (forwarder::legacyFirmware() ? " (launcher for firmware before 19.0.0)" : ""));
        ui.toast("The HOME Menu icon was updated", Icon::Home);
    } else {
        forwarderState_ = -1;
        logEvent("HOME Menu icon update failed: " + error);
        openMenu("Couldn't update the HOME Menu icon", {{"OK", Icon::Check}}, [](int) {},
                 error + "\n" + tr("Turn the icon off and on again in Settings, Advanced."));
    }
}

namespace {
bool removeTreeAt(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    bool ok = true;
    if (DIR* d = opendir(path.c_str())) {
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
        closedir(d);
        for (auto& n : names) ok = removeTreeAt(path + "/" + n) && ok;
    }
    return rmdir(path.c_str()) == 0 && ok;
}
}  // namespace

bool App::eraseAllData(std::string& error) {
    // Only ever SwitchMX's own folder.
    std::string d = dataDir;
    bool ours = d.size() > 16 && (d.rfind("/switch/SwitchMX/") == d.size() - 17 || d.rfind("/.switchmx/") == d.size() - 11);
    if (!ours) {
        error = "Unexpected data folder " + d;
        return false;
    }
    // Nothing may write while it's being emptied (an open file couldn't be deleted).
    web.stop();
    covers.stop();
    lib.stopBackgroundSaves();
    if (playerReady) player.stopLogging();
    erased = true;
    std::vector<std::string> names;
    if (DIR* dir = opendir(d.c_str())) {
        while (dirent* e = readdir(dir))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
        closedir(dir);
    }
    int failed = 0;
    for (auto& n : names) {
        std::string lower = n;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        bool keep = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".nro") == 0;  // the app itself
        keep |= lower == ".switchmx.nro.star" || lower == "screenshots";
        if (!keep && !removeTreeAt(d + n)) failed++;
    }
    if (failed) error = tr(std::to_string(failed) + (failed == 1 ? " item" : " items") + " couldn't be removed");
    return failed == 0;
}

std::vector<WebServer::Root> webRoots() {
    std::vector<WebServer::Root> out;
    for (auto& r : platform::storageRoots(false)) out.push_back({r.path, r.label});
    return out;
}

bool App::webStart() {
    if (web.running()) return true;
    web.setRoots(webRoots());
    web.onLog = [](const std::string& line) { appendEvent(line.c_str()); };
    if (!web.start(8080)) {
        ui.toast(web.error(), Icon::Info);
        logEvent("wifi transfer failed to start: " + web.error());
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_ipMutex);  // the background lookup may not have run yet
        if (g_ip.empty()) g_ip = platform::localAddress();
    }
    logEvent("wifi transfer on at " + webAddress() + " (port " + std::to_string(web.port()) + ")");
    return true;
}

void App::webStop() {
    if (!web.running()) return;
    web.stop();
    logEvent("wifi transfer off");
    if (screen != playerScreen) platform::setMediaPlayback(false);
}

std::string App::webAddress() const {
    std::string ip;
    {
        std::lock_guard<std::mutex> lk(g_ipMutex);
        ip = g_ip;
    }
    if (ip.empty()) return "";
    return "http://" + ip + (web.port() == 80 ? "" : ":" + std::to_string(web.port()));
}

void App::updateWeb(float dt) {
    // Developer mode keeps the server up (tried again every 10 s while Wi-Fi is away).
    dev.update();
    static float devRetry = 0;
    if (cfg.s.devMode && !web.running() && (devRetry -= dt) <= 0) {
        devRetry = 10;
        webStart();
    }
    if (!web.running()) return;
    // A transfer must not be cut off by the console going to sleep.
    platform::setMediaPlayback(true);
    webRootsTimer_ -= dt;
    if (webRootsTimer_ <= 0) {  // drives come and go
        webRootsTimer_ = 2;
        web.setRoots(webRoots());
    }
    if (web.takeChanged() && home) home->filesChanged();
    // Playlists, likes, favourites and resume points follow what a browser renamed, moved or deleted.
    auto moves = web.takeMoves();
    for (auto& m : moves) {
        struct stat st;
        if (m.second.empty()) {
            if (stat(m.first.c_str(), &st) != 0) lib.forgetFile(m.first);  // only if it's really gone
        } else {
            lib.pathMoved(m.first, m.second);
        }
    }
    if (!moves.empty()) lib.save();
}

// The film turned with its subtitles: mpv drew it upright into a texture, laid on its side here.
void App::drawRotatedVideo(float W, float H) {
    unsigned tex;
    int tw, th;
    if (!player.rotatedFrame(tex, tw, th)) return;
    if (!videoImage_ || tw != videoImageW_ || th != videoImageH_) {
        if (videoImage_) nvgDeleteImage(vg, videoImage_);
        // The texture is bottom-up, as OpenGL draws; NanoVG reads top-down.
        videoImage_ = nvglCreateImageFromHandleGL3(vg, tex, tw, th, NVG_IMAGE_NODELETE | NVG_IMAGE_FLIPY);
        videoImageW_ = tw;
        videoImageH_ = th;
    }
    bool side = player.rotation == 90 || player.rotation == 270;
    float dw = side ? H : W, dh = side ? W : H;  // the frame's size before turning, in screen units
    nvgSave(vg);
    nvgTranslate(vg, W / 2, H / 2);
    nvgRotate(vg, player.rotation * 3.14159265f / 180.0f);
    nvgBeginPath(vg);
    nvgRect(vg, -dw / 2, -dh / 2, dw, dh);
    nvgFillPaint(vg, nvgImagePattern(vg, -dw / 2, -dh / 2, dw, dh, 0, videoImage_, 1));
    nvgFill(vg);
    nvgRestore(vg);
}

void App::watchAudioOutput(float) {
    int now = platform::audioOutput();
    if (now == 0 || now == audioOut_) return;
    bool wasPersonal = audioOut_ == 2 || audioOut_ == 4 || audioOut_ == 5;
    int was = audioOut_;
    audioOut_ = now;
    if (!wasPersonal || now != 1 || !cfg.s.pauseOnUnplug) return;  // only headphones → speaker
    if (!playerReady || !player.active || player.paused || player.idle) return;
    player.setPause(true);
    const char* what = was == 5 ? "Bluetooth audio disconnected" : was == 4 ? "USB audio disconnected"
                                                                   : "Headphones unplugged";
    ui.toast(tr(what) + " · " + tr("paused"), Icon::Pause);
    logEvent(std::string(what) + ": playback paused");
}

void App::watchCharger(float dt) {
    chargeSplash_ = std::max(0.0f, chargeSplash_ - dt);
    int now = platform::charging() ? 1 : 0;
    if (chargerState_ >= 0 && now != chargerState_ && splashDone_) {
        int pct = platform::batteryPercent();
        if (now) chargeSplash_ = 2.4f;
        else ui.toast(tr("Charger removed") + (pct >= 0 ? " · " + std::to_string(pct) + "%" : std::string()), Icon::Power);
    }
    chargerState_ = now;
}

// A battery filling up with a bolt over it, in a card that rises in and fades out.
void App::drawChargeSplash() {
    if (chargeSplash_ <= 0) return;
    float total = 2.4f, age = total - chargeSplash_;
    float in = ui.ease(std::min(1.0f, age / 0.35f)), out = std::min(1.0f, chargeSplash_ / 0.4f);
    float a = in * out;
    int pct = std::max(0, platform::batteryPercent());
    float cx = ui.W / 2, cy = ui.H / 2 + 20 * (1 - in);
    Rect card{cx - 130, cy - 80, 260, 160};
    ui.pushAlpha(a);
    ui.panel(card, 1, 28);
    // Battery body; the fill sweeps up to the charge, then gently pulses.
    float bw = 92, bh = 46;
    Rect body{cx - bw / 2 - 4, card.y + 34, bw, bh};
    nvgBeginPath(vg);
    nvgRoundedRect(vg, body.x, body.y, body.w, body.h, 11);
    nvgStrokeColor(vg, withAlpha(ui.t.text, 0.85f));
    nvgStrokeWidth(vg, 3);
    nvgStroke(vg);
    ui.fillRect({body.right() + 3, body.cy() - 8, 6, 16}, withAlpha(ui.t.text, 0.85f), 3);
    float sweep = ui.ease(std::min(1.0f, age / 0.9f));
    float level = pct / 100.0f * sweep;
    float pulse = age > 0.9f ? 0.75f + 0.25f * std::sin((age - 0.9f) * 6) : 1;
    Rect fill{body.x + 5, body.y + 5, (body.w - 10) * std::max(0.04f, level), body.h - 10};
    nvgBeginPath(vg);
    nvgRoundedRect(vg, fill.x, fill.y, fill.w, fill.h, 6);
    nvgFillPaint(vg, nvgLinearGradient(vg, fill.x, 0, fill.x + body.w, 0, withAlpha(ui.t.success, pulse),
                                       withAlpha(nvgLerpRGBA(ui.t.success, nvgRGB(255, 255, 255), 0.35f), pulse)));
    nvgFill(vg);
    // Bolt
    float bx = body.cx(), by = body.cy();
    nvgBeginPath(vg);
    nvgMoveTo(vg, bx + 4, by - 16);
    nvgLineTo(vg, bx - 9, by + 3);
    nvgLineTo(vg, bx - 1, by + 3);
    nvgLineTo(vg, bx - 4, by + 16);
    nvgLineTo(vg, bx + 9, by - 3);
    nvgLineTo(vg, bx + 1, by - 3);
    nvgClosePath(vg);
    nvgFillColor(vg, hex(0xFFFFFF));
    nvgFill(vg);
    nvgStrokeColor(vg, hex(0x000000, 0.35f));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
    ui.text(cx, card.bottom() - 40, "Charging", 20, ui.t.text, CENTER, true);
    ui.text(cx, card.bottom() - 18, std::to_string(pct) + "%", 16, ui.t.textDim, CENTER);
    ui.popAlpha();
}

#ifndef __SWITCH__
void App::drawIconSheet() {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, ui.W, ui.H);
    nvgFillColor(vg, ui.t.bg);
    nvgFill(vg);
    const int count = (int)Icon::Book + 1;
    for (int i = 1; i < count; i++) {
        int col = (i - 1) % 12, row = (i - 1) / 12;
        float x = 40 + col * 74, y = 40 + row * 74;
        ui.icon((Icon)i, x + 20, y + 20, 36, ui.t.text);
        ui.icon((Icon)i, x + 54, y + 44, 18, ui.t.textDim);
    }
    float y = 520;
    for (int k = 0; k < 4; k++) {
        Icon ic = (Icon[]){Icon::Folder, Icon::Film, Icon::Music, Icon::Image}[k];
        Rect r{40 + k * 90.0f, y, 60, 60};
        ui.fillRect(r, accentColor(k * 2), 14);
        ui.icon(ic, r.cx(), r.cy(), 30, hex(0xFFFFFF));
    }
    ui.fillRect({420, y, 220, 60}, ui.t.accent, 30);
    ui.icon(Icon::Play, 455, y + 30, 26, ui.t.onAccent);
    ui.text(480, y + 30, "Play", 20, ui.t.onAccent, LEFT, true);
    drawLogo(vg, 700, 470, 160);
    drawLogo(vg, 900, 500, 88);
    drawLogo(vg, 1020, 520, 44);
    drawLogo(vg, 1090, 470, 160, false);
}
#endif

// Logo and credit shown once at startup, on top of the already running home screen.
void App::drawSplash(float dt) {
    // About 4.5 s: a field of light gathers, the three rings draw themselves and start turning, the
    // core lights up and sends out a pulse, the name assembles letter by letter, and at the end the
    // view flies through the rings into the app.
    const float hold = 4.3f, fade = 0.65f;
    splashTime_ += dt;
    float t = splashTime_;
    if (t < hold && t > 1.2f && (in.pressed || in.touchBegan)) splashTime_ = t = hold;  // skippable
    float out = std::clamp((t - hold) / fade, 0.0f, 1.0f);
    if (out >= 1) {
        splashDone_ = true;
        ui.blocked = false;
        return;
    }
    auto ease = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return 1 - (1 - x) * (1 - x) * (1 - x); };
    auto easeIn = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * x; };
    float W = ui.W, H = ui.H, cx = W / 2, cy = H / 2 - 58;
    float fadeAll = 1 - ease((out - 0.35f) / 0.65f);  // the background goes last
    ui.pushAlpha(fadeAll);

    // Background: near black, three slow lights and a faint vignette.
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, H);
    nvgFillColor(vg, nvgRGB(5, 5, 13));
    nvgFill(vg);
    struct Light { float x, y, r; NVGcolor c; float sp; };
    Light lights[] = {{0.22f, 0.28f, 0.6f, nvgRGBA(0x8B, 0x5C, 0xF6, 105), 0.31f},
                      {0.8f, 0.72f, 0.55f, nvgRGBA(0x22, 0xD3, 0xEE, 80), 0.23f},
                      {0.62f, 0.12f, 0.42f, nvgRGBA(0xFF, 0x5F, 0xC8, 60), 0.4f}};
    float la = ease(t / 1.0f);
    for (auto& l : lights) {
        float lx = W * l.x + std::sin(t * l.sp * 2 + l.r * 10) * 70, ly = H * l.y + std::cos(t * l.sp * 2) * 45;
        NVGcolor c = l.c, c0 = l.c;
        c.a *= la;
        c0.a = 0;
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, W, H);
        nvgFillPaint(vg, nvgRadialGradient(vg, lx, ly, 0, W * l.r, c, c0));
        nvgFill(vg);
    }
    // Particles drifting in towards the rings, like light being drawn in.
    for (int i = 0; i < 90; i++) {
        float h1 = std::fmod(std::sin(i * 12.9898f) * 43758.5453f, 1.0f), h2 = std::fmod(std::sin(i * 78.233f) * 12345.678f, 1.0f);
        h1 = std::fabs(h1);
        h2 = std::fabs(h2);
        float life = std::fmod(t * (0.10f + 0.08f * h2) + h1, 1.0f);
        float rad = 120 + (1 - life) * (560 + 200 * h2);
        float ang = h1 * 6.2832f + t * (0.15f + 0.2f * h2) + life * 1.4f;
        float px = cx + std::cos(ang) * rad * 1.25f, py = cy + std::sin(ang) * rad * 0.8f;
        float a = std::sin(life * 3.1416f) * 0.55f * la * (1 - out);
        nvgBeginPath(vg);
        nvgCircle(vg, px, py, 1.1f + 1.6f * h2);
        nvgFillColor(vg, h2 > 0.66f ? nvgRGBAf(1, 0.55f, 0.85f, a) : h2 > 0.33f ? nvgRGBAf(0.62f, 0.55f, 1, a) : nvgRGBAf(0.45f, 0.9f, 1, a));
        nvgFill(vg);
    }
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, H);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, H / 2, H * 0.45f, H * 1.05f, nvgRGBA(0, 0, 0, 0), nvgRGBA(0, 0, 0, 150)));
    nvgFill(vg);

    // The rings: drawn in, then turning; at the end they rush towards the viewer.
    const float R = 112;
    float reveal = ease((t - 0.25f) / 1.5f);
    float zoom = 1 + 7 * easeIn(out);
    float ringsA = 1 - ease((out - 0.25f) / 0.6f);
    ui.pushAlpha(ringsA);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, R * 2.4f * zoom);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, R * 0.3f, R * 2.4f * zoom, nvgRGBAf(0.5f, 0.38f, 1, 0.28f * reveal),
                                       nvgRGBAf(0.5f, 0.38f, 1, 0)));
    nvgFill(vg);
    // Pulses from the core once it lights up.
    for (float start : {1.35f, 2.15f, 2.95f}) {
        float k = (t - start) / 1.4f;
        if (k <= 0 || k >= 1) continue;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, (R * 0.3f + k * R * 2.6f) * zoom);
        nvgStrokeColor(vg, nvgRGBAf(0.6f, 0.75f, 1.0f, 0.42f * (1 - k) * (1 - k)));
        nvgStrokeWidth(vg, 2.5f * (1 - k) + 0.8f);
        nvgStroke(vg);
    }
    drawRings(vg, cx, cy, R * zoom, t * 1.15f, reveal, true);
    ui.popAlpha();

    // The name in the display face: letters come in from wide apart and settle, each with a neon glow.
    float textA = 1 - ease(out / 0.45f);
    ui.pushAlpha(textA);
    const char* name = "SWITCHMX";
    const float nameSize = 54, track = 17;  // wide capitals, widely spaced
    float ny = cy + R + 86;
    nvgFontFace(vg, "display");
    nvgFontSize(vg, nameSize);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    int n = (int)strlen(name);
    float adv[16], total = 0;
    for (int i = 0; i < n; i++) {
        char ch[2] = {name[i], 0};
        adv[i] = nvgTextBounds(vg, 0, 0, ch, nullptr, nullptr) + (i + 1 < n ? track : 0);
        total += adv[i];
    }
    float x = cx - total / 2;
    for (int i = 0; i < n; i++) {
        char ch[2] = {name[i], 0};
        float k = ease((t - 1.05f - i * 0.075f) / 0.6f);
        if (k > 0) {
            float spread = (1 - k) * (i - (n - 1) / 2.0f) * 26;  // tracking closes in
            float lx = x + spread;
            nvgFontBlur(vg, 16);
            nvgFillColor(vg, i < n / 3 ? nvgRGBAf(0.3f, 0.85f, 1, 0.7f * k) : i < 2 * n / 3 ? nvgRGBAf(0.6f, 0.45f, 1, 0.7f * k)
                                                                                            : nvgRGBAf(1, 0.4f, 0.8f, 0.7f * k));
            nvgText(vg, lx, ny, ch, nullptr);
            nvgFontBlur(vg, 0);
            nvgFillColor(vg, nvgRGBAf(1, 1, 1, k));
            nvgText(vg, lx, ny, ch, nullptr);
        }
        x += adv[i];
    }
    nvgFontBlur(vg, 0);
    // Tagline, widely spaced, and a short gradient rule between it and the name.
    float tg = ease((t - 1.95f) / 0.7f);
    if (tg > 0) {
        float rw = 140 * tg;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - rw / 2, ny + 50, rw, 2, 1);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx - rw / 2, 0, cx + rw / 2, 0, nvgRGBA(0x3B, 0xE0, 0xFF, 220), nvgRGBA(0xFF, 0x5F, 0xC8, 220)));
        nvgFill(vg);
        ui.pushAlpha(tg);
        ui.setFont(true);
        nvgFontSize(vg, 15);
        nvgTextLetterSpacing(vg, 6 * (1.4f - 0.4f * tg));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, nvgRGBA(196, 202, 235, 255));
        nvgText(vg, cx, ny + 82, "FILMS  ·  MUSIC  ·  PICTURES  ·  BOOKS", nullptr);
        nvgTextLetterSpacing(vg, 0);
        ui.popAlpha();
    }
    ui.popAlpha();

    // Credit and a thin progress line at the bottom.
    float cr = ease((t - 2.3f) / 0.7f) * (1 - ease(out / 0.4f));
    if (cr > 0) {
        ui.pushAlpha(cr);
        ui.text(cx, H - 70, "Developed by Farzin Abdi", 16, nvgRGBA(165, 172, 210, 255), CENTER);
        float pw = 200, pl = std::clamp(t / hold, 0.0f, 1.0f);
        ui.fillRect({cx - pw / 2, H - 40, pw, 2}, nvgRGBA(255, 255, 255, 22), 1);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - pw / 2, H - 40, pw * ease(pl), 2, 1);
        nvgFillPaint(vg, nvgLinearGradient(vg, cx - pw / 2, 0, cx + pw / 2, 0, nvgRGB(0x3B, 0xE0, 0xFF), nvgRGB(0xFF, 0x5F, 0xC8)));
        nvgFill(vg);
        ui.popAlpha();
    }
    ui.popAlpha();
}
