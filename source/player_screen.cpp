#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <ctime>

#include "app.hpp"
#include "i18n.hpp"
#include "netfs.hpp"

#include <cstring>
#include "platform.hpp"
#include "readahead.hpp"
#include "subtext.hpp"

namespace {
const char* kAspectLabels[] = {"Original", "16:9", "4:3", "21:9", "2.35:1", "1:1", "16:10"};
const char* kAspectValues[] = {"no", "16:9", "4:3", "21:9", "2.35:1", "1:1", "16:10"};
constexpr int kAspectCount = 7;
const char* kSleepLabels[] = {"Off", "5 min", "10 min", "15 min", "20 min", "30 min", "45 min", "60 min",
                              "90 min", "120 min", "End of file", "Custom…"};
const int kSleepMinutes[] = {0, 5, 10, 15, 20, 30, 45, 60, 90, 120, -1, -2};  // -1 end of file, -2 typed
constexpr int kSleepCount = 12;
constexpr float kSleepFade = 30;  // seconds of fading out before the timer pauses
const char* kEqProps[] = {"brightness", "contrast", "saturation", "gamma", "hue"};
const char* kEqLabels[] = {"Brightness", "Contrast", "Saturation", "Gamma", "Hue"};

std::string clockString(bool h24) {
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char buf[32];
    if (!tm) return "";
    strftime(buf, sizeof buf, h24 ? "%H:%M" : "%I:%M %p", tm);
    std::string s = buf;
    if (!h24 && s[0] == '0') s.erase(0, 1);
    return s;
}

std::string signedSecs(double s, int decimals) {
    char buf[32];
    snprintf(buf, sizeof buf, "%+.*f s", decimals, s);
    return buf;
}

std::string channelsLabel(int64_t ch) {
    switch (ch) {
        case 0: return "";
        case 1: return "Mono";
        case 2: return "Stereo";
        case 6: return "5.1";
        case 8: return "7.1";
        default: return std::to_string(ch) + " ch";
    }
}
}  // namespace

// ------------------------------------------------------------------ lifecycle

void PlayerScreen::enter() {}

void PlayerScreen::play(const std::vector<std::string>& files, int index) {
    Config& cfg = app.cfg;
    resumePill_ = 0;
    if (deinterlace_) {
        deinterlace_ = false;
        app.player.setProperty("deinterlace", "no");
    }
    if (swDecode_) {  // software decoding was for the last file only
        swDecode_ = false;
        app.player.setProperty("hwdec", cfg.hwdecValue());
    }
    app.player.resumeLookup = [this](const std::string& path) {
        if (forceResumeAt_ >= 0) {  // reopened to free memory: the same spot (the first file to load)
            double at = forceResumeAt_;
            forceResumeAt_ = -1;
            return at;
        }
        if (!app.cfg.s.resume) return 0.0;
        // A live stream (HLS): always where it is now, never back in its recording window.
        if (isUrl(path) && path.find(".m3u8") != std::string::npos) return 0.0;
        // Songs start from the beginning; long audio (audiobooks, podcasts, mixes of 20 min and
        // more) picks up where it stopped, like video.
        if (kindForExtension(fileExtension(path)) == MediaKind::Audio) {
            const HistoryItem* h = app.lib.find(path);
            if (!h || h->duration < 20 * 60) return 0.0;
        }
        return app.lib.resumePosition(path);
    };
    MediaKind first = files.empty() ? MediaKind::Other
                                     : kindForExtension(fileExtension(files[std::min<size_t>(index, files.size() - 1)]));
    imageMode_ = first == MediaKind::Image;
    musicKind_ = first == MediaKind::Audio;
    if (forceResumeAt_ < 0) {  // a fresh start, not a reopen
        memStep_ = 0;
        memReloaded_ = false;
    }
    immersive_ = false;
    infoOpen_ = false;
    shuffled_ = false;
    stripScroll_ = -1;
    slideshow_ = imageMode_ && cfg.slideshowSecs() > 0 && files.size() > 1;
    slideTimer_ = 0;
    app.logEvent("play: " + files[std::min<size_t>(index, files.size() - 1)] + " (" + std::to_string(files.size()) +
                 " in queue)");
    chosenPath_ = files[std::min<size_t>(index, files.size() - 1)];
    anyLoaded_ = false;
    app.player.open(files, index);
    backlightAtStart_ = platform::backlight();
    app.player.setSpeed(cfg.defaultSpeedValue());

    osd_ = true;
    osdTimer_ = 0;
    panel_ = P_NONE;
    panelAnim_ = 0;
    locked_ = false;
    gesture_ = G_NONE;
    hud_ = H_NONE;
    dtTimer_ = 0;
    boosting_ = false;
    sawPlaying_ = false;
    openTime_ = 0;
    saveTimer_ = 0;
    lastSavedPos_ = -1;
    endSleepFade();
    sleepRemaining_ = -1;
    sleepEndOfFile_ = false;
    sleepIdx_ = 0;
    aspectIdx_ = 0;
    currentPath_.clear();
    curDur_ = 0;
    for (int& e : eq_) e = 0;
    seekBarId_ = uid("osd.seekbar");
    platform::setMediaPlayback(true);
    app.ui.setFocus(uid("osd.play"));
}

bool PlayerScreen::closeIfOn(const std::string& root) {
    Player& p = app.player;
    bool onDrive = currentPath_.rfind(root, 0) == 0 || analyzer_.path().rfind(root, 0) == 0;
    for (auto& f : p.playlist)
        if (f.rfind(root, 0) == 0) onDrive = true;
    if (!onDrive) return false;
    app.logEvent("playback stopped: its drive was removed");
    analyzer_.stop();
    currentPath_.clear();  // nothing to save a resume point for: the file can't be read any more
    exit();
    return true;
}

void PlayerScreen::saveProgress() {
    if (currentPath_.empty() || !app.player.loaded || imageMode_) return;
    app.lib.updateHistory(currentPath_, app.player.position, curDur_, false, historyTitle());
}

// What Recent should call a stream: its address (".../index.m3u8?token=...") says nothing. The name
// it was opened with, the #EXTINF name in its playlist, or the playlist file that led to it.
std::string PlayerScreen::historyTitle() const {
    const Player& p = app.player;
    if (!isUrl(currentPath_) || netfs::isNetPath(currentPath_)) return "";
    auto named = p.titles.find(currentPath_);
    if (named != p.titles.end() && !named->second.empty()) return named->second;
    int64_t pos = p.playlistPos;
    if (pos >= 0 && pos < (int64_t)p.playlistTitles.size() && !p.playlistTitles[pos].empty()) return p.playlistTitles[pos];
    // mpv's title, unless it's only the address's last part (what it shows when there's no name).
    std::string bare = currentPath_.substr(0, currentPath_.find_first_of("?#"));
    const std::string& t = p.title;
    if (!t.empty() && t != baseName(bare) && t != baseName(currentPath_) && t.find("://") == std::string::npos &&
        t != netfs::decode(baseName(bare)))
        return t;
    if (p.playlist.size() == 1 && !isUrl(p.playlist[0])) return stripExtension(baseName(p.playlist[0]));
    return "";
}

void PlayerScreen::exit() {
    wakeScreen();
    saveProgress();
    app.lib.saveHistoryInBackground();  // going back shouldn't wait for the SD card
    app.cfg.save();
    endBoost();
    endHeavyVideo();
    analyzer_.stop();
    immersive_ = false;
    slideshow_ = false;
    infoOpen_ = false;
    app.player.stop();
    app.lastPlayed = currentPath_;
    currentPath_.clear();
    platform::setMediaPlayback(false);
    // The brightness gesture changes the real backlight; give the user their setting back.
    if (platform::supportsBacklight() && backlightAtStart_ >= 0) platform::setBacklight(backlightAtStart_);
    app.switchTo(app.home);
}

void PlayerScreen::handleEvents(const std::vector<PlayerEvent>& evs) {
    Player& p = app.player;
    if (!evs.empty()) {
        std::string note = takeReadAheadNote();
        if (!note.empty()) app.logEvent("player: " + note);
        if (note.find("memory to open") != std::string::npos)
            app.ui.toast("Not enough memory to open this file. Close SwitchMX and start it again", Icon::Info);
    }
    for (auto e : evs) {
        switch (e) {
            case PlayerEvent::FileLoaded: {
                sawPlaying_ = true;
                anyLoaded_ = true;
                failedPath_.clear();
                int64_t pos = p.playlistPos;
                bool inList = pos >= 0 && pos < (int64_t)p.playlist.size();
                // mpv's own path is authoritative: playlist files (.m3u) are expanded in place.
                currentPath_ = !p.path.empty() ? p.path : (inList ? p.playlist[pos] : "");
                curDur_ = p.duration;
                subFiles_.clear();
                imageMode_ = kindForExtension(fileExtension(currentPath_)) == MediaKind::Image;
                musicKind_ = kindForExtension(fileExtension(currentPath_)) == MediaKind::Audio;
                lastPos_ = -1;
                if (musicKind_ && !isUrl(currentPath_) && analyzer_.path() != currentPath_) analyzer_.start(currentPath_);
                if (imageMode_) {
                    slideTimer_ = 0;
                    p.setZoom(0);
                    p.setPan(0, 0);
                    break;
                }
                resumePill_ = 0;
                if (p.resumedAt > 0 && !musicKind_ && !imageMode_) {
                    resumePill_ = 10;  // "Resumed at … · Start over"
                    resumedFrom_ = p.resumedAt;
                } else if (p.resumedAt > 0) {
                    app.ui.toast(trf("Resumed from {0}", {formatTime(p.resumedAt)}), Icon::Refresh);
                }
                app.lib.updateHistory(currentPath_, p.resumedAt, curDur_, false, historyTitle());
                if (app.cfg.s.subAuto) loadExternalSubtitles();
                trackRestore_ = app.cfg.s.rememberTracks && !musicKind_ && !isUrl(currentPath_);
                trackRestored_ = 0;
                break;
            }
            case PlayerEvent::FileEnded:
                if (!currentPath_.empty()) {
                    if (!imageMode_) app.lib.updateHistory(currentPath_, p.endedPosition, curDur_);
                    currentPath_.clear();
                }
                if (sleepEndOfFile_ && app.screen == this && screenDark_) {
                    // The screen is off: stop here and keep it dark (it's woken by hand).
                    sleepEndOfFile_ = false;
                    p.stop();
                    break;
                }
                if (sleepEndOfFile_ && app.screen == this) {
                    app.ui.toast("Sleep timer finished", Icon::Moon);
                    exit();
                    return;
                }
                break;
            case PlayerEvent::Error:
                sawPlaying_ = true;
                if (!p.loaded) failedPath_ = p.lastErrorPath;
                app.logEvent("playback error: " + p.lastError + " (" + p.lastErrorPath + ")");
                app.ui.toast(trf("Can't play: {0}", {p.lastError}), Icon::Info);
                // The file that was picked can't be played: stop there rather than start whatever
                // follows it in the folder, which nobody chose. (A channel failing inside an .m3u
                // isn't the file picked, so a list still goes on to the next channel.)
                if (!anyLoaded_ && p.playlist.size() > 1 && p.lastErrorPath == chosenPath_) p.stop();
                break;
            case PlayerEvent::EndedEarly:
                app.logEvent("file ends early: " + p.endedEarlyPath + " at " + formatTime(p.endedEarlyAt) + " of " +
                             formatTime(p.endedEarlyLength));
                app.ui.toast(trf("This file stops at {0}: it may be damaged or incomplete", {formatTime(p.endedEarlyAt)}),
                             Icon::Info);
                break;
            default: break;
        }
    }
}

// ------------------------------------------------------------------ helpers

void PlayerScreen::heavyVideoWatch(float dt) {
    Player& p = app.player;
    bool heavy = p.loaded && !imageMode_ && !musicMode() && !screenDark_ && p.width * p.height > 2560LL * 1440;
    if (heavy != heavyVideo_) {
        heavyVideo_ = heavy;
        heavyTimer_ = 0;
        // Mitchell downscaling takes two passes over the full 4K picture; the GPU then couldn't keep
        // up. Bilinear is one pass (it can shimmer a little on fine detail, at 4K and only then).
        p.setProperty("dscale", heavy ? "bilinear" : "mitchell");
        int mhz = platform::setCpuClock(heavy ? 1785 : 0);
        app.logEvent(std::string(heavy ? "large video: CPU " : "CPU back to ") + std::to_string(mhz) + " MHz");
        return;
    }
    // Docking or the system's performance mode may put the clock back; check now and then.
    if (heavy && (heavyTimer_ += dt) > 5) {
        heavyTimer_ = 0;
        if (platform::setCpuClock(-1) < 1785) platform::setCpuClock(1785);
    }
}

void PlayerScreen::endHeavyVideo() {
    if (!heavyVideo_) return;
    heavyVideo_ = false;
    app.player.setProperty("dscale", "mitchell");
    platform::setCpuClock(0);
}

// Running out of memory ends the app at once (the console offers no way to carry on), so memory
// is watched while playing. Climbing use goes in the event log; near the limit the file is reopened
// where it was, which drops the decoder and all it holds; if that doesn't help, playback stops.
void PlayerScreen::memoryWatch(float dt) {
    memTimer_ += dt;
    if (memTimer_ < 1 && !platform::lowMemory()) return;
    memTimer_ = 0;
    unsigned long long used = 0, total = 0;
    if (!platform::memoryUsage(used, total) || !total) return;
    double frac = (double)used / (double)total;
    Player& p = app.player;
    auto mb = [](unsigned long long b) { return std::to_string(b >> 20); };
    int step = (int)(frac * 10);
    if (step >= 4 && step > memStep_) {
        memStep_ = step;
        app.logEvent("memory: " + mb(used) + " of " + mb(total) + " MB in use (" + std::to_string(p.seekCount) +
                     " seeks so far)");
    }
    // What's actually left decides, not a share: with the small (applet) heap 85% stopped 1080p films
    // that play fine with 28 MB still free.
    unsigned long long freeB = total > used ? total - used : 0;
    bool critical = freeB < std::max<unsigned long long>(20ull << 20, total / 20) || platform::lowMemory();
    if (!critical) {
        if (frac < 0.6) platform::rearmMemoryReserve();
        return;
    }
    platform::clearLowMemory();
    if (!memReloaded_ && !currentPath_.empty() && !p.playlist.empty()) {
        memReloaded_ = true;
        app.logEvent("memory: " + mb(used) + " of " + mb(total) + " MB, reopening " + baseName(currentPath_) +
                     " at " + formatTime(p.position) + " to free it");
        saveProgress();
        forceResumeAt_ = p.position;
        std::vector<std::string> files = p.playlist;
        int index = (int)std::clamp<int64_t>(p.playlistPos, 0, (int64_t)files.size() - 1);
        p.stop();
        play(files, index);
        return;
    }
    app.logEvent("memory: still " + mb(used) + " of " + mb(total) + " MB after reopening: playback stopped");
    exit();
    app.openMenu("Playback stopped", {{"OK", Icon::Check}}, [](int) {},
                 "Memory ran low, so playback stopped to keep SwitchMX running. Your place is saved.");
}

