// Resolution-independent vector icons drawn on a 24x24 grid, and the app logo.
//
// The icons are two-tone: a soft tint of the icon's colour fills each shape's body, a clean
// outline draws it, and small details (dots, heads, play marks) are solid. Every shape has rounded
// corners and the same stroke weight, so the set reads as one family at any size.
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

#include "ui.hpp"

namespace {
constexpr float PI = 3.14159265f;
constexpr float kStroke = 1.8f;
constexpr float kTone = 0.24f;  // body tint, relative to the icon colour's own alpha

float deg(float d) { return d * PI / 180.0f; }

struct Pen {
    NVGcontext* vg;
    NVGcolor c;

    NVGcolor toneColor() const {
        NVGcolor t = c;
        t.a *= kTone;
        return t;
    }
    void stroke() {
        nvgStrokeColor(vg, c);
        nvgStroke(vg);
    }
    void fill() {
        nvgFillColor(vg, c);
        nvgFill(vg);
    }
    void tone() {
        nvgFillColor(vg, toneColor());
        nvgFill(vg);
    }
    // Tinted body with an outline: the look of most shapes in the set.
    void body() {
        tone();
        stroke();
    }
    void width(float w) { nvgStrokeWidth(vg, w); }

    void line(float x1, float y1, float x2, float y2) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, x1, y1);
        nvgLineTo(vg, x2, y2);
        stroke();
    }
    void open(std::initializer_list<float> pts) {
        nvgBeginPath(vg);
        auto it = pts.begin();
        nvgMoveTo(vg, it[0], it[1]);
        for (size_t i = 2; i + 1 < pts.size(); i += 2) nvgLineTo(vg, it[i], it[i + 1]);
        stroke();
    }
    // A closed polygon with every corner rounded by r. Leaves the path ready to fill or stroke.
    void rpath(const float* p, size_t n, float r) {
        nvgBeginPath(vg);
        float sx = (p[2 * (n - 1)] + p[0]) / 2, sy = (p[2 * (n - 1) + 1] + p[1]) / 2;
        nvgMoveTo(vg, sx, sy);
        for (size_t i = 0; i < n; i++) {
            size_t j = (i + 1) % n;
            nvgArcTo(vg, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1], r);
        }
        nvgClosePath(vg);
    }
    void rpath(std::initializer_list<float> pts, float r) { rpath(pts.begin(), pts.size() / 2, r); }
    void rpath(const std::vector<float>& pts, float r) { rpath(pts.data(), pts.size() / 2, r); }

    void rrectPath(float x, float y, float w, float h, float r) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w, h, r);
    }
    void rrect(float x, float y, float w, float h, float r) {
        rrectPath(x, y, w, h, r);
        stroke();
    }
    void rrectBody(float x, float y, float w, float h, float r) {
        rrectPath(x, y, w, h, r);
        body();
    }
    void rrectFill(float x, float y, float w, float h, float r) {
        rrectPath(x, y, w, h, r);
        fill();
    }
    void circle(float x, float y, float r) {
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, r);
        stroke();
    }
    void circleBody(float x, float y, float r) {
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, r);
        body();
    }
    void dot(float x, float y, float r) {
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, r);
        fill();
    }
    void arc(float x, float y, float r, float a0, float a1, int dir = NVG_CW) {
        nvgBeginPath(vg);
        nvgArc(vg, x, y, r, a0, a1, dir);
        stroke();
    }
    // An open arrow head pointing along angle `a` with its tip at (x, y), drawn like the Back arrow.
    void head(float x, float y, float a, float len = 4.2f, float spread = 0.80f) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, x - std::cos(a - spread) * len, y - std::sin(a - spread) * len);
        nvgLineTo(vg, x, y);
        nvgLineTo(vg, x - std::cos(a + spread) * len, y - std::sin(a + spread) * len);
        stroke();
    }
    // An arc from a0 to a1 (clockwise on screen) with a head at a1 following the turn.
    void arcArrow(float cx, float cy, float r, float a0, float a1) {
        arc(cx, cy, r, a0, a1);
        float tx = -std::sin(a1), ty = std::cos(a1);
        head(cx + std::cos(a1) * r, cy + std::sin(a1) * r, std::atan2(ty, tx) - 0.12f);
    }
    void triangle(float x1, float y1, float x2, float y2, float x3, float y3, float r) {
        float pts[] = {x1, y1, x2, y2, x3, y3};
        rpath(pts, 3, r);
    }
};

// Circular arrow around (12, 13), open at the top, like a replay mark. The player writes the seek
// amount inside it. Backwards is the mirror image.
void circularArrow(Pen& p, bool clockwise) {
    nvgSave(p.vg);
    if (!clockwise) {
        nvgTranslate(p.vg, 24, 0);
        nvgScale(p.vg, -1, 1);
    }
    p.arcArrow(12, 13, 8, deg(-55), deg(250));
    nvgRestore(p.vg);
}

