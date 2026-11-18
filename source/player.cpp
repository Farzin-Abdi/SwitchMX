#include "player.hpp"
#include "netfs.hpp"

#include <sys/stat.h>

#include <cctype>
#include <clocale>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

#include "config.hpp"
#include "gl.h"
#include "library.hpp"
#include "platform.hpp"
#include "readahead.hpp"
#include "devtools.hpp"

namespace {
// Less than 1 GB for the whole app: the album's (applet) share of memory.
bool memoryTight() {
    unsigned long long used = 0, total = 0;
    return platform::memoryUsage(used, total) && total > 0 && total < (1ull << 30);
}
}  // namespace

namespace {
enum Prop : uint64_t {
    P_TIME_POS = 1,
    P_DURATION,
    P_PAUSE,
    P_VOLUME,
    P_MUTE,
    P_SPEED,
    P_TRACKS,
    P_CHAPTERS,
    P_CHAPTER,
    P_TITLE,
    P_PAUSED_CACHE,
    P_CACHE_STATE,
    P_CACHE_TIME,
    P_IDLE,
    P_PL_POS,
    P_PL_COUNT,
    P_PATH,
    P_WIDTH,
    P_HEIGHT,
    P_VCODEC,
    P_ACODEC,
    P_HWDEC,
    P_FPS,
    P_DROPS,
    P_SUB_DELAY,
    P_AUDIO_DELAY,
    P_ZOOM,
    P_PAN_X,
    P_PAN_Y,
    P_ROTATE,
    P_AB_A,
    P_AB_B,
    P_SEEKING,
    P_VFPS,
    P_DEC_DROPS,
    P_AVSYNC,
    P_VBITRATE,
    P_ABITRATE,
    P_PLAYLIST,
    P_CACHE_SPEED,
    P_CACHE_FWD,
    P_METADATA,
};

// Replies to the questions asked when a file has loaded; the load generation sits above these.
enum Reply : uint64_t { R_POS = 1, R_DURATION, R_TIME, R_PATH };

void* getProc(void*, const char* name) { return platform::glProcAddress(name); }

const char* nodeString(mpv_node* map, const char* key) {
    if (map->format != MPV_FORMAT_NODE_MAP) return nullptr;
    for (int i = 0; i < map->u.list->num; i++)
        if (!strcmp(map->u.list->keys[i], key) && map->u.list->values[i].format == MPV_FORMAT_STRING)
            return map->u.list->values[i].u.string;
    return nullptr;
}

bool nodeInt(mpv_node* map, const char* key, int64_t& out) {
    if (map->format != MPV_FORMAT_NODE_MAP) return false;
    for (int i = 0; i < map->u.list->num; i++) {
        if (strcmp(map->u.list->keys[i], key)) continue;
        mpv_node& v = map->u.list->values[i];
        if (v.format == MPV_FORMAT_INT64) { out = v.u.int64; return true; }
        if (v.format == MPV_FORMAT_DOUBLE) { out = (int64_t)v.u.double_; return true; }
    }
    return false;
}

bool nodeDouble(mpv_node* map, const char* key, double& out) {
    if (map->format != MPV_FORMAT_NODE_MAP) return false;
    for (int i = 0; i < map->u.list->num; i++) {
        if (strcmp(map->u.list->keys[i], key)) continue;
        mpv_node& v = map->u.list->values[i];
        if (v.format == MPV_FORMAT_DOUBLE) { out = v.u.double_; return true; }
        if (v.format == MPV_FORMAT_INT64) { out = (double)v.u.int64; return true; }
    }
    return false;
}

bool nodeFlag(mpv_node* map, const char* key) {
    if (map->format != MPV_FORMAT_NODE_MAP) return false;
    for (int i = 0; i < map->u.list->num; i++)
        if (!strcmp(map->u.list->keys[i], key) && map->u.list->values[i].format == MPV_FORMAT_FLAG)
            return map->u.list->values[i].u.flag != 0;
    return false;
}

std::string langName(const std::string& code) {
    static const char* table[][2] = {
        {"eng", "English"},   {"en", "English"},    {"jpn", "Japanese"}, {"ja", "Japanese"}, {"spa", "Spanish"},
        {"es", "Spanish"},    {"fre", "French"},    {"fra", "French"},   {"fr", "French"},   {"ger", "German"},
        {"deu", "German"},    {"de", "German"},     {"ita", "Italian"},  {"it", "Italian"},  {"por", "Portuguese"},
        {"pt", "Portuguese"}, {"rus", "Russian"},   {"ru", "Russian"},   {"kor", "Korean"},  {"ko", "Korean"},
        {"chi", "Chinese"},   {"zho", "Chinese"},   {"zh", "Chinese"},   {"ara", "Arabic"},  {"ar", "Arabic"},
        {"per", "Persian"},   {"fas", "Persian"},   {"fa", "Persian"},   {"hin", "Hindi"},   {"tur", "Turkish"},
        {"pol", "Polish"},    {"dut", "Dutch"},     {"nld", "Dutch"},    {"swe", "Swedish"}, {"tha", "Thai"},
        {"vie", "Vietnamese"}, {"ind", "Indonesian"}, {"heb", "Hebrew"}, {"gre", "Greek"},   {"ukr", "Ukrainian"},
    };
    for (auto& t : table)
        if (code == t[0]) return t[1];
    return code;
}
}  // namespace

std::string Track::label() const {
    std::string s;
    if (!title.empty()) s = title;
    if (!lang.empty()) {
        std::string l = langName(lang);
        if (s.empty()) s = l;
        else if (s.find(l) == std::string::npos) s = l + " · " + s;
    }
    if (s.empty()) s = "Track " + std::to_string(id);
    return s;
}

