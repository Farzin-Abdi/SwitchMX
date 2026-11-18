// The music player: its own screen for audio-only files, with cover art, tags and visualizers.
#include <algorithm>
#include <cstring>
#include <cmath>
#include <ctime>

#include "app.hpp"
#include "lyrics.hpp"
#include "platform.hpp"

namespace {
std::string clockText(bool h24) {
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char buf[32];
    if (!tm) return "";
    strftime(buf, sizeof buf, h24 ? "%H:%M" : "%I:%M %p", tm);
    std::string s = buf;
    if (!h24 && s[0] == '0') s.erase(0, 1);
    return s;
}

std::string codecLabel(const std::string& c) {
    if (c.rfind("pcm_", 0) == 0) return "PCM";
    static const char* names[][2] = {{"flac", "FLAC"},     {"mp3", "MP3"},   {"mp3float", "MP3"}, {"aac", "AAC"},
                                     {"opus", "Opus"},     {"libopus", "Opus"}, {"vorbis", "Vorbis"},
                                     {"alac", "ALAC"},     {"ape", "APE"},   {"wavpack", "WavPack"}, {"ac3", "AC-3"},
                                     {"eac3", "E-AC-3"},   {"dts", "DTS"},   {"truehd", "TrueHD"}, {"wmav2", "WMA"},
                                     {"dsd_lsbf", "DSD"},  {"dsd_msbf", "DSD"}};
    for (auto& n : names)
        if (c == n[0]) return n[1];
    std::string up = c;
    for (auto& ch : up) ch = (char)toupper((unsigned char)ch);
    return up;
}

bool lossless(const std::string& c) {
    return c == "flac" || c == "alac" || c == "ape" || c == "wavpack" || c == "truehd" || c.rfind("pcm_", 0) == 0;
}

// Draws the image so it fills the rect (cropping), at the given alpha.
void fillWithImage(NVGcontext* vg, int image, int iw, int ih, Rect r, float radius, float alpha) {
    float scale = std::max(r.w / iw, r.h / ih);
    float w = iw * scale, h = ih * scale;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgFillPaint(vg, nvgImagePattern(vg, r.cx() - w / 2, r.cy() - h / 2, w, h, 0, image, alpha));
    nvgFill(vg);
}

Rect lerpRect(Rect a, Rect b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}
}  // namespace

bool PlayerScreen::musicMode() const {
    if (imageMode_) return false;
    const Player& p = app.player;
    // Internet radio has no telling extension; it counts once it has loaded without a picture.
    return musicKind_ || (isUrl(currentPath_) && p.loaded && !p.tracks.empty() && !p.hasVideo());
}

bool PlayerScreen::videoMode() const { return !imageMode_ && !musicMode(); }