void PlayerScreen::noteTrack(const char* type, const Track* t) {
    if (!app.cfg.s.rememberTracks || currentPath_.empty() || isUrl(currentPath_) || musicKind_ || imageMode_) return;
    std::string v = !t ? "off" : (t->external ? "e" + t->title : "i" + std::to_string(t->id));
    app.lib.setTrackPref(currentPath_, type, v);
}

// The tracks chosen last time for this video. Subtitle files load a moment after the video, so
// this keeps looking for a few seconds.
void PlayerScreen::restoreTracks() {
    Player& p = app.player;
    if (currentPath_.empty() || openTime_ > 5) {
        trackRestore_ = false;
        return;
    }
    bool pending = false;
    for (const char* type : {"audio", "sub"}) {
        int bit = strcmp(type, "sub") == 0 ? 2 : 1;
        if (trackRestored_ & bit) continue;
        std::string want = app.lib.trackPref(currentPath_, type);
        if (want.empty() || (want == "off" && bit == 1)) {
            trackRestored_ |= bit;
            continue;
        }
        if (want == "off") {
            p.setTrack("sub", 0);
            trackRestored_ |= bit;
            continue;
        }
        const Track* found = nullptr;
        for (auto& t : p.tracks) {
            if (t.type != type) continue;
            if (want[0] == 'i' && !t.external && std::to_string(t.id) == want.substr(1)) found = &t;
            if (want[0] == 'e' && t.external && t.title == want.substr(1)) found = &t;
        }
        if (found) {
            if (!found->selected) p.setTrack(type, found->id);
            trackRestored_ |= bit;
        } else {
            pending = true;  // not loaded yet
        }
    }
    trackRestore_ = pending;
}

// Language code from names like "Movie.fa.srt", "Movie.persian.srt" or "Movie_eng.srt".
static std::string subtitleLang(const std::string& path) {
    std::string stem = stripExtension(baseName(path));
    size_t cut = stem.find_last_of("._- ");
    if (cut == std::string::npos) return "";
    std::string tag = stem.substr(cut + 1);
    for (auto& c : tag) c = (char)tolower((unsigned char)c);
    static const char* map[][2] = {
        {"fa", "per"},      {"per", "per"},  {"fas", "per"},  {"farsi", "per"}, {"persian", "per"},
        {"ar", "ara"},      {"ara", "ara"},  {"arabic", "ara"}, {"en", "eng"},  {"eng", "eng"},
        {"english", "eng"}, {"es", "spa"},   {"spa", "spa"},  {"fr", "fre"},    {"fre", "fre"},
        {"de", "ger"},      {"ger", "ger"},  {"it", "ita"},   {"pt", "por"},    {"ru", "rus"},
        {"rus", "rus"},     {"tr", "tur"},   {"tur", "tur"},  {"ja", "jpn"},    {"jpn", "jpn"},
        {"ko", "kor"},      {"kor", "kor"},  {"zh", "chi"},   {"chi", "chi"},   {"chs", "chi"},
        {"cht", "chi"},     {"hi", "hin"},   {"he", "heb"},   {"heb", "heb"},
    };
    for (auto& m : map)
        if (tag == m[0]) return m[1];
    return "";
}

std::string PlayerScreen::prepareSub(const std::string& path) {
    return prepareSubtitle(path, app.cfg.s.subEncoding, app.cfg.s.slang, app.dataDir + "subcache/");
}

void PlayerScreen::loadExternalSubtitles() {
    if (currentPath_.empty() || isUrl(currentPath_)) return;
    auto files = app.lib.subtitlesFor(currentPath_, true);
    if (files.empty()) return;
    if (files.size() > 12) files.resize(12);
    // Pick the file in the preferred language, else the first; keep the video's own choice if it made one.
    size_t pick = 0;
    std::string want = app.cfg.langCode(app.cfg.s.slang);
    for (size_t i = 0; i < files.size() && !want.empty(); i++) {
        std::string l = subtitleLang(files[i]);
        if (!l.empty() && want.find(l) != std::string::npos) {
            pick = i;
            break;
        }
    }
    bool select = !app.player.subtitleSelected();
    for (size_t i = 0; i < files.size(); i++)
        app.player.addSubtitle(prepareSub(files[i]), select && i == pick, stripExtension(displayName(files[i])),
                               subtitleLang(files[i]));
}

void PlayerScreen::hud(Hud type, float value, const std::string& text, Icon ic) {
    hud_ = type;
    hudValue_ = value;
    hudText_ = text;
    hudIcon_ = ic;
    hudTimer_ = 0;
}

// Many files name chapters by their start time ("00:06:17.210"): a number reads better then.
// What to call a queue entry. For an address, the name the playlist gave it is all there is: the
// channels of an IPTV list all live at .../index.m3u8 or .../mono.m3u8, so without it every one of
// them reads "index" or "mono". For a file on disk the name is left alone: mpv fills the same field
// from the file's own metadata tag, which is exactly the untidied "Mr.Robot.S01.E02(MoboMovies.co)"
// that shownTitle() is there to clean up.
static std::string entryName(const Player& p, size_t i, bool tidy) {
    if (i >= p.playlist.size()) return std::string();
    if (isUrl(p.playlist[i]) && i < p.playlistTitles.size() && !p.playlistTitles[i].empty())
        return p.playlistTitles[i];
    return shownTitle(p.playlist[i], tidy);
}

static std::string chapterName(const Player& p, size_t i) {
    const std::string& t = p.chapters[i].title;
    bool timeName = t.empty() || t.find_first_not_of("0123456789:.,") == std::string::npos;
    return timeName ? "Chapter " + std::to_string(i + 1) : t;
}

void PlayerScreen::seekRelative(double secs, bool showHud) {
    Player& p = app.player;
    p.seek(secs, true);
    if (showHud) {
        std::string t = (secs >= 0 ? "+" : "−") + formatTime(std::fabs(secs));
        hud(H_SEEK, (float)secs, t, secs >= 0 ? Icon::SeekForward : Icon::SeekBack);
    }
}

void PlayerScreen::changeVolume(double delta) {
    Player& p = app.player;
    double v = std::clamp(p.volume + delta, 0.0, (double)app.cfg.maxVolumeValue());
    p.setVolume(v);
    if (p.mute && delta > 0) p.setMute(false);
    hud(H_VOLUME, (float)v, std::to_string((int)std::lround(v)) + "%", v <= 0 ? Icon::VolumeMute : Icon::Volume);
}

void PlayerScreen::changeBrightness(float delta) {
    float v = std::clamp(platform::backlight() + delta, 0.0f, 1.0f);
    platform::setBacklight(v);
    if (!platform::supportsBacklight()) app.player.setEq("brightness", (int)std::lround((v - 1) * 70));
    hud(H_BRIGHTNESS, v, std::to_string((int)std::lround(v * 100)) + "%", Icon::Sun);
}

void PlayerScreen::subBrowseOpen(const std::string& dir) {
    subBrowseDir_ = dir;
    if (!subBrowseDir_.empty() && subBrowseDir_.back() != '/') subBrowseDir_ += '/';
    subBrowse_.clear();
    if (subBrowseDir_.empty()) {
        for (auto& r : platform::storageRoots(true)) {
            FileEntry e;
            e.path = r.path;
            e.name = r.label;
            e.kind = MediaKind::Folder;
            e.isDrive = true;
            e.detail = r.detail;
            subBrowse_.push_back(e);
        }
        return;
    }
    subBrowse_ = app.lib.list(subBrowseDir_, false, false, SORT_NAME);
}

void PlayerScreen::subSearchStart() {
    if (subSearchedFor_ != currentPath_ || subQuery_.empty())
        subQuery_ = isUrl(currentPath_) ? app.player.title : SubSearch::queryFor(currentPath_);
    subSearchedFor_ = currentPath_;
    // The chosen subtitle language, and English as well.
    std::string lang = app.cfg.langCode(app.cfg.s.slang);
    size_t comma = lang.find_last_of(',');
    if (comma != std::string::npos) lang = lang.substr(comma + 1);
    std::string langs = lang.empty() || lang == "en" ? "en" : (lang < "en" ? lang + ",en" : "en," + lang);
    subDownloading_ = 0;
    subSearch_.search(app.cfg.openSubtitlesKey, subQuery_, langs, isUrl(currentPath_) ? "" : currentPath_);
}

void PlayerScreen::subLoadDownloaded() {
    std::string path = subSearch_.savedPath();
    std::string note = subSearch_.message();
    auto results = subSearch_.results();
    std::string lang;
    for (auto& r : results)
        if (r.fileId == subDownloading_) lang = r.language;
    subDownloading_ = 0;
    // The finished download is a one-off: put the list back as it was.
    subSearch_ = SubSearch();
    subSearchedFor_.clear();
    if (path.empty()) return;
    app.player.addSubtitle(prepareSub(path), true, "OpenSubtitles", lang);
    app.ui.toast(tr("Subtitle loaded") + (note.empty() ? std::string() : " · " + tr(note)), Icon::Captions);
    openPanel(P_SUBS);
}

void PlayerScreen::openPanel(Panel p) {
    if (p == P_SUBFILES) subFiles_ = app.lib.subtitlesFor(currentPath_);
    panel_ = p;
    lastPanel_ = p;
    panelScroll_.reset();
    panelScroll_.id = uid("panel.scroll");
    panelFocusPending_ = true;
    app.ui.setFocus(uid("prow", 0));
}

void PlayerScreen::doAction(Action a) {
    Player& p = app.player;
    if (imageMode_) {
        if (a == Action::SeekBackLong || a == Action::PrevChapter) a = Action::PrevFile;
        if (a == Action::SeekForwardLong || a == Action::NextChapter) a = Action::NextFile;
    }
    switch (a) {
        case Action::None: break;
        case Action::SubtitlePanel: openPanel(P_SUBS); break;
        case Action::AudioPanel: openPanel(P_AUDIO); break;
        case Action::SpeedPanel: openPanel(P_SPEED); break;
        case Action::VideoPanel: openPanel(P_VIDEO); break;
        case Action::PlaylistPanel: openPanel(P_PLAYLIST); break;
        case Action::ChapterPanel: openPanel(P_CHAPTERS); break;
        case Action::PrevChapter:
        case Action::NextChapter: {
            int dir = a == Action::NextChapter ? 1 : -1;
            if (p.chapters.empty()) {
                seekRelative(dir * app.cfg.seekLongSecs());
                break;
            }
            // Quick presses add up from the chapter just asked for: mpv reports the new chapter only
            // once it has got there, so counting from it would send every press to the same place.
            bool chaining = pendingChapter_ >= 0 && time_ - pendingChapterAt_ < 1.5;
            int64_t from = chaining ? pendingChapter_ : p.chapter;
            int c = (int)std::clamp<int64_t>(from + dir, 0, (int64_t)p.chapters.size() - 1);
            if (!chaining && dir < 0 && p.chapter >= 0 && p.position - p.chapters[p.chapter].time > 3) c = (int)p.chapter;
            pendingChapter_ = c;
            pendingChapterAt_ = time_;
            p.setChapter(c);
            hud(H_TEXT, 0, chapterName(p, c), Icon::Bookmark);
            break;
        }
        case Action::PrevFile:
            if (imageMode_) p.prevFile();
            else p.prev();
            hud(H_TEXT, 0, "Previous", Icon::Prev);
            break;
        case Action::NextFile:
            if (p.playlistPos + 1 < p.playlistCount) {
                p.next();
                hud(H_TEXT, 0, "Next", Icon::Next);
            } else {
                hud(H_TEXT, 0, "Last file in queue", Icon::Playlist);
            }
            break;
        case Action::SeekBackLong: seekRelative(-app.cfg.seekLongSecs()); break;
        case Action::SeekForwardLong: seekRelative(app.cfg.seekLongSecs()); break;
        case Action::CycleSubtitle:
            p.cycleTrack("sub");
            trackHudType_ = 1;
            trackHudDelay_ = 0.2f;
            break;
        case Action::CycleAudio:
            p.cycleTrack("audio");
            trackHudType_ = 2;
            trackHudDelay_ = 0.2f;
            break;
        case Action::Screenshot:
            if (p.screenshot()) app.ui.toast("Screenshot saved", Icon::Camera);
            else app.ui.toast("Couldn't save the screenshot", Icon::Info);
            break;
        case Action::ToggleLock:
            locked_ = !locked_;
            lockHint_ = locked_ ? 2.0f : 0;
            if (locked_) {
                osd_ = false;
                panel_ = P_NONE;
            }
            app.ui.toast(locked_ ? "Controls locked" : "Controls unlocked", locked_ ? Icon::Lock : Icon::Unlock);
            break;
        case Action::SpeedDown:
        case Action::SpeedUp: {
            int idx = 0;
            for (int i = 0; i < kSpeedCount; i++)
                if (std::fabs(kSpeedValues[i] - p.speed) < std::fabs(kSpeedValues[idx] - p.speed)) idx = i;
            idx = std::clamp(idx + (a == Action::SpeedUp ? 1 : -1), 0, kSpeedCount - 1);
            p.setSpeed(kSpeedValues[idx]);
            hud(H_SPEED, kSpeedValues[idx], kSpeedLabels[idx], Icon::Speed);
            break;
        }
        case Action::ToggleStats: app.cfg.s.showStats = !app.cfg.s.showStats; break;
        case Action::CycleAspect:
            aspectIdx_ = (aspectIdx_ + 1) % kAspectCount;
            p.setAspect(kAspectValues[aspectIdx_]);
            hud(H_TEXT, 0, tr("Aspect") + ": " + tr(kAspectLabels[aspectIdx_]), Icon::Expand);
            break;
        case Action::ToggleLike:
            if (app.canCollect(currentPath_)) app.toggleLike(currentPath_);
            break;
        case Action::AddToPlaylist:
            if (app.canCollect(currentPath_)) app.addToPlaylistMenu({currentPath_});
            break;
        case Action::Mute:
            p.setMute(!p.mute);
            hud(H_VOLUME, p.mute ? 0 : (float)p.volume, p.mute ? "Muted" : std::to_string((int)p.volume) + "%",
                p.mute ? Icon::VolumeMute : Icon::Volume);
            break;
        default: break;
    }
}

// ------------------------------------------------------------------ frame

