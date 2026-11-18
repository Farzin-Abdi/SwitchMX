#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config.hpp"
#include "coverart.hpp"
#include "fileops.hpp"
#include "input.hpp"
#include "library.hpp"
#include "platform.hpp"
#include "player.hpp"
#include "ui.hpp"
#include "visualizer.hpp"
#include "subsearch.hpp"
#include "lyrics.hpp"
#include "webserver.hpp"

#include "version.hpp"
#include "i18n.hpp"
#include "names.hpp"
#include "updater.hpp"
#include "devtools.hpp"

class App;
class ReaderScreen;

class Screen {
public:
    explicit Screen(App& a) : app(a) {}
    virtual ~Screen() = default;
    virtual void enter() {}
    virtual void frame() = 0;
    App& app;
};

struct MenuItem {
    std::string label;
    Icon icon = Icon::None;
    std::string detail;
    bool danger = false;
    bool checked = false;
};

// ---------------------------------------------------------------- Home

// A UI font offered in Settings (see kUiFonts in app.cpp).
struct UiFontDef {
    const char* name;
    const unsigned char* regular;
    const unsigned int* regularSize;
    const unsigned char* bold;
    const unsigned int* boldSize;
};

class HomeScreen : public Screen {
public:
    enum Section { FILES, RECENT, FAVORITES, PLAYLISTS, STREAMS, NETWORK, MANAGER, SETTINGS, SECTION_COUNT };
    explicit HomeScreen(App& a) : Screen(a) {}
    void enter() override;
    void frame() override;
    void openDir(const std::string& dir, bool remember = true);
    void refresh();
    void setSection(Section s);
    void filesChanged();  // something else changed files on disk: reload the list on screen
    void cancelJob();
    void openSetup() { setupOpen_ = true; }
    void setSettingsTab(int t) { settingsTab_ = t; }
    void drivesChanged();  // a USB drive was plugged in or removed
    Section section() const { return section_; }
    const std::string& dir() const { return dir_; }

private:
    void drawSidebar();
    void drawHeader(Rect area);
    void drawFiles(Rect area);
    void drawRecent(Rect area);
    void drawFavorites(Rect area);
    void drawStreams(Rect area);

    // Playlists (playlists.cpp). An open list is an index into lib.playlists, or kLikedList.
    static constexpr int kNoList = -2, kLikedList = -1;
    void drawPlaylists(Rect area);
    void drawPlaylistIndex(Rect area);
    void drawPlaylistItems(Rect area);
    void openPlaylist(int list);
    const std::vector<std::string>* listItems(int list) const;
    void playList(int list, int start, bool shuffle);
    void newPlaylist();
    void playlistMenu(int list);
    void playlistItemMenu(size_t index);
    int playlistOpen_ = kNoList;
    std::vector<FileEntry> listEntries_;  // the open list, with file details looked up once
    uint64_t listRevision_ = 0;
    std::vector<FileEntry> favEntries_;
    uint64_t favRevision_ = 0;
    void listsChanged();  // what was looked up from the SD card may be out of date
    void drawSettings(Rect area);
    void drawHints();
    void drawEmpty(Rect area, Icon ic, const char* title, const char* sub);
    bool fileRow(uint32_t id, Rect r, const FileEntry& e, int index);
    bool fileTile(uint32_t id, Rect r, const FileEntry& e, int index);
    void fileTileIcon(Rect tile, const FileEntry& e, float iconSize);
    void drawCover(Rect tile, const CoverArt::Art& art, float radius);
    void openEntry(const FileEntry& e);
    void entryMenu(const FileEntry& e);
    void sortMenu();
    void readBenchmark(const std::string& path, const std::string& name);