void PlayerScreen::drawMusic(bool parentBlocked) {
    UI& ui = app.ui;
    Player& p = app.player;
    NVGcontext* vg = ui.vg;
    float W = ui.W, H = ui.H, dt = ui.dt;
    Config& cfg = app.cfg;

    // mpv reports the position in steps; run a clock in between so the visuals stay in time.
    if (p.position != lastPos_) {
        lastPos_ = p.position;
        musicClock_ = p.position;
    } else if (p.loaded && !p.paused) {
        musicClock_ = std::min(musicClock_ + dt * p.speed, lastPos_ + 0.6);
    }
    bool local = !currentPath_.empty() && !isUrl(currentPath_);
    // Lyrics: looked up again when the song changes or its tags arrive (they may hold the words).
    {
        std::string key = currentPath_ + "|" + std::to_string(p.metadata.size());
        if (key != lyricsKey_) {
            lyricsKey_ = key;
            lyrics_ = currentPath_.empty() ? Lyrics() : findLyrics(currentPath_, p.metadata, cfg.s.slang);
            lyricsScroll_ = 0;
        }
    }
    bool showLyrics = cfg.s.lyrics && !lyrics_.empty();
    vis::Frame frame;
    bool have = local && analyzer_.path() == currentPath_ && analyzer_.sample(musicClock_, frame);
    bool playing = p.loaded && !p.paused;
    vis::update(vis_, have ? &frame : nullptr, dt, playing && (have || !local));
    // A visualizer that never moves means the second decoder can't read the file (worth knowing
    // per drive), so it's noted once per song.
    if (have || !local || !playing) visStarved_ = have ? 0 : visStarved_;
    else visStarved_ += dt;
    if (visStarved_ > 4 && visReported_ != currentPath_) {
        visReported_ = currentPath_;
        app.logEvent("visualizer: no sound data after 4 s for " + currentPath_);
    }
    // A new record spins up briefly as it slides in.
    if (playing) vinylAngle_ += dt * (0.35f + (songT_ < 1 ? 2.2f * (1 - songT_) : 0));
    // The glow follows how loud the song is over a couple of seconds; reacting to every beat flickers.
    glow_ += ((playing ? vis_.energy : 0.0f) - glow_) * std::min(1.0f, dt * 0.8f);
    styleFlash_ = std::max(0.0f, styleFlash_ - dt);

    // Title, artist and album; generated art and colours come from the album, so its songs match.
    // Downloaded music often carries the site's address in every tag ("www.example.com"), which
    // would show instead of the song. Such a tag is ignored and the file name used instead.
    auto adTag = [](const std::string& t) {
        std::string l;
        for (char c : t) l += (char)tolower((unsigned char)c);
        if (l.find("www.") != std::string::npos || l.find("http://") != std::string::npos ||
            l.find("https://") != std::string::npos || l.find("@") != std::string::npos)
            return true;
        // A bare domain ("musickordi.com", "music.ir") with no spaces around it.
        if (l.find(' ') != std::string::npos) return false;
        for (const char* d : {".com", ".net", ".org", ".ir", ".info", ".biz", ".co", ".me", ".tv"}) {
            size_t at = l.rfind(d);
            if (at != std::string::npos && at + strlen(d) == l.size() && at > 0) return true;
        }
        return false;
    };
    std::string title = p.tag("title");
    if (adTag(title)) title.clear();
    if (title.empty())
        title = currentPath_.empty() ? stripExtension(p.title)
                                     : (app.cfg.s.tidyNames ? tidySong(displayName(currentPath_))
                                                            : stripExtension(displayName(currentPath_)));
    std::string artist = p.tag("artist");
    if (adTag(artist)) artist.clear();
    if (artist.empty()) artist = p.tag("album_artist");
    if (adTag(artist)) artist.clear();
    std::string album = p.tag("album"), year = p.tag("date");
    if (adTag(album)) album.clear();
    if (year.size() > 4) year = year.substr(0, 4);
    if (album.empty() && local) album = baseName(parentDir(currentPath_));
    std::string seed = !album.empty() ? album + "|" + artist : (!artist.empty() ? artist : title);
    const CoverArt::Art* art = currentPath_.empty() ? nullptr : app.covers.get(currentPath_, MediaKind::Audio);
    NVGcolor target = art ? vis::vivid(art->r, art->g, art->b) : ui.seedColor(seed);
    float k = std::min(1.0f, dt * 2.5f);
    coverR_ += (target.r - coverR_) * k;
    coverG_ += (target.g - coverG_) * k;
    coverB_ += (target.b - coverB_) * k;
    NVGcolor tint = nvgRGBf(coverR_, coverG_, coverB_);
    vis::Look look{cfg.s.musicPalette, tint, ui.t.accent, ui.t.accent2};
    int style = cfg.s.musicVisual;
    vis::Place place = vis::placement(style);
    float m = ui.smooth(uid("music.immersive"), immersive_ ? 1.0f : 0.0f, 7);
    float artA = art ? std::min(1.0f, art->age / 0.5f) : 0;
    // A new song: the one before it slides out the way the queue moved, the new one slides in.
    if (currentPath_ != songKey_) {
        if (!songKey_.empty() && ui.animMul > 0) {
            prevSong_ = lastSong_;
            songT_ = 0;
            songDir_ = (songPos_ >= 0 && p.playlistPos < songPos_) ? -1 : 1;
        }
        songKey_ = currentPath_;
    }
    songPos_ = p.playlistPos;
    songT_ = std::min(1.0f, songT_ + dt / 0.55f);
    float se = ui.ease(songT_);           // new song: 0 -> 1
    bool changing = songT_ < 1;
    if (art) {
        lastSong_.art = *art;
        lastSong_.hasArt = true;
    } else {
        lastSong_.hasArt = false;
    }
    lastSong_.title = title;
    lastSong_.artist = artist;
    lastSong_.album = album;
    lastSong_.seed = seed;
    float newA = changing ? se : 1;

    // ---- Background: the cover, stretched until it blurs, under a dark veil and a colour glow
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, H);
    nvgFillColor(vg, nvgRGB(6, 7, 11));
    nvgFill(vg);
    float drift = std::sin(vis_.time * 0.05f) * 40;
    if (changing && prevSong_.hasArt && prevSong_.art.blurred)
        fillWithImage(vg, prevSong_.art.blurred, 16, 16, {-W * 0.2f + drift, -H * 0.3f, W * 1.4f, H * 1.6f}, 0,
                      0.8f * (1 - se) * (1 - 0.45f * m));
    if (art && art->blurred) {
        fillWithImage(vg, art->blurred, 16, 16, {-W * 0.2f + drift, -H * 0.3f, W * 1.4f, H * 1.6f}, 0,
                      0.8f * artA * newA * (1 - 0.45f * m));
    }
    if (!art) {
        // No cover to blur: two broad lights in the album's colours instead
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, W, H);
        nvgFillPaint(vg, nvgRadialGradient(vg, W * 0.2f, H * 0.25f, 0, W * 0.7f, withAlpha(tint, 0.55f), withAlpha(tint, 0)));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, W, H);
        NVGcolor second = nvgLerpRGBA(tint, ui.t.accent2, 0.5f);
        nvgFillPaint(vg, nvgRadialGradient(vg, W * 0.85f, H * 0.9f, 0, W * 0.6f, withAlpha(second, 0.35f), withAlpha(second, 0)));
        nvgFill(vg);
    }
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, H);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, H, nvgRGBAf(0, 0, 0, 0.45f + 0.3f * m),
                                       nvgRGBAf(0, 0, 0, 0.85f + 0.1f * m)));
    nvgFill(vg);

    // ---- Layout: a grid, blended into the full-screen visualizer. The cover is on the left; on the
    // right the tags line up with its top edge and the transport with its bottom edge. Volume and
    // the other actions sit in a dock along the bottom of the screen.
    const float topBar = 92, dock = 88;
    float avail = H - topBar - dock;
    float side = std::min(avail - 16, W * 0.36f);
    Rect coverN{W * 0.065f, topBar + (avail - side) / 2, side, side};
    float x0 = coverN.right() + W * 0.055f, x1 = W - W * 0.06f;
    bool around = place == vis::Place::Around;
    float sideI = around ? H * 0.42f : side * 0.2f;
    Rect coverI{W / 2 - sideI / 2, H / 2 - sideI / 2 - 10, sideI, sideI};
    // A record sits inside the same box, leaving room for the visualizer drawn around it.
    Rect coverBox = around ? coverN.inset(side * 0.13f) : coverN;
    Rect cover = lerpRect(coverBox, coverI, m);
    float coverAlpha = around ? 1 : 1 - m;
    float metaTop = coverN.y;
    // The play button is the tallest control; this leaves clear space between it and the times
    // under the progress bar.
    float controlsY = coverN.bottom() - 30;
    float barY = controlsY - 90;
    float stageTop = metaTop + 166;
    Rect stageN{x0 - 6, stageTop, x1 - x0 + 12, std::max(60.0f, barY - 26 - stageTop)};
    Rect stageI{W * 0.04f, H * 0.12f, W * 0.92f, H * 0.74f};
    Rect stage = lerpRect(stageN, stageI, m);

    {
        nvgBeginPath(vg);
        float gx = cover.cx(), gy = cover.cy(), gr = cover.w * 1.4f;
        nvgCircle(vg, gx, gy, gr);
        nvgFillPaint(vg, nvgRadialGradient(vg, gx, gy, cover.w * 0.3f, gr,
                                           withAlpha(tint, (0.22f + 0.12f * glow_) * coverAlpha), withAlpha(tint, 0)));
        nvgFill(vg);
    }
    if (place == vis::Place::Backdrop) {
        // Softer while the text is on top of it
        ui.pushAlpha(0.5f + 0.5f * m);
        vis::draw(vg, style, look, vis_, {0, 0, W, H}, W / 2, H / 2, side / 2);
        ui.popAlpha();
    }
    if (place == vis::Place::Stage && (!showLyrics || m > 0.01f)) {
        ui.pushAlpha(0.95f * (showLyrics ? m : 1.0f));
        vis::draw(vg, style, look, vis_, stage, 0, 0, 0);
        ui.popAlpha();
    }

    // ---- Cover: a record that turns for the circular visualizers, a framed square otherwise
    float R = cover.w / 2;
    // During a change, the old cover first (sliding out, shrinking, fading), then the new one in.
    auto slideFor = [&](bool old) {
        float k = old ? se : se - 1;  // old: 0 -> 1 away; new: -1 -> 0 into place
        nvgTranslate(vg, -songDir_ * k * cover.w * 0.38f, 0);
        float sc = old ? 1 - 0.12f * se : 0.88f + 0.12f * se;
        nvgTranslate(vg, cover.cx(), cover.cy());
        nvgScale(vg, sc, sc);
        nvgTranslate(vg, -cover.cx(), -cover.cy());
    };
    if (changing && coverAlpha > 0.01f) {
        nvgSave(vg);
        slideFor(true);
        ui.pushAlpha((1 - se) * coverAlpha);
        if (around) {
            nvgBeginPath(vg);
            nvgCircle(vg, cover.cx(), cover.cy(), R);
            nvgFillColor(vg, nvgRGB(14, 14, 18));
            nvgFill(vg);
            Rect in{cover.cx() - R * 0.98f, cover.cy() - R * 0.98f, R * 1.96f, R * 1.96f};
            if (prevSong_.hasArt) fillWithImage(vg, prevSong_.art.image, prevSong_.art.w, prevSong_.art.h, in, R, 1);
            else ui.generatedArt(in, prevSong_.seed, prevSong_.title, R, true);
        } else {
            float rad = std::min(18.0f, cover.w * 0.06f);
            if (prevSong_.hasArt) fillWithImage(vg, prevSong_.art.image, prevSong_.art.w, prevSong_.art.h, cover, rad, 1);
            else ui.generatedArt(cover, prevSong_.seed, prevSong_.title, rad);
        }
        ui.popAlpha();
        nvgRestore(vg);
    }
    nvgSave(vg);
    if (changing) slideFor(false);
    ui.pushAlpha(newA);
    if (around) {
        vis::draw(vg, style, look, vis_, stage, cover.cx(), cover.cy(), R);
        nvgSave(vg);
        nvgTranslate(vg, cover.cx(), cover.cy());
        nvgRotate(vg, vinylAngle_);
        ui.shadow({-R, -R, R * 2, R * 2}, R, 30, 0.6f, 10);
        nvgBeginPath(vg);
        nvgCircle(vg, 0, 0, R);
        nvgFillColor(vg, nvgRGB(14, 14, 18));
        nvgFill(vg);
        if (art) fillWithImage(vg, art->image, art->w, art->h, {-R * 0.98f, -R * 0.98f, R * 1.96f, R * 1.96f}, R, artA);
        else ui.generatedArt({-R * 0.98f, -R * 0.98f, R * 1.96f, R * 1.96f}, seed, title, R, true);
        for (float g = 0.45f; g < 0.98f; g += 0.09f) {
            nvgBeginPath(vg);
            nvgCircle(vg, 0, 0, R * g);
            nvgStrokeColor(vg, nvgRGBAf(0, 0, 0, 0.12f));
            nvgStrokeWidth(vg, 1.2f);
            nvgStroke(vg);
        }
        nvgBeginPath(vg);
        nvgCircle(vg, 0, 0, R * 0.09f);
        nvgFillColor(vg, nvgRGB(10, 10, 14));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, 0, 0, R * 0.09f);
        nvgStrokeColor(vg, nvgRGBAf(1, 1, 1, 0.25f));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
        nvgRestore(vg);
    } else if (coverAlpha > 0.01f) {
        ui.pushAlpha(coverAlpha);
        float rad = std::min(18.0f, cover.w * 0.06f);
        ui.shadow(cover, rad, 34, 0.6f, 14);
        if (art) {
            fillWithImage(vg, art->image, art->w, art->h, cover, rad, artA);
        } else {
            ui.generatedArt(cover, seed, title, rad);
        }
        ui.popAlpha();
    }
    ui.popAlpha();
    nvgRestore(vg);
    // Loading the next song, or a seek that takes a moment: a spinner on the cover, faded in
    musicWait_ = (!p.loaded || p.seeking || p.buffering) ? musicWait_ + dt : 0;
    float wa = ui.smooth(uid("music.wait"), musicWait_ > 0.12f ? 1.0f : 0.0f, 10);
    if (wa > 0.01f) {
        ui.pushAlpha(wa);
        nvgBeginPath(vg);
        nvgCircle(vg, cover.cx(), cover.cy(), 40);
        nvgFillColor(vg, hex(0, 0.5f));
        nvgFill(vg);
        ui.spinner(cover.cx(), cover.cy(), 22, hex(0xFFFFFF, 0.95f), 4);
        ui.popAlpha();
    }

    // ---- Everything below fades away in the full-screen visualizer
    float ua = 1 - m;
    bool uiOff = parentBlocked || m > 0.5f;
    bool prevBlocked = ui.blocked;
    ui.blocked = uiOff;
    ui.pushAlpha(ua);
    NVGcolor white = hex(0xFFFFFF), dim = hex(0xFFFFFF, 0.68f), faint = hex(0xFFFFFF, 0.45f);

    // Top bar
    if (ui.iconButton(uid("music.back"), 50, 48, 52, Icon::Back, white, 0, false)) {
        ui.popAlpha();
        ui.blocked = prevBlocked;
        exit();
        return;
    }
    // A folder's name, or the drive's when the song sits at the top of one ("ums0:" means nothing).
    std::string from = local ? fromDiskName(baseName(parentDir(currentPath_))) : "Stream";
    if (local && app.cfg.s.tidyNames) from = tidySpaces(from);
    if (local)
        for (auto& d : app.drives())
            if (parentDir(currentPath_) == d.path) from = d.label;
    std::string where =
        p.playlistCount > 1 ? tr(std::to_string(p.playlistPos + 1) + " of " + std::to_string(p.playlistCount)) : "";
    ui.text(90, 40, "PLAYING FROM", 12, faint, LEFT, true);
    ui.text(90, 62, ui.ellipsize(from + (where.empty() ? "" : "  ·  " + where), W * 0.45f, 17, true), 17, white,
            LEFT, true);
    {
        float rx = W - 34;
        int pct = platform::batteryPercent();
        if (pct >= 0) {
            rx -= 30;
            ui.battery(rx, 48, pct, platform::charging(), true);
            rx -= 18;
        }
        ui.text(rx, 48, clockText(cfg.s.clock24), 20, white, RIGHT, true);
        rx -= ui.textWidth(clockText(cfg.s.clock24), 20, true) + 16;
        // The sleep timer, counting down; a tap changes it.
        if (sleepRemaining_ > 0 || sleepEndOfFile_) {
            std::string s = sleepRemaining_ > 0 ? formatTime(sleepRemaining_) : tr("End of this song");
            float w = ui.textWidth(s, 15, true) + 46;
            Rect r{rx - w, 48 - 16, w, 32};
            ui.fillRect(r, withAlpha(tint, ui.pressedVisual(r) ? 0.45f : 0.3f), 16);
            ui.icon(Icon::Moon, r.x + 18, r.cy(), 16, white);
            ui.text(r.x + 32, r.cy(), s, 15, white, LEFT, true);
            if (!parentBlocked && panel_ == P_NONE && ui.tapped(r)) openPanel(P_MORE);
            rx = r.x - 8;
        }
        // Not at normal speed: says so; a tap goes back to 1x.
        if (std::fabs(p.speed - 1.0) > 0.01) {
            char sp[16];
            snprintf(sp, sizeof sp, "%.2gx", p.speed);
            float w = ui.textWidth(sp, 15, true) + 46;
            Rect r{rx - w, 48 - 16, w, 32};
            ui.fillRect(r, withAlpha(tint, ui.pressedVisual(r) ? 0.45f : 0.3f), 16);
            ui.icon(Icon::Speed, r.x + 18, r.cy(), 16, white);
            ui.text(r.x + 32, r.cy(), sp, 15, white, LEFT, true);
            if (!parentBlocked && panel_ == P_NONE && ui.tapped(r)) {
                p.setSpeed(1.0);
                ui.toast("Normal speed", Icon::Speed);
            }
        }
    }

    float tw = x1 - x0;
    ui.text(x0, metaTop + 6, "NOW PLAYING", 13, withAlpha(tint, 0.95f), LEFT, true);
    if (changing) {  // the last song's words float up and away
        ui.pushAlpha(1 - se);
        float dy = -16 * se;
        ui.text(x0, metaTop + 44 + dy, ui.ellipsize(prevSong_.title, tw, 36, true), 36, white, LEFT, true);
        ui.text(x0, metaTop + 80 + dy, ui.ellipsize(prevSong_.artist, tw, 21), 21, dim, LEFT);
        if (!prevSong_.album.empty()) ui.text(x0, metaTop + 106 + dy, ui.ellipsize(prevSong_.album, tw, 17), 17, faint, LEFT);
        ui.popAlpha();
    }
    float tdy = changing ? 18 * (1 - se) : 0;
    ui.pushAlpha(newA);
    ui.marquee(uid("music.title"), x0, metaTop + 44 + tdy, tw, title, 36, white, LEFT, true);
    // Until the tags are read, no "Unknown artist" that would flash and change a moment later
    std::string artistLine = !artist.empty() ? artist : (p.loaded ? "Unknown artist" : "");
    ui.marquee(uid("music.artist"), x0, metaTop + 80 + tdy * 1.2f, tw, artistLine, 21, dim);
    std::string albumLine = album + (year.empty() ? "" : (album.empty() ? "" : "  ·  ") + year);
    if (!albumLine.empty()) ui.marquee(uid("music.album"), x0, metaTop + 106 + tdy * 1.4f, tw, albumLine, 17, faint);
    ui.popAlpha();
    {
        std::vector<std::string> chips;
        vis::FileInfo fi;
        if (local && analyzer_.path() == currentPath_ && analyzer_.info(fi)) {
            if (!fi.codec.empty()) chips.push_back(codecLabel(fi.codec));
            if (fi.bits > 0 && (lossless(fi.codec) || fi.bits > 16)) chips.push_back(std::to_string(fi.bits) + "-bit");
            if (fi.sampleRate > 0) {
                char b[16];
                snprintf(b, sizeof b, fi.sampleRate % 1000 ? "%.1f kHz" : "%.0f kHz", fi.sampleRate / 1000.0);
                chips.push_back(b);
            }
            if (fi.channels == 1) chips.push_back("Mono");
            else if (fi.channels == 2) chips.push_back("Stereo");
            else if (fi.channels > 2) chips.push_back(std::to_string(fi.channels) + " ch");
            if (!lossless(fi.codec) && fi.bitrate > 0) chips.push_back(std::to_string((int)(fi.bitrate / 1000)) + " kbps");
        } else if (!p.audioCodec.empty()) {
            chips.push_back(codecLabel(p.audioCodec));
        }
        float cx = x0;
        for (auto& c : chips) {
            float w = ui.textWidth(c, 13, true) + 22;
            if (cx + w > x1) break;
            ui.fillRect({cx, metaTop + 124, w, 24}, hex(0xFFFFFF, 0.1f), 12);
            ui.strokeRect({cx, metaTop + 124, w, 24}, hex(0xFFFFFF, 0.14f), 1, 12);
            ui.text(cx + w / 2, metaTop + 136, c, 13, dim, CENTER, true);
            cx += w + 8;
        }
    }

    // Where the stage visualizer isn't used, that space shows what plays next.
    if (showLyrics) drawLyrics({x0 - 6, stageN.y + 4, tw + 12, stageN.h - 4}, !uiOff);
    else if (place != vis::Place::Stage && p.playlistPos + 1 < (int64_t)p.playlist.size()) {
        float y = stageN.y + 14;
        ui.text(x0, y, "UP NEXT", 12, faint, LEFT, true);
        y += 16;
        for (int64_t i = p.playlistPos + 1, shown = 0;
             i < (int64_t)p.playlist.size() && shown < 4 && y + 36 < stageN.bottom(); i++, shown++) {
            Rect r{x0 - 10, y, tw + 20, 36};
            if (ui.pressedVisual(r)) ui.fillRect(r, hex(0xFFFFFF, 0.08f), 10);
            ui.text(x0, r.cy(), std::to_string(shown + 1), 15, faint, LEFT, true);
            ui.text(x0 + 26, r.cy(), ui.ellipsize(shownTitle(p.playlist[i], app.cfg.s.tidyNames), tw - 30, 17), 17, dim);
            if (ui.tapped(r)) {
                saveProgress();
                p.playIndex((int)i);
            }
            y += 38;
        }
    }

    // Seek bar and times
    Rect bar{x0, barY - 10, tw, 20};
    drawSeekBar(bar, ua);
    ui.text(x0, barY + 22, formatTime(gesture_ == G_SCRUB ? seekTarget_ : p.position), 15, dim, LEFT, true);
    ui.text(x1, barY + 22, "−" + formatTime(std::max(0.0, p.duration - p.position)), 15, faint, RIGHT, true);

    // Transport
    float cx = x0 + tw / 2;
    bool multi = p.playlistCount > 1;
    ui.setDefaultFocus(uid("music.play"));
    if (ui.iconButton(uid("music.shuffle"), cx - 178, controlsY, 48, Icon::Shuffle, shuffled_ ? tint : dim)) {
        if (p.queueLoading()) {
            ui.toast("The queue is still loading", Icon::Playlist);
        } else {
            shuffled_ = !shuffled_;
            p.shuffle(shuffled_);
            ui.toast(shuffled_ ? "Shuffle on" : "Shuffle off", Icon::Shuffle);
        }
    }
    if (ui.iconButton(uid("music.prev"), cx - 96, controlsY, 60, Icon::Prev, white)) doAction(Action::PrevFile);
    {
        float pr = 84;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, controlsY, pr / 2 + 26);
        nvgFillPaint(vg, nvgRadialGradient(vg, cx, controlsY, pr / 2, pr / 2 + 26, withAlpha(tint, 0.3f), withAlpha(tint, 0)));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, controlsY, pr / 2);
        nvgFillColor(vg, white);
        nvgFill(vg);
        if (ui.iconButton(uid("music.play"), cx, controlsY, pr, p.paused ? Icon::Play : Icon::Pause, nvgRGB(12, 13, 18), 0,
                          false))
            p.togglePause();
    }
    if (ui.iconButton(uid("music.next"), cx + 96, controlsY, 60, Icon::Next, multi ? white : faint))
        doAction(Action::NextFile);
    int loop = cfg.s.loopMode;
    if (ui.iconButton(uid("music.repeat"), cx + 178, controlsY, 48, Icon::Repeat, loop ? tint : dim)) {
        // Off → whole queue → this song → off
        cfg.s.loopMode = loop == 0 ? 2 : (loop == 2 ? 1 : 0);
        app.settingsChanged();
        ui.toast(cfg.s.loopMode == 2 ? "Repeat all" : (cfg.s.loopMode == 1 ? "Repeat this song" : "Repeat off"),
                 Icon::Repeat);
    }
    if (loop == 1) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx + 192, controlsY - 13, 8);
        nvgFillColor(vg, tint);
        nvgFill(vg);
        ui.text(cx + 192, controlsY - 13, "1", 11, nvgRGB(10, 10, 14), CENTER, true);
    }

    // The dock: volume under the cover, the other actions under the controls
    float dockY = H - dock / 2 + 4;
    {
        Rect vr{coverN.x + 34, dockY - 12, coverN.w - 34, 24};
        ui.icon(p.mute || p.volume <= 0 ? Icon::VolumeMute : Icon::Volume, coverN.x + 10, vr.cy(), 20, dim);
        uint32_t vid = uid("music.volume");
        ui.focusable(vid, vr);
        bool foc = ui.focused(vid) && ui.showFocus();
        if (foc) ui.strokeRect(vr.inset(-8, -4), withAlpha(ui.t.accent, 0.9f), 2, 14);
        float v = (float)p.volume;
        if (ui.slider(vid, vr, v, 0, (float)cfg.maxVolumeValue(), 5, ui.focused(vid) && !ui.blocked)) {
            p.setVolume(v);
            if (p.mute && v > 0) p.setMute(false);
        }
    }

    // Bottom row
    {
        float py = dockY, ph = 44;
        struct Pill { const char* key; Icon ic; std::string label; int action; bool show; };
        bool liked = app.lib.isLiked(currentPath_);
        bool collect = app.canCollect(currentPath_);
        Pill pills[] = {
            {"music.like", liked ? Icon::HeartFilled : Icon::Heart, liked ? "Liked" : "Like", 0, collect},
            {"music.add", Icon::Plus, "Add", 1, collect},
            {"music.lyrics", Icon::Captions, "Lyrics", 6, !lyrics_.empty()},
            {"music.visual", Icon::Waveform, vis::styleName(style), 2, true},
            {"music.queue", Icon::Playlist, "Queue", 3, multi},
            {"music.dark", Icon::Moon, "", 5, true},
            {"music.full", Icon::Expand, "", 4, true},
        };
        // Labels when they fit, otherwise icons only, so no action ever drops off the row.
        float need = 0;
        for (auto& pl : pills)
            if (pl.show) need += pl.label.empty() ? ph + 10 : ui.textWidth(pl.label, 15, true) + 62;
        bool iconsOnly = need > x1 - x0 - 56;
        float px = x0;
        for (auto& pl : pills) {
            if (!pl.show) continue;
            bool bare = iconsOnly || pl.label.empty();
            float w = bare ? ph : ui.textWidth(pl.label, 15, true) + 52;
            Rect r{px, py - ph / 2, w, ph};
            uint32_t id = uid(pl.key);
            ui.focusable(id, r);
            float fv = ui.smooth(id ^ 0xf0c, ui.focused(id) && ui.showFocus() ? 1.0f : 0.0f, 16);
            float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
            ui.fillRect(r, hex(0xFFFFFF, 0.1f + 0.1f * pv + 0.08f * fv), ph / 2);
            if (fv > 0.01f) ui.strokeRect(r.inset(-1.5f), withAlpha(ui.t.accent, fv), 3, ph / 2 + 1.5f);
            NVGcolor ic = pl.action == 0 && liked ? ui.t.danger : white;
            if (bare) {
                ui.icon(pl.ic, r.cx(), r.cy(), 20, ic);
            } else {
                ui.icon(pl.ic, r.x + 22, r.cy(), 19, ic);
                ui.text(r.x + 40, r.cy(), pl.label, 15, white, LEFT, true);
            }
            if (ui.tapped(r) || (ui.focused(id) && ui.confirm())) {
                switch (pl.action) {
                    case 0: doAction(Action::ToggleLike); break;
                    case 1: doAction(Action::AddToPlaylist); break;
                    case 2: openPanel(P_VISUAL); break;
                    case 3: openPanel(P_PLAYLIST); break;
                    case 4: immersive_ = true; break;
                    case 5: requestScreenOff(); break;
                    case 6:
                        cfg.s.lyrics = !cfg.s.lyrics;
                        app.saveSoon();
                        ui.toast(cfg.s.lyrics ? "Lyrics on" : "Lyrics off", Icon::Captions);
                        break;
                }
            }
            px += w + 10;
        }
        if (ui.iconButton(uid("music.more"), x1 - 22, py, 44, Icon::Gear, white)) openPanel(P_MORE);
    }
    ui.popAlpha();
    ui.blocked = prevBlocked;

    // ---- Full-screen visualizer: what's playing, in a corner
    if (m > 0.01f) {
        ui.pushAlpha(m);
        Rect thumb{34, H - 98, 64, 64};
        if (art) fillWithImage(vg, art->image, art->w, art->h, thumb, 10, artA);
        else ui.generatedArt(thumb, seed, title, 10);
        ui.marquee(uid("music.ititle"), thumb.right() + 16, thumb.y + 20, W * 0.5f, title, 22, white, LEFT, true);
        ui.marquee(uid("music.iartist"), thumb.right() + 16, thumb.y + 46, W * 0.5f, artist, 16, dim);
        float frac = p.duration > 0 ? (float)std::clamp(p.position / p.duration, 0.0, 1.0) : 0;
        ui.fillRect({0, H - 4, W, 4}, hex(0xFFFFFF, 0.12f), 0);
        ui.fillRect({0, H - 4, W * frac, 4}, tint, 0);
        // The line being sung, large, above the song's name.
        if (showLyrics && lyrics_.synced) {
            int cur = lyrics_.lineAt(musicClock_ + 0.15);
            if (cur >= 0 && !lyrics_.lines[cur].text.empty()) {
                ui.marquee(uid("music.ilyric"), W / 2, H - 150, W - 120, lyrics_.lines[cur].text, 28, white, CENTER, true);
                if (cur + 1 < (int)lyrics_.lines.size())
                    ui.text(W / 2, H - 116, ui.ellipsize(lyrics_.lines[cur + 1].text, W - 160, 18), 18, faint, CENTER);
            }
        }
        std::string tip = tr(vis::styleName(style)) + "   ·   " + tr("X next style") + "   ·   " + tr("any button to return");
        ui.text(W - 34, H - 66, tip, 15, faint, RIGHT, true);
        ui.popAlpha();
    }

    // Name of a newly picked style
    if (styleFlash_ > 0) {
        float a = std::min(1.0f, styleFlash_ / 0.3f);
        ui.pushAlpha(a);
        std::string name = vis::styleName(style);
        float w = ui.textWidth(name, 22, true) + 70;
        Rect r{W / 2 - w / 2, H * 0.1f, w, 50};
        ui.fillRect(r, hex(0, 0.6f), 25);
        ui.icon(Icon::Waveform, r.x + 30, r.cy(), 20, tint);
        ui.text(r.x + 50, r.cy(), name, 22, white, LEFT, true);
        ui.popAlpha();
    }
}

