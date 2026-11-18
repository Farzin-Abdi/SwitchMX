#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "input.hpp"
#include "nanovg.h"

class Config;

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
};

enum class Icon {
    None, Play, Pause, Next, Prev, SeekBack, SeekForward, Folder, File, Film, Music, Subtitles, Gear, Back,
    ChevronRight, ChevronLeft, ChevronDown, ChevronUp, Lock, Unlock, Speed, Volume, VolumeMute, Sun, Clock,
    Star, StarFilled, Globe, SdCard, Grid, List, Sort, Close, Check, Plus, Minus, Playlist, Bookmark, Camera,
    Repeat, Moon, Info, Trash, Expand, Sliders, Palette, Gesture, Gamepad, Chip, Home, Eye, EyeOff, Refresh,
    Image, Link, Rotate, Layers, Captions, Waveform, Power, Drive, Archive, FolderPlus, Search, Copy, Cut, Paste, Rename,
    Heart, HeartFilled, Shuffle, Wifi, Mail, Coin, Download, Language, Book, Server,
};

struct Theme {
    NVGcolor bg, bg2, surface, surface2, surfaceHi, text, textDim, textFaint, accent, accent2, onAccent, border,
        danger, success, warning, scrim;
    bool light = false;
    float radius = 14;
};

NVGcolor withAlpha(NVGcolor c, float a);
NVGcolor mix(NVGcolor a, NVGcolor b, float t);
NVGcolor hex(uint32_t rgb, float a = 1);
NVGcolor accentColor(int idx);
int accentCount();

uint32_t uid(const char* s, uint32_t salt = 0);
inline uint32_t uid(const char* s, int idx) { return uid(s, (uint32_t)idx * 2654435761u + 1); }

enum Align { LEFT = NVG_ALIGN_LEFT, CENTER = NVG_ALIGN_CENTER, RIGHT = NVG_ALIGN_RIGHT };

class UI;

// Kinetic scroll container with rubber-banding and focus following.
struct ScrollView {
    uint32_t id = 0;
    float offset = 0;
    float velocity = 0;
    float contentH = 0;
    Rect view;
    float idleTime = 10;
    bool dragging = false;
    bool swallowTap = false;
    float target = -1;  // smooth scroll target (-1 = none)

    void update(UI& ui, Rect view, float contentH);
    void ensureVisible(float y, float h, float margin = 24);
    // Opacity for a row at screen position y, so rows fade instead of being cut at the edges.
    float edgeFade(float y, float h) const;
    float maxOffset() const { return contentH > view.h ? contentH - view.h : 0; }
    void begin(UI& ui);  // scissor + translate
    void end(UI& ui);
    void drawScrollbar(UI& ui);
    void reset() { offset = velocity = 0; target = -1; }
};

class UI {
public:
    NVGcontext* vg = nullptr;
    // The UI font in use (Settings -> Appearance -> Font); -1 until App has created them.
    int fontId = -1, fontBoldId = -1;
    Input* in = nullptr;
    Theme t;
    float W = 1280, H = 720;
    float dt = 0, time = 0;
    float animMul = 1;
    int swapAB = 0;

    // Nested opacity; nanovg has no getter for the current global alpha.
    void pushAlpha(float a);
    void popAlpha();

    void init(NVGcontext* vg, Input* in);
    void setFont(bool bold);
    void applyTheme(const Config& cfg);
    void beginFrame(float dt, float W, float H);
    void endFrame();

    // ---- Input helpers ----
    bool confirm();       // A (or B when swapped); consumes
    bool back();          // B (or A when swapped); consumes
    bool peekConfirm() const;
    bool peekBack() const;
    bool blocked = false;  // modal layer above: widgets ignore input
    void consumeNav(uint32_t dirs) { navConsumed_ |= dirs; }
    bool tapped(Rect r);   // consumes the tap if inside
    bool pressedVisual(Rect r) const;
    // A long press inside r (and inside the visible part of any scroll view it's drawn in).
    bool longPressIn(Rect r) const;
    // Scroll views limit touches to what they show, so a row half hidden under the hint bar
    // can't take a tap meant for the bar.
    std::vector<Rect> touchClip;
    // Right-to-left screens are drawn mirrored (see App::run); text, pictures and codes are turned
    // back with these so they read normally.
    bool mirror = false;
    bool keepDigits = false;  // draw digits as they are (a PIN typed into a browser)
    std::string shown(const std::string& s) const;
    void beginUnmirrored(Rect r);
    void endUnmirrored();
    bool inTouchClip(float x, float y) const {
        return touchClip.empty() || touchClip.back().contains(x, y);
    }

    // ---- Focus (spatial navigation) ----
    void focusable(uint32_t id, Rect r, uint32_t group = 0);
    bool focused(uint32_t id) const { return focus == id && !blocked; }
    void setFocus(uint32_t id) {
        focus = id;
        focusMoved = navMoved_ = true;
    }
    void setDefaultFocus(uint32_t id) { defaultFocus_ = id; }
    bool showFocus() const { return in->usingButtons; }
    uint32_t focus = 0;
    bool focusMoved = false;  // focus changed via navigation this frame
    bool navEnabled = true;