    // File manager (manager.cpp)
    bool managerMode() const { return section_ == MANAGER; }
    void managerMenu(const FileEntry& e);
    void managerOpenFile(const FileEntry& e);
    void managerButtons(const FileEntry* focused);
    bool managerBack();
    // Selecting several files: Files and the File Manager both have it.
    bool selectMode() const { return selecting_ || !marked_.empty(); }
    void endSelect() { selecting_ = false; marked_.clear(); }
    void selectAll();
    void setClipboard(const std::vector<std::string>& paths, bool move);
    void folderMenu();
    void drawSelectBar();
    void toggleMark(const std::string& path);
    bool isMarked(const std::string& path) const;
    std::vector<std::string> actionTargets(const FileEntry& e) const;
    void startCopy(const std::vector<std::string>& paths, const std::string& dest, bool move);
    void paste();
    void pasteWith(ConflictPolicy policy);
    void askDelete(const std::vector<std::string>& paths);
    void askRename(const FileEntry& e);
    void askNewFolder();
    void askSearch();
    void askExtract(const std::string& archive, const std::string& dest);
    void askCompress(const std::vector<std::string>& paths);
    void showProperties(const std::vector<std::string>& paths);
    void openTextViewer(const std::string& path);
    void drawJob();
    void drawTextViewer();
    void startJob(FileJob* job, std::function<void(FileJob&)> onDone);
    void goUp();
    bool settingRow(const SettingDef& d, Rect r, uint32_t id);
    void homeIconRow(Rect r, uint32_t id);
    void settingChanged(const SettingDef& d);

    Section section_ = FILES;
    float sectionTime_ = 0;
    std::string dir_;
    std::vector<FileEntry> entries_;
    float listTime_ = 0;
    std::map<std::string, std::string> dirFocus_;  // dir -> last focused path
    ScrollView scroll_;
    ScrollView settingsScroll_;
    int settingsTab_ = 0;
    uint32_t pendingFocus_ = 0;

    // File manager state
    std::vector<std::string> marked_;
    bool selecting_ = false;
    bool transferOpen_ = false;
    void drawTransfer();
    // First start: choose the text size, with a live preview.
    bool setupOpen_ = false;
    void drawSetup();
    // About page (about.cpp): contact details and donation addresses, with QR codes.
    float drawAbout(Rect list, float y);
    void drawUpdatePill(float x, float y, float maxW, float contentTop, Rect list);
    bool updateAsked_ = false;
    int setupStep_ = 0;  // first start: 0 language, 1 text size
    void openQr(const std::string& title, const std::string& text, const std::string& shown, const std::string& note);
    void drawQrViewer();
    void askFactoryReset();
    bool qrOpen_ = false;
    float qrTime_ = 0, aboutH_ = 1400;
    std::string qrTitle_, qrText_, qrShown_, qrNote_;
    std::vector<std::string> clipboard_;
    bool clipboardMove_ = false;
    std::string extractPending_;  // archive waiting for "paste" as a destination
    // Network places (netfs.hpp): folders on a server are listed on another thread.
    struct NetListing;
    std::shared_ptr<NetListing> netJob_;
    std::map<std::string, std::pair<double, std::vector<FileEntry>>> netCache_;  // dir -> (time, entries)
    std::string netError_;
    bool netLoading_ = false, netJustOpened_ = false;
    // Network has its own place, separate from Files / File Manager: each keeps where it was.
    std::string netPlace_, localDir_ = "\x01";  // \x01: not set yet
    bool openedOnce_ = false;  // the saved folder opens once, at the start
    void startNetListing(bool force);
    void pollNetListing();
    void askServer(int index);  // -1: a new one
    void askServerDetails(int index, const std::string& type);
    void serverMenu(const FileEntry& e);
    bool netDir() const;
    bool searching_ = false;
    std::string searchTerm_;
    std::unique_ptr<FileJob> job_;
    std::function<void(FileJob&)> jobDone_;
    bool textOpen_ = false;
    std::string textTitle_;
    std::vector<std::string> textRows_;
    ScrollView textScroll_;
    int contextIndex_ = -1;
};

// ---------------------------------------------------------------- Player