// True when the first letter is Persian, Arabic or Hebrew: such lines read from the right.
static bool rightToLeft(const std::string& t) {
    for (size_t i = 0; i < t.size();) {
        unsigned char c = (unsigned char)t[i];
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        unsigned cp = len == 1 ? c : c & (0xFF >> (len + 1));
        for (int k = 1; k < len && i + k < t.size(); k++) cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3F);
        i += len;
        if (cp >= 0x0590 && cp <= 0x08FF) return true;
        if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0xC0 && cp < 0x0590)) return false;
    }
    return false;
}

// Lyrics in the space beside the cover: timed ones scroll with the song (the sung line bright,
// tap a line to go there); plain ones scroll through as the song plays.
void PlayerScreen::drawLyrics(Rect r, bool interactive) {
    UI& ui = app.ui;
    Player& p = app.player;
    NVGcontext* vg = ui.vg;
    if (r.h < 40) return;
    nvgSave(vg);
    nvgIntersectScissor(vg, r.x - 4, r.y, r.w + 8, r.h);
    const auto& lines = lyrics_.lines;
    if (lyrics_.synced) {
        const float lineH = 42;
        int cur = lyrics_.lineAt(musicClock_ + 0.15);
        float target = (float)std::max(cur, 0);
        if (std::fabs(target - lyricsScroll_) > 6) lyricsScroll_ = target;  // a seek: jump
        lyricsScroll_ += (target - lyricsScroll_) * std::min(1.0f, ui.dt * 7);
        float cy = r.y + r.h * 0.38f;
        int first = std::max(0, (int)(lyricsScroll_ - (cy - r.y) / lineH) - 1);
        for (int i = first; i < (int)lines.size(); i++) {
            float y = cy + (i - lyricsScroll_) * lineH;
            if (y > r.bottom() + lineH) break;
            float edge = std::min(std::min(1.0f, (y - r.y) / 40), std::min(1.0f, (r.bottom() - y) / 40));
            if (edge <= 0) continue;
            bool now = i == cur;
            std::string t = lines[i].text.empty() ? "• • •" : lines[i].text;
            NVGcolor c = hex(0xFFFFFF, (now ? 1.0f : (i < cur ? 0.32f : 0.5f)) * edge);
            bool rtl = rightToLeft(t);
            float tx = rtl ? r.right() - 6 : r.x + 6;
            int al = rtl ? RIGHT : LEFT;
            if (now) ui.marquee(uid("music.lyric"), tx, y, r.w - 12, t, 24, c, al, true);
            else ui.text(tx, y, ui.ellipsize(t, r.w - 12, 19), 19, c, al);
            if (interactive && !lines[i].text.empty() && ui.tapped({r.x, y - lineH / 2, r.w, lineH}))
                p.seek(lines[i].time, false);
        }
    } else {
        // Untimed: the whole text drifts up as the song goes on.
        const float lineH = 28;
        float total = lines.size() * lineH;
        float frac = p.duration > 0 ? (float)std::clamp(p.position / p.duration, 0.0, 1.0) : 0;
        float off = std::max(0.0f, total - r.h + 20) * frac;
        for (size_t i = 0; i < lines.size(); i++) {
            float y = r.y + 14 + i * lineH - off;
            if (y < r.y - lineH) continue;
            if (y > r.bottom() + lineH) break;
            float edge = std::min(std::min(1.0f, (y - r.y + 6) / 30), std::min(1.0f, (r.bottom() - y) / 30));
            if (edge <= 0) continue;
            bool rtl = rightToLeft(lines[i].text);
            ui.text(rtl ? r.right() - 6 : r.x + 6, y, ui.ellipsize(lines[i].text, r.w - 12, 18), 18,
                    hex(0xFFFFFF, 0.72f * edge), rtl ? RIGHT : LEFT);
        }
    }
    nvgRestore(vg);
}