bool Player::hasVideo() const {
    for (auto& t : tracks)
        if (t.type == "video" && t.selected) return true;
    return false;
}

bool Player::init(const Config& cfg, const std::string& configDir, const std::string& screenshotDir) {
    setlocale(LC_NUMERIC, "C");
    logPath_ = parentDir(configDir) + "mpv-log.txt";
    mpv_ = mpv_create();
    if (!mpv_) return false;

    if (!configDir.empty()) {
        // mpv notes "two config dirs found" in the log at startup: the console has no home
        // directory, so the second place it looks comes out empty. Setting MPV_HOME as well made no
        // difference, and it is a log line only, with nothing shown to the user.
        // Users can drop an mpv.conf / input.conf / fonts here for full control.
        mpv_set_option_string(mpv_, "config-dir", configDir.c_str());
        mpv_set_option_string(mpv_, "config", "yes");
        // Compiled shaders are kept, so a video's first frame isn't held up compiling them again.
        mkdir((configDir + "shadercache").c_str(), 0777);
        mpv_set_option_string(mpv_, "gpu-shader-cache-dir", (configDir + "shadercache").c_str());
    }
    mpv_set_option_string(mpv_, "vo", "libmpv");
    mpv_set_option_string(mpv_, "idle", "yes");
    mpv_set_option_string(mpv_, "keep-open", "no");
    // Music has its own screen with the cover art drawn by the app, so mpv shows no picture for it.
    mpv_set_option_string(mpv_, "audio-display", "no");
    mpv_set_option_string(mpv_, "osc", "no");
    mpv_set_option_string(mpv_, "osd-level", "0");
    mpv_set_option_string(mpv_, "input-default-bindings", "no");
    mpv_set_option_string(mpv_, "input-vo-keyboard", "no");
    mpv_set_option_string(mpv_, "ytdl", "no");
    mpv_set_option_string(mpv_, "terminal", "no");
    // Read ahead aggressively for SD-card files too: some files read slowly in places (e.g. when
    // fragmented), and a large cache filled during fast stretches carries playback through them.
    mpv_set_option_string(mpv_, "cache", "yes");
    mpv_set_option_string(mpv_, "cache-pause-initial", "no");
    mpv_set_option_string(mpv_, "cache-pause-wait", "2");
    // Somewhat larger reads than the 128 KiB default; very large blocks hurt files whose
    // audio and video are stored far apart (every jump re-reads a whole block).
    mpv_set_option_string(mpv_, "stream-buffer-size", "512KiB");
    mpv_set_option_string(mpv_, "demuxer-readahead-secs", "5");
    // Give up on an address that never answers. Without a limit mpv waits on the connection for as
    // long as the console stays awake, and because it waits on the thread that runs playback, every
    // later command queues behind it: open one dead channel in an IPTV list and nothing - not even a
    // file on the card - plays again until SwitchMX is restarted. Thirty seconds is longer than any
    // working stream here takes to answer, and a stream that is merely slow keeps its own cache.
    mpv_set_option_string(mpv_, "network-timeout", "30");
    // SwitchMX keeps its own resume points; mpv's would look for a file per playlist entry, which
    // took over a minute for a 10,000-channel list.
    mpv_set_option_string(mpv_, "resume-playback", "no");
    // A playlist on the card is the user's own: its FTP (and other) addresses play too. mpv
    // otherwise only follows http(s) entries and refused every ftp:// line in an .m3u.
    mpv_set_option_string(mpv_, "load-unsafe-playlists", "yes");
    mpv_set_option_string(mpv_, "screenshot-directory", screenshotDir.c_str());
    mpv_set_option_string(mpv_, "screenshot-template", "SwitchMX-%F-%wH-%wM-%wS"  /* no ":" — FAT cards refuse it */);
    mpv_set_option_string(mpv_, "screenshot-format", "jpg");
    mpv_set_option_string(mpv_, "reset-on-next-file", "video-zoom,video-pan-x,video-pan-y,video-rotate,"
                                                      "video-aspect-override,sub-delay,audio-delay,ab-loop-a,ab-loop-b");
#ifdef __SWITCH__
    mpv_set_option_string(mpv_, "vd-lavc-dr", "yes");
#endif
#ifdef __SWITCH__
    // Auto-detection can see a single core on Horizon; homebrew gets three.
    std::string threads = cfg.s.threads == 0 ? "3" : std::to_string(cfg.s.threads + 1);
#else
    std::string threads = cfg.s.threads == 0 ? "0" : std::to_string(cfg.s.threads + 1);
#endif
    mpv_set_option_string(mpv_, "vd-lavc-threads", threads.c_str());
    requestLogLevel();
    // Resume positions are decided per file at load time (works for queues, replays and loops).
    mpv_hook_add(mpv_, 1, "on_load", 50);

    if (mpv_initialize(mpv_) < 0) {
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }
    applySettings(cfg);
    netfs::registerStreams(mpv_);
    registerReadAhead(mpv_, (size_t)(memoryTight() ? 12 : std::max(16, cfg.demuxMBValue() / 2)) << 20);

    mpv_observe_property(mpv_, P_TIME_POS, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_DURATION, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_PAUSE, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, P_VOLUME, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_MUTE, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, P_SPEED, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_TRACKS, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, P_CHAPTERS, "chapter-list", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, P_CHAPTER, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_TITLE, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, P_PAUSED_CACHE, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, P_CACHE_STATE, "cache-buffering-state", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_CACHE_TIME, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_IDLE, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, P_PL_POS, "playlist-pos", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_PL_COUNT, "playlist-count", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_PATH, "path", MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, P_WIDTH, "width", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_HEIGHT, "height", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_VCODEC, "video-format", MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, P_ACODEC, "audio-codec-name", MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, P_HWDEC, "hwdec-current", MPV_FORMAT_STRING);
    mpv_observe_property(mpv_, P_FPS, "container-fps", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_DROPS, "frame-drop-count", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_SUB_DELAY, "sub-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_AUDIO_DELAY, "audio-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_ZOOM, "video-zoom", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_PAN_X, "video-pan-x", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_PAN_Y, "video-pan-y", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_ROTATE, "video-rotate", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_AB_A, "ab-loop-a", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_AB_B, "ab-loop-b", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_SEEKING, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv_, P_VFPS, "estimated-vf-fps", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_DEC_DROPS, "decoder-frame-drop-count", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_AVSYNC, "avsync", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_VBITRATE, "video-bitrate", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_ABITRATE, "audio-bitrate", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv_, P_PLAYLIST, "playlist", MPV_FORMAT_NODE);
    mpv_observe_property(mpv_, P_CACHE_SPEED, "cache-speed", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_CACHE_FWD, "demuxer-cache-state/fw-bytes", MPV_FORMAT_INT64);
    mpv_observe_property(mpv_, P_METADATA, "metadata", MPV_FORMAT_NODE);
    return true;
}