class PlayerScreen : public Screen {
public:
    explicit PlayerScreen(App& a) : Screen(a) {}
    void enter() override;
    void frame() override;
    void handleEvents(const std::vector<PlayerEvent>& evs);
    void play(const std::vector<std::string>& files, int index);
    void exit();
    bool musicMode() const;  // audio-only: the music player instead of the video controls
    bool closeIfOn(const std::string& root);  // a drive was removed: stop if playing from it
    bool videoMode() const;
    // For developer status: what's showing.
    bool osdShown() const { return osd_; }
    int panelShown() const { return (int)panel_; }
    bool lockedNow() const { return locked_; }

private:
    enum Panel { P_NONE, P_SUBS, P_SUBFILES, P_AUDIO, P_SPEED, P_VIDEO, P_PLAYLIST, P_CHAPTERS, P_MORE, P_VISUAL,
                 P_SUBBROWSE, P_SUBONLINE };
    bool subPanel(Panel p) const { return p == P_SUBFILES || p == P_SUBBROWSE || p == P_SUBONLINE; }
    // Choosing a subtitle file from any folder or drive.
    std::string subBrowseDir_;
    std::vector<FileEntry> subBrowse_;
    void subBrowseOpen(const std::string& dir);
    // Finding one online.
    SubSearch subSearch_;
    std::string subQuery_, subSearchedFor_;
    long long subDownloading_ = 0;
    void subSearchStart();
    void subLoadDownloaded();
    enum Gesture { G_NONE, G_SEEK, G_VOLUME, G_BRIGHTNESS, G_SCRUB, G_PAN };
    enum Hud { H_NONE, H_VOLUME, H_BRIGHTNESS, H_SEEK, H_ZOOM, H_SPEED, H_TEXT, H_PLAY, H_PAUSE };

    void handleGestures();
    void handleButtons();
    void handleImageGestures();
    void doAction(Action a);
    void showOsd() { osdTimer_ = 0; osd_ = true; }
    void hud(Hud type, float value, const std::string& text = "", Icon ic = Icon::None);
    void seekRelative(double secs, bool showHud = true);
    void changeVolume(double delta);
    void changeBrightness(float delta);
    void openPanel(Panel p);
    void loadExternalSubtitles();
    std::string prepareSub(const std::string& path);

    // Music player (player_music.cpp)
    void drawMusic(bool parentBlocked);
    // Screen off while music plays: a short notice, then the backlight goes out until a double tap.
    float screenOffIn_ = -1;  // seconds left of the notice before the light goes out
    float musicIdle_ = 0, darkTap_ = -10;
    double darkTapAt_ = -10, darkClock_ = 0;
    bool screenDark_ = false;
    void requestScreenOff();
    void wakeScreen();
    bool drawScreenOff();  // true while it owns the screen
public:
    void saveProgress();  // the resume point of what's playing
    std::string historyTitle() const;
    bool dark() const { return screenDark_; }
    // While dark nothing is drawn: the console may stop refreshing a screen whose light is off, and
    // waiting for it would freeze the app. This only watches for the wake-up. False once awake.
    bool darkTick();
private:
    void handleMusicButtons();
    void nextVisualizer(int dir);
    // Picture viewer (player_pictures.cpp)
    void drawPictureUi(float a);
    void drawPictureInfo();
    void drawPicturePlaceholder();
    void handlePictureButtons();
    void tickSlideshow(float dt);
    void toggleSlideshow();
    float slideInterval() const;
    void deletePicture();
    void drawOsd(float a);
    void drawSeekBar(Rect r, float a);
    void drawOpening();
    bool opening() const;  // a video is still opening: its own screen instead of the controls
    void drawHud();
    void drawDoubleTap();
    void drawPanel();
    void drawPanelBody();
    void drawLocked();
    void drawMiniProgress(float a);
    bool panelRow(uint32_t id, Rect r, const std::string& label, const std::string& detail, bool checked,
                  Icon ic = Icon::None);
    bool stepperRow(uint32_t id, Rect r, const std::string& label, const std::string& value, int& dir);
    bool sliderRow(uint32_t id, Rect r, const std::string& label, float& v, float lo, float hi, float step,
                   const char* fmt);