void PlayerScreen::frame() {
    UI& ui = app.ui;
    Player& p = app.player;
    Input& in = *ui.in;
    float dt = ui.dt;
    time_ += dt;
    openTime_ += dt;
    hudTimer_ += dt;
    if (dtTimer_ > 0) dtTimer_ -= dt;
    if (lockHint_ > 0) lockHint_ -= dt;
    if (!currentPath_.empty() && p.duration > 0) curDur_ = p.duration;
    // Settings > Controls: 0 while playing, 1 also while paused, 2 follow the system.
    int awake = app.cfg.s.keepAwake;
    platform::setMediaPlayback(p.loaded && awake != 2 && (awake == 1 || !p.paused));
    memoryWatch(dt);
    heavyVideoWatch(dt);
    if (app.screen != this) return;

    // Optionally power down wireless controllers during long stretches without input.
    bool anyInput = in.held || in.touching || std::fabs(in.lx) > 0.25f || std::fabs(in.ly) > 0.25f ||
                    std::fabs(in.rx) > 0.25f || std::fabs(in.ry) > 0.25f;
    inputIdle_ = anyInput ? 0 : inputIdle_ + dt;
    if (anyInput) controllersOff_ = false;
    int offAfter = app.cfg.controllerOffSecs();
    if (offAfter > 0 && !controllersOff_ && inputIdle_ > offAfter && p.loaded && !p.paused) {
        controllersOff_ = true;
        if (platform::disconnectWirelessControllers() > 0)
            ui.toast("Controllers turned off. Press any button to reconnect", Icon::Gamepad);
    }

    // Playback finished (or failed) → back to the library.
    if (sawPlaying_ && p.idle && openTime_ > 0.5f) {
        exit();
        return;
    }

    saveTimer_ += dt;
    if (saveTimer_ > 15) {
        saveTimer_ = 0;
        // The screen is drawn on this thread, so the SD card write happens on another one.
        if (std::fabs(p.position - lastSavedPos_) > 1) {
            lastSavedPos_ = p.position;
            saveProgress();
            app.lib.saveHistoryInBackground();
        }
    }
    if (sleepRemaining_ > 0 && !p.paused && tickSleep(dt)) {
        showOsd();
        app.ui.toast("Sleep timer: playback paused", Icon::Moon);
    }
    if (trackHudDelay_ > 0) {
        trackHudDelay_ -= dt;
        if (trackHudDelay_ <= 0) {
            const char* type = trackHudType_ == 1 ? "sub" : "audio";
            std::string label = trackHudType_ == 1 ? "Subtitles off" : "No audio";
            const Track* chosen = nullptr;
            for (auto& t : p.tracks)
                if (t.type == type && t.selected) {
                    label = t.label();
                    chosen = &t;
                }
            noteTrack(type, chosen);  // a button cycled to it: remember it like a pick from the list
            hud(H_TEXT, 0, label, trackHudType_ == 1 ? Icon::Captions : Icon::Music);
        }
    }

    if (trackRestore_) restoreTracks();
    if (!musicMode() && (screenDark_ || screenOffIn_ >= 0)) wakeScreen();  // a video came next
    if (musicMode()) {
        if (!ui.blocked && drawScreenOff()) return;
        bool parentBlocked = ui.blocked;
        ui.navEnabled = parentBlocked || !immersive_ || panel_ != P_NONE;
        panelAnim_ = ui.smooth(uid("panel.anim"), panel_ != P_NONE ? 1.0f : 0.0f, 14);
        bool prev = ui.blocked;
        ui.blocked = parentBlocked || panel_ != P_NONE;
        drawMusic(parentBlocked || panel_ != P_NONE);
        ui.blocked = prev;
        if (app.screen != this) return;
        if (!parentBlocked) handleMusicButtons();
        if (app.screen != this) return;
        drawHud();
        if (panelAnim_ > 0.01f) drawPanel();
        return;
    }
    if (imageMode_) {
        drawPicturePlaceholder();
        tickSlideshow(dt);
    } else {
        drawOpening();
    }

    // OSD visibility
    float timeout = app.cfg.osdTimeoutSecs();
    if (osd_ && panel_ == P_NONE && gesture_ == G_NONE && !p.paused && timeout > 0 && !in.touching) {
        osdTimer_ += dt;
        if (osdTimer_ > timeout) osd_ = false;
    }
    if (locked_) osd_ = false;
    osdAlpha_ = ui.smooth(uid("osd.alpha"), osd_ && panel_ == P_NONE && !opening() ? 1.0f : 0.0f, 12);
    // Subtitles rise above the controls while they show, and settle back after.
    {
        int want = app.cfg.s.subPos;
        if (osd_ && panel_ == P_NONE && !musicKind_ && !imageMode_ && !locked_) want = std::min(want, 80);
        if (want != subPosApplied_) {
            subPosApplied_ = want;
            app.player.setProperty("sub-pos", std::to_string(want).c_str());
        }
    }
    panelAnim_ = ui.smooth(uid("panel.anim"), panel_ != P_NONE ? 1.0f : 0.0f, 14);

    bool parentBlocked = ui.blocked;
    bool osdInteractive = osd_ && panel_ == P_NONE && !locked_;
    // A menu opened over the video (parentBlocked) needs the d-pad even while the controls are hidden.
    ui.navEnabled = parentBlocked || (osdInteractive && !imageMode_) || panel_ != P_NONE;

    drawMiniProgress(1 - osdAlpha_);
    if (osdAlpha_ > 0.01f) {
        ui.blocked = parentBlocked || !osdInteractive;
        if (imageMode_) drawPictureUi(osdAlpha_);
        else drawOsd(osdAlpha_);
        ui.blocked = parentBlocked;
        if (app.screen != this) return;
    }
    if (imageMode_) drawPictureInfo();
    if (!parentBlocked && !locked_ && panel_ == P_NONE && !imageMode_ && !musicKind_) drawPills();
    if (app.screen != this) return;

    if (!parentBlocked) {
        if (locked_) {
            int binds[] = {app.cfg.s.bindX, app.cfg.s.bindY, app.cfg.s.bindL, app.cfg.s.bindR, app.cfg.s.bindZL,
                           app.cfg.s.bindZR};
            uint32_t btns[] = {BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR};
            uint32_t unlockBtns = BTN_MINUS;
            for (int i = 0; i < 6; i++)
                if (binds[i] == (int)Action::ToggleLock) unlockBtns |= btns[i];
            if (in.tap || in.pan.began || in.pressed) {
                if (in.pressed & ~unlockBtns) app.ui.toast("Locked — press − or tap the lock to unlock", Icon::Lock);
                lockHint_ = 2.5f;
            }
        } else if (panel_ == P_NONE) {
            handleGestures();
        }
        handleButtons();
    }
    // A hold that the gesture code no longer sees (a panel opened, the controls locked, a menu
    // came up) must not leave the video running fast.
    if (boosting_ && (!in.touching || panel_ != P_NONE || locked_ || parentBlocked)) endBoost();

    drawDoubleTap();
    drawHud();
    if (panelAnim_ > 0.01f) drawPanel();
    drawLocked();

    // Buffering indicator
    // Brief stalls (<1/3 s) are common while seeking; don't flash the spinner for them.
    bufferingTime_ = p.buffering ? bufferingTime_ + dt : 0;
    seekingTime_ = p.seeking ? seekingTime_ + dt : 0;
    // Opening has its own screen (drawOpening); this is for stalls during playback.
    // A seek that takes more than a moment (a jump far ahead, a slow drive) gets the spinner too.
    bool loading = p.loaded && (bufferingTime_ > 0.35f || seekingTime_ > 0.15f);
    // The seek box sits in the same place and shows its own spinner (drawHud).
    if (hud_ == H_SEEK && hudTimer_ < 0.9f && !p.buffering) loading = false;
    float la = ui.smooth(uid("osd.loading"), loading ? 1.0f : 0.0f, 10);
    if (la > 0.01f) {
        ui.pushAlpha(la);
        nvgBeginPath(ui.vg);
        nvgCircle(ui.vg, ui.W / 2, ui.H / 2, 44);
        nvgFillColor(ui.vg, hex(0, 0.45f));
        nvgFill(ui.vg);
        ui.spinner(ui.W / 2, ui.H / 2, 26, ui.t.accent, 4);
        if (p.buffering && isUrl(currentPath_)) ui.text(ui.W / 2, ui.H / 2 + 72, std::to_string(p.cacheBuffering) + "%", 18, hex(0xFFFFFF),
                                 CENTER, true);
        ui.popAlpha();
    }
    if (boosting_) {
        float w = boostDir_ < 0 ? 200 : 130;
        Rect r{ui.W / 2 - w / 2, 24, w, 44};
        ui.fillRect(r, hex(0, 0.55f), 22);
        float pulse = 0.6f + 0.4f * std::sin(time_ * 8);
        // Two arrows pointing the way it's going, mirrored for rewind.
        nvgSave(ui.vg);
        if (boostDir_ < 0) {
            nvgTranslate(ui.vg, r.x + 36, 0);
            nvgScale(ui.vg, -1, 1);
            nvgTranslate(ui.vg, -(r.x + 36), 0);
        }
        ui.icon(Icon::Play, r.x + 30, r.cy(), 16, withAlpha(hex(0xFFFFFF), pulse));
        ui.icon(Icon::Play, r.x + 42, r.cy(), 16, withAlpha(hex(0xFFFFFF), 1.4f - pulse));
        nvgRestore(ui.vg);
        char buf[16];
        snprintf(buf, sizeof buf, "%gx", boostRate_);
        ui.text(r.x + 64, r.cy(), buf, 20, hex(0xFFFFFF), LEFT, true);
        if (boostDir_ < 0) ui.text(r.right() - 14, r.cy(), formatTime(rewindPos_, app.player.duration >= 3600), 14,
                                   hex(0xFFFFFF, 0.75f), RIGHT);
    }
}

// ------------------------------------------------------------------ input

// The chosen hold speed to start with, doubling after 1.5 s and again after 3.5 s, up to 8x.
float PlayerScreen::boostRateFor(float held) const {
    float base = app.cfg.longPressSpeed();
    float r = held > 3.5f ? base * 4 : (held > 1.5f ? base * 2 : base);
    return std::min(r, std::max(base, 8.0f));
}

void PlayerScreen::endBoost() {
    if (!boosting_) return;
    boosting_ = false;
    Player& p = app.player;
    if (boostDir_ > 0) {
        p.setSpeed(boostPrevSpeed_);
        if (boostWasPaused_) p.setPause(true);
    } else if (!boostWasPaused_) {
        p.setPause(false);
    }
}

void PlayerScreen::handleGestures() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    Player& p = app.player;
    Config& cfg = app.cfg;
    const uint32_t videoId = uid("video.gesture");

    // Hold for fast-forward
    bool onControls = osd_ && (in.touchStartY < 110 || in.touchStartY > ui.H - 170 ||
                               (std::fabs(in.touchStartY - ui.H / 2) < 60 && std::fabs(in.touchStartX - ui.W / 2) < 250));
    if (in.longPress && cfg.longPressSpeed() > 0 && !boosting_ && gesture_ == G_NONE && p.loaded && !onControls &&
        !imageMode_) {
        boosting_ = true;
        boostDir_ = in.touchStartX < ui.W / 2 ? -1 : 1;
        boostTime_ = 0;
        boostRate_ = boostRateFor(0);
        boostPrevSpeed_ = p.speed;
        boostWasPaused_ = p.paused;
        if (boostDir_ > 0) {
            p.setSpeed(boostRate_);
            if (p.paused) p.setPause(false);
        } else {
            // mpv can't play backwards: pause and step back with exact seeks instead.
            p.setPause(true);
            rewindPos_ = p.position;
            rewindSeekTimer_ = 0;
        }
    }
    if (boosting_) {
        if (in.longPressEnded || !in.touching) {
            endBoost();
        } else {
            boostTime_ += ui.dt;
            float rate = boostRateFor(boostTime_);
            if (rate != boostRate_) {
                boostRate_ = rate;
                if (boostDir_ > 0) p.setSpeed(rate);
            }
            if (boostDir_ < 0) {
                rewindPos_ = std::max(0.0, rewindPos_ - rate * ui.dt);
                rewindSeekTimer_ -= ui.dt;
                // One seek at a time, so the picture keeps up instead of queueing stale seeks.
                if (rewindSeekTimer_ <= 0 && !p.seeking) {
                    p.seek(rewindPos_, false);
                    rewindSeekTimer_ = 0.12f;
                }
                if (rewindPos_ <= 0) endBoost();
            }
        }
    }

    // Pinch zoom (+ two-finger pan)
    if (cfg.s.gPinchZoom && p.loaded) {
        if (in.pinch.began) {
            zoomBase_ = p.zoom;
            panBaseX_ = p.panX;
            panBaseY_ = p.panY;
            pinchStartX_ = in.pinch.cx;
            pinchStartY_ = in.pinch.cy;
            pinching_ = true;
        }
        if (pinching_ && (in.pinch.active || in.pinch.ended)) {
            double z = std::clamp(zoomBase_ + std::log2(std::max(0.05f, in.pinch.scale)), -2.0, 3.0);
            p.setZoom(z);
            if (in.pinch.active) {
                double scale = std::pow(2.0, z);
                p.setPan(std::clamp(panBaseX_ + (in.pinch.cx - pinchStartX_) / ui.W / scale, -1.0, 1.0),
                         std::clamp(panBaseY_ + (in.pinch.cy - pinchStartY_) / ui.H / scale, -1.0, 1.0));
            }
            int pct = (int)std::lround(std::pow(2.0, z) * 100);
            hud(H_ZOOM, (float)z, std::to_string(pct) + "%", Icon::Expand);
        }
        if (in.pinch.ended) {
            pinching_ = false;
            if (std::fabs(p.zoom) < 0.06) {
                p.setZoom(0);
                p.setPan(0, 0);
                hud(H_ZOOM, 0, "100%", Icon::Expand);
            }
        }
    }

    if (imageMode_) {
        handleImageGestures();
        return;
    }

    // Swipes
    bool onOsd = osd_ && (in.pan.startY < 110 || in.pan.startY > ui.H - 170);
    if (in.pan.began && in.pan.owner == 0 && !onOsd && !boosting_) {
        float dx = in.pan.x - in.pan.startX, dy = in.pan.y - in.pan.startY;
        if (std::fabs(dx) > std::fabs(dy)) {
            if (cfg.s.gSwipeSeek && p.duration > 0) {
                gesture_ = G_SEEK;
                gestureBase_ = p.position;
                seekTarget_ = p.position;
                in.pan.owner = videoId;
            }
        } else {
            bool left = in.pan.startX < ui.W / 2;
            int mode = left ? cfg.s.gLeftVertical : cfg.s.gRightVertical;
            // Left: 0 brightness, 1 volume. Right: 0 volume, 1 brightness.
            bool vol = left ? mode == 1 : mode == 0;
            if (mode != 2) {
                gesture_ = vol ? G_VOLUME : G_BRIGHTNESS;
                gestureValue_ = vol ? (float)p.volume : platform::backlight();
                in.pan.owner = videoId;
            }
        }
    }
    if (in.pan.owner == videoId && (in.pan.active || in.pan.ended)) {
        float tdx = in.pan.x - in.pan.startX, tdy = in.pan.y - in.pan.startY;
        if (gesture_ == G_SEEK) {
            double range = std::clamp(p.duration * 0.1, 60.0, 300.0) * cfg.swipeSensitivity();
            double frac = tdx / ui.W;
            double delta = (frac < 0 ? -1 : 1) * std::pow(std::fabs(frac), 1.25) * range;
            seekTarget_ = std::clamp(gestureBase_ + delta, 0.0, p.duration);
            double rel = seekTarget_ - gestureBase_;
            hud(H_SEEK, (float)rel, (rel >= 0 ? "+" : "−") + formatTime(std::fabs(rel)),
                rel >= 0 ? Icon::SeekForward : Icon::SeekBack);
            lastPreviewSeek_ += ui.dt;
            if (lastPreviewSeek_ > 0.15f) {
                lastPreviewSeek_ = 0;
                p.seekPreview(seekTarget_);
            }
        } else if (gesture_ == G_VOLUME) {
            double v = std::clamp(gestureValue_ - tdy / (ui.H * 0.7) * cfg.maxVolumeValue(), 0.0,
                                  (double)cfg.maxVolumeValue());
            changeVolume(v - p.volume);
        } else if (gesture_ == G_BRIGHTNESS) {
            float v = std::clamp(gestureValue_ - tdy / (ui.H * 0.7f), 0.0f, 1.0f);
            changeBrightness(v - platform::backlight());
        }
        if (in.pan.ended) {
            if (gesture_ == G_SEEK) p.seek(seekTarget_, false);
            gesture_ = G_NONE;
        }
    }
    if (!in.pan.active && gesture_ != G_NONE && gesture_ != G_SCRUB) gesture_ = G_NONE;

    // Double tap seeking (YouTube style: keep tapping to add more)
    float step = (float)cfg.doubleTapSecs();
    auto side = [&](float x) { return x < ui.W * 0.38f ? -1 : (x > ui.W * 0.62f ? 1 : 0); };
    if (cfg.s.gDoubleTap) {
        if (in.doubleTap) {
            int s = side(in.tapX);
            in.consumeTap();
            if (s == 0) {
                p.togglePause();
                hud(p.paused ? H_PAUSE : H_PLAY, 0);
            } else {
                dtSide_ = s;
                dtAmount_ = (int)step;
                dtTimer_ = 0.75f;
                dtX_ = in.tapX;
                dtY_ = in.tapY;
                seekRelative(s * step, false);
            }
        } else if (in.tap && dtTimer_ > 0 && side(in.tapX) == dtSide_ && dtSide_ != 0) {
            in.consumeTap();
            dtAmount_ += (int)step;
            dtTimer_ = 0.75f;
            dtX_ = in.tapX;
            dtY_ = in.tapY;
            seekRelative(dtSide_ * step, false);
        } else if (in.tapConfirmed && dtTimer_ <= 0) {
            osd_ = !osd_;
            osdTimer_ = 0;
        }
    } else if (in.tap) {
        in.consumeTap();
        osd_ = !osd_;
        osdTimer_ = 0;
    }
}