void folderBack(Pen& p) {
    // Rear panel with its tab.
    p.rpath({3, 18, 3, 4.5f, 9.5f, 4.5f, 11.5f, 7, 21, 7, 21, 18}, 2.2f);
}
}  // namespace

void drawIcon(NVGcontext* vg, Icon ic, float cx, float cy, float size, NVGcolor c) {
    if (ic == Icon::None) return;
    nvgSave(vg);
    nvgTranslate(vg, cx - size * 0.5f, cy - size * 0.5f);
    nvgScale(vg, size / 24.0f, size / 24.0f);
    // Thin lines vanish when small, so small icons get a slightly heavier stroke.
    nvgStrokeWidth(vg, size < 18 ? 2.1f : kStroke);
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    Pen p{vg, c};

    switch (ic) {
        case Icon::None: break;

        // ---- Transport: solid, softly rounded.
        case Icon::Play:
            p.triangle(7, 4.6f, 20, 12, 7, 19.4f, 2.4f);
            p.fill();
            break;
        case Icon::Pause:
            p.rrectFill(5.5f, 4.5f, 4.8f, 15, 1.8f);
            p.rrectFill(13.7f, 4.5f, 4.8f, 15, 1.8f);
            break;
        case Icon::Next:
            p.triangle(4.5f, 5.2f, 15, 12, 4.5f, 18.8f, 2);
            p.fill();
            p.rrectFill(16.5f, 5.2f, 3, 13.6f, 1.5f);
            break;
        case Icon::Prev:
            p.triangle(19.5f, 5.2f, 9, 12, 19.5f, 18.8f, 2);
            p.fill();
            p.rrectFill(4.5f, 5.2f, 3, 13.6f, 1.5f);
            break;
        case Icon::SeekBack: circularArrow(p, false); break;
        case Icon::SeekForward: circularArrow(p, true); break;

        // ---- Files and media.
        case Icon::Folder: {
            NVGcolor full = c;
            folderBack(p);
            p.c.a = full.a * 0.5f;
            p.fill();
            p.c = full;
            p.rrectFill(3, 9, 18, 11, 2.2f);
            break;
        }
        case Icon::FolderPlus:
            folderBack(p);
            p.body();
            p.line(12, 10.2f, 12, 15.8f);
            p.line(9.2f, 13, 14.8f, 13);
            break;
        case Icon::File:
            p.rpath({5, 3, 14, 3, 19, 8, 19, 21, 5, 21}, 2.2f);
            p.body();
            p.rpath({14, 3, 14, 8, 19, 8}, 0.8f);
            p.fill();
            p.stroke();
            p.line(8.5f, 13, 15.5f, 13);
            p.line(8.5f, 16.8f, 13, 16.8f);
            break;
        case Icon::Film:
            p.rrectBody(3, 4.5f, 18, 15, 2.6f);
            p.line(7, 4.5f, 7, 19.5f);
            p.line(17, 4.5f, 17, 19.5f);
            for (float y : {7.6f, 11.2f, 14.8f}) {
                p.rrectFill(4.2f, y, 1.6f, 1.6f, 0.5f);
                p.rrectFill(18.2f, y, 1.6f, 1.6f, 0.5f);
            }
            p.triangle(10.3f, 9.2f, 14.6f, 12, 10.3f, 14.8f, 0.9f);
            p.fill();
            break;
        case Icon::Music:
            // Beamed pair of notes.
            p.rpath({8.6f, 5.6f, 20, 3, 20, 6.8f, 8.6f, 9.4f}, 0.8f);
            p.fill();
            p.line(9.4f, 7, 9.4f, 17.5f);
            p.line(19.2f, 5, 19.2f, 15.5f);
            nvgBeginPath(vg);
            nvgEllipse(vg, 6.8f, 18, 3, 2.5f);
            p.fill();
            nvgBeginPath(vg);
            nvgEllipse(vg, 16.6f, 16, 3, 2.5f);
            p.fill();
            break;
        case Icon::Image:
            p.rrectBody(3, 4, 18, 16, 2.6f);
            p.dot(8.5f, 9, 1.9f);
            p.rpath({4.2f, 18.8f, 4.2f, 17, 9, 12.2f, 12.6f, 15.6f, 15.6f, 12.8f, 19.8f, 16.8f, 19.8f, 18.8f}, 1.1f);
            p.fill();
            break;
        case Icon::Subtitles:
        case Icon::Captions:
            p.rrectBody(2.5f, 5, 19, 14, 3.2f);
            p.line(6.5f, 11.2f, 8.6f, 11.2f);
            p.line(11.4f, 11.2f, 17.5f, 11.2f);
            p.line(6.5f, 15, 12.6f, 15);
            p.line(15.4f, 15, 17.5f, 15);
            break;
        case Icon::Playlist:
            p.line(4, 6, 16, 6);
            p.line(4, 11, 16, 11);
            p.line(4, 16, 10.5f, 16);
            p.triangle(14.2f, 13.6f, 21, 17.4f, 14.2f, 21.2f, 1.1f);
            p.fill();
            break;
        case Icon::Archive:
            p.rrectBody(3.5f, 4, 17, 5, 1.6f);
            p.rpath({5, 9, 19, 9, 19, 20.5f, 5, 20.5f}, 2);
            p.body();
            p.rrectFill(10, 12, 4, 2.6f, 1.2f);
            break;
        case Icon::Waveform: {
            static const float hts[] = {4, 9, 14, 7, 16, 10, 5};
            nvgStrokeWidth(vg, 2.2f);
            for (int i = 0; i < 7; i++) {
                float x = 3.8f + i * 2.73f;
                p.line(x, 12 - hts[i] / 2, x, 12 + hts[i] / 2);
            }
            break;
        }

        // ---- Navigation.
        case Icon::Back:
            p.line(19.5f, 12, 5, 12);
            p.open({11, 5.5f, 4.5f, 12, 11, 18.5f});
            break;
        case Icon::ChevronRight: p.open({9, 5.5f, 15.5f, 12, 9, 18.5f}); break;
        case Icon::ChevronLeft: p.open({15, 5.5f, 8.5f, 12, 15, 18.5f}); break;
        case Icon::ChevronDown: p.open({5.5f, 9, 12, 15.5f, 18.5f, 9}); break;
        case Icon::ChevronUp: p.open({5.5f, 15, 12, 8.5f, 18.5f, 15}); break;
        case Icon::Close:
            nvgStrokeWidth(vg, 2.1f);
            p.line(6.5f, 6.5f, 17.5f, 17.5f);
            p.line(17.5f, 6.5f, 6.5f, 17.5f);
            break;
        case Icon::Check:
            nvgStrokeWidth(vg, 2.3f);
            p.open({4.5f, 12.8f, 9.6f, 17.6f, 19.5f, 6.6f});
            break;
        case Icon::Plus:
            nvgStrokeWidth(vg, 2.1f);
            p.line(12, 5, 12, 19);
            p.line(5, 12, 19, 12);
            break;
        case Icon::Minus:
            nvgStrokeWidth(vg, 2.1f);
            p.line(5, 12, 19, 12);
            break;
        case Icon::Home:
            p.rpath({3.5f, 11.2f, 12, 3.6f, 20.5f, 11.2f, 20.5f, 20.5f, 3.5f, 20.5f}, 2.2f);
            p.body();
            p.rpath({9.8f, 20.5f, 9.8f, 15, 14.2f, 15, 14.2f, 20.5f}, 1.2f);
            p.fill();
            break;
        case Icon::Expand:
            p.open({4, 9.5f, 4, 4, 9.5f, 4});
            p.open({14.5f, 4, 20, 4, 20, 9.5f});
            p.open({20, 14.5f, 20, 20, 14.5f, 20});
            p.open({9.5f, 20, 4, 20, 4, 14.5f});
            p.rrectPath(8.5f, 8.5f, 7, 7, 1.6f);
            p.tone();
            break;
        case Icon::Grid:
            p.rrectFill(4, 4, 7, 7, 2);
            p.rrectBody(13, 4, 7, 7, 2);
            p.rrectBody(4, 13, 7, 7, 2);
            p.rrectBody(13, 13, 7, 7, 2);
            break;
        case Icon::List:
            for (float y : {6.0f, 12.0f, 18.0f}) {
                p.rrectFill(3.4f, y - 1.7f, 3.4f, 3.4f, 1);
                p.line(10, y, 20.5f, y);
            }
            break;
        case Icon::Sort:
            p.line(7.5f, 4.5f, 7.5f, 18.5f);
            p.head(7.5f, 18.5f, deg(90));
            p.line(16.5f, 19.5f, 16.5f, 5.5f);
            p.head(16.5f, 5.5f, deg(-90));
            break;
        case Icon::Search:
            p.circleBody(10.5f, 10.5f, 6.6f);
            nvgStrokeWidth(vg, 2.6f);
            p.line(15.6f, 15.6f, 20.2f, 20.2f);
            break;
        case Icon::Layers:
            p.rpath({12, 3.5f, 21, 8.2f, 12, 12.9f, 3, 8.2f}, 1.4f);
            p.body();
            p.open({3, 12.2f, 12, 16.9f, 21, 12.2f});
            p.open({3, 16.2f, 12, 20.9f, 21, 16.2f});
            break;
        case Icon::Sliders:
            p.line(4, 7, 20, 7);
            p.line(4, 12, 20, 12);
            p.line(4, 17, 20, 17);
            for (auto k : {std::pair<float, float>{9, 7}, {15.5f, 12}, {7.5f, 17}}) {
                p.circleBody(k.first, k.second, 2.4f);
                p.dot(k.first, k.second, 1.0f);
            }
            break;
        case Icon::Link:
            nvgSave(vg);
            nvgTranslate(vg, 12, 12);
            nvgRotate(vg, deg(-45));
            p.rrectBody(-10, -3.6f, 10.6f, 7.2f, 3.6f);
            p.rrect(-0.6f, -3.6f, 10.6f, 7.2f, 3.6f);
            nvgRestore(vg);
            break;

        // ---- File actions.
        case Icon::Copy:
            p.rpath({15.5f, 8.5f, 15.5f, 3.5f, 3.5f, 3.5f, 3.5f, 15.5f, 8.5f, 15.5f}, 2);
            p.stroke();
            p.rrectBody(8.5f, 8.5f, 12, 12, 2.2f);
            break;
        case Icon::Cut:
            p.line(8.6f, 15.4f, 18.6f, 3.5f);
            p.line(15.4f, 15.4f, 5.4f, 3.5f);
            p.circleBody(6.6f, 18, 2.9f);
            p.circleBody(17.4f, 18, 2.9f);
            p.dot(12, 11.6f, 1.1f);
            break;
        case Icon::Paste:
            p.rrectBody(5, 5, 14, 16, 2.4f);
            p.rrectFill(8.6f, 3, 6.8f, 4.4f, 1.5f);
            p.line(8.8f, 12, 15.2f, 12);
            p.line(8.8f, 16, 13, 16);
            break;
        case Icon::Rename:
            p.rpath({4, 20, 4.8f, 15.8f, 15.4f, 5.2f, 18.8f, 8.6f, 8.2f, 19.2f}, 0.9f);
            p.body();
            p.line(13.4f, 7.2f, 16.8f, 10.6f);
            p.rpath({15.4f, 5.2f, 17, 3.6f, 20.4f, 7, 18.8f, 8.6f}, 0.8f);
            p.fill();
            break;
        case Icon::Trash:
            p.line(3.8f, 6.5f, 20.2f, 6.5f);
            p.rpath({9, 6.5f, 9, 3.6f, 15, 3.6f, 15, 6.5f}, 1);
            p.stroke();
            p.rpath({5.8f, 6.5f, 18.2f, 6.5f, 17, 20.4f, 7, 20.4f}, 1.8f);
            p.body();
            p.line(10, 10.5f, 10, 16.4f);
            p.line(14, 10.5f, 14, 16.4f);
            break;
        case Icon::Bookmark:
            p.rpath({6, 3.5f, 18, 3.5f, 18, 20.5f, 12, 16.2f, 6, 20.5f}, 1.8f);
            p.body();
            break;

        // ---- Settings and status.
        case Icon::Gear: {
            std::vector<float> pts;
            const int teeth = 8;
            for (int i = 0; i < teeth; i++) {
                float a0 = (i / (float)teeth) * 2 * PI;
                float w = 2 * PI / teeth;
                float outer = 10, inner = 7.6f;
                for (auto k : {std::pair<float, float>{-0.30f, inner}, {-0.16f, outer}, {0.16f, outer}, {0.30f, inner}}) {
                    float a = a0 + k.first * w * 1.6f;
                    pts.push_back(12 + std::cos(a) * k.second);
                    pts.push_back(12 + std::sin(a) * k.second);
                }
            }
            p.rpath(pts, 0.9f);
            p.body();
            p.circle(12, 12, 3.2f);
            break;
        }
        case Icon::Lock:
        case Icon::Unlock:
            nvgBeginPath(vg);
            nvgMoveTo(vg, 8, 10.5f);
            nvgLineTo(vg, 8, 7.5f);
            if (ic == Icon::Lock) {
                nvgArc(vg, 12, 7.5f, 4, PI, 0, NVG_CW);
                nvgLineTo(vg, 16, 10.5f);
            } else {
                nvgArc(vg, 12, 7.5f, 4, PI, deg(-25), NVG_CW);
            }
            p.stroke();
            p.rrectBody(4.5f, 10.5f, 15, 10.5f, 2.8f);
            p.dot(12, 15, 1.6f);
            p.line(12, 15.5f, 12, 17.6f);
            break;
        case Icon::Speed: {
            // Gauge with a tinted sweep up to the needle.
            nvgBeginPath(vg);
            nvgMoveTo(vg, 12, 14.5f);
            nvgArc(vg, 12, 14.5f, 8.8f, deg(160), deg(305), NVG_CW);
            nvgClosePath(vg);
            p.tone();
            p.arc(12, 14.5f, 8.8f, deg(160), deg(20));
            nvgStrokeWidth(vg, 2.2f);
            p.line(12, 14.5f, 16.6f, 9);
            p.dot(12, 14.5f, 2);
            break;
        }
        case Icon::Volume:
        case Icon::VolumeMute:
            p.rpath({3.5f, 9, 7.5f, 9, 12.5f, 4.5f, 12.5f, 19.5f, 7.5f, 15, 3.5f, 15}, 1.2f);
            p.fill();
            if (ic == Icon::Volume) {
                p.arc(13, 12, 3.8f, deg(-48), deg(48));
                p.arc(13, 12, 7.4f, deg(-50), deg(50));
            } else {
                nvgStrokeWidth(vg, 2.1f);
                p.line(16, 9.2f, 21, 14.8f);
                p.line(21, 9.2f, 16, 14.8f);
            }
            break;
        case Icon::Sun:
            p.circleBody(12, 12, 4.3f);
            for (int i = 0; i < 8; i++) {
                float a = i * PI / 4;
                p.line(12 + std::cos(a) * 7.3f, 12 + std::sin(a) * 7.3f, 12 + std::cos(a) * 9.6f,
                       12 + std::sin(a) * 9.6f);
            }
            break;
        case Icon::Moon:
            nvgBeginPath(vg);
            nvgMoveTo(vg, 12.5f, 3.2f);
            nvgArc(vg, 16.5f, 8, 6.2f, deg(-140), deg(48), NVG_CCW);
            nvgArc(vg, 12, 12, 8.8f, deg(4), deg(-84), NVG_CW);
            nvgClosePath(vg);
            p.body();
            p.dot(18.6f, 4.6f, 1);
            break;
        case Icon::Clock:
            p.circleBody(12, 12, 9);
            p.open({12, 7, 12, 12, 15.6f, 14.2f});
            p.dot(12, 12, 1.2f);
            break;
        case Icon::Info:
            p.circleBody(12, 12, 9);
            nvgStrokeWidth(vg, 2.2f);
            p.line(12, 11, 12, 16.5f);
            p.dot(12, 7.6f, 1.3f);
            break;
        case Icon::Power:
            p.arc(12, 13, 8, deg(-52), deg(232));
            nvgStrokeWidth(vg, 2.2f);
            p.line(12, 3, 12, 11.2f);
            break;
        case Icon::Refresh:
            p.arcArrow(12, 12, 8, deg(-20), deg(285));
            break;
        case Icon::Rotate:
            p.arcArrow(12, 12.5f, 8, deg(0), deg(265));
            p.rrectPath(9.5f, 10, 5, 5, 1.2f);
            p.tone();
            break;
        case Icon::Repeat:
            nvgBeginPath(vg);
            nvgMoveTo(vg, 3.5f, 12.5f);
            nvgLineTo(vg, 3.5f, 10.5f);
            nvgArcTo(vg, 3.5f, 6.5f, 7.5f, 6.5f, 4);
            nvgLineTo(vg, 20, 6.5f);
            p.stroke();
            p.head(20, 6.5f, 0);
            nvgBeginPath(vg);
            nvgMoveTo(vg, 20.5f, 11.5f);
            nvgLineTo(vg, 20.5f, 13.5f);
            nvgArcTo(vg, 20.5f, 17.5f, 16.5f, 17.5f, 4);
            nvgLineTo(vg, 4, 17.5f);
            p.stroke();
            p.head(4, 17.5f, PI);
            break;
        case Icon::Shuffle:
            nvgBeginPath(vg);
            nvgMoveTo(vg, 3, 7);
            nvgLineTo(vg, 6.5f, 7);
            nvgBezierTo(vg, 11.5f, 7, 12.5f, 17, 17.5f, 17);
            nvgLineTo(vg, 20.5f, 17);
            nvgMoveTo(vg, 3, 17);
            nvgLineTo(vg, 6.5f, 17);
            nvgBezierTo(vg, 8.4f, 17, 9.6f, 15.6f, 10.5f, 13.9f);
            nvgMoveTo(vg, 13.5f, 10.1f);
            nvgBezierTo(vg, 14.4f, 8.4f, 15.6f, 7, 17.5f, 7);
            nvgLineTo(vg, 20.5f, 7);
            p.stroke();
            p.head(20.5f, 7, 0);
            p.head(20.5f, 17, 0);
            break;
        case Icon::Heart:
        case Icon::HeartFilled:
            nvgBeginPath(vg);
            nvgMoveTo(vg, 12, 20.2f);
            nvgBezierTo(vg, 5, 15.2f, 3, 11.6f, 3, 8.6f);
            nvgBezierTo(vg, 3, 5.6f, 5.3f, 3.8f, 7.6f, 3.8f);
            nvgBezierTo(vg, 9.3f, 3.8f, 11, 4.8f, 12, 6.7f);
            nvgBezierTo(vg, 13, 4.8f, 14.7f, 3.8f, 16.4f, 3.8f);
            nvgBezierTo(vg, 18.7f, 3.8f, 21, 5.6f, 21, 8.6f);
            nvgBezierTo(vg, 21, 11.6f, 19, 15.2f, 12, 20.2f);
            nvgClosePath(vg);
            if (ic == Icon::HeartFilled) p.fill();
            else p.tone();
            p.stroke();
            break;
        case Icon::Star:
        case Icon::StarFilled: {
            std::vector<float> pts;
            for (int i = 0; i < 10; i++) {
                float a = -PI / 2 + i * PI / 5;
                float r = (i % 2) ? 4.4f : 9.6f;
                pts.push_back(12 + std::cos(a) * r);
                pts.push_back(12.9f + std::sin(a) * r);
            }
            p.rpath(pts, 1.1f);
            if (ic == Icon::StarFilled) p.fill();
            else p.tone();
            p.stroke();
            break;
        }
        case Icon::Globe:
            p.circleBody(12, 12, 9);
            nvgBeginPath(vg);
            nvgEllipse(vg, 12, 12, 3.9f, 9);
            p.stroke();
            p.line(3, 12, 21, 12);
            p.arc(12, 23, 13.5f, deg(-118), deg(-62));
            break;
        case Icon::Palette:
            nvgBeginPath(vg);
            nvgMoveTo(vg, 12, 3);
            nvgArc(vg, 12, 12, 9, deg(-90), deg(200), NVG_CW);
            nvgQuadTo(vg, 5, 11, 9, 12.5f);
            nvgQuadTo(vg, 12, 13.5f, 11, 17);
            nvgQuadTo(vg, 10, 21, 12, 21);
            nvgClosePath(vg);
            p.body();
            p.dot(7.8f, 9, 1.5f);
            p.dot(12.4f, 6.8f, 1.5f);
            p.dot(16.6f, 9.4f, 1.5f);
            p.dot(16.4f, 14.6f, 1.5f);
            break;
        case Icon::Gesture:
            // A tap: a fingertip with ripples around it.
            p.circleBody(12, 12, 5.2f);
            p.dot(12, 12, 2.2f);
            for (int i = 0; i < 4; i++) {
                float a = deg(45 + i * 90);
                p.arc(12, 12, 9.2f, a - deg(26), a + deg(26));
            }
            break;
        case Icon::Gamepad:
            p.rpath({6.5f, 6.5f, 17.5f, 6.5f, 22, 18.5f, 18, 19.5f, 15, 15.5f, 9, 15.5f, 6, 19.5f, 2, 18.5f}, 3.2f);
            p.body();
            p.rrectFill(6.1f, 10.1f, 4.8f, 1.8f, 0.6f);
            p.rrectFill(7.6f, 8.6f, 1.8f, 4.8f, 0.6f);
            p.dot(15.6f, 9.6f, 1.2f);
            p.dot(17.8f, 12, 1.2f);
            break;
        case Icon::Chip:
            for (float v : {9.5f, 14.5f}) {
                p.line(v, 2.5f, v, 6);
                p.line(v, 18, v, 21.5f);
                p.line(2.5f, v, 6, v);
                p.line(18, v, 21.5f, v);
            }
            p.rrectBody(6, 6, 12, 12, 2.4f);
            p.rrectFill(9.4f, 9.4f, 5.2f, 5.2f, 1.2f);
            break;
        case Icon::Camera:
            p.rpath({3, 7.5f, 7.4f, 7.5f, 9, 4.8f, 15, 4.8f, 16.6f, 7.5f, 21, 7.5f, 21, 19.5f, 3, 19.5f}, 2.2f);
            p.body();
            p.circle(12, 13.3f, 3.8f);
            p.dot(12, 13.3f, 1.6f);
            p.dot(17.6f, 10.2f, 0.9f);
            break;
        case Icon::Eye:
        case Icon::EyeOff:
            nvgBeginPath(vg);
            nvgMoveTo(vg, 2.5f, 12);
            nvgBezierTo(vg, 6, 5.5f, 18, 5.5f, 21.5f, 12);
            nvgBezierTo(vg, 18, 18.5f, 6, 18.5f, 2.5f, 12);
            nvgClosePath(vg);
            p.body();
            p.dot(12, 12, 3.2f);
            if (ic == Icon::EyeOff) {
                // Gap behind the slash so it reads at small sizes.
                nvgStrokeWidth(vg, 2.2f);
                p.line(4, 4, 20, 20);
            }
            break;

        case Icon::Wifi:
            nvgStrokeWidth(vg, 2.1f);
            for (float r : {13.5f, 9.0f, 4.6f}) p.arc(12, 19, r, deg(-138), deg(-42));
            p.dot(12, 19, 1.8f);
            break;

        case Icon::Mail:
            p.rrectBody(3, 5.5f, 18, 13, 2.6f);
            p.open({3.8f, 7.2f, 12, 13, 20.2f, 7.2f});
            break;
        case Icon::Coin:
            p.circleBody(12, 12, 9);
            p.circle(12, 12, 5.6f);
            p.line(12, 9.4f, 12, 14.6f);
            break;
        case Icon::Download:
            // An arrow down into a tray.
            p.line(12, 3.5f, 12, 14.5f);
            p.open({7.5f, 10.5f, 12, 15, 16.5f, 10.5f});
            p.open({4, 15.5f, 4, 19.5f, 20, 19.5f, 20, 15.5f});
            break;
        case Icon::Language:
            // A speech bubble with "A" and a second script mark.
            p.rpath({3, 4, 21, 4, 21, 16, 10, 16, 6, 20, 6, 16, 3, 16}, 2.5f);
            p.body();
            p.open({7, 13, 9.5f, 7, 12, 13});
            p.line(7.9f, 11, 11.1f, 11);
            p.line(14, 8, 18, 8);
            p.open({16, 8, 16, 9, 14.6f, 12.6f});
            p.line(15.2f, 10.6f, 18, 12.8f);
            break;
        case Icon::Book:
            // An open book: two pages meeting at the spine.
            p.rpath({12, 6.5f, 9.5f, 4.8f, 3, 4.8f, 3, 18.2f, 9.5f, 18.2f, 12, 20}, 1.6f);
            p.body();
            p.rpath({12, 6.5f, 14.5f, 4.8f, 21, 4.8f, 21, 18.2f, 14.5f, 18.2f, 12, 20}, 1.6f);
            p.body();
            p.line(12, 6.5f, 12, 20);
            break;

        // ---- Storage.
        case Icon::SdCard:
            p.rpath({8, 3, 19, 3, 19, 21, 5, 21, 5, 6}, 2);
            p.body();
            for (float x : {9.0f, 12.0f, 15.0f}) p.rrectFill(x - 0.9f, 6, 1.8f, 3.8f, 0.8f);
            break;
        case Icon::Server:
            // A two-unit server (a NAS): status lights on the left, drive slots on the right.
            p.rrectBody(3.5f, 3.5f, 17, 7.5f, 2.2f);
            p.rrectBody(3.5f, 13, 17, 7.5f, 2.2f);
            p.dot(7.2f, 7.25f, 1.1f);
            p.dot(7.2f, 16.75f, 1.1f);
            p.line(11.5f, 7.25f, 17, 7.25f);
            p.line(11.5f, 16.75f, 17, 16.75f);
            break;
        case Icon::Drive:
            // A USB stick.
            p.rrect(8.5f, 2.5f, 7, 5.5f, 1.2f);
            p.rrectFill(10, 4.3f, 1.4f, 1.4f, 0.3f);
            p.rrectFill(12.6f, 4.3f, 1.4f, 1.4f, 0.3f);
            p.rrectBody(6, 8, 12, 13.5f, 2.6f);
            p.dot(12, 17.2f, 1.2f);
            break;
    }
    nvgRestore(vg);
}

