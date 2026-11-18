#pragma once
#include <string>
#include <vector>

enum class SettingType { Toggle, Choice, Slider, Color };

// Actions that can be bound to buttons in the player.
enum class Action {
    None,
    SubtitlePanel,
    AudioPanel,
    SpeedPanel,
    VideoPanel,
    PlaylistPanel,
    ChapterPanel,
    PrevChapter,
    NextChapter,
    PrevFile,
    NextFile,
    SeekBackLong,
    SeekForwardLong,
    CycleSubtitle,
    CycleAudio,
    Screenshot,
    ToggleLock,
    SpeedDown,
    SpeedUp,
    ToggleStats,
    CycleAspect,
    Mute,
    ToggleLike,
    AddToPlaylist,
    Count
};
const char* actionName(Action a);

struct Settings {
    // Appearance
    int language = 0;  // index into i18n::languages()
    int theme = 0;
    int uiFont = 0;  // Appearance -> Font (kUiFonts in app.cpp)
    int accent = 7;
    int uiScale = 1;
    int roundness = 14;
    int animSpeed = 2;
    int background = 0;
    int browserLayout = 0;
    int coverArt = 1;
    int videoThumbs = 1;
    int showExtensions = 1;
    int tidyNames = 1;  // video names without channel tags and release details
    int showFileInfo = 1;
    int showHidden = 0;
    int mediaOnly = 1;
    int sortMode = 0;
    int clock24 = 1;
    int batteryPct = 1;

    // Playback
    int hwdec = 0;
    int resume = 1;
    int autoplayNext = 1;
    int loopMode = 0;
    int defaultSpeed = 3;
    int defaultVolume = 100;
    int maxVolume = 2;
    int seekShort = 1;
    int seekLong = 1;
    int osdTimeout = 2;
    int showRemaining = 0;
    int miniProgress = 1;
    int seekbarStyle = 1;
    int videoFit = 0;
    int imageSlideshow = 0;
    int musicVisual = 1;  // Radial
    int musicPalette = 0;
    int musicScreenOff = 0;  // turn the screen off after this long without input while music plays
    int deband = 0;
    int nightMode = 0;
    int soundEq = 0;        // equalizer preset (see Config::eqFilter)
    int lyrics = 1;         // show lyrics in the music player when a song has them
    int rememberTracks = 1; // reopen each video with the audio and subtitle track last chosen for it
    // Document reader, remembered between documents (not in the settings screen).
    int readerNight = 0;    // pages shown light-on-dark
    int readerFit = 0;      // 0 whole page, 1 page width
    int readerFont = 11;    // text size of e-books, in points
    int readerRotate = 0;   // quarter turns, for holding the console upright
    int audioOut = 0;  // 0 stereo, 1 surround when docked
    int alang = 0;

    // Gestures
    int gDoubleTap = 1;
    int gDoubleTapSeek = 1;
    int gSwipeSeek = 1;
    int gSwipeSensitivity = 1;
    int gLeftVertical = 0;
    int gRightVertical = 0;
    int gPinchZoom = 1;
    int gLongPress = 2;

    // Subtitles
    int subAuto = 1;
    int slang = 0;
    int subScale = 100;
    int subEncoding = 0;
    int subColor = 0;
    int subBorder = 3;
    int subBackground = 1;
    int subPos = 95;
    int subBold = 1;
    int subOverride = 1;

    // Controls
    int swapAB = 0;
    int keepAwake = 0;
    int webAutostart = 0;
    int pauseOnUnplug = 1;  // pause when headphones / Bluetooth audio go away
    int setupDone = 0;      // the first-start text size question was answered
    int controllerOff = 0;
    int dpadVertical = 0;
    int bindX = (int)Action::SubtitlePanel;
    int bindY = (int)Action::AudioPanel;
    int bindL = (int)Action::SeekBackLong;
    int bindR = (int)Action::SeekForwardLong;
    int bindZL = (int)Action::PrevChapter;
    int bindZR = (int)Action::NextChapter;

    // Advanced
    int cacheSecs = 1;
    int demuxMB = 1;
    int threads = 0;
    int fastDecode = 1;

    // Debug
    int showStats = 0;
    int debugGraph = 1;
    int debugCorner = 1;
    int debugLog = 0;
    int devMode = 0;   // developer mode: a computer on the network may test the app (devtools.hpp)
};

struct SettingDef {
    const char* key;
    int section;
    const char* label;
    const char* desc;
    SettingType type;
    int Settings::*field;
    std::vector<std::string> choices;  // Choice / Color labels
    int min = 0, max = 1, step = 1;    // Slider
    const char* unit = "";
    // Only shown when this toggle/choice is non-zero (nullptr = always).
    int Settings::*dependsOn = nullptr;
};

class Config {
public:
    Settings s;
    std::string lastDir;
    std::string devToken;          // what a computer must send in developer mode
    std::string openSubtitlesKey;  // the user's own API key for finding subtitles online
    int volume = -1;  // last used volume, -1 = default

    void load(const std::string& path);
    void save();
    void reset();

    static const std::vector<SettingDef>& defs();
    static const std::vector<const char*>& sections();

    // Derived values
    int seekShortSecs() const;
    int seekLongSecs() const;
    int doubleTapSecs() const;
    float osdTimeoutSecs() const;  // <= 0 means never hide
    float defaultSpeedValue() const;
    int maxVolumeValue() const;
    float longPressSpeed() const;  // 0 = off
    float uiScaleValue() const;
    float animMultiplier() const;  // 0 = no animation
    float swipeSensitivity() const;
    const char* hwdecValue() const;
    const char* slideshowValue() const;
    float slideshowSecs() const;  // 0 when off
    int controllerOffSecs() const;  // 0 = never
    const char* langCode(int idx) const;
    int cacheSecsValue() const;
    int demuxMBValue() const;
    unsigned subColorRGB() const;

private:
    std::string path_;
};

extern const std::vector<std::string> kSpeedLabels;
extern const float kSpeedValues[];
extern const int kSpeedCount;
