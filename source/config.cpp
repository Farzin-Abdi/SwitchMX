#include "config.hpp"
#include "app.hpp"
#include "i18n.hpp"

#include "library.hpp"
#include "visualizer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "subtext.hpp"

const std::vector<std::string> kSpeedLabels = {"0.25x", "0.5x", "0.75x", "1.0x", "1.25x", "1.5x",
                                               "1.75x", "2.0x", "2.5x", "3.0x"};
const float kSpeedValues[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
const int kSpeedCount = 10;

namespace {
const std::vector<std::string> kLangNames = {"Automatic", "English", "Japanese",   "Spanish", "French",
                                             "German",    "Italian", "Portuguese", "Russian", "Korean",
                                             "Chinese",   "Arabic",  "Persian",    "Hindi",   "Turkish"};
const char* kLangCodes[] = {"",        "eng,en", "jpn,ja", "spa,es", "fre,fra,fr", "ger,deu,de", "ita,it",
                            "por,pt",  "rus,ru", "kor,ko", "chi,zho,zh", "ara,ar", "per,fas,fa", "hin,hi",
                            "tur,tr"};

std::vector<std::string> actionChoices() {
    std::vector<std::string> v;
    for (int i = 0; i < (int)Action::Count; i++) v.push_back(actionName((Action)i));
    return v;
}

using S = Settings;
std::vector<SettingDef> buildDefs() {
    std::vector<SettingDef> d;
    auto toggle = [&](const char* key, int sec, const char* label, const char* desc, int S::*f,
                      int S::*dep = nullptr) {
        SettingDef x{key, sec, label, desc, SettingType::Toggle, f};
        x.dependsOn = dep;
        d.push_back(x);
    };
    auto choice = [&](const char* key, int sec, const char* label, const char* desc, int S::*f,
                      std::vector<std::string> c, int S::*dep = nullptr) {
        SettingDef x{key, sec, label, desc, SettingType::Choice, f, std::move(c)};
        x.dependsOn = dep;
        d.push_back(x);
    };
    auto slider = [&](const char* key, int sec, const char* label, const char* desc, int S::*f, int mn,
                      int mx, int step, const char* unit) {
        SettingDef x{key, sec, label, desc, SettingType::Slider, f};
        x.min = mn; x.max = mx; x.step = step; x.unit = unit;
        d.push_back(x);
    };

    // 0: Appearance
    {
        std::vector<std::string> names;
        for (auto& l : i18n::languages()) names.push_back(l.name);
        choice("language", 0, "Language", "The language of menus and messages", &S::language, names);
    }
    choice("theme", 0, "Theme", "Overall colour scheme of the interface", &S::theme,
           {"Midnight", "AMOLED Black", "Graphite", "Nord Frost", "Mocha", "Deep Ocean", "Daylight"});
    {
        // The names are the designers' own and stay as they are in every language.
        std::vector<std::string> fonts{"Automatic"};
        for (int i = 0; i < App::uiFontCount(); i++) fonts.push_back(App::uiFontName(i));
        choice("ui_font", 0, "Font", "Typeface for menus and lists; Automatic suits the language",
               &S::uiFont, fonts);
    }
    {
        SettingDef x{"accent", 0, "Accent colour", "Highlights, progress bars and focus glow",
                     SettingType::Color, &S::accent,
                     {"Crimson", "Tangerine", "Amber", "Lime", "Emerald", "Teal", "Sky", "Azure", "Indigo",
                      "Violet", "Orchid", "Neon Red", "Neon Blue"}};
        d.push_back(x);
    }
    choice("ui_scale", 0, "Interface size", "Scales every element on screen", &S::uiScale,
           {"Compact", "Default", "Large", "Extra large"});
    slider("roundness", 0, "Corner roundness", "From sharp edges to soft pill shapes", &S::roundness, 0, 28, 2,
           "px");
    choice("anim_speed", 0, "Animations", "Speed of transitions and motion", &S::animSpeed,
           {"Off", "Snappy", "Smooth", "Relaxed"});
    choice("background", 0, "Background", "Backdrop of the library screens", &S::background,
           {"Aurora", "Gradient", "Solid"});
    choice("browser_layout", 0, "Library layout", "How files are shown in folders", &S::browserLayout,
           {"List", "Grid"});
    toggle("cover_art", 0, "Cover art", "Show posters, album art and picture previews in the browser", &S::coverArt);
    toggle("video_thumbs", 0, "Video thumbnails", "Take a frame from videos that have no cover art", &S::videoThumbs,
           &S::coverArt);
    toggle("show_ext", 0, "Show file extensions", "Display .mkv, .mp4 … after names", &S::showExtensions);
    toggle("tidy_names", 0, "Tidy video names", "“Better Call Saul S01E02” instead of the release name; the File Manager shows names as they are",
           &S::tidyNames);
    toggle("show_info", 0, "Show file details", "Size and date under each file", &S::showFileInfo);
    toggle("clock24", 0, "24-hour clock", "Use 24-hour time in the status bar", &S::clock24);
    toggle("battery_pct", 0, "Battery percentage", "Show the battery level as a number", &S::batteryPct);

    // 1: Playback
    choice("hwdec", 1, "Hardware decoding", "Uses the Switch video chip for H.264, HEVC, VP9 and MPEG-2", &S::hwdec,
           {"Automatic", "NVTEGRA only", "Off (CPU)"});
    toggle("resume", 1, "Resume playback", "Continue where you left off", &S::resume);
    toggle("autoplay", 1, "Auto-play next", "Queue the other videos in the folder", &S::autoplayNext);
    toggle("pause_on_unplug", 1, "Pause when headphones disconnect",
           "Wired, USB or Bluetooth: playback pauses instead of going to the speaker", &S::pauseOnUnplug);
    choice("loop", 1, "Repeat", "What happens when playback ends", &S::loopMode,
           {"Off", "Repeat file", "Repeat folder"});
    choice("default_speed", 1, "Default speed", "Playback speed for new files", &S::defaultSpeed, kSpeedLabels);
    slider("default_volume", 1, "Default volume", "Volume when the player starts", &S::defaultVolume, 0, 100, 5,
           "%");
    choice("max_volume", 1, "Volume boost", "Allow volume above 100%", &S::maxVolume,
           {"100%", "130%", "150%", "200%"});
    choice("seek_short", 1, "Short seek", "D-pad left/right seek step", &S::seekShort,
           {"5 s", "10 s", "15 s", "20 s", "30 s"});
    choice("seek_long", 1, "Long seek", "Seek step for the long-seek action", &S::seekLong,
           {"30 s", "60 s", "90 s", "2 min", "5 min"});
    choice("osd_timeout", 1, "Hide controls after", "Time before the overlay fades out", &S::osdTimeout,
           {"2 s", "3 s", "4 s", "6 s", "10 s", "Never"});
    toggle("show_remaining", 1, "Show remaining time", "Countdown instead of total duration", &S::showRemaining);
    choice("mini_progress", 1, "Mini progress bar", "Progress line while the controls are hidden",
           &S::miniProgress, {"Off", "Subtle", "Colourful"});
    choice("seekbar_style", 1, "Seek bar style", "Look of the progress bar", &S::seekbarStyle,
           {"Slim", "Bold", "Neon glow"});
    choice("video_fit", 1, "Default video fit", "How video fills the screen", &S::videoFit,
           {"Fit", "Fill (crop)", "Stretch"});
    choice("image_slideshow", 1, "Image slideshow", "Starts a slideshow when you open a picture", &S::imageSlideshow,
           {"Off", "3 s", "5 s", "10 s", "30 s"});
    {
        std::vector<std::string> styles, palettes;
        for (int i = 0; i < vis::styleCount(); i++) styles.push_back(vis::styleName(i));
        for (int i = 0; i < vis::paletteCount(); i++) palettes.push_back(vis::paletteName(i));
        choice("music_visual", 1, "Music visualizer", "Press X in the music player to try them all", &S::musicVisual,
               styles);
        choice("music_screen_off", 1, "Turn screen off while music plays",
               "After no touch or button for this long; tap twice to wake", &S::musicScreenOff,
               {"Never", "After 30 s", "After 1 min", "After 2 min", "After 5 min"});
        choice("music_palette", 1, "Visualizer colours", "Album colours follow each song's cover", &S::musicPalette,
               palettes);
    }
    toggle("deband", 1, "Deband", "Smooth colour banding (uses more GPU)", &S::deband);
    choice("audio_out", 1, "Audio output", "Surround needs a TV or receiver with 5.1 over HDMI", &S::audioOut,
           {"Stereo (speakers, headphones)", "Surround when docked"});
    toggle("night_mode", 1, "Night mode audio", "Compress loud explosions and boost dialogue", &S::nightMode);
    choice("sound_eq", 1, "Equalizer", "Shapes the sound of music and films", &S::soundEq,
           {"Off", "Bass boost", "Treble boost", "Vocal", "Loudness", "Small speakers", "Headphones"});
    toggle("lyrics", 1, "Lyrics", "Shown in the music player from a .lrc file or the song's tags", &S::lyrics);
    toggle("remember_tracks", 1, "Remember track choices",
           "Each video reopens with the audio and subtitle track you picked", &S::rememberTracks);
    choice("alang", 1, "Preferred audio language", "Picked automatically when available", &S::alang,
           kLangNames);

    // 2: Gestures
    toggle("g_doubletap", 2, "Double-tap to seek", "Double-tap the left or right side", &S::gDoubleTap);
    choice("g_doubletap_secs", 2, "Double-tap seek amount", "Each extra tap adds more", &S::gDoubleTapSeek,
           {"5 s", "10 s", "15 s", "30 s"}, &S::gDoubleTap);
    toggle("g_swipe_seek", 2, "Swipe to seek", "Drag horizontally across the video", &S::gSwipeSeek);
    choice("g_swipe_sens", 2, "Swipe sensitivity", "How far a swipe seeks", &S::gSwipeSensitivity,
           {"Precise", "Normal", "Fast"}, &S::gSwipeSeek);
    choice("g_left", 2, "Left side swipe", "Vertical drag on the left half", &S::gLeftVertical,
           {"Brightness", "Volume", "Off"});
    choice("g_right", 2, "Right side swipe", "Vertical drag on the right half", &S::gRightVertical,
           {"Volume", "Brightness", "Off"});
    toggle("g_pinch", 2, "Pinch to zoom", "Two fingers to zoom, drag to pan", &S::gPinchZoom);
    choice("g_longpress", 2, "Hold for fast-forward", "Press and hold the video to speed up", &S::gLongPress,
           {"Off", "1.5x", "2x", "3x"});

    // 3: Subtitles
    toggle("sub_auto", 3, "Auto-load subtitles", "Find .srt/.ass files next to the video", &S::subAuto);
    choice("slang", 3, "Preferred language", "Picked automatically when available", &S::slang, kLangNames);
    slider("sub_scale", 3, "Text size", "Works for SRT and styled ASS subtitles", &S::subScale, 50, 300, 10, "%");
    choice("sub_encoding", 3, "Text encoding", "For old .srt files that show garbled letters", &S::subEncoding,
           subtitleEncodingNames());
    choice("sub_color", 3, "Text colour", "Colour of subtitle text", &S::subColor,
           {"White", "Yellow", "Cyan", "Green", "Pink", "Orange", "Sky blue", "Red", "Lilac", "Cream"});
    slider("sub_border", 3, "Outline thickness", "Black outline around letters", &S::subBorder, 0, 8, 1, "");
    choice("sub_bg", 3, "Background", "Box behind subtitle text", &S::subBackground,
           {"None", "Shadow", "Translucent box", "Solid box"});
    slider("sub_pos", 3, "Vertical position", "100 = bottom edge of the screen", &S::subPos, 50, 100, 1, "%");
    toggle("sub_bold", 3, "Bold text", "Heavier subtitle font", &S::subBold);
    choice("sub_override", 3, "Styled subtitles (ASS)", "Whether your style replaces the file's", &S::subOverride,
           {"Keep original", "Scale only", "Force my style"});

    // 4: Controls
    toggle("swap_ab", 4, "Swap A and B", "Use B to confirm and A to go back", &S::swapAB);
    choice("keep_awake", 4, "Keep console awake", "Stops the screen dimming and auto-sleep", &S::keepAwake,
           {"While playing", "Also while paused", "Follow system settings"});
    choice("controller_off", 4, "Turn off wireless controllers", "After no input while playing; any button wakes them",
           &S::controllerOff, {"Never", "After 5 min", "After 15 min", "After 30 min"});
    choice("dpad_vertical", 4, "D-pad up/down in player", "When the controls are hidden", &S::dpadVertical,
           {"Volume", "Brightness", "Chapters"});
    auto acts = actionChoices();
    choice("bind_x", 4, "X button", "Action in the player", &S::bindX, acts);
    choice("bind_y", 4, "Y button", "Action in the player", &S::bindY, acts);
    choice("bind_l", 4, "L button", "Action in the player", &S::bindL, acts);
    choice("bind_r", 4, "R button", "Action in the player", &S::bindR, acts);
    choice("bind_zl", 4, "ZL button", "Action in the player", &S::bindZL, acts);
    choice("bind_zr", 4, "ZR button", "Action in the player", &S::bindZR, acts);

    // 5: Advanced
    choice("cache_secs", 5, "Network cache", "Seconds buffered ahead when streaming", &S::cacheSecs,
           {"10 s", "30 s", "60 s", "120 s"});
    choice("demux_mb", 5, "Demuxer memory", "Lower this if large files crash in applet mode", &S::demuxMB,
           {"32 MB", "64 MB", "128 MB", "192 MB"});
    choice("threads", 5, "Decoder threads", "Software decoding threads", &S::threads, {"Auto", "2", "3", "4"});
    choice("fast_decode", 5, "Fast software decoding", "Trades a little sharpness for smooth CPU playback",
           &S::fastDecode, {"Off", "Balanced", "Fastest"});
    toggle("wifi_autostart", 5, "Start Wi-Fi transfer with the app",
           "Lets a browser on your network manage files as soon as SwitchMX opens", &S::webAutostart);

    // 6: Debug
    toggle("show_stats", 6, "Debug overlay", "FPS, frame times, decoder, dropped frames and memory", &S::showStats);
    toggle("debug_graph", 6, "Frame-time graph", "Live graph of how long each frame takes", &S::debugGraph,
           &S::showStats);
    choice("debug_corner", 6, "Overlay position", "Where the debug overlay sits", &S::debugCorner,
           {"Top left", "Top right", "Bottom left", "Bottom right"}, &S::showStats);
    toggle("debug_log", 6, "Save debug log", "Writes a detailed playback log to switch/SwitchMX/mpv-log.txt",
           &S::debugLog);
    toggle("dev_mode", 6, "Developer mode",
           "A computer on your network can watch and test SwitchMX while it runs (with the token below)",
           &S::devMode);
    return d;
}

bool parseInt(const char* s, int& out) {
    char* end;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    out = (int)v;
    return true;
}
}  // namespace

const char* actionName(Action a) {
    static const char* names[] = {"Nothing",        "Subtitles menu", "Audio menu",       "Speed menu",
                                  "Video menu",     "Playlist",       "Chapters",         "Previous chapter",
                                  "Next chapter",   "Previous file",  "Next file",        "Long seek back",
                                  "Long seek fwd",  "Next subtitle",  "Next audio track", "Screenshot",
                                  "Lock controls",  "Slower",         "Faster",           "Performance overlay",
                                  "Cycle aspect",   "Mute",           "Like / unlike",    "Add to playlist"};
    int i = (int)a;
    return (i >= 0 && i < (int)Action::Count) ? names[i] : "?";
}

const std::vector<SettingDef>& Config::defs() {
    static std::vector<SettingDef> d = buildDefs();
    return d;
}

const std::vector<const char*>& Config::sections() {
    static std::vector<const char*> s = {"Appearance", "Playback", "Gestures", "Subtitles", "Controls", "Advanced",
                                         "Debug"};
    return s;
}

void Config::load(const std::string& path) {
    path_ = path;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) f = fopen((path + ".tmp").c_str(), "r");  // a save was cut off just before finishing
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* val = eq + 1;
        size_t n = strlen(val);
        while (n && (val[n - 1] == '\n' || val[n - 1] == '\r')) val[--n] = 0;
        std::string key = line;
        if (key == "last_dir") { lastDir = val; continue; }
        if (key == "opensubtitles_key") { openSubtitlesKey = val; continue; }
        if (key == "dev_token") { devToken = val; continue; }
        if (key == "setup_done") { parseInt(val, s.setupDone); continue; }
        if (key == "volume") { parseInt(val, volume); continue; }
        if (key == "show_hidden") { parseInt(val, s.showHidden); continue; }
        if (key == "media_only") { parseInt(val, s.mediaOnly); continue; }
        if (key == "reader_night") { parseInt(val, s.readerNight); continue; }
        if (key == "reader_fit") { parseInt(val, s.readerFit); s.readerFit = s.readerFit ? 1 : 0; continue; }
        if (key == "reader_font") { parseInt(val, s.readerFont); s.readerFont = std::max(6, std::min(s.readerFont, 32)); continue; }
        if (key == "reader_rotate") { parseInt(val, s.readerRotate); s.readerRotate &= 3; continue; }
        if (key == "sort_mode") { parseInt(val, s.sortMode); s.sortMode = s.sortMode < 0 || s.sortMode > 3 ? 0 : s.sortMode; continue; }
        for (auto& d : defs()) {
            if (key != d.key) continue;
            int v;
            if (!parseInt(val, v)) break;
            int lo = 0, hi = 1;
            switch (d.type) {
                case SettingType::Toggle: lo = 0; hi = 1; break;
                case SettingType::Choice:
                case SettingType::Color: lo = 0; hi = (int)d.choices.size() - 1; break;
                case SettingType::Slider: lo = d.min; hi = d.max; break;
            }
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            s.*(d.field) = v;
            break;
        }
    }
    fclose(f);
}