// Pictures: swipe for next/previous, drag to move when zoomed, double-tap to zoom.
void PlayerScreen::handleImageGestures() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    Player& p = app.player;
    const uint32_t id = uid("image.gesture");
    bool zoomed = std::fabs(p.zoom) > 0.01;
    bool onOsd = osd_ && in.pan.startY < 110;
    if (in.pan.began && in.pan.owner == 0 && !onOsd) {
        in.pan.owner = id;
        gesture_ = zoomed ? G_PAN : G_SEEK;
        panBaseX_ = p.panX;
        panBaseY_ = p.panY;
    }
    if (in.pan.owner == id && (in.pan.active || in.pan.ended)) {
        float tdx = in.pan.x - in.pan.startX, tdy = in.pan.y - in.pan.startY;
        if (gesture_ == G_PAN) {
            double scale = std::pow(2.0, p.zoom);
            p.setPan(std::clamp(panBaseX_ + tdx / ui.W / scale, -1.0, 1.0),
                     std::clamp(panBaseY_ + tdy / ui.H / scale, -1.0, 1.0));
        } else if (in.pan.ended && std::fabs(tdx) > 90 && std::fabs(tdx) > std::fabs(tdy)) {
            doAction(tdx < 0 ? Action::NextFile : Action::PrevFile);
        }
        if (in.pan.ended) gesture_ = G_NONE;
    }
    if (in.doubleTap) {
        in.consumeTap();
        p.setZoom(zoomed ? 0 : 1);
        p.setPan(0, 0);
        hud(H_ZOOM, zoomed ? 0 : 1, zoomed ? "100%" : "200%", Icon::Expand);
    } else if (in.tapConfirmed) {
        osd_ = !osd_;
        osdTimer_ = 0;
    }
}

void PlayerScreen::handleButtons() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    Player& p = app.player;
    Config& cfg = app.cfg;

    if (in.pressed && osd_) osdTimer_ = 0;
    if (resumePill_ > 0 && panel_ == P_NONE && !locked_ && in.down(BTN_Y)) {  // "Start over"
        in.pressed &= ~BTN_Y;
        resumePill_ = 0;
        p.seek(0, false);
        hud(H_TEXT, 0, tr("From the beginning"), Icon::Refresh);
        return;
    }
    if (nextPill_ && panel_ == P_NONE && !locked_ && in.down(BTN_Y)) {  // "Up next · Play now"
        in.pressed &= ~BTN_Y;
        nextPill_ = false;
        p.next();
        return;
    }
    if (opening() && panel_ == P_NONE) {
        if (ui.back()) exit();
        return;
    }

    if (locked_) {
        bool unlock = in.down(BTN_MINUS);
        int binds[] = {cfg.s.bindX, cfg.s.bindY, cfg.s.bindL, cfg.s.bindR, cfg.s.bindZL, cfg.s.bindZR};
        uint32_t btns[] = {BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR};
        for (int i = 0; i < 6; i++)
            if (binds[i] == (int)Action::ToggleLock && in.down(btns[i])) unlock = true;
        if (unlock) doAction(Action::ToggleLock);
        return;
    }

    if (panel_ != P_NONE) {
        if (ui.back() || in.down(BTN_PLUS)) {
            if (subPanel(panel_)) openPanel(P_SUBS);
            else panel_ = P_NONE;
            ui.setFocus(uid("osd.play"));
        }
        return;
    }

    if (imageMode_) {
        handlePictureButtons();
        return;
    }

    // Bound actions work whether or not the OSD is shown.
    struct Bind { uint32_t btn; int act; } binds[] = {
        {BTN_X, cfg.s.bindX}, {BTN_Y, cfg.s.bindY}, {BTN_L, cfg.s.bindL},
        {BTN_R, cfg.s.bindR}, {BTN_ZL, cfg.s.bindZL}, {BTN_ZR, cfg.s.bindZR},
    };
    for (auto& b : binds) {
        Action a = (Action)b.act;
        bool seek = a == Action::SeekBackLong || a == Action::SeekForwardLong;
        bool repeatable = seek || a == Action::SpeedDown || a == Action::SpeedUp;
        if (!seek) {
            if (repeatable ? in.rep(b.btn) : in.down(b.btn)) doAction(a);
            continue;
        }
        // Held seeks go at a steady five a second (the general repeat speeds up to ~30, which went
        // through a whole film in two seconds), and stop short of the end instead of running on into
        // the next file.
        float held = in.holdTime((Btn)b.btn);
        bool fire = in.down(b.btn);
        if (!fire && in.isHeld(b.btn) && held > 0.4f) {
            float prev = held - ui.dt;
            fire = std::floor((held - 0.4f) / 0.2f) != std::floor(std::max(0.0f, prev - 0.4f) / 0.2f) || prev <= 0.4f;
            bool forward = a == Action::SeekForwardLong;
            if (fire && forward && p.duration > 0 && p.position > p.duration - 15) fire = false;
        }
        if (fire) doAction(a);
    }
    if (in.down(BTN_MINUS)) doAction(Action::ToggleLock);
    if (in.down(BTN_PLUS)) openPanel(P_MORE);

    if (osd_) {
        if (ui.back()) exit();
        return;
    }

    // Controls hidden: d-pad drives playback directly.
    if (ui.confirm()) {
        p.togglePause();
        hud(p.paused ? H_PAUSE : H_PLAY, 0);
    }
    if (ui.back()) {
        showOsd();
        ui.setFocus(uid("osd.play"));
        ui.toast("Press B again to close the player", Icon::Back);
    }
    float hold = std::max(in.holdTime(BTN_LEFT), in.holdTime(BTN_RIGHT));
    double mul = hold > 3 ? 4 : (hold > 1.2f ? 2 : 1);
    if (in.rep(BTN_LEFT)) seekRelative(-cfg.seekShortSecs() * mul);
    if (in.rep(BTN_RIGHT)) seekRelative(cfg.seekShortSecs() * mul);
    int vdir = in.rep(BTN_UP) ? 1 : (in.rep(BTN_DOWN) ? -1 : 0);
    if (vdir) {
        switch (cfg.s.dpadVertical) {
            case 0: changeVolume(vdir * 5); break;
            case 1: changeBrightness(vdir * 0.05f); break;
            case 2:
                if (in.down(BTN_UP) || in.down(BTN_DOWN))
                    doAction(vdir > 0 ? Action::NextChapter : Action::PrevChapter);
                break;
        }
    }
}

// ------------------------------------------------------------------ drawing

// While a video opens: its frame or poster, its title and a spinner, so the wait looks intended
// rather than like a frozen black screen. Fades out with the first frames.
bool PlayerScreen::opening() const {
    const Player& p = app.player;
    return !imageMode_ && (!p.loaded || (!p.paused && p.position <= 0.01 && openTime_ < 3));
}

void PlayerScreen::drawOpening() {
    UI& ui = app.ui;
    Player& p = app.player;
    bool opening = this->opening();
    float a = ui.smooth(uid("open.fade"), opening ? 1.0f : 0.0f, opening ? 30.0f : 7.0f);
    if (a < 0.01f) return;
    std::string path = currentPath_;
    if (path.empty() && p.playlistPos >= 0 && p.playlistPos < (int64_t)p.playlist.size()) path = p.playlist[p.playlistPos];
    NVGcontext* vg = ui.vg;
    float W = ui.W, H = ui.H;
    ui.pushAlpha(a);
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, H);
    nvgFillColor(vg, nvgRGB(5, 6, 10));
    nvgFill(vg);
    const CoverArt::Art* art = path.empty() ? nullptr : app.covers.get(path, MediaKind::Video);
    if (art && art->blurred) {
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, W, H);
        nvgFillPaint(vg, nvgImagePattern(vg, -W * 0.1f, -H * 0.1f, W * 1.2f, H * 1.2f, 0, art->blurred, 0.55f));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, W, H);
        nvgFillColor(vg, nvgRGBAf(0, 0, 0, 0.45f));
        nvgFill(vg);
    }
    float cy = H / 2 - 40;
    if (art) {
        float ph = std::min(H * 0.42f, 300.0f), pw = ph * art->w / art->h;
        if (pw > W * 0.5f) {
            pw = W * 0.5f;
            ph = pw * art->h / art->w;
        }
        Rect poster{W / 2 - pw / 2, cy - ph / 2, pw, ph};
        ui.shadow(poster, 14, 30, 0.6f, 10);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, poster.x, poster.y, poster.w, poster.h, 14);
        nvgFillPaint(vg, nvgImagePattern(vg, poster.x, poster.y, poster.w, poster.h, 0, art->image, 1));
        nvgFill(vg);
        cy = poster.bottom();
    } else {
        nvgBeginPath(vg);
        nvgCircle(vg, W / 2, cy, 64);
        nvgFillPaint(vg, nvgLinearGradient(vg, W / 2 - 64, cy - 64, W / 2 + 64, cy + 64, ui.t.accent, ui.t.accent2));
        nvgFill(vg);
        ui.icon(Icon::Film, W / 2, cy, 54, hex(0xFFFFFF));
        cy += 64;
    }
    std::string title = path.empty() ? stripExtension(p.title) : shownTitle(path, app.cfg.s.tidyNames);
    // Only when the playlist's current entry is the file being opened: mpv reports the new position
    // a moment after the new path, and in between this would put the next entry's name on screen.
    size_t pos = (size_t)p.playlistPos;
    if (p.playlistPos >= 0 && pos < p.playlist.size() && p.playlist[pos] == path)
        title = entryName(p, pos, app.cfg.s.tidyNames);
    auto named = p.titles.find(path);  // a stream by its name in the Streams list
    if (named != p.titles.end()) title = named->second;
    ui.text(W / 2, cy + 42, ui.ellipsize(title, W - 160, 24, true), 24, hex(0xFFFFFF), CENTER, true);
    if (!failedPath_.empty() && p.path == failedPath_ && !p.loaded) {
        // It isn't opening any more, so saying so would be a lie and the spinner would never stop.
        // The toast that reported the error has long faded by the time anyone reads this.
        std::string why = p.lastError.empty() ? tr("Can't play this") : trf("Can't play: {0}", {p.lastError});
        ui.icon(Icon::Info, W / 2 - 46, cy + 80, 20, hex(0xFFFFFF, 0.7f));
        ui.text(W / 2 - 28, cy + 80, ui.ellipsize(why, W - 200, 17, true), 17, hex(0xFFFFFF, 0.7f), LEFT, true);
        ui.text(W / 2, cy + 112, tr("Press B to go back"), 15, hex(0xFFFFFF, 0.45f), CENTER, true);
    } else {
        ui.spinner(W / 2 - 46, cy + 80, 10, ui.t.accent, 3);
        ui.text(W / 2 - 28, cy + 80, "Opening", 17, hex(0xFFFFFF, 0.7f), LEFT, true);
    }
    // The controls stay hidden until there's a picture; leaving must still be possible.
    if (opening && ui.iconButton(uid("open.back"), 50, 50, 52, Icon::Back, hex(0xFFFFFF), 0, false)) {
        ui.popAlpha();
        exit();
        return;
    }
    ui.popAlpha();
}