bool Player::initRender() {
    if (!mpv_) return false;
    mpv_opengl_init_params gl{getProc, nullptr};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&render_, mpv_, params) < 0) return false;
    mpv_render_context_set_update_callback(render_, onRenderUpdate, this);
    return true;
}

// Called on one of mpv's threads; it may only note that something changed.
void Player::onRenderUpdate(void* self) { ((Player*)self)->dirty_ = true; }

void Player::shutdown() {
    if (render_) mpv_render_context_free(render_);
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (tex_) glDeleteTextures(1, &tex_);
    fbo_ = tex_ = 0;
    render_ = nullptr;
    if (mpv_) mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
    if (logFile_) fclose(logFile_);
    logFile_ = nullptr;
}

static std::string appPath(const std::string& s);

static int logLevelNumber(const std::string& s) {
    static const std::pair<const char*, int> levels[] = {
        {"no", 0}, {"fatal", 10}, {"error", 20}, {"warn", 30}, {"info", 40}, {"v", 50}, {"debug", 60}, {"trace", 70}};
    for (auto& l : levels)
        if (s == l.first) return l.second;
    return -1;
}

void Player::requestLogLevel() {
    if (!mpv_) return;
    int want = std::max(devLogLevel_, logFile_ ? 50 : 30);
    static const char* names[] = {"no", "fatal", "error", "warn", "info", "v", "debug", "trace"};
    mpv_request_log_messages(mpv_, names[std::min(7, std::max(0, want / 10))]);
}

void Player::setDevLogLevel(const std::string& level) {
    int n = logLevelNumber(level);
    devLogLevel_ = n < 0 ? 50 : n;
    requestLogLevel();
}

void Player::writeLog(const mpv_event_log_message* m) {
    if (!logFile_ || m->log_level > 50) return;
    // FFmpeg's HLS reader reports every playlist line it skips (thousands for a live channel with
    // catch-up, again on each reload); they say nothing and would fill the card.
    if (!strncmp(m->text, "hls: Skip ('", 12)) return;
    if (logBytes_ > (8u << 20)) {  // left on for long, it starts again rather than filling the card
        logFile_ = freopen(logPath_.c_str(), "w", logFile_);
        logBytes_ = 0;
        if (!logFile_) return;
    }
    char head[64];
    static const auto t0 = std::chrono::steady_clock::now();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int n = snprintf(head, sizeof head, "[%10.3f][%s] ", t, m->level);
    fputs(head, logFile_);
    fputs(m->prefix, logFile_);
    fputs(": ", logFile_);
    fputs(m->text, logFile_);
    logBytes_ += (n > 0 ? n : 0) + strlen(m->prefix) + strlen(m->text) + 2;
}

// The app's own name for one of mpv's playlist entries (by the id an event carries), or "".
std::string Player::entryPath(int64_t entryId) {
    std::string out;
    mpv_node list;
    if (!mpv_ || mpv_get_property(mpv_, "playlist", MPV_FORMAT_NODE, &list) < 0) return out;
    if (list.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < list.u.list->num && out.empty(); i++) {
            mpv_node& e = list.u.list->values[i];
            if (e.format != MPV_FORMAT_NODE_MAP) continue;
            int64_t id = -1;
            const char* file = nullptr;
            for (int k = 0; k < e.u.list->num; k++) {
                mpv_node& v = e.u.list->values[k];
                if (!strcmp(e.u.list->keys[k], "id") && v.format == MPV_FORMAT_INT64) id = v.u.int64;
                if (!strcmp(e.u.list->keys[k], "filename") && v.format == MPV_FORMAT_STRING) file = v.u.string;
            }
            if (id == entryId && file) out = appPath(file);
        }
    }
    mpv_free_node_contents(&list);
    return out;
}