void Config::save() {
    if (path_.empty()) return;
    std::string text;
    auto put = [&](const char* key, int v) { text += std::string(key) + "=" + std::to_string(v) + "\n"; };
    for (auto& d : defs()) put(d.key, s.*(d.field));
    // Fields not exposed in the settings screen but persisted.
    put("show_hidden", s.showHidden);
    put("media_only", s.mediaOnly);
    put("sort_mode", s.sortMode);
    put("reader_night", s.readerNight);
    put("reader_fit", s.readerFit);
    put("reader_font", s.readerFont);
    put("reader_rotate", s.readerRotate);
    put("volume", volume);
    text += "last_dir=" + lastDir + "\n";
    if (!openSubtitlesKey.empty()) text += "opensubtitles_key=" + openSubtitlesKey + "\n";
    if (!devToken.empty()) text += "dev_token=" + devToken + "\n";
    text += "setup_done=" + std::to_string(s.setupDone) + "\n";
    writeFileAtomic(path_, text);
}

void Config::reset() {
    int setup = s.setupDone, lang = s.language;  // the language stays: a reset shouldn't hide the menus
    s = Settings{};
    s.setupDone = setup;
    s.language = lang;
    volume = -1;
}

int Config::seekShortSecs() const { static const int v[] = {5, 10, 15, 20, 30}; return v[s.seekShort]; }
int Config::seekLongSecs() const { static const int v[] = {30, 60, 90, 120, 300}; return v[s.seekLong]; }
int Config::doubleTapSecs() const { static const int v[] = {5, 10, 15, 30}; return v[s.gDoubleTapSeek]; }
float Config::osdTimeoutSecs() const {
    static const float v[] = {2, 3, 4, 6, 10, 0};
    return v[s.osdTimeout];
}
float Config::defaultSpeedValue() const { return kSpeedValues[s.defaultSpeed]; }
int Config::maxVolumeValue() const { static const int v[] = {100, 130, 150, 200}; return v[s.maxVolume]; }
float Config::longPressSpeed() const { static const float v[] = {0, 1.5f, 2, 3}; return v[s.gLongPress]; }
float Config::uiScaleValue() const { static const float v[] = {0.9f, 1.0f, 1.12f, 1.25f}; return v[s.uiScale]; }
float Config::animMultiplier() const { static const float v[] = {0, 1.6f, 1.0f, 0.6f}; return v[s.animSpeed]; }
float Config::swipeSensitivity() const { static const float v[] = {0.5f, 1.0f, 2.0f}; return v[s.gSwipeSensitivity]; }
const char* Config::hwdecValue() const {
#ifdef __SWITCH__
    // Switch mpv has no NVTEGRA display interop, so frames are copied back from the decoder.
    // Plain "nvtegra" silently fell back to software decoding.
    static const char* v[] = {"nvtegra-copy,auto-copy", "nvtegra-copy", "no"};
#else
    static const char* v[] = {"auto-safe", "auto-safe", "no"};
#endif
    return v[s.hwdec];
}
const char* Config::slideshowValue() const {
    static const char* v[] = {"inf", "3", "5", "10", "30"};
    return v[s.imageSlideshow];
}
float Config::slideshowSecs() const {
    static const float v[] = {0, 3, 5, 10, 30};
    return v[s.imageSlideshow];
}
int Config::controllerOffSecs() const {
    static const int v[] = {0, 300, 900, 1800};
    return v[s.controllerOff];
}
const char* Config::langCode(int idx) const { return kLangCodes[idx]; }
int Config::cacheSecsValue() const { static const int v[] = {10, 30, 60, 120}; return v[s.cacheSecs]; }
int Config::demuxMBValue() const { static const int v[] = {32, 64, 128, 192}; return v[s.demuxMB]; }
unsigned Config::subColorRGB() const {
    static const unsigned v[] = {0xFFFFFF, 0xFFE14D, 0x5CE1FF, 0x7CFF8A, 0xFF8AD8,
                                 0xFFA94D, 0x8FB8FF, 0xFF6161, 0xC59BFF, 0xFFF1C9};
    return v[std::clamp(s.subColor, 0, 9)];
}