// The SwitchMX mark: three open rings turning around a bright core (a lens, a disc, sound going
// out), in the app's cyan, violet and pink.
void drawRings(NVGcontext* vg, float cx, float cy, float R, float time, float reveal, bool glow) {
    struct Ring { float r, w, sweep, start, speed; NVGcolor a, b; };
    const Ring rings[3] = {
        {1.00f, 0.135f, 5.15f, -0.95f, 0.22f, nvgRGB(0x3B, 0xE0, 0xFF), nvgRGB(0x8B, 0x6B, 0xFF)},
        {0.70f, 0.135f, 4.25f, 1.75f, -0.38f, nvgRGB(0x8B, 0x6B, 0xFF), nvgRGB(0xFF, 0x5F, 0xC8)},
        {0.42f, 0.135f, 3.30f, 3.90f, 0.61f, nvgRGB(0xFF, 0x5F, 0xC8), nvgRGB(0xFF, 0xB4, 0x8A)},
    };
    auto ease = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return 1 - (1 - x) * (1 - x) * (1 - x);
    };
    nvgSave(vg);
    nvgLineCap(vg, NVG_ROUND);
    for (int i = 0; i < 3; i++) {
        const Ring& g = rings[i];
        float k = ease(reveal * 1.6f - i * 0.3f);  // the rings draw themselves one after another
        if (k <= 0.001f) continue;
        float rr = R * g.r, w = std::max(1.0f, R * g.w);
        float a0 = g.start + (time >= 0 ? time * g.speed : 0) - (1 - k) * 1.2f, a1 = a0 + g.sweep * k;
        NVGpaint paint = nvgLinearGradient(vg, cx + std::cos(a0) * rr, cy + std::sin(a0) * rr, cx + std::cos(a1) * rr,
                                           cy + std::sin(a1) * rr, g.a, g.b);
        if (glow) {  // a soft halo under each ring
            NVGcolor ga = g.a, gb = g.b;
            ga.a = gb.a = 0.22f * k;
            nvgBeginPath(vg);
            nvgArc(vg, cx, cy, rr, a0, a1, NVG_CW);
            nvgStrokeWidth(vg, w * 2.8f);
            nvgStrokePaint(vg, nvgLinearGradient(vg, cx + std::cos(a0) * rr, cy + std::sin(a0) * rr,
                                                 cx + std::cos(a1) * rr, cy + std::sin(a1) * rr, ga, gb));
            nvgStroke(vg);
        }
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, rr, a0, a1, NVG_CW);
        nvgStrokeWidth(vg, w);
        nvgStrokePaint(vg, paint);
        nvgStroke(vg);
    }
    // The core: springs in once the rings are drawn, and breathes a little while animated.
    float c = ease(reveal * 2.2f - 1.0f);
    if (c > 0.001f) {
        float pulse = time >= 0 ? 1 + 0.06f * std::sin(time * 2.4f) : 1;
        float cr = R * 0.17f * c * pulse;
        if (glow) {
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy, cr * 3.2f);
            nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, cr * 0.6f, cr * 3.2f, nvgRGBAf(0.75f, 0.85f, 1, 0.45f * c),
                                               nvgRGBAf(0.55f, 0.45f, 1, 0)));
            nvgFill(vg);
        }
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, cr);
        nvgFillPaint(vg, nvgRadialGradient(vg, cx - cr * 0.3f, cy - cr * 0.3f, 0, cr * 1.3f, nvgRGB(255, 255, 255),
                                           nvgRGB(0x9F, 0xE8, 0xFF)));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