void Player::applySettings(const Config& cfg) {
    if (!mpv_) return;
    const Settings& s = cfg.s;
    char buf[128];
    auto set = [&](const char* k, const std::string& v) { setAsync(k, v); };

    // Verbose mpv log on the SD card for diagnosing playback problems (see logFile_).
    if (s.debugLog && !logFile_) {
        logFile_ = fopen(logPath_.c_str(), "w");
        if (logFile_) setvbuf(logFile_, nullptr, _IOFBF, 64 << 10);
        logBytes_ = 0;
    } else if (!s.debugLog && logFile_) {
        fclose(logFile_);
        logFile_ = nullptr;
    }
    requestLogLevel();
    set("hwdec", cfg.hwdecValue());
    // Everything the video chip decodes except MJPEG: photos can exceed its size limits.
    set("hwdec-codecs", "h264,hevc,vp8,vp9,vc1,wmv3,mpeg1video,mpeg2video,mpeg4");
    // The picture viewer runs slideshows itself, so it can show their progress and pause them.
    set("image-display-duration", "inf");
    // Only affects software decoding (e.g. 10-bit H.264 or codecs the video chip lacks).
    static const char* skip[] = {"default", "nonref", "all"};
    set("vd-lavc-skiploopfilter", skip[s.fastDecode]);
    set("vd-lavc-fast", s.fastDecode ? "yes" : "no");
    set("volume-max", std::to_string(cfg.maxVolumeValue()));
    // External subtitles are loaded by the app so legacy encodings can be converted first.
    set("sub-auto", "no");
    set("alang", cfg.langCode(s.alang));
    set("slang", cfg.langCode(s.slang));
    set("deband", s.deband ? "yes" : "no");
    // mpv's default scalers (lanczos, plus linear-light "correct" downscaling) cost the Switch's GPU
    // most of a frame on 1080p films shown at 720p. These look nearly the same and are several
    // times cheaper, so every resolution plays at the full frame rate.
    set("scale", "catmull_rom");
    set("cscale", "bilinear");
    set("dscale", "mitchell");
    set("correct-downscaling", "no");
    set("linear-downscaling", "no");
    set("sigmoid-upscaling", "no");
    set("hdr-compute-peak", "no");
    // The Switch's speakers and headphones are stereo. Sending them 5.1 plays only the front pair and
    // loses the centre channel, which carries the dialogue, so it's mixed down unless asked otherwise.
    set("audio-channels", s.audioOut == 1 && platform::isDocked() ? "stereo,5.1,7.1" : "stereo");
    {
        // Night mode, then the equalizer; a limiter at the end keeps boosted sound from clipping.
        static const char* eq[] = {"",
                                   "bass=g=7:f=100:w=0.7",
                                   "treble=g=6:f=5000",
                                   "equalizer=f=250:t=q:w=1:g=-3,equalizer=f=2800:t=q:w=1.2:g=5",
                                   "bass=g=5:f=90,treble=g=4:f=8000",
                                   "highpass=f=120,equalizer=f=1500:t=q:w=1:g=3,bass=g=3:f=180",
                                   "bass=g=4:f=70,treble=g=2:f=10000"};
        std::vector<std::string> chain;
        if (s.nightMode) chain.push_back("acompressor=threshold=-21dB:ratio=4:attack=5:release=120:makeup=6");
        int e = std::clamp(s.soundEq, 0, (int)(sizeof eq / sizeof *eq) - 1);
        if (e) {
            chain.push_back("volume=-5dB");
            chain.push_back(eq[e]);
            chain.push_back("alimiter=limit=0.95:level=0");
        }
        std::string af;
        for (auto& f : chain) af += (af.empty() ? "" : ",") + f;
        af = af.empty() ? "" : "lavfi=[" + af + "]";
        // Changing the chain restarts the audio for a moment: only when it really changed.
        if (af != afApplied_) {
            afApplied_ = af;
            set("af", af);
        }
    }
    set("cache-secs", std::to_string(std::max(60, cfg.cacheSecsValue())));
    // Started with little memory (from the Album, or an old HOME Menu icon): the buffers are kept
    // small, or a seek in a big film runs the console out of graphics memory and the app stops.
    int demux = memoryTight() ? std::min(cfg.demuxMBValue(), 32) : cfg.demuxMBValue();
    set("demuxer-max-bytes", std::to_string(demux) + "MiB");
    set("demuxer-max-back-bytes", std::to_string(memoryTight() ? 4 : demux / 4) + "MiB");
    // The simple render path: no in-between textures for scaling, which is where a fast-forward
    // ran out of graphics memory.
    set("gpu-dumb-mode", memoryTight() ? "yes" : "auto");

    switch (s.videoFit) {
        case 0: set("keepaspect", "yes"); set("panscan", "0"); break;
        case 1: set("keepaspect", "yes"); set("panscan", "1"); break;
        case 2: set("keepaspect", "no"); set("panscan", "0"); break;
    }
    set("loop-file", s.loopMode == 1 ? "inf" : "no");
    set("loop-playlist", s.loopMode == 2 ? "inf" : "no");

    // Subtitle style
    // Vazirmatn (bundled) covers Latin, Persian and Arabic; it's written to <config-dir>/fonts.
    set("sub-font", "Vazirmatn");
    set("sub-scale", std::to_string(s.subScale / 100.0));
    snprintf(buf, sizeof buf, "#FF%06X", cfg.subColorRGB());
    set("sub-color", buf);
    set("sub-border-color", "#FF000000");
    set("sub-border-size", std::to_string(s.subBorder));
    set("sub-pos", std::to_string(s.subPos));
    set("sub-bold", s.subBold ? "yes" : "no");
    static const char* override[] = {"no", "scale", "force"};
    set("sub-ass-override", override[s.subOverride]);
    switch (s.subBackground) {
        case 0:
            set("sub-border-style", "outline-and-shadow");
            set("sub-shadow-offset", "0");
            break;
        case 1:
            set("sub-border-style", "outline-and-shadow");
            set("sub-shadow-offset", "2");
            set("sub-shadow-color", "#A0000000");
            break;
        case 2:
        case 3: {
            const char* c = s.subBackground == 2 ? "#99000000" : "#FF000000";
            set("sub-border-style", "background-box");
            set("sub-back-color", c);
            set("sub-shadow-color", c);
            set("sub-shadow-offset", "4");
            break;
        }
    }
}

