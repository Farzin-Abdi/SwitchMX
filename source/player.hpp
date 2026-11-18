#pragma once
#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

class Config;

struct Track {
    int64_t id = 0;
    std::string type;  // "video", "audio", "sub"
    std::string title, lang, codec;
    bool selected = false, external = false, isDefault = false, forced = false;
    int64_t channels = 0;
    int64_t w = 0, h = 0;
    std::string label() const;
};

struct Chapter {
    std::string title;
    double time = 0;
};

enum class PlayerEvent { FileStarted, FileLoaded, FileEnded, PlaylistFinished, Error, EndedEarly };

class Player {
public:
    bool init(const Config& cfg, const std::string& configDir, const std::string& screenshotDir);
    bool initRender();
    void shutdown();
    void applySettings(const Config& cfg);

    void open(const std::vector<std::string>& files, int index);

    // Asked for every file as it loads (mpv's on_load hook); return the resume position or 0.
    std::function<double(const std::string& path)> resumeLookup;
    double resumedAt = 0;  // resume position applied to the current file
    void stop();

    // Drains mpv events. Returns events relevant to the app.
    std::vector<PlayerEvent> poll();
    void render(int fbo, int w, int h);
    void reportSwap();
    void stopLogging() { setAsync("log-file", ""); }
    // A turned frame for the app to draw itself: its texture and size, or false.
    bool rotatedFrame(unsigned& tex, int& w, int& h) const {
        if (!rotation || noFbo_ || !fbo_ || blank_) return false;
        tex = tex_;
        w = fboW_;
        h = fboH_;
        return true;
    }
    void renderTo(int fbo, int w, int h);

    // Controls
    void togglePause();
    void setPause(bool p);
    void seek(double secs, bool relative);
    void seekPreview(double secs);  // fast keyframe seek while scrubbing
    void setVolume(double v);
    void setMute(bool m);
    void setSpeed(double s);
    void setTrack(const char* type, int64_t id);  // id 0 disables
    void cycleTrack(const char* type);
    void addSubtitle(const std::string& path, bool select, const std::string& title = "", const std::string& lang = "");
    bool subtitleSelected();
    void setSubDelay(double d);
    void setAudioDelay(double d);
    void setZoom(double log2zoom);
    void setPan(double x, double y);
    void setRotation(int deg);
    void setAspect(const char* aspect);
    void setEq(const char* prop, int value);  // brightness/contrast/saturation/gamma/hue -100..100
    void playIndex(int i);
    void next();
    void prevFile();
    void prev();
    void setChapter(int i);
    bool screenshot();
    void loadEntry(const std::string& file, const char* mode);
    std::map<std::string, std::string> titles;  // names for streams (address -> name)
    void abLoop();
    void setLoopFile(bool on);
    void setProperty(const char* name, const char* value);
    double getDouble(const char* name, double fallback = 0);

    // Observed state
    bool active = false;  // a file is loaded or loading
    bool loaded = false;
    double position = 0, duration = 0;
    bool paused = false, buffering = false, seeking = false, idle = true, mute = false;
    double volume = 100, speed = 1;
    int64_t cacheBuffering = 100;
    double cacheTime = 0;
    std::string title, path, videoCodec, audioCodec, hwdecCurrent;
    int64_t width = 0, height = 0;
    double fps = 0;
    int64_t droppedFrames = 0, decoderDrops = 0;
    double outputFps = 0, avsync = 0, videoBitrate = 0, audioBitrate = 0;
    int64_t readSpeed = 0, cacheBytes = 0;  // bytes/s, bytes buffered ahead
    std::vector<Track> tracks;
    std::vector<Chapter> chapters;
    int64_t chapter = -1;
    int64_t playlistPos = 0, playlistCount = 0;
    std::vector<std::string> playlist;
    // The name each entry carried in the .m3u (#EXTINF). An IPTV list names its channels there, and
    // the addresses all end in the same index.m3u8 or mono.m3u8, so the name is all there is to show
    // before the stream opens. Empty where the playlist gave none.
    std::vector<std::string> playlistTitles;
    double subDelay = 0, audioDelay = 0;
    double zoom = 0, panX = 0, panY = 0;
    int rotation = 0;
    double abA = -1, abB = -1;
    std::string lastError;
    // The entry that failed (lastError) as the app names it. Not `path`: by the time the error is
    // handled mpv has usually moved on to the next entry, and `path` already names that one.
    std::string lastErrorPath;
    // Where the file that just ended (FileEnded) was: `position` already belongs to the next one,
    // since mpv starts it before the end is handled.
    double endedPosition = 0;
    // A file that reached its end long before its length (EndedEarly): damaged or cut short.
    std::string endedEarlyPath;
    double endedEarlyAt = 0, endedEarlyLength = 0;
    bool hasVideo() const;
    // Developer mode: how much of mpv's log goes to the developer log ("warn", "v", ...).
    void setDevLogLevel(const std::string& level);

    // Tags of the current file (lower-case keys: title, artist, album, date, genre, ...).
    std::map<std::string, std::string> metadata;
    int64_t seekCount = 0;  // seeks and chapter jumps since start (for the memory log)
    std::string tag(const char* key) const;
    mpv_handle* handle() const { return mpv_; }  // developer mode (devtools.cpp)
    bool queueLoading() const { return queueNext_ < queuePending_.size(); }
    void shuffle(bool on);
    void removeCurrent();  // drops the current entry from the queue; mpv moves on to the next

private:
    std::string afApplied_ = "\x01";  // the audio filters last given to mpv
    void command(std::initializer_list<const char*> args);
    void parseMetadata(mpv_node* n);
    void setAsync(const char* name, const std::string& value);
    uint64_t loadGen_ = 0;
    void handleProperty(mpv_event_property* p, uint64_t id);
    void parseTracks(mpv_node* n);
    void parseChapters(mpv_node* n);

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ = nullptr;
    // mpv draws into this only when it has a new frame (24-30 times a second for most films); every
    // UI frame just copies it, which is far cheaper than scaling and converting the video 60 times.
    unsigned fbo_ = 0, tex_ = 0;
    int fboW_ = 0, fboH_ = 0;
    bool noFbo_ = false;
    bool renderedFrame_ = false;
    // From the start of a new file until its first frame is drawn, the previous file's last frame
    // must not be shown.
    bool blank_ = true, unblankOnDraw_ = false;
    std::atomic<bool> dirty_{true};
    static void onRenderUpdate(void* self);
    struct QueueItem {
        std::string file;
        int moveTo;  // position to move it to once appended, or -1 to leave it at the end
    };
    std::string queueUrl(const std::string& file) const;
    void pumpQueue();
    std::vector<QueueItem> queuePending_;
    size_t queueNext_ = 0;
    int queueCount_ = 0;
    std::string logPath_;
    // Settings > Debug > Save debug log. Written by the app, not mpv's log-file: that one turns on
    // debug output everywhere and writes it from the thread that produced it, and HLS parsing (a
    // line per playlist entry, thousands per reload) then ran so slowly that live TV never opened.
    FILE* logFile_ = nullptr;
    size_t logBytes_ = 0;
    int devLogLevel_ = 30;  // MPV_LOG_LEVEL_WARN
    void requestLogLevel();
    void writeLog(const mpv_event_log_message* m);
    std::string entryPath(int64_t entryId);
    bool startPending_ = false;
};