    // ---- Animation ----
    float smooth(uint32_t id, float target, float rate = 14);
    float& value(uint32_t id, float init = 0);
    float ease(float x) const;  // easeOutCubic

    // ---- Drawing ----
    void fillRect(Rect r, NVGcolor c, float radius = -1);
    void strokeRect(Rect r, NVGcolor c, float width, float radius = -1);
    void gradientRect(Rect r, NVGcolor top, NVGcolor bottom, float radius = 0);
    void shadow(Rect r, float radius, float blur, float alpha, float dy = 6);
    void panel(Rect r, float alpha = 1, float radius = -1);  // floating card with shadow
    void glass(Rect r, float radius = -1, float opacity = 0.78f);
    void focusRing(Rect r, float radius, float amount);
    void text(float x, float y, const std::string& s, float size, NVGcolor c, int align = LEFT, bool bold = false);
    void textBox(float x, float y, float w, const std::string& s, float size, NVGcolor c, bool bold = false);
    float textWidth(const std::string& s, float size, bool bold = false);
    std::string ellipsize(const std::string& s, float maxW, float size, bool bold = false);
    // Text that fits is drawn as usual; longer text glides to its end and back, pausing at each
    // end, so the whole name can be read. `id` keeps its place from frame to frame.
    void marquee(uint32_t id, float x, float y, float maxW, const std::string& s, float size, NVGcolor c,
                 int align = LEFT, bool bold = false);
    void icon(Icon ic, float cx, float cy, float size, NVGcolor c);
    // Artwork for music without a cover: colours from `seed` (the album), the initial of `label`.
    void generatedArt(Rect r, const std::string& seed, const std::string& label, float radius, bool circle = false);
    NVGcolor seedColor(const std::string& seed) const;
    void spinner(float cx, float cy, float r, NVGcolor c, float width = 3);
    void glyph(uint32_t btn, float cx, float cy, float size);  // controller button badge
    float hint(float x, float cy, uint32_t btn, const char* label);  // returns width
    // onDark: drawn over video or the music player, which stay dark in every theme.
    void battery(float x, float cy, int pct, bool charging, bool onDark = false);
    void background(int style);
    void progressRing(float cx, float cy, float r, float p, NVGcolor c, float width = 3);

    // ---- Widgets ----
    enum ButtonStyle { BTN_FILLED, BTN_TONAL, BTN_GHOST, BTN_DANGER };
    bool button(uint32_t id, Rect r, const char* label, Icon ic = Icon::None, ButtonStyle st = BTN_TONAL,
                uint32_t group = 0);
    bool iconButton(uint32_t id, float cx, float cy, float size, Icon ic, NVGcolor c, uint32_t group = 0,
                    bool background = true);
    bool toggle(uint32_t id, Rect r, bool on);  // draws a switch knob within r
    void toggleKnob(Rect r, bool on, uint32_t id);
    // Horizontal slider, returns true while value changes.
    bool slider(uint32_t id, Rect r, float& v, float lo, float hi, float step, bool focusedRow);
    void toast(const std::string& msg, Icon ic = Icon::Info);
    void drawToasts();

private:
    std::vector<bool> unmirrorDepth_;
    struct FocusItem {
        uint32_t id;
        Rect r;
        uint32_t group;
    };
    std::vector<FocusItem> items_;
    std::unordered_map<uint32_t, uint32_t> groupMemory_;
    std::unordered_map<uint32_t, float> anims_;
    std::unordered_map<std::string, std::string> ellipsized_;
    struct Marquee {
        size_t hash = 0;
        float t = 0, lastSeen = 0;
    };
    std::unordered_map<uint32_t, Marquee> marquees_;
    uint32_t defaultFocus_ = 0;
    uint32_t navConsumed_ = 0;
    bool navMoved_ = false;  // focus moved at the end of the last frame
    Rect lastFocusRect_;
    bool haveFocusRect_ = false;
    bool confirmUsed_ = false, backUsed_ = false;
    int bg_ = 0;
    struct Toast {
        std::string msg;
        Icon ic;
        float age;
    };
    std::vector<Toast> toasts_;
    uint32_t sliderDrag_ = 0;
    std::vector<float> alphaStack_;
    float alpha_ = 1;
};

void drawIcon(NVGcontext* vg, Icon ic, float cx, float cy, float size, NVGcolor c);
// The app logo in a size x size square at (x, y); `rounded` false fills the square edge to edge.
// `bars` scales the three equaliser bars (the splash animates them).
// ringsY / ringsR: where the rings sit and how big, as fractions of the size (the HOME Menu icon
// raises them to make room for the name).
void drawLogo(NVGcontext* vg, float x, float y, float size, bool rounded = true, float time = -1, float ringsY = 0.5f,
              float ringsR = 0.335f);
// Just the rings and core, centred on (cx, cy) with outer radius R; reveal 0..1 draws them in.
void drawRings(NVGcontext* vg, float cx, float cy, float R, float time = -1, float reveal = 1, bool glow = true);