void Player::command(std::initializer_list<const char*> args) {
    if (!mpv_) return;
    const char* argv[16];
    int n = 0;
    for (auto a : args) {
        if (n < 15) argv[n++] = a;
    }
    argv[n] = nullptr;
    mpv_command_async(mpv_, 0, argv);
}

void Player::open(const std::vector<std::string>& files, int index) {
    if (!mpv_ || files.empty()) return;
    playlist = files;
    blank_ = true;
    unblankOnDraw_ = false;
    tracks.clear();
    chapters.clear();
    position = 0;
    duration = 0;
    loaded = false;
    active = true;
    lastError.clear();
    const std::string& first = files[index < (int)files.size() ? index : 0];
    auto named = titles.find(first);
    title = named != titles.end() ? named->second : baseName(first);

    // Not waiting for mpv: it may still be closing the previous file, and the screen is drawn here.
    const char* clear[] = {"playlist-clear", nullptr};
    mpv_command_async(mpv_, 0, clear);
    // Only the chosen file goes in now, so it starts at once however large the folder is. The rest
    // follow a few per frame (see pumpQueue), the ones before it being moved up into place.
    int start = index >= 0 && index < (int)files.size() ? index : 0;
    loadEntry(files[start], "replace");
    queuePending_.clear();
    queueNext_ = 0;
    queueCount_ = 1;
    queuePending_.reserve(files.size());
    for (size_t i = (size_t)start + 1; i < files.size(); i++) queuePending_.push_back({files[i], -1});
    for (int i = 0; i < start; i++) queuePending_.push_back({files[i], i});
    setPause(false);
}

// A path as mpv reports it, turned back into the app's own form: no read-ahead prefix, no login.
static std::string appPath(const std::string& s) {
    return netfs::withoutCredentials(netfs::fromStreamUrl(stripReadAheadUrl(s)));
}

std::string Player::queueUrl(const std::string& file) const {
    // Local media goes through SwitchMX's own read-ahead stream (see readahead.hpp); a network
    // place's files get its saved login (see netfs.hpp).
    MediaKind k = kindForExtension(fileExtension(file));
    bool local = !isUrl(file) && (k == MediaKind::Video || k == MediaKind::Audio);
    if (local) return readAheadUrl(file);
    std::string net = netfs::streamUrl(file);  // a saved server's file: SwitchMX's own FTP stream
    return net != file ? net : netfs::withCredentials(file);
}

// Adds one file to mpv's playlist; a named stream carries its name (a live channel has none).
void Player::loadEntry(const std::string& file, const char* mode) {
    auto named = titles.find(file);
    std::string url = queueUrl(file), name = named != titles.end() ? named->second : "";
    if (name.empty()) {
        const char* cmd[] = {"loadfile", url.c_str(), mode, nullptr};
        mpv_command_async(mpv_, 0, cmd);
        return;
    }
    std::string opt = "force-media-title=%" + std::to_string(name.size()) + "%" + name;  // %n%: taken as is
    const char* cmd[] = {"loadfile", url.c_str(), mode, "-1", opt.c_str(), nullptr};
    mpv_command_async(mpv_, 0, cmd);
}

void Player::pumpQueue() {
    // Not waiting for replies: mpv can be busy for a while opening a file, and this thread draws.
    constexpr size_t kPerFrame = 64;
    for (size_t n = 0; n < kPerFrame && queueNext_ < queuePending_.size(); n++, queueNext_++) {
        const QueueItem& item = queuePending_[queueNext_];
        loadEntry(item.file, "append");
        if (item.moveTo >= 0) {
            std::string from = std::to_string(queueCount_), to = std::to_string(item.moveTo);
            const char* move[] = {"playlist-move", from.c_str(), to.c_str(), nullptr};
            mpv_command_async(mpv_, 0, move);
        }
        queueCount_++;
    }
    if (queueNext_ >= queuePending_.size() && !queuePending_.empty()) {
        queuePending_.clear();
        queuePending_.shrink_to_fit();
        queueNext_ = 0;
    }
}

void Player::stop() {
    if (!mpv_) return;
    queuePending_.clear();
    queueNext_ = 0;
    // Not waiting: closing a large file takes mpv a moment, and the screen is drawn on this thread.
    const char* cmd[] = {"stop", nullptr};
    mpv_command_async(mpv_, 0, cmd);
    active = false;
    loaded = false;
    blank_ = true;
    unblankOnDraw_ = false;
}

