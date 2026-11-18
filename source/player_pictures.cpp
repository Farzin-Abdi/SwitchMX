// The picture viewer: mpv shows the picture; this draws the bar, the thumbnail strip, the info
// card and runs slideshows.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "platform.hpp"

namespace {
constexpr float kThumbW = 104, kThumbH = 68, kThumbGap = 10;

std::string formatName(const std::string& codec, const std::string& path) {
    static const char* names[][2] = {{"mjpeg", "JPEG"}, {"png", "PNG"},   {"webp", "WebP"}, {"gif", "GIF"},
                                     {"bmp", "BMP"},    {"tiff", "TIFF"}, {"apng", "APNG"}, {"jpegxl", "JPEG XL"},
                                     {"av1", "AVIF"},   {"hevc", "HEIC"}};
    for (auto& n : names)
        if (codec == n[0]) return n[1];
    std::string ext = fileExtension(path);
    for (auto& c : ext) c = (char)toupper((unsigned char)c);
    return ext;
}

// Fits the whole image inside the rect.
void drawContained(NVGcontext* vg, int image, int iw, int ih, Rect r, float alpha) {
    float scale = std::min(r.w / iw, r.h / ih);
    float w = iw * scale, h = ih * scale;
    nvgBeginPath(vg);
    nvgRect(vg, r.cx() - w / 2, r.cy() - h / 2, w, h);
    nvgFillPaint(vg, nvgImagePattern(vg, r.cx() - w / 2, r.cy() - h / 2, w, h, 0, image, alpha));
    nvgFill(vg);
}

void drawCropped(NVGcontext* vg, int image, int iw, int ih, Rect r, float radius, float alpha) {
    float scale = std::max(r.w / iw, r.h / ih);
    float w = iw * scale, h = ih * scale;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgFillPaint(vg, nvgImagePattern(vg, r.cx() - w / 2, r.cy() - h / 2, w, h, 0, image, alpha));
    nvgFill(vg);
}
}  // namespace

// While the next picture loads, its thumbnail stands in, so browsing never flashes black.
void PlayerScreen::drawPicturePlaceholder() {
    Player& p = app.player;
    if (p.loaded || p.playlistPos < 0 || p.playlistPos >= (int64_t)p.playlist.size()) return;
    const CoverArt::Art* art = app.covers.get(p.playlist[p.playlistPos], MediaKind::Image);
    if (!art) return;
    drawContained(app.ui.vg, art->image, art->w, art->h, {0, 0, app.ui.W, app.ui.H}, 0.85f);
}

void PlayerScreen::tickSlideshow(float dt) {
    Player& p = app.player;
    if (!slideshow_ || !p.loaded || panel_ != P_NONE || gesture_ != G_NONE || app.ui.blocked) return;
    slideTimer_ += dt;
    if (slideTimer_ < slideInterval()) return;
    slideTimer_ = 0;
    // A slideshow goes round instead of stopping at the last picture.
    if (p.playlistPos + 1 < p.playlistCount) p.next();
    else p.playIndex(0);
}

float PlayerScreen::slideInterval() const {
    float s = app.cfg.slideshowSecs();
    return s > 0 ? s : 5;
}

void PlayerScreen::toggleSlideshow() {
    Player& p = app.player;
    if (p.playlistCount < 2) {
        app.ui.toast("This is the only picture here", Icon::Image);
        return;
    }
    slideshow_ = !slideshow_;
    slideTimer_ = 0;
    if (slideshow_) osd_ = false;  // a slideshow is for watching
    app.ui.toast(slideshow_ ? "Slideshow started" : "Slideshow stopped", slideshow_ ? Icon::Play : Icon::Pause);
}

void PlayerScreen::deletePicture() {
    std::string path = currentPath_;
    if (path.empty()) return;
    app.confirm("Delete this picture?", trf("\"{0}\" will be permanently removed.", {displayName(path)}), "Delete",
                [this, path] {
                    if (unlink(path.c_str()) != 0) {
                        app.ui.toast("Couldn't delete the picture", Icon::Info);
                        return;
                    }
                    app.lib.forgetFile(path);
                    app.lib.save();
                    app.ui.toast("Picture deleted", Icon::Trash);
                    if (app.player.playlistCount > 1) app.player.removeCurrent();
                    else exit();
                },
                true);
}