    bool osd_ = true;
    float osdTimer_ = 0;
    float osdAlpha_ = 1;
    Panel panel_ = P_NONE;
    float panelAnim_ = 0;
    Panel lastPanel_ = P_NONE;
    ScrollView panelScroll_;
    bool locked_ = false;
    float lockHint_ = 0;

    Gesture gesture_ = G_NONE;
    double gestureBase_ = 0;
    double seekTarget_ = 0;
    float lastPreviewSeek_ = 0;
    float gestureValue_ = 0;
    double zoomBase_ = 0, panBaseX_ = 0, panBaseY_ = 0;
    float pinchStartX_ = 0, pinchStartY_ = 0;
    bool pinching_ = false;

    Hud hud_ = H_NONE;
    float hudTimer_ = 0, hudValue_ = 0;
    std::string hudText_;
    Icon hudIcon_ = Icon::None;

    int dtSide_ = 0;  // -1 left, 1 right
    float dtTimer_ = 0;
    int dtAmount_ = 0;
    float dtX_ = 0, dtY_ = 0;

    // Holding on the video: right side plays faster, left side rewinds, both speeding up over time.
    bool boosting_ = false;
    int boostDir_ = 1;          // 1 forward, -1 rewind
    float boostTime_ = 0;       // how long the hold has lasted
    float boostRate_ = 0;       // current rate shown and applied
    double boostPrevSpeed_ = 1;
    bool boostWasPaused_ = false;
    double rewindPos_ = 0;      // where rewinding has got to
    float rewindSeekTimer_ = 0;
    void endBoost();
    float boostRateFor(float held) const;