std::vector<PlayerEvent> Player::poll() {
    std::vector<PlayerEvent> out;
    if (!mpv_) return out;
    pumpQueue();
    for (;;) {
        mpv_event* e = mpv_wait_event(mpv_, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        switch (e->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE:
                handleProperty((mpv_event_property*)e->data, e->reply_userdata);
                break;
            case MPV_EVENT_START_FILE:
                rotation = 0;  // each file starts upright, like the other per-file view settings
                blank_ = true;
                unblankOnDraw_ = false;
                loadGen_++;  // answers still on their way belong to the previous file
                loaded = false;
                metadata.clear();
                idle = false;
                position = 0;
                duration = 0;
                out.push_back(PlayerEvent::FileStarted);
                break;
            case MPV_EVENT_HOOK: {
                auto* h = (mpv_event_hook*)e->data;
                resumedAt = 0;
                if (!strcmp(h->name, "on_load") && resumeLookup) {
                    if (char* p = mpv_get_property_string(mpv_, "path")) {
                        resumedAt = resumeLookup(appPath(p));
                        // Animated GIFs loop like they do everywhere else.
                        if (fileExtension(p) == "gif")
                            mpv_set_property_string(mpv_, "file-local-options/loop-file", "inf");
                        mpv_free(p);
                    }
                    if (resumedAt > 0) {
                        mpv_set_property_string(mpv_, "file-local-options/start", std::to_string(resumedAt).c_str());
                        // To the keyframe just before: an exact jump decodes from there anyway and
                        // took seconds with 10-bit HEVC. Precise seeking returns once it's there.
                        mpv_set_property_string(mpv_, "file-local-options/hr-seek", "no");
                        startPending_ = true;
                    }
                }
                mpv_hook_continue(mpv_, h->id);
                break;
            }
            case MPV_EVENT_FILE_LOADED: {
                // Property notifications may lag behind, so the essentials are asked for. Without
                // waiting: right now mpv is busy starting the decoders and the audio, and a blocking
                // question would freeze the screen for about half a second. The path is asked last;
                // its answer arrives after the others and completes the load.
                loaded = true;
                uint64_t gen = (uint64_t)loadGen_ << 8;
                mpv_get_property_async(mpv_, gen | R_POS, "playlist-pos", MPV_FORMAT_INT64);
                mpv_get_property_async(mpv_, gen | R_DURATION, "duration", MPV_FORMAT_DOUBLE);
                mpv_get_property_async(mpv_, gen | R_TIME, "time-pos", MPV_FORMAT_DOUBLE);
                mpv_get_property_async(mpv_, gen | R_PATH, "path", MPV_FORMAT_STRING);
                break;
            }
            case MPV_EVENT_GET_PROPERTY_REPLY: {
                auto* prop = (mpv_event_property*)e->data;
                if ((e->reply_userdata >> 8) != loadGen_ || e->error < 0 || !prop || !prop->data) break;
                switch (e->reply_userdata & 0xFF) {
                    case R_POS:
                        if (prop->format == MPV_FORMAT_INT64) playlistPos = *(int64_t*)prop->data;
                        break;
                    case R_DURATION:
                        if (prop->format == MPV_FORMAT_DOUBLE) duration = *(double*)prop->data;
                        break;
                    case R_TIME:
                        if (prop->format == MPV_FORMAT_DOUBLE) position = *(double*)prop->data;
                        break;
                    case R_PATH:
                        if (prop->format == MPV_FORMAT_STRING) path = appPath(*(char**)prop->data);
                        out.push_back(PlayerEvent::FileLoaded);
                        break;
                }
                break;
            }
            case MPV_EVENT_PLAYBACK_RESTART:
                if (blank_) unblankOnDraw_ = true;
                // The start seek has happened; clear it so "repeat file" loops from 0:00.
                if (startPending_) {
                    mpv_set_property_string(mpv_, "file-local-options/start", "none");
                    mpv_set_property_string(mpv_, "file-local-options/hr-seek", "default");
                    startPending_ = false;
                }
                break;
            case MPV_EVENT_END_FILE: {
                auto* ef = (mpv_event_end_file*)e->data;
                endedPosition = position;
                if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                    lastError = mpv_error_string(ef->error);
                    lastErrorPath = entryPath(ef->playlist_entry_id);
                    out.push_back(PlayerEvent::Error);
                } else if (ef->reason == MPV_END_FILE_REASON_EOF && loaded && !path.empty() && !isUrl(path) &&
                           duration > 120 && position > 0 && position < duration - 60 && position < duration * 0.97) {
                    // "The end" an hour early: the rest of the file is missing or unreadable (a copy
                    // that stopped, a damaged card). mpv just goes on to the next file; say why.
                    endedEarlyPath = path;
                    endedEarlyAt = position;
                    endedEarlyLength = duration;
                    out.push_back(PlayerEvent::EndedEarly);
                }
                out.push_back(PlayerEvent::FileEnded);
                break;
            }
            case MPV_EVENT_LOG_MESSAGE: {
                auto* m = (mpv_event_log_message*)e->data;
                writeLog(m);
                if (m->log_level > devLogLevel_) break;  // asked for the debug log only
                fprintf(stderr, "[mpv/%s] %s", m->prefix, m->text);
                DevTools::log("mpv", std::string(m->prefix) + ": " + m->text);
                break;
            }
            default: break;
        }
    }
    return out;
}