void PlayerScreen::drawSeekBar(Rect r, float a) {
    UI& ui = app.ui;
    Player& p = app.player;
    Input& in = *ui.in;
    NVGcontext* vg = ui.vg;
    Rect hit = r.inset(-4, -14);
    ui.focusable(seekBarId_, r);
    bool foc = ui.focused(seekBarId_);
    double dur = p.duration;

    // Touch scrubbing
    if (!ui.blocked && dur > 0) {
        if (in.touchBegan && hit.contains(in.touchX, in.touchY)) {
            gesture_ = G_SCRUB;
            seekTarget_ = std::clamp((in.touchX - r.x) / r.w, 0.0f, 1.0f) * dur;
        }
        if (gesture_ == G_SCRUB) {
            if (in.pan.began && in.pan.owner == 0) in.pan.owner = seekBarId_;
            if (in.touching) {
                seekTarget_ = std::clamp((in.touchX - r.x) / r.w, 0.0f, 1.0f) * dur;
                osdTimer_ = 0;
                lastPreviewSeek_ += ui.dt;
                if (in.touchMoved && lastPreviewSeek_ > 0.12f) {
                    lastPreviewSeek_ = 0;
                    p.seekPreview(seekTarget_);
                }
            } else {
                p.seek(seekTarget_, false);
                gesture_ = G_NONE;
                in.consumeTap();
            }
        }
        if (foc) {
            float hold = std::max(in.holdTime(BTN_LEFT), in.holdTime(BTN_RIGHT));
            double step = app.cfg.seekShortSecs() * (hold > 3 ? 4 : (hold > 1.2f ? 2 : 1));
            if (in.rep(BTN_LEFT)) seekRelative(-step);
            if (in.rep(BTN_RIGHT)) seekRelative(step);
            ui.consumeNav(BTN_LEFT | BTN_RIGHT);
        }
    }

    bool scrubbing = gesture_ == G_SCRUB || gesture_ == G_SEEK;
    double shown = scrubbing ? seekTarget_ : p.position;
    float frac = dur > 0 ? (float)std::clamp(shown / dur, 0.0, 1.0) : 0;
    float cacheFrac = dur > 0 ? (float)std::clamp(p.cacheTime / dur, 0.0, 1.0) : 0;
    int style = app.cfg.s.seekbarStyle;
    float th = style == 0 ? 4 : (style == 1 ? 7 : 5);
    th *= 1 + 0.4f * ui.smooth(seekBarId_ ^ 0x77, scrubbing || (foc && ui.showFocus()) ? 1.0f : 0.0f, 14);
    float cy = r.cy();

    ui.fillRect({r.x, cy - th / 2, r.w, th}, hex(0xFFFFFF, 0.22f), th / 2);
    if (cacheFrac > frac) ui.fillRect({r.x, cy - th / 2, r.w * cacheFrac, th}, hex(0xFFFFFF, 0.3f), th / 2);
    if (style == 2) {
        nvgBeginPath(vg);
        nvgRect(vg, r.x - 10, cy - 20, r.w * frac + 20, 40);
        nvgFillPaint(vg, nvgBoxGradient(vg, r.x, cy - th / 2, r.w * frac, th, th / 2, 14,
                                        withAlpha(ui.t.accent, 0.7f), withAlpha(ui.t.accent, 0)));
        nvgFill(vg);
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, cy - th / 2, std::max(th, r.w * frac), th, th / 2);
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, 0, r.right(), 0, ui.t.accent, ui.t.accent2));
    nvgFill(vg);

    // Chapter gaps
    if (dur > 0) {
        for (auto& c : p.chapters) {
            if (c.time <= 0.5 || c.time >= dur - 0.5) continue;
            float x = r.x + r.w * (float)(c.time / dur);
            nvgBeginPath(vg);
            nvgRect(vg, x - 1.5f, cy - th / 2 - 1, 3, th + 2);
            nvgFillColor(vg, hex(0, 0.75f));
            nvgFill(vg);
        }
        for (double ab : {p.abA, p.abB}) {
            if (ab < 0) continue;
            float x = r.x + r.w * (float)(ab / dur);
            ui.fillRect({x - 2, cy - 12, 4, 24}, ui.t.warning, 2);
        }
    }

    float kx = r.x + r.w * frac;
    float kr = ui.smooth(seekBarId_ ^ 0x6b, scrubbing ? 12.0f : (foc && ui.showFocus() ? 10.0f : 7.0f), 18);
    if (foc && ui.showFocus()) {
        nvgBeginPath(vg);
        nvgCircle(vg, kx, cy, kr + 7);
        nvgFillColor(vg, withAlpha(ui.t.accent, 0.35f));
        nvgFill(vg);
    }
    nvgBeginPath(vg);
    nvgCircle(vg, kx, cy + 1, kr + 1);
    nvgFillColor(vg, hex(0, 0.35f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, kx, cy, kr);
    nvgFillColor(vg, hex(0xFFFFFF));
    nvgFill(vg);

    // Scrub bubble
    if (scrubbing && dur > 0) {
        std::string t = formatTime(seekTarget_);
        std::string chap;
        for (size_t i = 0; i < p.chapters.size(); i++)
            if (p.chapters[i].time <= seekTarget_) chap = chapterName(p, i);
        float w = std::max(ui.textWidth(t, 22, true), chap.empty() ? 0 : std::min(260.0f, ui.textWidth(chap, 14))) + 32;
        float h = chap.empty() ? 44 : 62;
        float bx = std::clamp(kx - w / 2, r.x, r.right() - w);
        Rect b{bx, cy - 28 - h, w, h};
        ui.shadow(b, 12, 16, 0.4f, 4);
        ui.fillRect(b, hex(0x101218, 0.92f), 12);
        ui.text(b.cx(), b.y + 22, t, 22, hex(0xFFFFFF), CENTER, true);
        if (!chap.empty()) ui.text(b.cx(), b.y + 45, ui.ellipsize(chap, 260, 14), 14, hex(0xFFFFFF, 0.7f), CENTER);
    }
    (void)a;
}

void PlayerScreen::drawOsd(float a) {
    UI& ui = app.ui;
    Player& p = app.player;
    NVGcontext* vg = ui.vg;
    float W = ui.W, H = ui.H;
    ui.pushAlpha(a);

    // Scrims
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, 160);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, 160, hex(0, 0.72f), hex(0, 0)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, 0, H - 240, W, 240);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, H - 240, 0, H, hex(0, 0), hex(0, 0.85f)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, H);
    nvgFillColor(vg, hex(0, 0.12f));
    nvgFill(vg);

    NVGcolor white = hex(0xFFFFFF);
    NVGcolor dim = hex(0xFFFFFF, 0.7f);

    // Top bar
    float slide = -20 * (1 - a);
    if (ui.iconButton(uid("osd.back"), 50, 50 + slide, 52, Icon::Back, white, 0, false)) {
        exit();
        ui.popAlpha();
        return;
    }
    std::string sub;
    if (p.width > 0) sub = std::to_string(p.width) + "×" + std::to_string(p.height);
    if (!p.videoCodec.empty()) sub += (sub.empty() ? "" : "  ·  ") + p.videoCodec;
    if (!p.hwdecCurrent.empty() && p.hwdecCurrent != "no") sub += "  ·  HW";
    if (p.playlistCount > 1)
        sub += (sub.empty() ? "" : "  ·  ") + tr(std::to_string(p.playlistPos + 1) + " of " +
                                                  std::to_string(p.playlistCount));
    float titleRight = W - 200;
    // A local video by its tidied name: the file's own title tag is often a release name too.
    std::string osdTitle = !currentPath_.empty() && !isUrl(currentPath_) && app.cfg.s.tidyNames &&
                                   kindForExtension(fileExtension(currentPath_)) == MediaKind::Video
                               ? tidyTitle(displayName(currentPath_))
                               : fromDiskName(p.title);
    ui.marquee(uid("osd.title"), 90, 40 + slide, titleRight - 90, osdTitle, 22, white, LEFT, true);
    ui.text(90, 66 + slide, ui.ellipsize(sub, titleRight - 90, 15), 15, dim);
    int pct = platform::batteryPercent();
    float rx = W - 34;
    if (pct >= 0) {
        rx -= 30;
        ui.battery(rx, 46 + slide, pct, platform::charging(), true);
        if (app.cfg.s.batteryPct) {
            ui.text(rx - 8, 47 + slide, std::to_string(pct) + "%", 16, dim, RIGHT);
            rx -= ui.textWidth(std::to_string(pct) + "%", 16) + 8;
        }
        rx -= 18;
    }
    ui.text(rx, 46 + slide, clockString(app.cfg.s.clock24), 20, white, RIGHT, true);

    // Centre transport
    float cx = W / 2, cy = H / 2;
    bool multi = p.playlistCount > 1;
    ui.setDefaultFocus(uid("osd.play"));
    float navOffset = 210;
    if (ui.iconButton(uid("osd.prev"), cx - navOffset, cy, 60, Icon::Prev, multi ? white : hex(0xFFFFFF, 0.35f)))
        doAction(Action::PrevFile);
    int ss = app.cfg.seekShortSecs();
    if (ui.iconButton(uid("osd.rew"), cx - 110, cy, 68, Icon::SeekBack, white)) seekRelative(-ss);
    float pr = 96;
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, pr / 2);
    nvgFillColor(vg, withAlpha(ui.t.accent, 0.9f));
    nvgFill(vg);
    if (ui.iconButton(uid("osd.play"), cx, cy, pr, p.paused ? Icon::Play : Icon::Pause, white, 0, false)) {
        p.togglePause();
        osdTimer_ = 0;
    }
    if (ui.iconButton(uid("osd.fwd"), cx + 110, cy, 68, Icon::SeekForward, white)) seekRelative(ss);
    if (ui.iconButton(uid("osd.next"), cx + 210, cy, 60, Icon::Next, multi ? white : hex(0xFFFFFF, 0.35f)))
        doAction(Action::NextFile);

    // Times + seek bar
    float barY = H - 112;
    double shown = (gesture_ == G_SCRUB || gesture_ == G_SEEK) ? seekTarget_ : p.position;
    ui.text(40, barY - 28, formatTime(shown, p.duration >= 3600), 18, white, LEFT, true);
    std::string right = app.cfg.s.showRemaining && p.duration > 0
                            ? "−" + formatTime(std::max(0.0, p.duration - shown), p.duration >= 3600)
                            : formatTime(p.duration);
    ui.text(W - 40, barY - 28, right, 18, dim, RIGHT, true);
    if (p.chapter >= 0 && p.chapter < (int64_t)p.chapters.size()) {
        ui.text(W / 2, barY - 28, ui.ellipsize(chapterName(p, (size_t)p.chapter), W * 0.4f, 15), 15, dim, CENTER);
    }
    drawSeekBar({40, barY - 10, W - 80, 20}, a);

    // Bottom pills
    struct Pill { const char* key; Icon ic; std::string label; Action act; bool show; };
    char speedBuf[16];
    snprintf(speedBuf, sizeof speedBuf, "%gx", p.speed);
    Pill pills[] = {
        {"pill.lock", Icon::Lock, "Lock", Action::ToggleLock, true},
        {"pill.subs", Icon::Captions, "Subtitles", Action::SubtitlePanel, true},
        {"pill.audio", Icon::Music, "Audio", Action::AudioPanel, true},
        {"pill.speed", Icon::Speed, speedBuf, Action::SpeedPanel, true},
        {"pill.video", Icon::Sliders, "Video", Action::VideoPanel, true},
        {"pill.chap", Icon::Bookmark, "Chapters", Action::ChapterPanel, !p.chapters.empty()},
        {"pill.list", Icon::Playlist, "Queue", Action::PlaylistPanel, p.playlistCount > 1},
        {"pill.like", app.lib.isLiked(currentPath_) ? Icon::HeartFilled : Icon::Heart,
         app.lib.isLiked(currentPath_) ? "Liked" : "Like", Action::ToggleLike, app.canCollect(currentPath_)},
    };
    float px = 34, py = H - 58, ph = 44;
    float total = 0;
    for (auto& pl : pills)
        if (pl.show) total += ui.textWidth(pl.label, 16, true) + 64;
    bool iconsOnly = total + 80 > W - 68;
    for (auto& pl : pills) {
        if (!pl.show) continue;
        float w = iconsOnly ? ph : ui.textWidth(pl.label, 16, true) + 56;
        Rect r{px, py, w, ph};
        uint32_t id = uid(pl.key);
        ui.focusable(id, r);
        bool foc = ui.focused(id) && ui.showFocus();
        float fv = ui.smooth(id ^ 0xf0c, foc ? 1.0f : 0.0f, 16);
        float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
        ui.fillRect(r, hex(0xFFFFFF, 0.12f + 0.12f * pv + 0.1f * fv), ph / 2);
        if (fv > 0.01f) ui.strokeRect(r.inset(-1.5f), withAlpha(ui.t.accent, fv), 3, ph / 2 + 1.5f);
        if (iconsOnly) {
            ui.icon(pl.ic, r.cx(), r.cy(), 20, white);
        } else {
            ui.icon(pl.ic, r.x + 24, r.cy(), 20, white);
            ui.text(r.x + 42, r.cy(), pl.label, 16, white, LEFT, true);
        }
        if (ui.tapped(r) || (ui.focused(id) && ui.confirm())) doAction(pl.act);
        px += w + 10;
    }
    // More button on the far right
    if (ui.iconButton(uid("pill.more"), W - 34 - ph / 2, py + ph / 2, ph, Icon::Gear, white)) openPanel(P_MORE);
    // Status chips (sleep timer, A-B loop)
    float sx = W - 34 - ph - 14;
    auto chip = [&](const std::string& s, Icon ic) {
        float w = ui.textWidth(s, 15, true) + 46;
        Rect r{sx - w, py + 6, w, ph - 12};
        ui.fillRect(r, withAlpha(ui.t.accent, 0.3f), r.h / 2);
        ui.icon(ic, r.x + 18, r.cy(), 16, white);
        ui.text(r.x + 32, r.cy(), s, 15, white, LEFT, true);
        sx -= w + 8;
    };
    if (sleepRemaining_ > 0) chip(formatTime(sleepRemaining_), Icon::Moon);
    else if (sleepEndOfFile_) chip(tr("End of this video"), Icon::Moon);
    if (p.abA >= 0) chip(p.abB >= 0 ? "A-B loop" : "A set", Icon::Repeat);
    if (std::fabs(p.zoom) > 0.01) chip(std::to_string((int)std::lround(std::pow(2.0, p.zoom) * 100)) + "%", Icon::Expand);

    ui.popAlpha();
}