void PlayerScreen::nextVisualizer(int dir) {
    int n = vis::styleCount();
    app.cfg.s.musicVisual = (app.cfg.s.musicVisual + dir + n) % n;
    app.saveSoon();
    styleFlash_ = 1.6f;
}

void PlayerScreen::requestScreenOff() {
    screenOffIn_ = 1.6f;
    panel_ = P_NONE;
}

void PlayerScreen::wakeScreen() {
    platform::setScreenOff(false);
    screenDark_ = false;
    screenOffIn_ = -1;
    musicIdle_ = 0;
    darkClock_ = 0;  // the next dark spell starts its own clock
}

// While the screen is going or gone dark: black, and only a double tap (or A / B) wakes it, so
// nothing is pressed by accident in a pocket.
bool PlayerScreen::darkTick() {
    Input& in = *app.ui.in;
    Player& p = app.player;
    double now = platform::nowSeconds();
    // A long gap means the console was asleep: the system lit the screen again on waking.
    bool slept = darkClock_ > 0 && now - darkClock_ > 2;
    float dt = darkClock_ > 0 ? (float)std::min(now - darkClock_, 0.5) : 0;
    darkClock_ = now;
    memoryWatch(dt);
    if (app.screen != this) return false;
    // The last song has ended: the screen stays off (waking it at the end of an album lit up a
    // console sitting in a bag) and the console may now go to sleep by itself.
    bool finished = sawPlaying_ && p.idle;
    int awake = app.cfg.s.keepAwake;
    platform::setMediaPlayback(!finished && p.loaded && awake != 2 && (awake == 1 || !p.paused));
    // The sleep timer keeps counting with the screen off.
    if (sleepRemaining_ > 0 && !p.paused && !finished) tickSleep(dt);
    bool wake = slept || in.down(BTN_A) || in.down(BTN_B) || !platform::screenOff() || (!finished && !musicMode());
    if (in.tap) {
        if (now - darkTapAt_ < 0.6) wake = true;
        darkTapAt_ = now;
    }
    if (!wake) return true;
    wakeScreen();
    in.consumeTap();
    return false;
}