void PlayerScreen::drawPictureUi(float a) {
    UI& ui = app.ui;
    Player& p = app.player;
    NVGcontext* vg = ui.vg;
    float W = ui.W, H = ui.H;
    bool multi = p.playlistCount > 1;
    NVGcolor white = hex(0xFFFFFF), dim = hex(0xFFFFFF, 0.7f);
    // Pictures are driven by dedicated buttons, so no control should look selected.
    ui.focusable(uid("pic.none"), {-10, -10, 1, 1});
    ui.setFocus(uid("pic.none"));
    ui.pushAlpha(a);

    // Scrims
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, 150);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, 150, hex(0, 0.75f), hex(0, 0)));
    nvgFill(vg);
    if (multi) {
        nvgBeginPath(vg);
        nvgRect(vg, 0, H - 170, W, 170);
        nvgFillPaint(vg, nvgLinearGradient(vg, 0, H - 170, 0, H, hex(0, 0), hex(0, 0.8f)));
        nvgFill(vg);
    }

    // Top bar
    float slide = -20 * (1 - a);
    if (ui.iconButton(uid("pic.back"), 50, 50 + slide, 52, Icon::Back, white, 0, false)) {
        ui.popAlpha();
        exit();
        return;
    }
    if (infoPath_ != currentPath_) {
        infoPath_ = currentPath_;
        struct stat st;
        infoSize_ = 0;
        infoDate_ = 0;
        if (stat(currentPath_.c_str(), &st) == 0) {
            infoSize_ = (uint64_t)st.st_size;
            infoDate_ = st.st_mtime;
        }
    }
    // Each part is translated before the line is joined: as one string ("2 of 2 · 1920 × 1080") it
    // matches no pattern, so the count stayed in English beside Persian or Arabic text.
    auto num = [](long long n) { return i18n::localDigits(std::to_string(n)); };
    std::string sub;
    if (multi) sub = trf("{0} of {1}", {num(p.playlistPos + 1), num(p.playlistCount)});
    if (p.width > 0) sub += (sub.empty() ? "" : "  ·  ") + num(p.width) + " × " + num(p.height);
    if (infoSize_) sub += "  ·  " + formatSize(infoSize_);
    float right = W - 34;
    struct Btn { const char* key; Icon ic; NVGcolor c; int act; };
    bool fav = app.lib.isFavorite(currentPath_);
    Btn btns[] = {
        {"pic.delete", Icon::Trash, white, 0},
        {"pic.fav", fav ? Icon::StarFilled : Icon::Star, fav ? ui.t.warning : white, 1},
        {"pic.rotate", Icon::Rotate, white, 2},
        {"pic.info", Icon::Info, infoOpen_ ? ui.t.accent : white, 3},
        {"pic.slides", slideshow_ ? Icon::Pause : Icon::Play, slideshow_ ? ui.t.accent : white, 4},
    };
    float bx = right - 24;
    for (auto& b : btns) {
        if (b.act == 4 && !multi) continue;
        if (b.act == 4 && slideshow_) {
            // Progress to the next picture, around the button
            nvgBeginPath(vg);
            nvgArc(vg, bx, 50 + slide, 22, -NVG_PI / 2, -NVG_PI / 2 + 2 * NVG_PI * std::min(1.0f, slideTimer_ / slideInterval()),
                   NVG_CW);
            nvgStrokeColor(vg, ui.t.accent);
            nvgStrokeWidth(vg, 3);
            nvgStroke(vg);
        }
        if (ui.iconButton(uid(b.key), bx, 50 + slide, 48, b.ic, b.c, 0, false)) {
            switch (b.act) {
                case 0: deletePicture(); break;
                case 1:
                    app.lib.toggleFavorite(currentPath_);
                    app.lib.save();
                    ui.toast(fav ? "Removed from favorites" : "Added to favorites", Icon::StarFilled);
                    break;
                case 2: p.setRotation(p.rotation + 90); break;
                case 3: infoOpen_ = !infoOpen_; break;
                case 4: toggleSlideshow(); break;
            }
        }
        bx -= 56;
    }
    float titleRight = bx - 10;
    ui.marquee(uid("pic.title"), 90, 40 + slide, titleRight - 90, displayName(currentPath_.empty() ? p.title : currentPath_),
            22, white, LEFT, true);
    ui.text(90, 66 + slide, ui.ellipsize(sub, titleRight - 90, 15), 15, dim);

    // Large arrows at the sides
    if (multi) {
        if (ui.iconButton(uid("pic.prev"), 46, H / 2 - 40, 60, Icon::ChevronLeft, white, 0, true)) doAction(Action::PrevFile);
        if (ui.iconButton(uid("pic.next"), W - 46, H / 2 - 40, 60, Icon::ChevronRight, white, 0, true))
            doAction(Action::NextFile);
    }

    // Thumbnail strip, centred on the current picture
    if (multi) {
        float step = kThumbW + kThumbGap;
        float target = (float)p.playlistPos * step;
        if (stripScroll_ < 0) stripScroll_ = target;
        stripScroll_ += (target - stripScroll_) * std::min(1.0f, ui.dt * 10);
        float y = H - 24 - kThumbH, cx = W / 2;
        int first = std::max(0, (int)((stripScroll_ - cx) / step) - 1);
        int last = std::min((int)p.playlist.size() - 1, (int)((stripScroll_ + cx) / step) + 1);
        for (int i = first; i <= last; i++) {
            bool cur = i == p.playlistPos;
            float s = cur ? 1.12f : 1;
            float w = kThumbW * s, h = kThumbH * s;
            Rect r{cx + i * step - stripScroll_ - w / 2, y + kThumbH - h, w, h};
            if (r.right() < 0 || r.x > W) continue;
            const CoverArt::Art* art = app.covers.get(p.playlist[i], MediaKind::Image);
            if (art) {
                drawCropped(vg, art->image, art->w, art->h, r, 8, (cur ? 1.0f : 0.7f) * std::min(1.0f, art->age / 0.25f));
            } else {
                ui.fillRect(r, hex(0xFFFFFF, 0.1f), 8);
                ui.icon(Icon::Image, r.cx(), r.cy(), 22, hex(0xFFFFFF, 0.4f));
            }
            if (cur) ui.strokeRect(r.inset(-2), ui.t.accent, 3, 10);
            if (!cur && ui.tapped(r)) {
                slideTimer_ = 0;
                p.playIndex(i);
            }
        }
    }

    // Controller hints
    if (ui.in->usingButtons) {
        float hy = multi ? H - 24 - kThumbH - 26 : H - 30, hx = 34;
        hx += ui.hint(hx, hy, BTN_A, "Hide");
        hx += ui.hint(hx, hy, BTN_X, "Info");
        if (multi) hx += ui.hint(hx, hy, BTN_Y, slideshow_ ? "Stop slideshow" : "Slideshow");
        hx += ui.hint(hx, hy, BTN_R, "Rotate");
        ui.hint(hx, hy, BTN_PLUS, "Adjust");
    }
    ui.popAlpha();
}