// Small pills over the video: "Resumed at 23:10 · Start over" for a few seconds after resuming,
// and "Up next" in the last seconds of an episode with more after it.
void PlayerScreen::drawPills() {
    UI& ui = app.ui;
    Player& p = app.player;
    float dt = ui.dt;
    float baseY = ui.H - (osd_ ? 216 : 92);  // above the controls (and the time) when they show
    auto pill = [&](uint32_t id, float x, float alpha, const std::string& lead, const std::string& action,
                    uint32_t btn) {
        float lw = ui.textWidth(lead, 16), aw = ui.textWidth(action, 16, true);
        float w = 24 + lw + 14 + (btn ? 30 : 0) + aw + 22;
        Rect r{x, baseY, w, 46};
        ui.pushAlpha(alpha);
        float pv = ui.smooth(id, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
        ui.fillRect(r, hex(0x000000, 0.62f + 0.15f * pv), 23);
        ui.strokeRect(r.inset(0.5f), hex(0xFFFFFF, 0.12f), 1, 23);
        float tx = r.x + 24;
        ui.text(tx, r.cy(), lead, 16, hex(0xFFFFFF, 0.8f));
        tx += lw + 14;
        if (btn) {
            ui.glyph(btn, tx + 12, r.cy(), 22);
            tx += 30;
        }
        ui.text(tx, r.cy(), action, 16, ui.t.accent, LEFT, true);
        ui.popAlpha();
        return alpha > 0.5f && ui.tapped(r);
    };
    // Resumed: a way back to the beginning.
    // Moved well away from where it resumed (a seek): "Start over" no longer fits.
    if (resumePill_ > 0 && p.loaded && std::fabs(p.position - resumedFrom_) > 20) resumePill_ = 0;
    if (resumePill_ > 0) {
        resumePill_ -= dt;
        float a = std::min(1.0f, resumePill_ / 0.4f);
        std::string lead = trf("Resumed at {0}", {formatTime(resumedFrom_)});
        if (pill(uid("pill.resume"), 40, a, lead, tr("Start over"), BTN_Y)) {
            resumePill_ = 0;
            p.seek(0, false);
            hud(H_TEXT, 0, tr("From the beginning"), Icon::Refresh);
        }
    }
    // The next episode, in the last 10 seconds.
    bool nextUp = p.loaded && p.duration > 30 && !p.paused && app.cfg.s.loopMode != 1 && p.playlistPos + 1 < p.playlistCount &&
                  p.playlistPos + 1 < (int64_t)p.playlist.size() && p.duration - p.position <= 10 && !boosting_;
    float na = ui.smooth(uid("pill.next.a"), nextUp ? 1.0f : 0.0f, 8);
    nextPill_ = nextUp && resumePill_ <= 0;
    if (na > 0.01f && p.playlistPos + 1 < (int64_t)p.playlist.size()) {
        std::string name = entryName(p, (size_t)p.playlistPos + 1, app.cfg.s.tidyNames);
        std::string lead = tr("Up next") + ": " + ui.ellipsize(name, 360, 16);
        float w = 24 + ui.textWidth(lead, 16) + 14 + 30 + ui.textWidth(tr("Play now"), 16, true) + 22;
        if (pill(uid("pill.next"), ui.W - 40 - w, na, lead, tr("Play now"), BTN_Y)) p.next();
    }
}

// Counts the timer down (while playing); in its last half minute the sound fades out. True when it
// has just run out and paused playback.
bool PlayerScreen::tickSleep(float dt) {
    Player& p = app.player;
    sleepRemaining_ -= dt;
    if (sleepRemaining_ > 0 && sleepRemaining_ < kSleepFade) {
        if (sleepFadeFrom_ < 0) sleepFadeFrom_ = p.volume;
        double v = sleepFadeFrom_ * std::max(0.0f, sleepRemaining_) / kSleepFade;
        if (std::fabs(v - p.volume) > 0.5) p.setVolume(v);
    }
    if (sleepRemaining_ > 0) return false;
    sleepRemaining_ = -1;
    sleepIdx_ = 0;
    p.setPause(true);
    endSleepFade();
    return true;
}

// The volume as it was before the fade (the timer ran out, was changed or was turned off).
void PlayerScreen::endSleepFade() {
    if (sleepFadeFrom_ >= 0) app.player.setVolume(sleepFadeFrom_);
    sleepFadeFrom_ = -1;
}

void PlayerScreen::askGoToTime() {
    Player& p = app.player;
    std::string s;
    if (!app.askText("Go to time (h:mm:ss)", formatTime(p.position), s)) return;
    // "1:02:03", "62:03", "3723" (Persian or Arabic digits too)
    double t = 0;
    std::string num;
    std::vector<double> parts;
    for (size_t i = 0; i <= s.size(); i++) {
        unsigned char c = i < s.size() ? (unsigned char)s[i] : ':';
        if (c == 0xDB && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0xB0 && (unsigned char)s[i + 1] <= 0xB9) {
            num += (char)('0' + (unsigned char)s[++i] - 0xB0);
        } else if (c == 0xD9 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0xA0 && (unsigned char)s[i + 1] <= 0xA9) {
            num += (char)('0' + (unsigned char)s[++i] - 0xA0);
        } else if (isdigit(c) || c == '.') {
            num += (char)c;
        } else if (c == ':' || c == ' ') {
            if (!num.empty()) parts.push_back(atof(num.c_str()));
            num.clear();
        } else {
            app.ui.toast("Type a time like 1:05:30", Icon::Clock);
            return;
        }
    }
    if (parts.empty() || parts.size() > 3) {
        app.ui.toast("Type a time like 1:05:30", Icon::Clock);
        return;
    }
    for (double v : parts) t = t * 60 + v;
    if (p.duration > 0) t = std::min(t, std::max(0.0, p.duration - 1));
    p.seek(t, false);
    hud(H_TEXT, 0, formatTime(t), Icon::Clock);
}

void PlayerScreen::drawMiniProgress(float a) {
    UI& ui = app.ui;
    Player& p = app.player;
    int mode = app.cfg.s.miniProgress;  // 0 off, 1 subtle hairline, 2 the accent colours
    if (!mode || a < 0.01f || p.duration <= 0 || locked_ || imageMode_) return;
    float frac = (float)std::clamp(p.position / p.duration, 0.0, 1.0);
    // Subtle: a 2 px dim line that reads as an edge, not a lit bar over a dark scene.
    float h = mode == 1 ? 2.0f : 4.0f;
    ui.pushAlpha(a * (mode == 1 ? 0.38f : 1.0f));
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, 0, ui.H - h, ui.W * frac, h);
    if (mode == 1) nvgFillColor(ui.vg, hex(0xFFFFFF));
    else nvgFillPaint(ui.vg, nvgLinearGradient(ui.vg, 0, 0, ui.W, 0, ui.t.accent, ui.t.accent2));
    nvgFill(ui.vg);
    ui.popAlpha();
}

void PlayerScreen::drawHud() {
    UI& ui = app.ui;
    bool held = gesture_ != G_NONE && gesture_ != G_SCRUB;
    float target = (hud_ != H_NONE && (hudTimer_ < 0.9f || held)) ? 1.0f : 0.0f;
    float a = ui.smooth(uid("hud.alpha"), target, 14);
    if (a < 0.01f || hud_ == H_NONE) return;
    ui.pushAlpha(a);
    float W = ui.W, H = ui.H;
    NVGcolor white = hex(0xFFFFFF);

    if (hud_ == H_PLAY || hud_ == H_PAUSE) {
        float s = 1 + 0.25f * ui.ease(hudTimer_ / 0.5f);
        nvgBeginPath(ui.vg);
        nvgCircle(ui.vg, W / 2, H / 2, 46 * s);
        nvgFillColor(ui.vg, hex(0, 0.5f));
        nvgFill(ui.vg);
        ui.icon(hud_ == H_PLAY ? Icon::Play : Icon::Pause, W / 2 + (hud_ == H_PLAY ? 3 : 0), H / 2, 44 * s, white);
    } else if (hud_ == H_SEEK) {
        Player& p = app.player;
        double shownPos = (gesture_ == G_SEEK) ? seekTarget_ : p.position;
        float w = 260, h = 110;
        Rect r{W / 2 - w / 2, H / 2 - h / 2 - (osd_ ? 110 : 0), w, h};
        ui.fillRect(r, hex(0, 0.6f), 22);
        // A seek still being carried out: the spinner instead of the arrow.
        if (p.seeking && seekingTime_ > 0.15f) ui.spinner(r.x + 40, r.y + 40, 13, ui.t.accent, 3);
        else ui.icon(hudIcon_, r.x + 40, r.y + 40, 30, white);
        ui.text(r.x + 70, r.y + 42, hudText_, 34, white, LEFT, true);
        ui.text(r.cx(), r.y + 84, formatTime(shownPos) + " / " + formatTime(p.duration), 17, hex(0xFFFFFF, 0.75f),
                CENTER);
    } else if (hud_ == H_VOLUME || hud_ == H_BRIGHTNESS) {
        float frac = hud_ == H_VOLUME ? hudValue_ / (float)app.cfg.maxVolumeValue() : hudValue_;
        float w = 300, h = 60;
        Rect r{W / 2 - w / 2, 70, w, h};
        ui.fillRect(r, hex(0, 0.6f), h / 2);
        ui.icon(hudIcon_, r.x + 32, r.cy(), 24, white);
        Rect bar{r.x + 60, r.cy() - 4, w - 140, 8};
        ui.fillRect(bar, hex(0xFFFFFF, 0.25f), 4);
        nvgBeginPath(ui.vg);
        nvgRoundedRect(ui.vg, bar.x, bar.y, bar.w * std::clamp(frac, 0.0f, 1.0f), bar.h, 4);
        bool boosted = hud_ == H_VOLUME && hudValue_ > 100;
        nvgFillColor(ui.vg, boosted ? ui.t.warning : white);
        nvgFill(ui.vg);
        if (hud_ == H_VOLUME && app.cfg.maxVolumeValue() > 100) {
            float x100 = bar.x + bar.w * 100.0f / app.cfg.maxVolumeValue();
            ui.fillRect({x100 - 1, bar.y - 4, 2, bar.h + 8}, hex(0xFFFFFF, 0.6f), 1);
        }
        ui.text(r.right() - 26, r.cy(), hudText_, 18, white, RIGHT, true);
    } else {
        float w = std::min(W - 80, ui.textWidth(hudText_, 20, true) + 80);
        float h = 56;
        Rect r{W / 2 - w / 2, 70, w, h};
        ui.fillRect(r, hex(0, 0.6f), h / 2);
        ui.icon(hudIcon_, r.x + 32, r.cy(), 22, white);
        ui.text(r.x + 56, r.cy(), ui.ellipsize(hudText_, w - 76, 20, true), 20, white, LEFT, true);
    }
    ui.popAlpha();
}

void PlayerScreen::drawDoubleTap() {
    UI& ui = app.ui;
    float a = ui.smooth(uid("dt.alpha"), dtTimer_ > 0 ? 1.0f : 0.0f, 12);
    if (a < 0.01f || dtSide_ == 0) return;
    NVGcontext* vg = ui.vg;
    float W = ui.W, H = ui.H;
    ui.pushAlpha(a);
    nvgSave(vg);
    float cx = dtSide_ < 0 ? -W * 0.12f : W * 1.12f;
    nvgScissor(vg, dtSide_ < 0 ? 0 : W * 0.6f, 0, W * 0.4f, H);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, H / 2, H * 0.78f);
    nvgFillColor(vg, hex(0xFFFFFF, 0.12f));
    nvgFill(vg);
    // Ripple from the tap point
    float rp = 1 - dtTimer_ / 0.75f;
    nvgBeginPath(vg);
    nvgCircle(vg, dtX_, dtY_, 40 + 160 * rp);
    nvgFillColor(vg, hex(0xFFFFFF, 0.18f * (1 - rp)));
    nvgFill(vg);
    nvgRestore(vg);

    float tx = dtSide_ < 0 ? W * 0.16f : W * 0.84f;
    for (int i = 0; i < 3; i++) {
        float ph = std::fmod(time_ * 3 + i * 0.33f, 1.0f);
        float ix = tx + (dtSide_ < 0 ? -1 : 1) * (i - 1) * 18;
        ui.icon(dtSide_ < 0 ? Icon::ChevronLeft : Icon::ChevronRight, ix, H / 2 - 18, 24,
                hex(0xFFFFFF, 0.35f + 0.65f * (1 - ph)));
    }
    ui.text(tx, H / 2 + 20, std::to_string(dtAmount_) + " seconds", 18, hex(0xFFFFFF), CENTER, true);
    ui.popAlpha();
}

void PlayerScreen::drawLocked() {
    UI& ui = app.ui;
    float a = ui.smooth(uid("lock.alpha"), locked_ && lockHint_ > 0 ? 1.0f : 0.0f, 12);
    if (a < 0.01f) return;
    ui.pushAlpha(a);
    bool prev = ui.blocked;
    if (!locked_ || a < 0.6f) ui.blocked = true;  // must be visible before it can be tapped
    // Touch only: as a focusable button it took the controller's focus, and A unlocked the controls.
    Rect lr{ui.W / 2 - 38, 90 - 38, 76, 76};
    float pv = ui.smooth(uid("lock.press"), ui.pressedVisual(lr) ? 1.0f : 0.0f, 25);
    nvgBeginPath(ui.vg);
    nvgCircle(ui.vg, lr.cx(), lr.cy(), 38);
    nvgFillColor(ui.vg, hex(0x000000, 0.45f + 0.2f * pv));
    nvgFill(ui.vg);
    ui.icon(Icon::Lock, lr.cx(), lr.cy(), 34, hex(0xFFFFFF));
    if (!ui.blocked && ui.tapped(lr)) doAction(Action::ToggleLock);
    ui.blocked = prev;
    ui.text(ui.W / 2, 150, "Tap to unlock", 16, hex(0xFFFFFF, 0.85f), CENTER, true);
    ui.popAlpha();
}

// ------------------------------------------------------------------ panels

bool PlayerScreen::panelRow(uint32_t id, Rect r, const std::string& label, const std::string& detail, bool checked,
                            Icon ic) {
    UI& ui = app.ui;
    ui.focusable(id, r);
    bool foc = ui.focused(id);
    if (panelFocusPending_ && checked) {
        ui.setFocus(id);
        panelScroll_.ensureVisible(r.y - panelScroll_.view.y + panelScroll_.offset, r.h);
    }
    if (foc && ui.focusMoved) panelScroll_.ensureVisible(r.y - panelScroll_.view.y + panelScroll_.offset, r.h);
    float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
    float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
    float cv = ui.smooth(id ^ 0xc4c, checked ? 1.0f : 0.0f, 16);
    ui.pushAlpha(panelScroll_.edgeFade(r.y, r.h));
    Rect card = r.inset(0, 3);
    if (cv > 0.01f) ui.fillRect(card, withAlpha(ui.t.accent, 0.18f * cv));
    if (fv > 0.01f || pv > 0.01f) ui.fillRect(card, hex(0xFFFFFF, 0.08f * std::max(fv, pv)));
    if (fv > 0.01f) ui.strokeRect(card.inset(-1), withAlpha(ui.t.accent, fv), 2.5f, ui.t.radius + 1);
    float x = card.x + 18;
    NVGcolor fg = ui.t.light ? ui.t.text : hex(0xFFFFFF);
    if (ic != Icon::None) {
        ui.icon(ic, x + 11, card.cy(), 22, checked ? ui.t.accent : withAlpha(fg, 0.75f));
        x += 36;
    }
    float right = card.right() - 16;
    if (checked) {
        ui.icon(Icon::Check, right - 10, card.cy(), 20, ui.t.accent);
        right -= 30;
    }
    if (!detail.empty()) {
        std::string d = ui.ellipsize(detail, 140, 15);
        ui.text(right, card.cy(), d, 15, withAlpha(fg, 0.55f), RIGHT);
        right -= ui.textWidth(d, 15) + 12;
    }
    // The highlighted row glides a long name (a subtitle file, a track) instead of cutting it.
    if (foc && ui.showFocus()) ui.marquee(id ^ 0x71c, x, card.cy(), right - x, label, 18, fg, LEFT, checked);
    else ui.text(x, card.cy(), ui.ellipsize(label, right - x, 18, checked), 18, fg, LEFT, checked);
    ui.popAlpha();
    return ui.tapped(card) || (foc && ui.confirm());
}