bool PlayerScreen::drawScreenOff() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    float dt = ui.dt;
    // Idle timer for the automatic setting.
    static const float after[] = {0, 30, 60, 120, 300};
    int setting = std::clamp(app.cfg.s.musicScreenOff, 0, 4);
    bool input = in.pressed || in.touching || in.tap;
    musicIdle_ = input ? 0 : musicIdle_ + dt;
    if (!screenDark_ && screenOffIn_ < 0 && setting > 0 && musicIdle_ > after[setting] && app.player.loaded &&
        !app.player.paused && panel_ == P_NONE)
        requestScreenOff();
    if (screenDark_ && !platform::screenOff()) wakeScreen();  // the system turned it back on (HOME)
    if (screenOffIn_ < 0 && !screenDark_) return false;

    // Two taps within 0.6 s, counted here: the usual double-tap window is short for a dark screen.
    bool wake = in.down(BTN_A) || in.down(BTN_B);
    if (in.tap) {
        if (ui.time - darkTap_ < 0.6f) wake = true;
        darkTap_ = ui.time;
    }
    if (screenOffIn_ >= 0 && screenOffIn_ < 1.4f && (in.pressed || in.tap)) wake = true;  // changed their mind
    if (wake) {
        wakeScreen();
        in.consumeTap();
        ui.back();
        ui.confirm();
        return true;
    }
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, 0, 0, ui.W, ui.H);
    nvgFillColor(ui.vg, nvgRGB(0, 0, 0));
    nvgFill(ui.vg);
    if (screenOffIn_ >= 0) {
        float a = std::min(1.0f, (1.6f - screenOffIn_) / 0.25f) * std::min(1.0f, screenOffIn_ / 0.3f + 0.2f);
        ui.pushAlpha(a);
        ui.icon(Icon::Moon, ui.W / 2, ui.H / 2 - 40, 56, hex(0xFFFFFF, 0.9f));
        ui.text(ui.W / 2, ui.H / 2 + 22, "Screen off", 26, hex(0xFFFFFF), CENTER, true);
        ui.text(ui.W / 2, ui.H / 2 + 56, "The music keeps playing. Tap twice to wake.", 17, hex(0xFFFFFF, 0.6f), CENTER);
        ui.popAlpha();
        screenOffIn_ -= dt;
        if (screenOffIn_ < 0) {
            screenDark_ = true;
            platform::setScreenOff(true);
        }
    }
    return true;
}