void PlayerScreen::drawPictureInfo() {
    UI& ui = app.ui;
    Player& p = app.player;
    float a = ui.smooth(uid("pic.info.anim"), infoOpen_ ? 1.0f : 0.0f, 14);
    if (a < 0.01f) return;
    float w = std::min(380.0f, ui.W - 60);
    Rect r{ui.W - w - 20 + 40 * (1 - a), 96, w, 0};
    struct Row { const char* label; std::string value; };
    auto num = [](long long n) { return i18n::localDigits(std::to_string(n)); };
    std::string dims = p.width > 0 ? num(p.width) + " × " + num(p.height) : "";
    if (p.width > 0) {
        char mp[32];
        snprintf(mp, sizeof mp, "  (%.1f MP)", p.width * p.height / 1e6);
        dims += mp;
    }
    std::vector<Row> rows = {
        {"Name", displayName(currentPath_)},
        {"Folder", platform::displayPath(parentDir(currentPath_))},
        {"Size", dims},
        {"File size", infoSize_ ? formatSize(infoSize_) : ""},
        {"Modified", formatDate(infoDate_)},
        {"Format", formatName(p.videoCodec, currentPath_)},
    };
    if (p.playlistCount > 1)
        rows.push_back({"Position", trf("{0} of {1}", {num(p.playlistPos + 1), num(p.playlistCount)})});
    if (std::fabs(p.zoom) > 0.01)
        rows.push_back({"Zoom", std::to_string((int)std::lround(std::pow(2.0, p.zoom) * 100)) + "%"});
    r.h = 64 + rows.size() * 50.0f;
    ui.pushAlpha(a);
    ui.glass(r, ui.t.radius + 6, 0.92f);
    NVGcolor fg = ui.t.light ? ui.t.text : hex(0xFFFFFF);
    ui.text(r.x + 24, r.y + 34, "Details", 20, fg, LEFT, true);
    float y = r.y + 70;
    for (auto& row : rows) {
        ui.text(r.x + 24, y, row.label, 13, withAlpha(fg, 0.55f), LEFT, true);
        ui.text(r.x + 24, y + 20, ui.ellipsize(row.value.empty() ? "—" : row.value, r.w - 48, 16), 16, fg);
        y += 50;
    }
    ui.popAlpha();
}

void PlayerScreen::handlePictureButtons() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    Player& p = app.player;
    if (ui.back()) {
        if (infoOpen_) infoOpen_ = false;
        else exit();
        return;
    }
    if (ui.confirm()) {
        osd_ = !osd_;
        osdTimer_ = 0;
    }
    if (in.rep(BTN_LEFT) || in.down(BTN_ZL)) {
        slideTimer_ = 0;
        doAction(Action::PrevFile);
    }
    if (in.rep(BTN_RIGHT) || in.down(BTN_ZR)) {
        slideTimer_ = 0;
        doAction(Action::NextFile);
    }
    int zdir = in.rep(BTN_UP) ? 1 : (in.rep(BTN_DOWN) ? -1 : 0);
    if (zdir) {
        double z = std::clamp(p.zoom + zdir * 0.15, -1.0, 3.0);
        p.setZoom(z);
        if (std::fabs(z) < 0.01) p.setPan(0, 0);
        hud(H_ZOOM, (float)z, std::to_string((int)std::lround(std::pow(2.0, z) * 100)) + "%", Icon::Expand);
    }
    if (in.down(BTN_X)) infoOpen_ = !infoOpen_;
    if (in.down(BTN_Y)) toggleSlideshow();
    if (in.down(BTN_R)) p.setRotation(p.rotation + 90);
    if (in.down(BTN_L)) p.setRotation(p.rotation - 90);
    if (in.down(BTN_PLUS)) openPanel(P_VIDEO);
    if (in.down(BTN_MINUS)) doAction(Action::ToggleLock);
}