    bool sawPlaying_ = false;
    // The entry that failed to load, while mpv is still holding it with nothing loaded. A dead
    // address in a stream list leaves it there, so without this the opening screen would spin
    // "Opening" for as long as the user let it, long after the error toast had faded. Keeping the
    // path rather than a flag clears it by itself: the moment mpv moves to another entry, this no
    // longer matches and the spinner is right again.
    std::string failedPath_;
    std::string chosenPath_;  // the file play() was asked for (not the rest of its queue)
    bool anyLoaded_ = false;
    float openTime_ = 0;
    float saveTimer_ = 0;
    double lastSavedPos_ = -1;
    float sleepRemaining_ = -1;  // seconds, -1 = off
    double sleepFadeFrom_ = -1;  // the volume before the timer's fade-out
    bool tickSleep(float dt);
    void endSleepFade();
    bool sleepEndOfFile_ = false;
    int aspectIdx_ = 0;
    int eq_[5] = {0, 0, 0, 0, 0};
    std::vector<std::string> subFiles_;
    uint32_t seekBarId_ = 0;
    float time_ = 0;
    std::string currentPath_;
    double curDur_ = 0;
    int sleepIdx_ = 0;
    float panelContentH_ = 0;
    bool panelFocusPending_ = false;
    float trackHudDelay_ = 0;
    int trackHudType_ = 0;
    float bufferingTime_ = 0, seekingTime_ = 0;
    float backlightAtStart_ = -1;
    bool imageMode_ = false;
    bool musicKind_ = false;  // the current file is audio
    vis::Analyzer analyzer_;
    vis::State vis_;
    bool immersive_ = false;
    double musicClock_ = 0, lastPos_ = -1;
    float visStarved_ = 0;          // seconds of playback the visualizer had no sound data for
    std::string visReported_;       // the song that was already noted in the event log
    // Memory: checked once a second while playing (see memoryWatch).
    float resumePill_ = 0;         // seconds the "Resumed at … · Start over" pill still shows
    double resumedFrom_ = 0;
    bool nextPill_ = false;        // "Up next" is showing (Y plays it)
    bool swDecode_ = false;
    bool deinterlace_ = false;      // Video panel, for the files being played        // the hardware decoder turned off for this session (Video panel)
    void drawPills();
    void askGoToTime();
    int subPosApplied_ = -1;       // where the subtitles are now (raised above the controls)
    int pendingChapter_ = -1;       // the chapter last jumped to, until mpv reports it
    double pendingChapterAt_ = -10;
    float memTimer_ = 0;
    int memStep_ = 0;               // last tenth of memory use written to the log
    bool memReloaded_ = false;      // the file was already reopened to free memory
    double forceResumeAt_ = -1;     // where a reopened file starts
    void memoryWatch(float dt);
    // Above 1440p (4K films): simpler downscaling and the CPU at 1785 MHz while it plays; measured
    // on the console, 10-bit 4K HEVC went from about a quarter of its frames dropped to about 4%.
    void heavyVideoWatch(float dt);
public:
    void endHeavyVideo();  // the usual clock and scaling again (leaving the player or the app)
private:
    bool heavyVideo_ = false;
    float heavyTimer_ = 0;
    bool trackRestore_ = false;     // the video's remembered tracks are still to be chosen
    int trackRestored_ = 0;         // 1 audio, 2 subtitles: already set
    void noteTrack(const char* type, const Track* t);  // remembers a choice (nullptr: off)
    void restoreTracks();
    Lyrics lyrics_;                 // of the song playing
    std::string lyricsKey_;         // which song (and tag set) lyrics_ was looked up for
    float lyricsScroll_ = 0;        // the line in view, gliding toward the one being sung
    void drawLyrics(Rect r, bool interactive);
    float vinylAngle_ = 0, styleFlash_ = 0, glow_ = 0, musicWait_ = 0;
    // Changing songs: the last song's cover and words stay a moment to slide away (see drawMusic).
    struct SongLook {
        CoverArt::Art art;
        bool hasArt = false;
        std::string title, artist, album, seed;
    };
    SongLook prevSong_, lastSong_;
    std::string songKey_;
    float songT_ = 1;  // 0 -> 1 over the change
    int songDir_ = 1;  // +1 forward (next), -1 back
    int64_t songPos_ = -1;
    float coverR_ = 0.3f, coverG_ = 0.4f, coverB_ = 0.9f;
    bool shuffled_ = false;
    bool infoOpen_ = false, slideshow_ = false;
    float slideTimer_ = 0, stripScroll_ = -1;
    std::string infoPath_;
    uint64_t infoSize_ = 0;
    time_t infoDate_ = 0;
    float inputIdle_ = 0;
    bool controllersOff_ = false;
};

// ---------------------------------------------------------------- App

class App {
public:
    bool init();
    void run();
    void shutdown();

    void switchTo(Screen* s);
    void playPath(const std::string& path);
    void playFiles(const std::vector<std::string>& files, int index);
    void settingsChanged();
    void applyUiFont();
    static const char* uiFontName(int index);
    static int uiFontCount();
    // Saves the settings a second from now, without re-applying them (a folder opened, a view toggled).
    void saveSoon() { if (!erased) cfgSaveIn_ = 1.0f; }

    void openMenu(const std::string& title, const std::vector<MenuItem>& items, std::function<void(int)> cb,
                  const std::string& message = "");
    void confirm(const std::string& title, const std::string& message, const std::string& okLabel,
                 std::function<void()> onOk, bool danger = false);
    bool modalOpen() const { return modal_.open; }

    Config cfg;
    Library lib;
    Player player;
    CoverArt covers;

    // HOME Menu icon
    bool forwarderAvailable();
    bool forwarderInstalled();
    bool forwarderRunningFromIcon();
    // Adds the icon now, or as soon as the console's keys are on the SD card (checked at every start).
    void forwarderInstall(bool quiet = false);
    void forwarderRemove();
    bool forwarderPending();  // asked for, but waiting for the keys
    bool forwarderOutdated();  // installed, but an older build than this app carries
    void setForwarderPending(bool on);

    // Mounted drives, refreshed once a second, for what is drawn every frame
    const std::vector<platform::StorageRoot>& drives() const { return driveRoots_; }