// The mark on its deep glass tile. `rounded` is false for the HOME Menu icon, which the console
// crops itself. With `time` >= 0 the rings turn.
void drawLogo(NVGcontext* vg, float x, float y, float s, bool rounded, float time, float ringsY, float ringsR) {
    nvgSave(vg);
    nvgTranslate(vg, x, y);
    float r = rounded ? s * 0.27f : 0;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 0, 0, s, s, r);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, s, s, nvgRGB(0x1C, 0x15, 0x4A), nvgRGB(0x07, 0x06, 0x14)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 0, 0, s, s, r);
    nvgFillPaint(vg, nvgRadialGradient(vg, s * 0.72f, s * 0.78f, 0, s * 0.6f, nvgRGBA(0xFF, 0x5F, 0xC8, 60),
                                       nvgRGBA(0xFF, 0x5F, 0xC8, 0)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 0, 0, s, s, r);
    nvgFillPaint(vg, nvgRadialGradient(vg, s * 0.22f, s * 0.18f, 0, s * 0.6f, nvgRGBA(0x22, 0xD3, 0xEE, 70),
                                       nvgRGBA(0x22, 0xD3, 0xEE, 0)));
    nvgFill(vg);
    if (rounded) nvgScissor(vg, 0, 0, s, s);
    drawRings(vg, s / 2, s * ringsY, s * ringsR, time, 1, s >= 40);
    nvgResetScissor(vg);
    // Glass sheen and a hairline edge.
    nvgBeginPath(vg);
    nvgRoundedRect(vg, 0, 0, s, s, r);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, s * 0.55f, nvgRGBA(255, 255, 255, 22), nvgRGBA(255, 255, 255, 0)));
    nvgFill(vg);
    if (rounded) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, s * 0.006f, s * 0.006f, s * 0.988f, s * 0.988f, r);
        nvgStrokeWidth(vg, std::max(1.0f, s * 0.012f));
        nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 34));
        nvgStroke(vg);
    }
    nvgRestore(vg);
}