void PlayerScreen::handleMusicButtons() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    Player& p = app.player;

    if (panel_ != P_NONE) {
        if (ui.back() || in.down(BTN_PLUS)) {
            panel_ = P_NONE;
            ui.setFocus(uid("music.play"));
        }
        return;
    }
    if (in.down(BTN_X)) nextVisualizer(1);
    if (immersive_) {
        // Anything else brings the controls back.
        bool any = (in.pressed & ~BTN_X) || in.tap;
        if (any) {
            in.consumeTap();
            ui.back();
            immersive_ = false;
            ui.setFocus(uid("music.play"));
        }
        return;
    }
    if (ui.back()) {
        exit();
        return;
    }
    if (in.down(BTN_Y)) doAction(Action::ToggleLike);
    if (in.down(BTN_L)) doAction(Action::PrevFile);
    if (in.down(BTN_R)) doAction(Action::NextFile);
    if (in.rep(BTN_ZL)) seekRelative(-10);
    if (in.rep(BTN_ZR)) seekRelative(10);
    if (in.down(BTN_PLUS)) openPanel(P_MORE);
    if (in.down(BTN_MINUS)) immersive_ = true;

    // A swipe across the screen changes the song; a tap on empty space shows the visualizer full screen.
    if (in.pan.ended && in.pan.owner == 0) {
        float dx = in.pan.x - in.pan.startX, dy = in.pan.y - in.pan.startY;
        if (std::fabs(dx) > 120 && std::fabs(dx) > std::fabs(dy) * 1.5f)
            doAction(dx < 0 ? Action::NextFile : Action::PrevFile);
    }
    if (in.tapConfirmed) immersive_ = true;
    (void)p;
}