    // Likes and playlists, from any screen
    void addToPlaylistMenu(const std::vector<std::string>& paths);
    bool canCollect(const std::string& path) const;  // music or video on local storage
    void toggleLike(const std::string& path);
    Input in;
    UI ui;
    NVGcontext* vg = nullptr;
    HomeScreen* home = nullptr;
    PlayerScreen* playerScreen = nullptr;
    ReaderScreen* reader = nullptr;  // documents (reader.cpp)
    void openDocument(const std::string& path);
    // Wi-Fi transfer: a file manager for browsers on the same network.
    WebServer web;
    bool webStart();
    void webStop();
    std::string webAddress() const;
    Screen* screen = nullptr;
    bool running = true;
    // One line per notable event in switch/SwitchMX/events.log, so problems can be traced afterwards.
    void logEvent(const std::string& what);
    // Factory reset: deletes everything SwitchMX saved (settings, history, playlists, likes,
    // favourites, covers, caches, logs). The app itself and its screenshots stay. Returns false if
    // something couldn't be removed. The app must close afterwards.
    bool eraseAllData(std::string& error);
    std::string selfPath;  // this .nro, as the loader started it
    Updater updater;
    DevTools dev{*this};  // developer mode: testing from a computer (devtools.hpp)
    // Where an update is written: the running .nro, or SwitchMX's usual place.
    std::string nroPath() const;
    bool erased = false;  // after a reset nothing more is written on the way out
    std::string lastPlayed;  // the file showing when the player closed, so the browser can return to it
    // The system keyboard; says so when it can't open instead of looking like nothing happened.
    bool askText(const std::string& header, const std::string& initial, std::string& out);
    void playStream(const std::string& url, const std::string& name);
    std::string streamName(const std::string& url) const;  // its name in the Streams list, or ""
    bool playerReady = false;
    std::string dataDir;
    float time = 0;

private:
    friend class DevTools;
    void drawModal();
    void askForwarderOnce();
    int forwarderState_ = -1;  // -1 unknown, 0 not installed, 1 installed
    bool forwarderAsked_ = false;
    void drawIconSheet();
    void drawRotatedVideo(float W, float H);
    int videoImage_ = 0, videoImageW_ = 0, videoImageH_ = 0;
    // Plugging in the charger shows a short animation; unplugging says so.
    void watchCharger(float dt);
    void drawChargeSplash();
    int chargerState_ = -1;  // -1 unknown, 0 unplugged, 1 charging
    float chargeSplash_ = 0;
    void drawSplash(float dt);
    float splashTime_ = 0;
    bool splashDone_ = false;
    void drawDebugOverlay();
    void writeDiagnostics(float dt);
    void watchDrives(float dt);
    void updateWeb(float dt);
    float cfgSaveIn_ = -1;
    // Headphones or Bluetooth gone: pause, as phones do, instead of blaring from the speaker.
    void watchAudioOutput(float dt);
    int audioOut_ = 0;
    float webRootsTimer_ = 0;
    float driveTimer_ = 0;
    std::vector<std::string> drivePaths_;
    std::vector<platform::StorageRoot> driveRoots_;
    bool wasDocked_ = false;
    float diagTimer_ = 0;
    int diagFrames_ = 0;
    static constexpr int kFrameHistory = 120;
    float frameTimes_[kFrameHistory] = {};
    int frameIndex_ = 0;
    float cpu_[4] = {};
    int cpuCores_ = 0;
    float cpuTimer_ = 0;

    struct Modal {
        bool open = false;
        bool closing = false;
        std::string title, message;
        std::vector<MenuItem> items;
        std::function<void(int)> cb;
        float anim = 0;
        uint32_t savedFocus = 0;
        int result = -1;
        ScrollView scroll;
    } modal_;
    float screenFade_ = 1;
    int fontRegular_[24], fontBold_[24];  // one per bundled typeface (kUiFonts in app.cpp)
    std::map<Screen*, uint32_t> savedFocus_;
};