bool PlayerScreen::stepperRow(uint32_t id, Rect r, const std::string& label, const std::string& value, int& dir) {
    UI& ui = app.ui;
    Input& in = *ui.in;
    dir = 0;
    ui.focusable(id, r);
    bool foc = ui.focused(id);
    if (foc && ui.focusMoved) panelScroll_.ensureVisible(r.y - panelScroll_.view.y + panelScroll_.offset, r.h);
    float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
    ui.pushAlpha(panelScroll_.edgeFade(r.y, r.h));
    Rect card = r.inset(0, 3);
    if (fv > 0.01f) {
        ui.fillRect(card, hex(0xFFFFFF, 0.08f * fv));
        ui.strokeRect(card.inset(-1), withAlpha(ui.t.accent, fv), 2.5f, ui.t.radius + 1);
    }
    NVGcolor fg = ui.t.light ? ui.t.text : hex(0xFFFFFF);
    ui.text(card.x + 18, card.cy(), label, 18, fg);
    float bs = 38;
    Rect plus{card.right() - 12 - bs, card.cy() - bs / 2, bs, bs};
    Rect minus{plus.x - 110 - bs, plus.y, bs, bs};
    for (int i = 0; i < 2; i++) {
        Rect b = i ? plus : minus;
        bool pr = ui.pressedVisual(b.inset(-6));
        ui.fillRect(b, withAlpha(fg, pr ? 0.25f : 0.12f), bs / 2);  // fg: visible on light panels too
        ui.icon(i ? Icon::Plus : Icon::Minus, b.cx(), b.cy(), 18, fg);
    }
    ui.text((minus.right() + plus.x) / 2, card.cy(), value, 17, fg, CENTER, true);
    ui.popAlpha();
    if (ui.tapped(minus.inset(-6))) dir = -1;
    else if (ui.tapped(plus.inset(-6))) dir = 1;
    if (foc && !ui.blocked) {
        if (in.rep(BTN_LEFT)) dir = -1;
        if (in.rep(BTN_RIGHT)) dir = 1;
        if (ui.confirm()) dir = 1;
        ui.consumeNav(BTN_LEFT | BTN_RIGHT);
    }
    return dir != 0;
}

bool PlayerScreen::sliderRow(uint32_t id, Rect r, const std::string& label, float& v, float lo, float hi, float step,
                             const char* fmt) {
    UI& ui = app.ui;
    ui.focusable(id, r);
    bool foc = ui.focused(id);
    if (foc && ui.focusMoved) panelScroll_.ensureVisible(r.y - panelScroll_.view.y + panelScroll_.offset, r.h);
    float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
    ui.pushAlpha(panelScroll_.edgeFade(r.y, r.h));
    Rect card = r.inset(0, 3);
    if (fv > 0.01f) {
        ui.fillRect(card, hex(0xFFFFFF, 0.08f * fv));
        ui.strokeRect(card.inset(-1), withAlpha(ui.t.accent, fv), 2.5f, ui.t.radius + 1);
    }
    NVGcolor fg = ui.t.light ? ui.t.text : hex(0xFFFFFF);
    char buf[32];
    snprintf(buf, sizeof buf, fmt, v);
    ui.text(card.x + 18, card.y + 22, label, 17, fg);
    ui.text(card.right() - 18, card.y + 22, buf, 16, withAlpha(fg, 0.7f), RIGHT, true);
    bool changed = ui.slider(id, {card.x + 22, card.y + 40, card.w - 44, 24}, v, lo, hi, step, foc && !ui.blocked);
    ui.popAlpha();
    if (foc && ui.confirm()) {
        v = (lo < 0 && hi > 0) ? 0 : v;  // A resets centred sliders
        changed = true;
    }
    return changed;
}

// The side panel reads right to left in those languages, like the rest of the menus.
void PlayerScreen::drawPanel() {
    UI& ui = app.ui;
    bool mirrored = i18n::rtl();
    if (mirrored) {
        ui.in->mirrorX(ui.W);
        nvgSave(ui.vg);
        nvgTranslate(ui.vg, ui.W, 0);
        nvgScale(ui.vg, -1, 1);
        ui.mirror = true;
    }
    drawPanelBody();
    if (mirrored) {
        ui.mirror = false;
        nvgRestore(ui.vg);
        ui.in->mirrorX(ui.W);
    }
}