void Player::handleProperty(mpv_event_property* p, uint64_t id) {
    bool has = p->format != MPV_FORMAT_NONE && p->data;
    auto d = [&](double fb) { return has && p->format == MPV_FORMAT_DOUBLE ? *(double*)p->data : fb; };
    auto i = [&](int64_t fb) { return has && p->format == MPV_FORMAT_INT64 ? *(int64_t*)p->data : fb; };
    auto f = [&]() { return has && p->format == MPV_FORMAT_FLAG && *(int*)p->data; };
    auto s = [&]() { return has && p->format == MPV_FORMAT_STRING ? std::string(*(char**)p->data) : std::string(); };
    switch (id) {
        case P_TIME_POS: if (has) position = d(position); break;
        case P_DURATION: duration = d(0); break;
        case P_PAUSE: paused = f(); break;
        case P_VOLUME: volume = d(volume); break;
        case P_MUTE: mute = f(); break;
        case P_SPEED: speed = d(1); break;
        case P_TRACKS: tracks.clear(); if (has) parseTracks((mpv_node*)p->data); break;
        case P_CHAPTERS: chapters.clear(); if (has) parseChapters((mpv_node*)p->data); break;
        case P_CHAPTER: chapter = i(-1); break;
        case P_TITLE: if (has) title = s(); break;
        case P_PAUSED_CACHE: buffering = f(); break;
        case P_CACHE_STATE: cacheBuffering = i(100); break;
        case P_CACHE_TIME: cacheTime = d(0); break;
        case P_IDLE:
            idle = f();
            break;
        case P_PL_POS: playlistPos = i(0); break;
        case P_PL_COUNT: playlistCount = i(0); break;
        case P_PATH: path = appPath(s()); break;
        case P_WIDTH: width = i(0); break;
        case P_HEIGHT: height = i(0); break;
        case P_VCODEC: videoCodec = s(); break;
        case P_ACODEC: audioCodec = s(); break;
        case P_HWDEC: hwdecCurrent = s(); break;
        case P_FPS: fps = d(0); break;
        case P_DROPS: droppedFrames = i(0); break;
        case P_SUB_DELAY: subDelay = d(0); break;
        case P_AUDIO_DELAY: audioDelay = d(0); break;
        case P_ZOOM: zoom = d(0); break;
        case P_PAN_X: panX = d(0); break;
        case P_PAN_Y: panY = d(0); break;
        case P_ROTATE:
            if (noFbo_) rotation = (int)i(0);  // otherwise the app keeps it (setRotation)
            break;
        case P_AB_A: abA = d(-1); break;
        case P_AB_B: abB = d(-1); break;
        case P_SEEKING: seeking = f(); break;
        case P_VFPS: outputFps = d(0); break;
        case P_DEC_DROPS: decoderDrops = i(0); break;
        case P_AVSYNC: avsync = d(0); break;
        case P_VBITRATE: videoBitrate = d(0); break;
        case P_ABITRATE: audioBitrate = d(0); break;
        case P_CACHE_SPEED: readSpeed = i(0); break;
        case P_CACHE_FWD: cacheBytes = i(0); break;
        case P_METADATA:
            metadata.clear();
            if (has && p->format == MPV_FORMAT_NODE) parseMetadata((mpv_node*)p->data);
            break;
        case P_PLAYLIST:
            if (has && p->format == MPV_FORMAT_NODE) {
                mpv_node* n = (mpv_node*)p->data;
                if (n->format == MPV_FORMAT_NODE_ARRAY) {
                    playlist.clear();
                    playlistTitles.clear();
                    for (int k = 0; k < n->u.list->num; k++) {
                        mpv_node* e = &n->u.list->values[k];
                        if (auto name = nodeString(e, "filename")) {
                            playlist.push_back(appPath(name));
                            const char* t = nodeString(e, "title");
                            playlistTitles.push_back(t ? t : "");
                        }
                    }
                }
            }
            break;
    }
}

void Player::parseTracks(mpv_node* n) {
    if (n->format != MPV_FORMAT_NODE_ARRAY) return;
    for (int k = 0; k < n->u.list->num; k++) {
        mpv_node* m = &n->u.list->values[k];
        Track t;
        nodeInt(m, "id", t.id);
        if (auto v = nodeString(m, "type")) t.type = v;
        if (auto v = nodeString(m, "title")) t.title = v;
        if (auto v = nodeString(m, "lang")) t.lang = v;
        if (auto v = nodeString(m, "codec")) t.codec = v;
        t.selected = nodeFlag(m, "selected");
        t.external = nodeFlag(m, "external");
        t.isDefault = nodeFlag(m, "default");
        t.forced = nodeFlag(m, "forced");
        nodeInt(m, "demux-channel-count", t.channels);
        nodeInt(m, "demux-w", t.w);
        nodeInt(m, "demux-h", t.h);
        tracks.push_back(t);
    }
}

void Player::parseMetadata(mpv_node* n) {
    if (n->format != MPV_FORMAT_NODE_MAP) return;
    for (int k = 0; k < n->u.list->num; k++) {
        if (n->u.list->values[k].format != MPV_FORMAT_STRING) continue;
        std::string key = n->u.list->keys[k];
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        metadata[key] = n->u.list->values[k].u.string;
    }
}

std::string Player::tag(const char* key) const {
    auto it = metadata.find(key);
    return it != metadata.end() ? it->second : "";
}

void Player::shuffle(bool on) { command({on ? "playlist-shuffle" : "playlist-unshuffle"}); }

void Player::removeCurrent() { command({"playlist-remove", "current"}); }

void Player::parseChapters(mpv_node* n) {
    if (n->format != MPV_FORMAT_NODE_ARRAY) return;
    for (int k = 0; k < n->u.list->num; k++) {
        mpv_node* m = &n->u.list->values[k];
        Chapter c;
        if (auto v = nodeString(m, "title")) c.title = v;
        nodeDouble(m, "time", c.time);
        if (c.title.empty()) c.title = "Chapter " + std::to_string(k + 1);
        chapters.push_back(c);
    }
}

void Player::render(int fbo, int screenW, int screenH) {
    if (!render_ || screenW <= 0 || screenH <= 0) return;
    if (noFbo_) return renderTo(fbo, screenW, screenH);
    // Turned a quarter, the frame is drawn tall and the app lays it on its side.
    bool side = rotation == 90 || rotation == 270;
    int w = side ? screenH : screenW, h = side ? screenW : screenH;
    bool resized = w != fboW_ || h != fboH_ || !fbo_;
    if (resized) {
        if (!fbo_) glGenFramebuffers(1, &fbo_);
        if (!tex_) glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
        bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok) {  // fall back to drawing straight to the screen every frame
            glDeleteFramebuffers(1, &fbo_);
            fbo_ = 0;
            noFbo_ = true;
            int64_t v = rotation;
            mpv_set_property_async(mpv_, 0, "video-rotate", MPV_FORMAT_INT64, &v);
            renderTo(fbo, screenW, screenH);
            return;
        }
        fboW_ = w;
        fboH_ = h;
    }
    bool draw = resized || unblankOnDraw_;
    if (dirty_.exchange(false)) draw = (mpv_render_context_update(render_) & MPV_RENDER_UPDATE_FRAME) || draw;
    if (draw) {
        renderTo((int)fbo_, w, h);
        if (unblankOnDraw_) blank_ = unblankOnDraw_ = false;
    }
    if (blank_ || rotation) {  // black so far, or the app draws it turned
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
}

void Player::renderTo(int fbo, int w, int h) {
    renderedFrame_ = true;
    mpv_opengl_fbo target{fbo, w, h, 0};
    int flip = 1;
    int block = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context_render(render_, params);
}

void Player::reportSwap() {
    // Only frames mpv drew count: it times video against these.
    if (render_ && renderedFrame_) mpv_render_context_report_swap(render_);
    renderedFrame_ = false;
}

void Player::togglePause() { setPause(!paused); }

void Player::setPause(bool p) {
    if (!mpv_) return;
    int v = p ? 1 : 0;
    mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &v);
    paused = p;
}

void Player::seek(double secs, bool relative) {
    seekCount++;
    std::string v = std::to_string(secs);
    command({"seek", v.c_str(), relative ? "relative" : "absolute"});
    if (relative) position += secs;
    else position = secs;
    if (position < 0) position = 0;
    if (duration > 0 && position > duration) position = duration;
}

void Player::seekPreview(double secs) {
    seekCount++;
    std::string v = std::to_string(secs);
    command({"seek", v.c_str(), "absolute+keyframes"});
}

void Player::setVolume(double v) {
    if (!mpv_) return;
    mpv_set_property_async(mpv_, 0, "volume", MPV_FORMAT_DOUBLE, &v);
    volume = v;
}

void Player::setMute(bool m) {
    if (!mpv_) return;
    int v = m;
    mpv_set_property_async(mpv_, 0, "mute", MPV_FORMAT_FLAG, &v);
    mute = m;
}

void Player::setSpeed(double s) {
    if (!mpv_) return;
    mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &s);
    speed = s;
}

void Player::setTrack(const char* type, int64_t id) {
    const char* prop = !strcmp(type, "sub") ? "sid" : (!strcmp(type, "audio") ? "aid" : "vid");
    std::string v = id > 0 ? std::to_string(id) : "no";
    setAsync(prop, v);
}

void Player::cycleTrack(const char* type) {
    const char* prop = !strcmp(type, "sub") ? "sid" : "aid";
    command({"cycle", prop});
}

void Player::addSubtitle(const std::string& p, bool select, const std::string& subTitle, const std::string& lang) {
    command({"sub-add", p.c_str(), select ? "select" : "auto", subTitle.c_str(), lang.c_str()});
}

// From the observed track list, so asking never waits for mpv.
bool Player::subtitleSelected() {
    for (auto& t : tracks)
        if (t.type == "sub" && t.selected) return true;
    return false;
}

void Player::setAsync(const char* name, const std::string& value) {
    if (!mpv_) return;
    const char* v = value.c_str();  // mpv copies the value before this returns
    mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_STRING, &v);
}

void Player::setSubDelay(double d) {
    mpv_set_property_async(mpv_, 0, "sub-delay", MPV_FORMAT_DOUBLE, &d);
    subDelay = d;
}

void Player::setAudioDelay(double d) {
    mpv_set_property_async(mpv_, 0, "audio-delay", MPV_FORMAT_DOUBLE, &d);
    audioDelay = d;
}

void Player::setZoom(double z) {
    mpv_set_property_async(mpv_, 0, "video-zoom", MPV_FORMAT_DOUBLE, &z);
    zoom = z;
}

void Player::setPan(double x, double y) {
    mpv_set_property_async(mpv_, 0, "video-pan-x", MPV_FORMAT_DOUBLE, &x);
    mpv_set_property_async(mpv_, 0, "video-pan-y", MPV_FORMAT_DOUBLE, &y);
    panX = x;
    panY = y;
}

void Player::setRotation(int deg) {
    int64_t v = ((deg % 360) + 360) % 360;
    rotation = (int)v;
    // mpv turns only the picture: subtitles stay upright. So mpv draws the frame unrotated, with
    // its subtitles, and the app turns the whole frame (see App::drawRotatedVideo). Without the
    // frame copy (an old GPU driver), mpv's own rotation is the fallback.
    if (noFbo_) mpv_set_property_async(mpv_, 0, "video-rotate", MPV_FORMAT_INT64, &v);
    dirty_ = true;
}

void Player::setAspect(const char* aspect) { setAsync("video-aspect-override", aspect); }

void Player::setEq(const char* prop, int value) {
    int64_t v = value;
    mpv_set_property_async(mpv_, 0, prop, MPV_FORMAT_INT64, &v);
}

void Player::playIndex(int i) {
    std::string v = std::to_string(i);
    command({"playlist-play-index", v.c_str()});
}

void Player::next() { command({"playlist-next", "weak"}); }
void Player::prevFile() { command({"playlist-prev", "weak"}); }
void Player::prev() {
    // Like most players: restart the file unless we're near its start.
    if (position > 5 || playlistPos <= 0) seek(0, false);
    else command({"playlist-prev", "weak"});
}

void Player::setChapter(int i) {
    seekCount++;
    int64_t v = i;
    mpv_set_property_async(mpv_, 0, "chapter", MPV_FORMAT_INT64, &v);
}

// Waits for the file to be written (a moment), so the caller can say whether it worked.
bool Player::screenshot() {
    if (!mpv_) return false;
    const char* argv[] = {"screenshot", "video", nullptr};
    return mpv_command(mpv_, argv) >= 0;
}
void Player::abLoop() { command({"ab-loop"}); }
void Player::setLoopFile(bool on) { setAsync("loop-file", on ? "inf" : "no"); }
void Player::setProperty(const char* name, const char* value) { setAsync(name, value); }

double Player::getDouble(const char* name, double fallback) {
    double v = fallback;
    if (mpv_ && mpv_get_property(mpv_, name, MPV_FORMAT_DOUBLE, &v) < 0) return fallback;
    return v;
}