void PlayerScreen::drawPanelBody() {
    const Panel drawnPanel = panel_;  // a row may open another panel; its rows are drawn next frame
    UI& ui = app.ui;
    Player& p = app.player;
    Input& in = *ui.in;
    Panel pn = panel_ != P_NONE ? panel_ : lastPanel_;
    float a = panelAnim_;
    float w = std::min(440.0f, ui.W - 40);
    Rect r{ui.W - w - 16 + (w + 40) * (1 - ui.ease(a)), 16, w, ui.H - 32};

    bool prevBlocked = ui.blocked;
    if (panel_ == P_NONE) ui.blocked = true;

    // Tap outside closes
    if (!ui.blocked && in.tap && !r.contains(in.tapX, in.tapY)) {
        in.consumeTap();
        panel_ = P_NONE;
        ui.blocked = prevBlocked;
        return;
    }

    ui.pushAlpha(std::min(1.0f, a * 1.5f));
    // Nearly solid: a bright film behind would otherwise show through the text.
    ui.glass(r, ui.t.radius + 6, ui.t.light ? 0.97f : 0.96f);
    const char* titles[] = {"", "Subtitles", "Subtitle files", "Audio", "Playback speed", "Video",
                            "Queue", "Chapters", "More options", "Visualizer", "Choose a file", "Find online"};
    NVGcolor fg = ui.t.light ? ui.t.text : hex(0xFFFFFF);
    float hx = r.x + 24;
    if (subPanel(pn)) {
        if (ui.iconButton(uid("panel.back"), r.x + 36, r.y + 40, 44, Icon::Back, fg, 0, false)) openPanel(P_SUBS);
        hx += 34;
    }
    ui.text(hx, r.y + 40, titles[pn], 22, fg, LEFT, true);
    if (ui.iconButton(uid("panel.close"), r.right() - 36, r.y + 40, 44, Icon::Close, fg, 0, false)) panel_ = P_NONE;

    Rect view{r.x + 8, r.y + 76, r.w - 16, r.h - 84};
    panelScroll_.update(ui, view, panelContentH_);
    panelScroll_.begin(ui);
    float y = view.y - panelScroll_.offset;
    float x = view.x + 6, rw = view.w - 12;  // room for focus rings inside the clip area
    int idx = 0;
    const float RH = 56;
    auto next = [&](float h) {
        Rect rr{x, y, rw, h};
        y += h;
        return rr;
    };
    auto header = [&](const char* t) {
        y += 8;
        ui.text(x + 18, y + 16, t, 13, withAlpha(fg, 0.5f), LEFT, true);
        y += 30;
    };
    auto rowId = [&]() { return uid("prow", idx++); };

    switch (pn) {
        case P_SUBS: {
            bool any = false;
            for (auto& t : p.tracks)
                if (t.type == "sub" && t.selected) any = true;
            if (panelRow(rowId(), next(RH), "Off", "", !any, Icon::Close)) {
                p.setTrack("sub", 0);
                noteTrack("sub", nullptr);
            }
            for (auto& t : p.tracks) {
                if (t.type != "sub") continue;
                std::string d = t.external ? "External" : t.codec;
                if (t.forced) d = "Forced";
                if (panelRow(rowId(), next(RH), t.label(), d, t.selected, Icon::Captions)) {
                    p.setTrack("sub", t.id);
                    noteTrack("sub", &t);
                }
            }
            header("ADD A SUBTITLE");
            if (!currentPath_.empty() && !isUrl(currentPath_) &&
                panelRow(rowId(), next(RH), "Next to this video", "", false, Icon::Folder))
                openPanel(P_SUBFILES);
            if (panelRow(rowId(), next(RH), "Choose a file…", "SD card or USB", false, Icon::File)) {
                subBrowseOpen(!currentPath_.empty() && !isUrl(currentPath_) ? parentDir(currentPath_) : "");
                openPanel(P_SUBBROWSE);
            }
            if (panelRow(rowId(), next(RH), "Find online…", "OpenSubtitles", false, Icon::Globe)) {
                openPanel(P_SUBONLINE);
                if (!app.cfg.openSubtitlesKey.empty() && subSearchedFor_ != currentPath_) subSearchStart();
            }
            header("TIMING & STYLE");
            int dir;
            if (stepperRow(rowId(), next(RH), "Delay", signedSecs(p.subDelay, 1), dir))
                p.setSubDelay(std::round((p.subDelay + dir * 0.1) * 10) / 10);
            if (stepperRow(rowId(), next(RH), "Size", std::to_string(app.cfg.s.subScale) + "%", dir)) {
                app.cfg.s.subScale = std::clamp(app.cfg.s.subScale + dir * 10, 50, 300);
                app.settingsChanged();
            }
            {
                // Colour: a row of swatches, tapped or stepped through with left/right.
                Rect rr = next(RH);
                uint32_t id = rowId();
                ui.focusable(id, rr);
                bool foc = ui.focused(id);
                if (foc && ui.focusMoved)
                    panelScroll_.ensureVisible(rr.y - panelScroll_.view.y + panelScroll_.offset, rr.h);
                float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
                Rect card = rr.inset(0, 3);
                if (fv > 0.01f) {
                    ui.fillRect(card, hex(0xFFFFFF, 0.08f * fv));
                    ui.strokeRect(card.inset(-1), withAlpha(ui.t.accent, fv), 2.5f, ui.t.radius + 1);
                }
                ui.text(card.x + 18, card.cy(), "Colour", 18, fg);
                const int n = 10;
                float d = 22, step = std::min(31.0f, (card.w - 110) / n);
                float sx = card.right() - 14 - (n - 1) * step - d / 2;
                int pick = -1;
                for (int i = 0; i < n; i++) {
                    float cx = sx + i * step, cy = card.cy();
                    int saved = app.cfg.s.subColor;
                    app.cfg.s.subColor = i;
                    unsigned rgb = app.cfg.subColorRGB();
                    app.cfg.s.subColor = saved;
                    bool sel = i == saved;
                    if (sel) {
                        nvgBeginPath(ui.vg);
                        nvgCircle(ui.vg, cx, cy, d / 2 + 4);
                        nvgStrokeColor(ui.vg, fg);
                        nvgStrokeWidth(ui.vg, 2);
                        nvgStroke(ui.vg);
                    }
                    nvgBeginPath(ui.vg);
                    nvgCircle(ui.vg, cx, cy, d / 2);
                    nvgFillColor(ui.vg, hex(rgb));
                    nvgFill(ui.vg);
                    nvgStrokeColor(ui.vg, hex(0x000000, 0.35f));
                    nvgStrokeWidth(ui.vg, 1);
                    nvgStroke(ui.vg);
                    if (ui.tapped({cx - step / 2, cy - RH / 2, step, RH})) pick = i;
                }
                if (foc && !ui.blocked) {
                    if (ui.in->rep(BTN_LEFT)) pick = (app.cfg.s.subColor + n - 1) % n;
                    if (ui.in->rep(BTN_RIGHT)) pick = (app.cfg.s.subColor + 1) % n;
                }
                if (pick >= 0 && pick != app.cfg.s.subColor) {
                    app.cfg.s.subColor = pick;
                    app.settingsChanged();
                }
            }
            if (stepperRow(rowId(), next(RH), "Position", std::to_string(app.cfg.s.subPos) + "%", dir)) {
                app.cfg.s.subPos = std::clamp(app.cfg.s.subPos + dir, 50, 100);
                app.settingsChanged();
            }
            static const char* bgNames[] = {"None", "Shadow", "Box", "Solid"};
            if (stepperRow(rowId(), next(RH), "Background", bgNames[app.cfg.s.subBackground], dir)) {
                app.cfg.s.subBackground = (app.cfg.s.subBackground + dir + 4) % 4;
                app.settingsChanged();
            }
            break;
        }
        case P_SUBFILES: {
            if (subFiles_.empty()) {
                ui.text(x + 18, y + 30, "No subtitle files found next to this video.", 16, withAlpha(fg, 0.6f));
                y += 60;
            }
            for (auto& f : subFiles_) {
                if (panelRow(rowId(), next(RH), displayName(f), fileExtension(f), false, Icon::Captions)) {
                    p.addSubtitle(prepareSub(f), true, stripExtension(displayName(f)), subtitleLang(f));
                    app.ui.toast("Subtitle loaded", Icon::Captions);
                    openPanel(P_SUBS);
                    break;
                }
            }
            break;
        }
        case P_SUBBROWSE: {
            ui.text(x + 18, y + 14, ui.ellipsize(subBrowseDir_.empty() ? "Drives" : platform::displayPath(subBrowseDir_),
                                                 rw - 36, 14), 14, withAlpha(fg, 0.55f));
            y += 34;
            std::string go;
            bool any = false;
            if (!subBrowseDir_.empty() && panelRow(rowId(), next(RH), "Up one folder", "", false, Icon::Back)) {
                std::string up = parentDir(subBrowseDir_.substr(0, subBrowseDir_.size() - 1));
                bool isRoot = false;
                for (auto& rt : platform::storageRoots(false)) isRoot |= rt.path == subBrowseDir_;
                go = isRoot ? "\x01" : up;
            }
            for (auto& e : subBrowse_) {
                if (!go.empty()) break;
                bool folder = e.kind == MediaKind::Folder;
                if (!folder && e.kind != MediaKind::Subtitle) continue;
                any |= !folder;
                std::string name = e.name;
                while (!name.empty() && name.back() == '/') name.pop_back();
                if (panelRow(rowId(), next(RH), name, folder ? (e.isDrive ? e.detail : "") : fileExtension(e.name), false,
                             folder ? (e.isDrive ? Icon::Drive : Icon::Folder) : Icon::Captions)) {
                    if (folder) go = e.path;
                    else {
                        p.addSubtitle(prepareSub(e.path), true, stripExtension(displayName(e.path)), subtitleLang(e.path));
                        app.ui.toast("Subtitle loaded", Icon::Captions);
                        openPanel(P_SUBS);
                        break;
                    }
                }
            }
            if (!any && !subBrowseDir_.empty() && go.empty()) {
                ui.text(x + 18, y + 24, "No subtitle files here. Open a folder.", 15, withAlpha(fg, 0.55f));
                y += 50;
            }
            if (!go.empty()) {
                subBrowseOpen(go == "\x01" ? "" : go);
                panelScroll_.reset();
                app.ui.setFocus(uid("prow", 0));
            }
            break;
        }
        case P_SUBONLINE: {
            auto note = [&](const std::string& t, float size = 15) {
                // Wrapped by hand: at most two lines of guidance.
                ui.text(x + 18, y + 18, ui.ellipsize(t, rw - 36, size), size, withAlpha(fg, 0.6f));
                y += 32;
            };
            if (app.cfg.openSubtitlesKey.empty()) {
                note("Searching needs your own free OpenSubtitles API key:");
                note("opensubtitles.com → sign in → Profile → API consumers");
                note("→ New consumer, then copy its API key.");
                y += 8;
                if (panelRow(rowId(), next(RH), "Enter API key…", "", false, Icon::Rename)) {
                    std::string k;
                    if (app.askText("OpenSubtitles API key", "", k) && !k.empty()) {
                        while (!k.empty() && k.back() == ' ') k.pop_back();
                        app.cfg.openSubtitlesKey = k;
                        app.cfg.save();
                        subSearchStart();
                    }
                }
                break;
            }
            if (panelRow(rowId(), next(RH), "Search for", subQuery_.empty() ? "…" : subQuery_, false, Icon::Search)) {
                std::string q;
                if (app.askText("Search subtitles for", subQuery_, q) && !q.empty()) {
                    subQuery_ = q;
                    subSearchStart();
                }
            }
            SubSearch::State st = subSearch_.state();
            if (st == SubSearch::Busy && !subSearch_.downloading()) {
                ui.spinner(x + 34, y + 28, 10, ui.t.accent, 3);
                ui.text(x + 56, y + 28, "Searching…", 16, withAlpha(fg, 0.75f));
                y += 56;
            } else if (st == SubSearch::Failed && !subSearch_.downloading()) {
                note(subSearch_.message());
            } else {
                if (subSearch_.downloading() && st == SubSearch::Done) {
                    std::string err = subSearch_.takeDownloadError();
                    if (!err.empty()) {
                        app.ui.toast(err, Icon::Info);
                        subDownloading_ = 0;
                    } else if (!subSearch_.savedPath().empty()) {
                        subLoadDownloaded();
                        break;
                    }
                }
                auto results = subSearch_.results();
                if (results.empty() && st == SubSearch::Done)
                    note(subSearch_.message().empty() ? trf("Nothing found for \"{0}\"", {subQuery_}) : subSearch_.message());
                for (auto& res : results) {
                    std::string detail = res.language;
                    for (auto& c : detail) c = (char)toupper((unsigned char)c);
                    if (res.downloads >= 1000) detail += " · " + std::to_string(res.downloads / 1000) + "k";
                    else detail += " · " + std::to_string(res.downloads);
                    if (res.hashMatch) detail = tr("Exact match") + " · " + detail;
                    Rect rr = next(RH);
                    bool busy = subSearch_.downloading() && subDownloading_ == res.fileId;
                    if (panelRow(rowId(), rr, res.release, busy ? "Downloading…" : detail, res.hashMatch,
                                 res.hashMatch ? Icon::Check : Icon::Captions) &&
                        !subSearch_.downloading()) {
                        std::string dir = isUrl(currentPath_) ? app.dataDir + "subcache/" : parentDir(currentPath_);
                        std::string out = dir + stripExtension(baseName(currentPath_.empty() ? res.release : currentPath_)) +
                                          "." + (res.language.empty() ? "sub" : res.language) + ".srt";
                        // Saved next to the video when possible, so it loads by itself next time.
                        struct stat st;
                        bool existed = stat(out.c_str(), &st) == 0;
                        if (FILE* t = fopen(out.c_str(), "ab")) {
                            fclose(t);
                            if (!existed) unlink(out.c_str());  // only checking the folder can be written to
                        } else {
                            out = app.dataDir + "subcache/" + std::to_string(res.fileId) + ".srt";
                        }
                        subDownloading_ = res.fileId;
                        subSearch_.download(app.cfg.openSubtitlesKey, res.fileId, out);
                    }
                }
            }
            header("ACCOUNT");
            if (panelRow(rowId(), next(RH), "Change API key…", "", false, Icon::Rename)) {
                std::string k;
                if (app.askText("OpenSubtitles API key", app.cfg.openSubtitlesKey, k)) {
                    app.cfg.openSubtitlesKey = k;
                    app.cfg.save();
                    if (!k.empty()) subSearchStart();
                }
            }
            break;
        }
        case P_AUDIO: {
            for (auto& t : p.tracks) {
                if (t.type != "audio") continue;
                std::string d = t.codec;
                std::string ch = channelsLabel(t.channels);
                if (!ch.empty()) d += " " + tr(ch);
                if (panelRow(rowId(), next(RH), t.label(), d, t.selected, Icon::Music)) {
                    p.setTrack("audio", t.id);
                    noteTrack("audio", &t);
                }
            }
            header("SOUND");
            float vol = (float)p.volume;
            if (sliderRow(rowId(), next(76), "Volume", vol, 0, (float)app.cfg.maxVolumeValue(), 5, "%.0f%%"))
                p.setVolume(vol);
            int dir;
            if (stepperRow(rowId(), next(RH), "Audio delay", signedSecs(p.audioDelay, 2), dir))
                p.setAudioDelay(std::round((p.audioDelay + dir * 0.05) * 100) / 100);
            if (panelRow(rowId(), next(RH), "Night mode", "Compress loud scenes", app.cfg.s.nightMode != 0,
                         Icon::Moon)) {
                app.cfg.s.nightMode = !app.cfg.s.nightMode;
                app.settingsChanged();
            }
            {
                static const char* eqNames[] = {"Off", "Bass boost", "Treble boost", "Vocal", "Loudness",
                                                "Small speakers", "Headphones"};
                const int n = 7;
                int& eq = app.cfg.s.soundEq;
                if (stepperRow(rowId(), next(RH), "Equalizer", eqNames[std::clamp(eq, 0, n - 1)], dir)) {
                    eq = (eq + dir + n) % n;
                    app.settingsChanged();
                }
            }
            if (panelRow(rowId(), next(RH), p.mute ? "Unmute" : "Mute", "", p.mute, Icon::VolumeMute))
                p.setMute(!p.mute);
            break;
        }
        case P_SPEED: {
            for (int i = 0; i < kSpeedCount; i++) {
                bool sel = std::fabs(p.speed - kSpeedValues[i]) < 0.01;
                if (panelRow(rowId(), next(RH), kSpeedLabels[i], i == 3 ? "Normal" : "", sel, Icon::Speed))
                    p.setSpeed(kSpeedValues[i]);
            }
            break;
        }
        case P_VIDEO: {
            int dir;
            if (stepperRow(rowId(), next(RH), "Aspect ratio", kAspectLabels[aspectIdx_], dir)) {
                aspectIdx_ = (aspectIdx_ + dir + kAspectCount) % kAspectCount;
                p.setAspect(kAspectValues[aspectIdx_]);
            }
            static const char* fitNames[] = {"Fit", "Fill", "Stretch"};
            if (stepperRow(rowId(), next(RH), "Fit", fitNames[app.cfg.s.videoFit], dir)) {
                app.cfg.s.videoFit = (app.cfg.s.videoFit + dir + 3) % 3;
                app.settingsChanged();
            }
            if (stepperRow(rowId(), next(RH), "Zoom",
                           std::to_string((int)std::lround(std::pow(2.0, p.zoom) * 100)) + "%", dir))
                p.setZoom(std::clamp(p.zoom + dir * 0.1, -2.0, 3.0));
            if (stepperRow(rowId(), next(RH), "Rotate", std::to_string(p.rotation) + "°", dir))
                p.setRotation(p.rotation + dir * 90);
            if (panelRow(rowId(), next(RH), "Reset zoom & position", "", false, Icon::Refresh)) {
                p.setZoom(0);
                p.setPan(0, 0);
            }
            header("PICTURE");
            for (int i = 0; i < 5; i++) {
                float v = (float)eq_[i];
                if (sliderRow(rowId(), next(76), kEqLabels[i], v, -100, 100, 5, "%+.0f")) {
                    eq_[i] = (int)v;
                    p.setEq(kEqProps[i], eq_[i]);
                }
            }
            if (panelRow(rowId(), next(RH), "Deband", "Removes colour banding", app.cfg.s.deband != 0,
                         Icon::Layers)) {
                app.cfg.s.deband = !app.cfg.s.deband;
                app.settingsChanged();
            }
            // For TV recordings (.ts) that show combing on movement; this file only.
            if (panelRow(rowId(), next(RH), "Deinterlace", "For TV recordings with lines on movement", deinterlace_,
                         Icon::Layers)) {
                deinterlace_ = !deinterlace_;
                p.setProperty("deinterlace", deinterlace_ ? "yes" : "no");
            }
            header("DECODER");
            // Hardware unless turned off here (for a file that shows wrong colours or blocks with it).
            {
                std::string hw = p.hwdecCurrent.empty() || p.hwdecCurrent == "no" ? tr("Software") : tr("Hardware");
                int d;
                if (stepperRow(rowId(), next(RH), "Decoding", hw, d)) {
                    swDecode_ = !swDecode_;
                    p.setProperty("hwdec", swDecode_ ? "no" : app.cfg.hwdecValue());
                    hud(H_TEXT, 0, tr(swDecode_ ? "Software decoding" : "Hardware decoding"), Icon::Chip);
                }
            }
            break;
        }
        case P_PLAYLIST: {
            for (size_t i = 0; i < p.playlist.size(); i++) {
                bool cur = (int64_t)i == p.playlistPos;
                uint32_t rowid = rowId();
                Rect row = next(RH);
                // A folder can queue thousands of files; rows out of view only need to be reachable.
                if (!cur && !ui.focused(rowid) && (row.bottom() < view.y || row.y > view.bottom())) {
                    ui.focusable(rowid, row);
                    continue;
                }
                std::string name = entryName(p, i, app.cfg.s.tidyNames);
                float prog = app.lib.progress(p.playlist[i]);
                std::string d = prog >= 1 ? "Watched" : (prog > 0 ? std::to_string((int)(prog * 100)) + "%" : "");
                bool song = kindForExtension(fileExtension(p.playlist[i])) == MediaKind::Audio;
                if (panelRow(rowid, row, name, d, cur, cur ? Icon::Play : song ? Icon::Music : Icon::Film) && !cur) {
                    saveProgress();
                    p.playIndex((int)i);
                }
            }
            break;
        }
        case P_CHAPTERS: {
            for (size_t i = 0; i < p.chapters.size(); i++) {
                bool cur = (int64_t)i == p.chapter;
                if (panelRow(rowId(), next(RH), chapterName(p, i), formatTime(p.chapters[i].time), cur,
                             Icon::Bookmark))
                    p.setChapter((int)i);
            }
            break;
        }
        case P_VISUAL: {
            header("Style");
            for (int i = 0; i < vis::styleCount(); i++) {
                if (panelRow(rowId(), next(RH), vis::styleName(i), "", app.cfg.s.musicVisual == i, Icon::Waveform)) {
                    app.cfg.s.musicVisual = i;
                    app.saveSoon();
                    styleFlash_ = 1.2f;
                }
            }
            header("Colours");
            for (int i = 0; i < vis::paletteCount(); i++)
                if (panelRow(rowId(), next(RH), vis::paletteName(i), "", app.cfg.s.musicPalette == i, Icon::Palette)) {
                    app.cfg.s.musicPalette = i;
                    app.saveSoon();
                }
            break;
        }
        case P_MORE: {
            int dir;
            // Counting down: the time left; at the end of the file: which kind of file.
            std::string sleepValue = sleepRemaining_ > 0 ? formatTime(sleepRemaining_)
                                     : sleepEndOfFile_  ? tr(musicKind_ ? "End of this song" : "End of this video")
                                                        : tr(kSleepLabels[sleepIdx_]);
            if (stepperRow(rowId(), next(RH), "Sleep timer", sleepValue, dir)) {
                sleepIdx_ = (sleepIdx_ + dir + kSleepCount) % kSleepCount;
                int m = kSleepMinutes[sleepIdx_];
                if (m == -2) {  // any number of minutes
                    std::string s;
                    int mins = 0;
                    if (app.askText("Sleep after how many minutes?", "25", s)) {
                        for (size_t i = 0; i < s.size(); i++) {
                            unsigned char c = (unsigned char)s[i];
                            if (isdigit(c)) mins = mins * 10 + (c - '0');
                            else if ((c == 0xDB || c == 0xD9) && i + 1 < s.size()) {  // Persian / Arabic digits
                                unsigned char d = (unsigned char)s[++i];
                                if (c == 0xDB && d >= 0xB0 && d <= 0xB9) mins = mins * 10 + (d - 0xB0);
                                if (c == 0xD9 && d >= 0xA0 && d <= 0xA9) mins = mins * 10 + (d - 0xA0);
                            }
                            if (mins > 24 * 60) break;
                        }
                    }
                    mins = std::min(mins, 24 * 60);
                    if (mins <= 0) sleepIdx_ = 0;
                    m = mins;
                }
                endSleepFade();
                sleepEndOfFile_ = m == -1;
                sleepRemaining_ = m > 0 ? m * 60.0f : -1;
                if (m > 0) app.ui.toast(trf("Sleep in {0} min", {std::to_string(m)}), Icon::Moon);
            }
            if (musicMode()) {
                // Audiobooks and podcasts: a little faster or slower, the voice keeps its pitch.
                char sp[16];
                snprintf(sp, sizeof sp, "%.2gx", p.speed);
                int sd;
                if (stepperRow(rowId(), next(RH), "Playback speed", sp, sd)) {
                    double v = std::clamp(std::round((p.speed + sd * 0.25) * 4) / 4, 0.5, 2.0);
                    p.setSpeed(v);
                }
            }
            if (musicMode() && panelRow(rowId(), next(RH), "Sound", "Volume, equalizer, night mode", false, Icon::Volume))
                openPanel(P_AUDIO);
            if (p.duration > 0 && panelRow(rowId(), next(RH), "Go to time…", formatTime(p.position), false, Icon::Clock)) {
                panel_ = P_NONE;
                askGoToTime();
                return;
            }
            std::string ab = p.abA < 0 ? "Set A" : (p.abB < 0 ? "Set B" : "Clear");
            std::string abd = p.abA < 0 ? "" : formatTime(p.abA) + (p.abB < 0 ? "" : " – " + formatTime(p.abB));
            if (panelRow(rowId(), next(RH), tr("A-B loop") + ": " + tr(ab), abd, p.abA >= 0 && p.abB >= 0, Icon::Repeat))
                p.abLoop();
            bool loopFile = app.cfg.s.loopMode == 1;
            if (panelRow(rowId(), next(RH), "Repeat this file", "", loopFile, Icon::Refresh)) {
                app.cfg.s.loopMode = loopFile ? 0 : 1;
                app.settingsChanged();
            }
            if (!musicMode() && panelRow(rowId(), next(RH), "Take screenshot", "", false, Icon::Camera)) {
                panel_ = P_NONE;
                doAction(Action::Screenshot);
            }
            if (panelRow(rowId(), next(RH), "Debug overlay", "FPS & stats", app.cfg.s.showStats != 0, Icon::Waveform))
                app.cfg.s.showStats = !app.cfg.s.showStats;
            if (panelRow(rowId(), next(RH), "Lock controls", "", false, Icon::Lock)) {
                panel_ = P_NONE;
                doAction(Action::ToggleLock);
            }
            if (!p.chapters.empty() && panelRow(rowId(), next(RH), "Chapters", std::to_string(p.chapters.size()),
                                                false, Icon::Bookmark))
                openPanel(P_CHAPTERS);
            if (p.playlistCount > 1 && panelRow(rowId(), next(RH), "Queue", std::to_string(p.playlistCount), false,
                                                Icon::Playlist))
                openPanel(P_PLAYLIST);
            if (app.canCollect(currentPath_) &&
                panelRow(rowId(), next(RH), "Add to playlist", "", false, Icon::Playlist)) {
                panel_ = P_NONE;
                ui.setFocus(uid("osd.play"));
                doAction(Action::AddToPlaylist);
            }
            if (panelRow(rowId(), next(RH), "Close player", "", false, Icon::Power)) {
                panelScroll_.end(ui);
                ui.popAlpha();
                ui.blocked = prevBlocked;
                exit();
                return;
            }
            break;
        }
        default: break;
    }
    panelContentH_ = y + panelScroll_.offset - view.y + 8;
    if (panel_ == drawnPanel) panelFocusPending_ = false;
    panelScroll_.end(ui);
    panelScroll_.drawScrollbar(ui);
    ui.popAlpha();
    ui.blocked = prevBlocked;
}
