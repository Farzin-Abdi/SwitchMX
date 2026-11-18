#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "bidi.hpp"
#include "i18n.hpp"
#include "config.hpp"

namespace {
constexpr float PI = 3.14159265f;
constexpr float kCapCentre = 0.355f;  // half of Vazirmatn's cap height, in ems

struct ThemeDef {
    uint32_t bg, bg2, surface, surface2, surfaceHi, text, textDim, textFaint;
    bool light;
};

const ThemeDef kThemes[] = {
    // Midnight
    {0x0A0D14, 0x121826, 0x151B27, 0x1C2333, 0x262F43, 0xF3F5FA, 0xA2ABBE, 0x6B7489, false},
    // AMOLED Black
    {0x000000, 0x07070A, 0x0E0E12, 0x17171C, 0x232329, 0xF5F5F7, 0x9C9CA6, 0x62626C, false},
    // Graphite
    {0x15161A, 0x1D1F25, 0x222429, 0x2A2D33, 0x363940, 0xEDEEF0, 0xA4A7AE, 0x6E7178, false},
    // Nord Frost
    {0x242933, 0x2E3440, 0x323946, 0x3B4252, 0x4C566A, 0xECEFF4, 0xBAC3D3, 0x7F8AA0, false},
    // Mocha
    {0x161622, 0x1E1E2E, 0x232334, 0x2D2D42, 0x3A3A52, 0xCDD6F4, 0xA6ADC8, 0x6C7086, false},
    // Deep Ocean
    {0x041420, 0x072033, 0x0A2538, 0x0E3048, 0x16405E, 0xE6F4FF, 0x93B4CC, 0x5D7E96, false},
    // Daylight
    {0xEEF0F5, 0xE2E6EE, 0xFFFFFF, 0xF3F5F9, 0xE6E9F0, 0x12151C, 0x535B6B, 0x8C94A3, true},
};

const uint32_t kAccents[] = {0xFF3D5E, 0xFF7A3D, 0xFFB627, 0x9BDB4D, 0x1FCF8B, 0x18C5B4, 0x2EB8FF,
                             0x3B82F6, 0x6366F1, 0x9B5CF6, 0xE356D8, 0xFF1F44, 0x00B4FF};
}  // namespace

NVGcolor withAlpha(NVGcolor c, float a) {
    c.a *= a;
    return c;
}

NVGcolor mix(NVGcolor a, NVGcolor b, float t) { return nvgLerpRGBA(a, b, t); }

NVGcolor hex(uint32_t rgb, float a) {
    return nvgRGBAf(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

NVGcolor accentColor(int idx) { return hex(kAccents[std::clamp(idx, 0, accentCount() - 1)]); }
int accentCount() { return (int)(sizeof(kAccents) / sizeof(kAccents[0])); }

uint32_t uid(const char* s, uint32_t salt) {
    uint32_t h = 2166136261u ^ salt;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h ? h : 1;
}

// ------------------------------------------------------------------ ScrollView

void ScrollView::update(UI& ui, Rect v, float ch) {
    view = v;
    contentH = ch;
    Input& in = *ui.in;
    float maxO = maxOffset();
    idleTime += ui.dt;

    if (!ui.blocked) {
        // A touch on a gliding list stops it; that tap must not open what's under the finger.
        if (in.touchBegan && view.contains(in.touchX, in.touchY)) {
            swallowTap = std::fabs(velocity) > 120 || target >= 0;
            velocity = 0;
        }
        if (swallowTap && in.tap) {
            if (view.contains(in.tapX, in.tapY)) in.consumeTap();
            swallowTap = false;
        }
        if (!in.touching && !in.tap) swallowTap = false;
        if (in.pan.began && in.pan.owner == 0 && view.contains(in.pan.startX, in.pan.startY) &&
            std::fabs(in.pan.dy) >= std::fabs(in.pan.dx) * 0.7f) {
            in.pan.owner = id;
            dragging = true;
            target = -1;
        }
        if (dragging && in.pan.owner == id) {
            float d = in.pan.dy;
            // Rubber-band resistance beyond the edges.
            if (offset < 0 || offset > maxO) d *= 0.4f;
            offset -= d;
            velocity = -in.pan.vy;
            idleTime = 0;
        }
        if (dragging && (!in.pan.active || in.pan.owner != id)) dragging = false;
        // Right stick scrolls freely, faster the further it is pushed.
        if (std::fabs(in.ry) > 0.2f) {
            float speed = (std::fabs(in.ry) - 0.2f) / 0.8f;
            offset = std::clamp(offset - (in.ry > 0 ? 1 : -1) * speed * speed * 1600 * ui.dt, 0.0f, maxO);
            target = -1;
            velocity = 0;
            idleTime = 0;
        }
        if (in.wheel != 0 && view.contains(in.touchX, in.touchY)) {
            target = std::clamp((target < 0 ? offset : target) - in.wheel * 90, 0.0f, maxO);
            velocity = 0;
            idleTime = 0;
        }
    }

    if (!dragging) {
        if (target >= 0) {
            float a = 1 - std::exp(-ui.dt * 16);
            offset += (target - offset) * a;
            if (std::fabs(target - offset) < 0.5f) {
                offset = target;
                target = -1;
            }
            velocity = 0;
        } else if (offset < 0 || offset > maxO) {
            float edge = offset < 0 ? 0 : maxO;
            offset += (edge - offset) * (1 - std::exp(-ui.dt * 14));
            if (std::fabs(edge - offset) < 0.5f) offset = edge;
            velocity = 0;
        } else if (std::fabs(velocity) > 5) {
            offset += velocity * ui.dt;
            velocity *= std::exp(-ui.dt * 3.2f);
            idleTime = 0;
            if (offset < 0 || offset > maxO) velocity *= 0.5f;
        } else {
            velocity = 0;
        }
    }
}

void ScrollView::ensureVisible(float y, float h, float margin) {
    float maxO = maxOffset();
    float cur = target >= 0 ? target : offset;
    float t = cur;
    if (y - margin < cur) t = y - margin;
    else if (y + h + margin > cur + view.h) t = y + h + margin - view.h;
    t = std::clamp(t, 0.0f, maxO);
    if (std::fabs(t - cur) > 0.5f) {
        target = t;
        idleTime = 0;
    }
}

float ScrollView::edgeFade(float y, float h) const {
    const float range = std::min(h * 0.6f, 44.0f);
    float top = (y + h - view.y) / range;
    float bottom = (view.bottom() - y) / range;
    return std::clamp(std::min(top, bottom), 0.0f, 1.0f);
}

void ScrollView::begin(UI& ui) {
    ui.touchClip.push_back(view);
    nvgSave(ui.vg);
    nvgScissor(ui.vg, view.x, view.y, view.w, view.h);
}

void ScrollView::end(UI& ui) {
    if (!ui.touchClip.empty()) ui.touchClip.pop_back();
    nvgRestore(ui.vg);
}

void ScrollView::drawScrollbar(UI& ui) {
    if (contentH <= view.h + 1) return;
    float a = ui.smooth(id ^ 0x5c011, idleTime < 0.9f ? 1.0f : 0.0f, 8);
    if (a < 0.01f) return;
    float trackH = view.h - 16;
    float barH = std::max(36.0f, trackH * view.h / contentH);
    float p = std::clamp(offset / maxOffset(), 0.0f, 1.0f);
    float y = view.y + 8 + (trackH - barH) * p;
    ui.fillRect({view.right() - 7, y, 4, barH}, withAlpha(ui.t.textDim, 0.55f * a), 2);
}

// ------------------------------------------------------------------ UI core

// Picks the chosen UI font, falling back to the built-in one before App has set them.
void UI::setFont(bool bold) {
    int id = bold ? fontBoldId : fontId;
    if (id >= 0) nvgFontFaceId(vg, id);
    else nvgFontFace(vg, bold ? "sans-bold" : "sans");
}

void UI::init(NVGcontext* v, Input* i) {
    vg = v;
    in = i;
}

void UI::applyTheme(const Config& cfg) {
    const ThemeDef& d = kThemes[std::clamp(cfg.s.theme, 0, (int)(sizeof(kThemes) / sizeof(kThemes[0])) - 1)];
    t.bg = hex(d.bg);
    t.bg2 = hex(d.bg2);
    t.surface = hex(d.surface);
    t.surface2 = hex(d.surface2);
    t.surfaceHi = hex(d.surfaceHi);
    t.text = hex(d.text);
    t.textDim = hex(d.textDim);
    t.textFaint = hex(d.textFaint);
    t.light = d.light;
    t.accent = accentColor(cfg.s.accent);
    // Secondary accent: neighbour in the palette, for gradients.
    t.accent2 = mix(t.accent, accentColor((cfg.s.accent + 1) % accentCount()), 0.65f);
    float lum = 0.299f * t.accent.r + 0.587f * t.accent.g + 0.114f * t.accent.b;
    t.onAccent = lum > 0.68f ? hex(0x101215) : hex(0xFFFFFF);
    t.border = d.light ? hex(0x000000, 0.08f) : hex(0xFFFFFF, 0.07f);
    t.danger = hex(0xFF4D5E);
    t.success = hex(0x2BD48A);
    t.warning = hex(0xFFB020);
    t.scrim = hex(0x000000, 0.55f);
    t.radius = (float)cfg.s.roundness;
    animMul = cfg.animMultiplier();
    swapAB = cfg.s.swapAB;
    bg_ = cfg.s.background;
}

void UI::beginFrame(float d, float w, float h) {
    touchClip.clear();
    dt = d;
    time += d;
    W = w;
    H = h;
    items_.clear();
    // One entry per row ever shown. Dropping them only restarts a few animations.
    if (anims_.size() > 16384) anims_.clear();
    navConsumed_ = 0;
    confirmUsed_ = backUsed_ = false;
    // Navigation happens in endFrame, so lists see the move on the following frame.
    focusMoved = navMoved_;
    navMoved_ = false;
    blocked = false;
    navEnabled = true;
    alphaStack_.clear();
    alpha_ = 1;
}

void UI::endFrame() {
    // Make sure something sensible is focused.
    bool found = false;
    for (auto& it : items_)
        if (it.id == focus) {
            found = true;
            lastFocusRect_ = it.r;
            haveFocusRect_ = true;
        }
    // Focus set on purpose may only appear next frame (e.g. a row restored as a menu closes).
    if (!found && !items_.empty() && !navMoved_) {
        uint32_t pick = items_[0].id;
        bool usedDefault = false;
        for (auto& it : items_)
            if (it.id == defaultFocus_) {
                pick = defaultFocus_;
                usedDefault = true;
            }
        if (!usedDefault && haveFocusRect_) {
            // The focused item vanished (deleted file, hidden setting): pick its nearest neighbour.
            float best = 1e18f;
            for (auto& it : items_) {
                float dx = it.r.cx() - lastFocusRect_.cx(), dy = it.r.cy() - lastFocusRect_.cy();
                float d = dx * dx + dy * dy;
                if (d < best) {
                    best = d;
                    pick = it.id;
                }
            }
        }
        focus = pick;
    }
    defaultFocus_ = 0;

    if (!navEnabled || items_.empty()) return;
    uint32_t dirs = in->repeat & BTN_DIRS & ~navConsumed_;
    if (!dirs) return;

    const FocusItem* cur = nullptr;
    for (auto& it : items_)
        if (it.id == focus) cur = &it;
    if (!cur) return;

    float dx = (dirs & BTN_RIGHT) ? 1 : (dirs & BTN_LEFT) ? -1 : 0;
    float dy = (dirs & BTN_DOWN) ? 1 : (dirs & BTN_UP) ? -1 : 0;
    if (dx != 0 && dy != 0) dy = 0;

    const FocusItem* best = nullptr;
    float bestScore = 1e9f;
    Rect c = cur->r;
    for (auto& it : items_) {
        if (it.id == cur->id) continue;
        Rect r = it.r;
        // Left or right means "beside me", so it never drops onto something that sits below in the
        // same column of controls. Without this, right from a sidebar row landed on the Exit button
        // under it rather than in the file grid: Exit is only a little to the right, while the grid
        // is far to the right and far up, so Exit always won on distance. Exit then acted on the
        // next press. Buttons side by side (Wi-Fi next to Exit) share a row and are unaffected.
        if (dx != 0 && it.group == cur->group && r.y >= c.bottom()) continue;
        float primary, ortho;
        if (dx > 0) {
            if (r.cx() <= c.cx() + 1 || r.right() <= c.right() - 1) continue;
            primary = std::max(0.0f, r.x - c.right());
            ortho = (r.bottom() > c.y && r.y < c.bottom()) ? std::fabs(r.cy() - c.cy()) * 0.1f
                                                           : std::fabs(r.cy() - c.cy());
        } else if (dx < 0) {
            if (r.cx() >= c.cx() - 1 || r.x >= c.x + 1) continue;
            primary = std::max(0.0f, c.x - r.right());
            ortho = (r.bottom() > c.y && r.y < c.bottom()) ? std::fabs(r.cy() - c.cy()) * 0.1f
                                                           : std::fabs(r.cy() - c.cy());
        } else if (dy > 0) {
            if (r.cy() <= c.cy() + 1 || r.bottom() <= c.bottom() - 1) continue;
            primary = std::max(0.0f, r.y - c.bottom());
            ortho = (r.right() > c.x && r.x < c.right()) ? std::fabs(r.cx() - c.cx()) * 0.1f
                                                         : std::fabs(r.cx() - c.cx());
        } else {
            if (r.cy() >= c.cy() - 1 || r.y >= c.y + 1) continue;
            primary = std::max(0.0f, c.y - r.bottom());
            ortho = (r.right() > c.x && r.x < c.right()) ? std::fabs(r.cx() - c.cx()) * 0.1f
                                                         : std::fabs(r.cx() - c.cx());
        }
        float score = primary + ortho * 2.2f;
        if (score < bestScore) {
            bestScore = score;
            best = &it;
        }
    }
    if (!best) return;

    uint32_t next = best->id;
    // Entering another group (e.g. sidebar -> list) restores its last focus — but only somewhere
    // near the spatial landing spot. A stale memory (the Wi-Fi/Exit pills, say) would otherwise
    // teleport focus across the screen, right onto buttons that act on a single press.
    if (best->group != cur->group && best->group != 0) {
        auto m = groupMemory_.find(best->group);
        if (m != groupMemory_.end()) {
            for (auto& it : items_)
                if (it.id == m->second &&
                    std::fabs(it.r.cy() - best->r.cy()) < 170 && std::fabs(it.r.cx() - best->r.cx()) < 400)
                    next = it.id;
        }
    }
    if (cur->group) groupMemory_[cur->group] = cur->id;
    focus = next;
    navMoved_ = true;
}

bool UI::peekConfirm() const { return !blocked && !confirmUsed_ && in->down(swapAB ? BTN_B : BTN_A); }
bool UI::peekBack() const { return !blocked && !backUsed_ && in->down(swapAB ? BTN_A : BTN_B); }

bool UI::confirm() {
    if (!peekConfirm()) return false;
    confirmUsed_ = true;
    return true;
}

bool UI::back() {
    if (!peekBack()) return false;
    backUsed_ = true;
    return true;
}

bool UI::tapped(Rect r) {
    if (blocked || !in->tapIn(r.x, r.y, r.w, r.h) || !inTouchClip(in->tapX, in->tapY)) return false;
    in->consumeTap();
    return true;
}

bool UI::pressedVisual(Rect r) const {
    return !blocked && in->touchDownIn(r.x, r.y, r.w, r.h) && inTouchClip(in->touchX, in->touchY);
}

bool UI::longPressIn(Rect r) const {
    return !blocked && in->longPress && r.contains(in->touchX, in->touchY) && inTouchClip(in->touchX, in->touchY);
}

void UI::focusable(uint32_t id, Rect r, uint32_t group) {
    if (blocked) return;
    items_.push_back({id, r, group});
    if (id == focus && group) groupMemory_[group] = id;
}

float UI::smooth(uint32_t id, float target, float rate) {
    auto it = anims_.find(id);
    if (it == anims_.end()) {
        anims_[id] = target;
        return target;
    }
    if (animMul <= 0) {
        it->second = target;
        return target;
    }
    float a = 1 - std::exp(-dt * rate * animMul);
    it->second += (target - it->second) * a;
    if (std::fabs(target - it->second) < 0.0005f) it->second = target;
    return it->second;
}

void UI::pushAlpha(float a) {
    alphaStack_.push_back(alpha_);
    alpha_ *= a;
    nvgGlobalAlpha(vg, alpha_);
}

void UI::popAlpha() {
    if (alphaStack_.empty()) return;
    alpha_ = alphaStack_.back();
    alphaStack_.pop_back();
    nvgGlobalAlpha(vg, alpha_);
}

float& UI::value(uint32_t id, float init) {
    auto it = anims_.find(id);
    if (it == anims_.end()) it = anims_.emplace(id, init).first;
    return it->second;
}

float UI::ease(float x) const {
    x = std::clamp(x, 0.0f, 1.0f);
    float i = 1 - x;
    return 1 - i * i * i;
}

// ------------------------------------------------------------------ Drawing

void UI::fillRect(Rect r, NVGcolor c, float radius) {
    if (radius < 0) radius = t.radius;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, std::min(radius, std::min(r.w, r.h) * 0.5f));
    nvgFillColor(vg, c);
    nvgFill(vg);
}

void UI::strokeRect(Rect r, NVGcolor c, float width, float radius) {
    if (radius < 0) radius = t.radius;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, std::min(radius, std::min(r.w, r.h) * 0.5f));
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
}

void UI::gradientRect(Rect r, NVGcolor top, NVGcolor bottom, float radius) {
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, r.y, r.x, r.bottom(), top, bottom));
    nvgFill(vg);
}

void UI::shadow(Rect r, float radius, float blur, float alpha, float dy) {
    if (radius < 0) radius = t.radius;
    NVGpaint p = nvgBoxGradient(vg, r.x, r.y + dy, r.w, r.h, radius + blur * 0.25f, blur, hex(0, alpha), hex(0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, r.x - blur * 2, r.y - blur * 2 + dy, r.w + blur * 4, r.h + blur * 4);
    nvgFillPaint(vg, p);
    nvgFill(vg);
}

void UI::panel(Rect r, float alpha, float radius) {
    if (radius < 0) radius = t.radius + 4;
    shadow(r, radius, 28, (t.light ? 0.18f : 0.5f) * alpha, 10);
    fillRect(r, withAlpha(t.surface, alpha), radius);
    strokeRect(r.inset(0.5f), withAlpha(t.border, alpha), 1, radius);
}

void UI::glass(Rect r, float radius, float opacity) {
    if (radius < 0) radius = t.radius + 4;
    NVGcolor base = t.light ? hex(0xFFFFFF, opacity) : withAlpha(mix(t.bg, hex(0), 0.3f), opacity);
    shadow(r, radius, 30, 0.35f, 8);
    fillRect(r, base, radius);
    // Subtle top sheen.
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, r.y, r.x, r.y + 80, hex(0xFFFFFF, t.light ? 0.4f : 0.06f),
                                       hex(0xFFFFFF, 0)));
    nvgFill(vg);
    strokeRect(r.inset(0.5f), hex(0xFFFFFF, t.light ? 0.5f : 0.09f), 1, radius);
}

void UI::focusRing(Rect r, float radius, float amount) {
    if (amount <= 0.01f) return;
    if (radius < 0) radius = t.radius;
    float pulse = 0.75f + 0.25f * std::sin(time * 4.0f);
    // Outer glow
    NVGpaint glow = nvgBoxGradient(vg, r.x, r.y, r.w, r.h, radius, 14, withAlpha(t.accent, 0.45f * amount * pulse),
                                   withAlpha(t.accent, 0));
    nvgBeginPath(vg);
    nvgRect(vg, r.x - 20, r.y - 20, r.w + 40, r.h + 40);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, glow);
    nvgFill(vg);
    strokeRect(r.inset(-1.5f), withAlpha(t.accent, amount), 3, radius + 1.5f);
}

// On a mirrored (right-to-left) screen the whole drawing is flipped; text is flipped back around
// its anchor so its letters read normally, and its alignment swaps sides.
void UI::text(float x, float y, const std::string& s, float size, NVGcolor c, int align, bool bold) {
    setFont(bold);
    nvgFontSize(vg, size);
    nvgFillColor(vg, c);
    if (mirror) {
        nvgSave(vg);
        nvgTranslate(vg, x, 0);
        nvgScale(vg, -1, 1);
        nvgTranslate(vg, -x, 0);
        align = align == LEFT ? RIGHT : (align == RIGHT ? LEFT : align);
    }
    // Centre on the capital letters, not the line box: Vazirmatn's metrics reserve room for tall
    // Arabic marks, so NVG_ALIGN_MIDDLE leaves Latin text visibly above neighbouring icons.
    nvgTextAlign(vg, align | NVG_ALIGN_BASELINE);
    std::string shaped = shapeForDisplay(shown(s));
    nvgText(vg, x, y + size * kCapCentre, shaped.c_str(), nullptr);
    if (mirror) nvgRestore(vg);
}

void UI::textBox(float x, float y, float w, const std::string& s, float size, NVGcolor c, bool bold) {
    setFont(bold);
    nvgFontSize(vg, size);
    nvgFillColor(vg, c);
    if (mirror) {
        nvgSave(vg);
        nvgTranslate(vg, x + w / 2, 0);
        nvgScale(vg, -1, 1);
        nvgTranslate(vg, -(x + w / 2), 0);
    }
    nvgTextAlign(vg, (mirror ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT) | NVG_ALIGN_TOP);
    nvgTextLineHeight(vg, 1.25f);
    std::string shaped = shapeForDisplay(shown(s));
    nvgTextBox(vg, x, y, w, shaped.c_str(), nullptr);
    if (mirror) nvgRestore(vg);
}

// The text as shown: translated, and in Persian or Arabic, numbers on their own (a time, a count, a
// percentage) in that language's digits.
std::string UI::shown(const std::string& s) const {
    std::string t = tr(s);
    if (!i18n::rtl()) return t;
    // A line mixing Persian or Arabic with Latin (a path, a codec) reads right to left as a whole.
    bool rtlText = false;
    for (unsigned char c : t)
        if (c >= 0xD6 && c <= 0xDB) rtlText = true;
    if (rtlText && t.compare(0, 3, "\xE2\x80\x8F") != 0) {
        // Ending punctuation of a Latin word or path ("Root/") stays with that word. Brackets are
        // left out: the bidi pass mirrors a pair itself, and pinning the closing one to the Latin
        // run broke the pair, so "(SMB)" came out as "((SMB".
        std::string m = "\xE2\x80\x8F";
        for (size_t i = 0; i < t.size(); i++) {
            m += t[i];
            char c = t[i];
            if ((c == '/' || c == '.') && i > 0 && isalnum((unsigned char)t[i - 1])) m += "\xE2\x80\x8E";
        }
        t = m;
    }
    if (keepDigits) return t;
    for (unsigned char c : t)
        if (isalpha(c)) return t;
    // An address or a version ("192.168.2.15", "1.0.0") is an identifier, not a number: two or more
    // dots between digits mean it is read and typed as written, so it keeps Latin digits. One dot
    // ("30.2") is a decimal and one colon ("00:53") a time, which both belong in the local digits.
    int dots = 0;
    for (size_t i = 1; i + 1 < t.size(); i++)
        if (t[i] == '.' && isdigit((unsigned char)t[i - 1]) && isdigit((unsigned char)t[i + 1])) dots++;
    if (dots >= 2) return t;
    return i18n::localDigits(t);
}

void UI::beginUnmirrored(Rect r) {
    unmirrorDepth_.push_back(mirror);
    if (!mirror) return;
    nvgSave(vg);
    nvgTranslate(vg, r.cx(), 0);
    nvgScale(vg, -1, 1);
    nvgTranslate(vg, -r.cx(), 0);
    mirror = false;
}

void UI::endUnmirrored() {
    if (unmirrorDepth_.empty()) return;
    bool was = unmirrorDepth_.back();
    unmirrorDepth_.pop_back();
    if (!was) return;
    nvgRestore(vg);
    mirror = true;
}

float UI::textWidth(const std::string& s, float size, bool bold) {
    setFont(bold);
    nvgFontSize(vg, size);
    std::string shaped = shapeForDisplay(shown(s));
    return nvgTextBounds(vg, 0, 0, shaped.c_str(), nullptr, nullptr);
}

void UI::marquee(uint32_t id, float x, float y, float maxW, const std::string& text0, float size, NVGcolor c, int align,
                 bool bold) {
    std::string s = shown(text0);
    float w = textWidth(s, size, bold);
    if (w <= maxW || maxW <= 0) {
        text(x, y, s, size, c, align, bold);
        return;
    }
    if (marquees_.size() > 64) {  // forget ones no longer drawn
        for (auto it = marquees_.begin(); it != marquees_.end();)
            it = time - it->second.lastSeen > 2 ? marquees_.erase(it) : std::next(it);
    }
    Marquee& m = marquees_[id];
    size_t h = std::hash<std::string>{}(s);
    if (m.hash != h || time - m.lastSeen > 0.5f) {  // new text, or shown again: start from the beginning
        m.hash = h;
        m.t = 0;
    }
    m.lastSeen = time;
    m.t += dt;
    const float pause = 1.8f, speed = 38;
    float over = w - maxW + 6, travel = over / speed;
    float p = std::fmod(m.t, 2 * (pause + travel));
    float off;
    if (p < pause) off = 0;
    else if (p < pause + travel) off = (p - pause) * speed;
    else if (p < 2 * pause + travel) off = over;
    else off = over - (p - 2 * pause - travel) * speed;
    // Ease the turns so it doesn't jerk at either end.
    float k = std::clamp(off / over, 0.0f, 1.0f);
    off = over * (k * k * (3 - 2 * k));
    float left = align == CENTER ? x - maxW / 2 : (align == RIGHT ? x - maxW : x);
    nvgSave(vg);
    nvgIntersectScissor(vg, left - 1, y - size * 1.2f, maxW + 2, size * 2.4f);
    text(left - off, y, s, size, c, LEFT, bold);
    nvgRestore(vg);
}

std::string UI::ellipsize(const std::string& in, float maxW, float size, bool bold) {
    std::string s = shown(in);  // measured as it will be shown
    // Lists ask for the same few strings every frame, and each answer takes several measurements.
    char suffix[32];
    snprintf(suffix, sizeof suffix, "\x01%d\x01%d\x01%d", (int)(maxW * 4), (int)(size * 4), bold ? 1 : 0);
    std::string key = s + suffix;
    auto cached = ellipsized_.find(key);
    if (cached != ellipsized_.end()) return cached->second;
    if (ellipsized_.size() > 2048) ellipsized_.clear();
    if (textWidth(s, size, bold) <= maxW) {
        ellipsized_.emplace(std::move(key), s);
        return s;
    }
    const char* ell = "…";
    float ew = textWidth(ell, size, bold);
    // Binary search on UTF-8 boundaries.
    std::vector<size_t> cuts;
    for (size_t i = 0; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) cuts.push_back(i);
    cuts.push_back(s.size());
    int lo = 0, hi = (int)cuts.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (textWidth(s.substr(0, cuts[mid]), size, bold) + ew <= maxW) lo = mid;
        else hi = mid - 1;
    }
    std::string r = s.substr(0, cuts[lo]);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    r += ell;
    ellipsized_.emplace(std::move(key), r);
    return r;
}

void UI::icon(Icon ic, float cx, float cy, float size, NVGcolor c) {
    // On a mirrored screen only arrows point the other way; a play sign or a clock stays as it is.
    bool turns = ic == Icon::Back || ic == Icon::ChevronLeft || ic == Icon::ChevronRight;
    if (!mirror || turns) return drawIcon(vg, ic, cx, cy, size, c);
    nvgSave(vg);
    nvgTranslate(vg, cx, 0);
    nvgScale(vg, -1, 1);
    nvgTranslate(vg, -cx, 0);
    drawIcon(vg, ic, cx, cy, size, c);
    nvgRestore(vg);
}

namespace {
float seedHue(const std::string& seed) { return (uid(seed.c_str(), 7) % 997) / 997.0f; }

// The first letter of a title, skipping track numbers and punctuation ("03 - Song" → "S").
std::string initialOf(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (c >= 0x80) return s.substr(i, len);
        if (std::isalpha(c)) return std::string(1, (char)std::toupper(c));
        i += len;
    }
    return "";
}
}  // namespace

NVGcolor UI::seedColor(const std::string& seed) const { return nvgHSL(seedHue(seed), 0.62f, 0.56f); }

void UI::generatedArt(Rect r, const std::string& seed, const std::string& label, float radius, bool circle) {
    uint32_t h = uid(seed.c_str(), 7);
    float hue = seedHue(seed), hue2 = std::fmod(hue + 0.07f + (h >> 12) % 100 / 800.0f, 1.0f);
    auto shape = [&] {
        nvgBeginPath(vg);
        if (circle) nvgCircle(vg, r.cx(), r.cy(), r.w / 2);
        else nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    };
    shape();
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, r.y, r.right(), r.bottom(), nvgHSL(hue, 0.6f, 0.47f), nvgHSL(hue2, 0.68f, 0.22f)));
    nvgFill(vg);
    // Two soft lights, placed by the seed, so albums look different from each other
    float lx = r.x + r.w * (0.15f + 0.5f * ((h >> 3) & 15) / 15.0f), ly = r.y + r.h * (0.1f + 0.3f * ((h >> 7) & 7) / 7.0f);
    shape();
    nvgFillPaint(vg, nvgRadialGradient(vg, lx, ly, 0, r.w * 0.75f, nvgHSLA(std::fmod(hue + 0.04f, 1.0f), 0.8f, 0.7f, 120),
                                       nvgHSLA(hue, 0.8f, 0.7f, 0)));
    nvgFill(vg);
    shape();
    nvgFillPaint(vg, nvgRadialGradient(vg, r.right(), r.bottom(), 0, r.w * 0.8f, nvgHSLA(hue2, 0.7f, 0.12f, 150),
                                       nvgHSLA(hue2, 0.7f, 0.12f, 0)));
    nvgFill(vg);
    // Rings like the grooves of a record, in a corner
    nvgSave(vg);
    nvgScissor(vg, r.x + radius * 0.3f, r.y + radius * 0.3f, r.w - radius * 0.6f, r.h - radius * 0.6f);
    float rcx = circle ? r.cx() : r.right() - r.w * 0.12f, rcy = circle ? r.cy() : r.bottom() - r.h * 0.12f;
    for (float k = circle ? 0.2f : 0.3f; k < (circle ? 0.5f : 1.0f); k += 0.11f) {
        nvgBeginPath(vg);
        nvgCircle(vg, rcx, rcy, r.w * k);
        nvgStrokeColor(vg, hex(0xFFFFFF, 0.07f));
        nvgStrokeWidth(vg, std::max(1.0f, r.w * 0.006f));
        nvgStroke(vg);
    }
    nvgRestore(vg);
    std::string letter = circle ? "" : initialOf(label);  // a turning record shows only its colours
    if (circle) return;
    if (letter.empty()) {
        icon(Icon::Music, r.cx(), r.cy(), r.h * 0.36f, hex(0xFFFFFF, 0.92f));
    } else {
        text(r.cx(), r.cy() - (circle ? r.h * 0.18f : 0), letter, r.h * (circle ? 0.22f : 0.46f), hex(0xFFFFFF, 0.94f),
             CENTER, true);
        if (!circle && r.h >= 120) icon(Icon::Music, r.x + r.w * 0.13f, r.y + r.h * 0.13f, r.h * 0.09f, hex(0xFFFFFF, 0.55f));
    }
}

void UI::spinner(float cx, float cy, float r, NVGcolor c, float width) {
    float a0 = time * 5.5f;
    float sweep = 1.2f + 0.9f * std::sin(time * 2.2f);
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r, a0, a0 + sweep * 2.2f, NVG_CW);
    nvgStrokeColor(vg, c);
    nvgStrokeWidth(vg, width);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
}

void UI::progressRing(float cx, float cy, float r, float p, NVGcolor c, float width) {
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r);
    nvgStrokeColor(vg, withAlpha(t.textFaint, 0.35f));
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
    if (p <= 0) return;
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r, -PI / 2, -PI / 2 + 2 * PI * std::min(p, 0.999f), NVG_CW);
    nvgStrokeColor(vg, c);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
}

void UI::glyph(uint32_t btn, float cx, float cy, float size) {
    const char* label = "?";
    bool round = true;
    NVGcolor bgc = t.light ? hex(0x2A2E36) : hex(0xFFFFFF, 0.92f);
    NVGcolor fg = t.light ? hex(0xFFFFFF) : hex(0x15171C);
    switch (btn) {
        case BTN_A: label = "A"; break;
        case BTN_B: label = "B"; break;
        case BTN_X: label = "X"; break;
        case BTN_Y: label = "Y"; break;
        case BTN_L: label = "L"; round = false; break;
        case BTN_R: label = "R"; round = false; break;
        case BTN_ZL: label = "ZL"; round = false; break;
        case BTN_ZR: label = "ZR"; round = false; break;
        case BTN_PLUS: label = "+"; break;
        case BTN_MINUS: label = "−"; break;
        default: break;
    }
    if (round) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, size * 0.5f);
        nvgFillColor(vg, bgc);
        nvgFill(vg);
        text(cx, cy, label, size * 0.62f, fg, CENTER, true);
    } else {
        float w = size * (strlen(label) > 1 ? 1.5f : 1.15f);
        fillRect({cx - w / 2, cy - size * 0.42f, w, size * 0.84f}, bgc, size * 0.25f);
        text(cx, cy, label, size * 0.55f, fg, CENTER, true);
    }
}

float UI::hint(float x, float cy, uint32_t btn, const char* label) {
    // Map logical confirm/back to the physical button when swapped.
    if (swapAB && (btn == BTN_A || btn == BTN_B)) btn = btn == BTN_A ? BTN_B : BTN_A;
    float gs = 24;
    bool wide = btn == BTN_L || btn == BTN_R || btn == BTN_ZL || btn == BTN_ZR;
    float gw = wide ? gs * 1.5f : gs;
    glyph(btn, x + gw / 2, cy, gs);
    float tw = textWidth(label, 17);
    // Each hint is also a button, so everything works without a controller.
    Rect hit{x - 6, cy - 20, gw + 8 + tw + 12, 40};
    if (pressedVisual(hit)) fillRect(hit, withAlpha(t.textFaint, 0.2f), 20);
    text(x + gw + 8, cy, label, 17, t.textDim);
    if (tapped(hit)) in->inject(btn);
    return gw + 8 + tw + 22;
}

void UI::battery(float x, float cy, int pct, bool charging, bool onDark) {
    if (pct < 0) return;
    float w = 26, h = 13;
    NVGcolor ink = onDark ? hex(0xFFFFFF) : t.text;
    Rect body{x, cy - h / 2, w, h};
    strokeRect(body, withAlpha(ink, 0.85f), 1.5f, 3.5f);
    fillRect({x + w + 1.5f, cy - 3, 2.5f, 6}, withAlpha(ink, 0.85f), 1);
    NVGcolor fc = pct <= 15 && !charging ? t.danger : (charging ? t.success : ink);
    fillRect({x + 2.5f, cy - h / 2 + 2.5f, (w - 5) * pct / 100.0f, h - 5}, fc, 1.5f);
    if (charging) {  // a light running along the charge
        float fw = (w - 5) * pct / 100.0f, sx = x + 2.5f + std::fmod(time * 0.6f, 1.0f) * (w - 5);
        if (sx < x + 2.5f + fw) {
            nvgBeginPath(vg);
            nvgRect(vg, x + 2.5f, cy - h / 2 + 2.5f, fw, h - 5);
            nvgFillPaint(vg, nvgLinearGradient(vg, sx - 6, 0, sx + 6, 0, hex(0xFFFFFF, 0), hex(0xFFFFFF, 0.55f)));
            nvgFill(vg);
        }
    }
    if (charging) {
        nvgBeginPath(vg);
        float bx = x + w / 2, by = cy;
        nvgMoveTo(vg, bx + 1.5f, by - 7);
        nvgLineTo(vg, bx - 3.5f, by + 1);
        nvgLineTo(vg, bx, by + 1);
        nvgLineTo(vg, bx - 1.5f, by + 7);
        nvgLineTo(vg, bx + 3.5f, by - 1);
        nvgLineTo(vg, bx, by - 1);
        nvgClosePath(vg);
        nvgFillColor(vg, t.light && !onDark ? hex(0x111111) : hex(0xFFFFFF));
        nvgFill(vg);
    }
}

void UI::background(int style) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, W, H);
    if (style == 2) {
        nvgFillColor(vg, t.bg);
        nvgFill(vg);
        return;
    }
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, W * 0.3f, H, t.bg2, t.bg));
    nvgFill(vg);
    if (style != 0) return;
    // Aurora: slow drifting light blobs tinted with the accent colours.
    float tm = time * 0.12f;
    struct Blob { float x, y, r, a; NVGcolor c; } blobs[] = {
        {W * (0.78f + 0.08f * std::sin(tm * 1.3f)), H * (0.12f + 0.08f * std::cos(tm)), 520, 0.20f, t.accent},
        {W * (0.20f + 0.10f * std::cos(tm * 0.9f)), H * (0.95f + 0.06f * std::sin(tm * 1.1f)), 600, 0.14f, t.accent2},
        {W * (0.55f + 0.15f * std::sin(tm * 0.7f + 2)), H * (0.55f + 0.1f * std::cos(tm * 0.8f)), 420, 0.07f,
         mix(t.accent, hex(0xFFFFFF), 0.2f)},
    };
    float strength = t.light ? 0.6f : 1.0f;
    for (auto& b : blobs) {
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, W, H);
        nvgFillPaint(vg, nvgRadialGradient(vg, b.x, b.y, 0, b.r, withAlpha(b.c, b.a * strength), withAlpha(b.c, 0)));
        nvgFill(vg);
    }
}

// ------------------------------------------------------------------ Widgets

bool UI::button(uint32_t id, Rect r, const char* label, Icon ic, ButtonStyle st, uint32_t group) {
    focusable(id, r, group);
    bool foc = focused(id) && showFocus();
    bool press = pressedVisual(r) || (focused(id) && in->isHeld(swapAB ? BTN_B : BTN_A) && !blocked);
    float pv = smooth(id ^ 0x9e37, press ? 1.0f : 0.0f, 25);
    float fv = smooth(id ^ 0x7f4a, foc ? 1.0f : 0.0f, 16);
    float s = 1 - 0.04f * pv;
    Rect rr{r.cx() - r.w * s / 2, r.cy() - r.h * s / 2, r.w * s, r.h * s};
    float rad = std::min(t.radius, r.h / 2);

    NVGcolor bgc, fg;
    switch (st) {
        case BTN_FILLED:
            bgc = mix(t.accent, hex(0xFFFFFF), 0.12f * fv);
            fg = t.onAccent;
            break;
        case BTN_TONAL:
            bgc = mix(t.surface2, t.surfaceHi, std::max(fv, pv));
            fg = t.text;
            break;
        case BTN_DANGER:  // tonal, in the warning colour: for what can't be undone
            bgc = withAlpha(t.danger, 0.14f + 0.12f * std::max(fv, pv));
            fg = t.danger;
            break;
        default:
            bgc = withAlpha(t.surfaceHi, std::max(fv, pv) * 0.9f);
            fg = t.text;
            break;
    }
    if (st == BTN_FILLED) {
        shadow(rr, rad, 16, 0.3f, 4);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, rr.x, rr.y, rr.w, rr.h, rad);
        nvgFillPaint(vg, nvgLinearGradient(vg, rr.x, rr.y, rr.right(), rr.bottom(), bgc, mix(bgc, t.accent2, 0.6f)));
        nvgFill(vg);
    } else {
        fillRect(rr, bgc, rad);
    }
    focusRing(rr, rad, fv);

    float iconSize = std::min(24.0f, r.h * 0.5f);
    float tw = label && *label ? textWidth(label, 19, true) : 0;
    float total = tw + (ic != Icon::None ? iconSize + (tw > 0 ? 10 : 0) : 0);
    float x = rr.cx() - total / 2;
    if (ic != Icon::None) {
        icon(ic, x + iconSize / 2, rr.cy(), iconSize, fg);
        x += iconSize + 10;
    }
    if (tw > 0) text(x, rr.cy(), label, 19, fg, LEFT, true);

    bool activated = tapped(r) || (focused(id) && confirm());
    return activated;
}

bool UI::iconButton(uint32_t id, float cx, float cy, float size, Icon ic, NVGcolor c, uint32_t group,
                    bool bgOn) {
    Rect r{cx - size / 2, cy - size / 2, size, size};
    focusable(id, r, group);
    bool foc = focused(id) && showFocus();
    bool press = pressedVisual(r.inset(-8)) || (focused(id) && in->isHeld(swapAB ? BTN_B : BTN_A) && !blocked);
    float pv = smooth(id ^ 0x9e37, press ? 1.0f : 0.0f, 25);
    float fv = smooth(id ^ 0x7f4a, foc ? 1.0f : 0.0f, 16);
    float bgA = (bgOn ? 0.16f : 0) + 0.18f * pv + 0.1f * fv;
    if (bgA > 0.01f) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, size / 2 * (1 - 0.06f * pv));
        nvgFillColor(vg, hex(t.light && !bgOn ? 0x000000 : 0xFFFFFF, bgA));
        nvgFill(vg);
    }
    if (fv > 0.01f) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, size / 2 + 2);
        nvgStrokeColor(vg, withAlpha(t.accent, fv));
        nvgStrokeWidth(vg, 3);
        nvgStroke(vg);
    }
    icon(ic, cx, cy, size * 0.5f * (1 - 0.08f * pv), c);
    return tapped(r.inset(-8)) || (focused(id) && confirm());
}

void UI::toggleKnob(Rect r, bool on, uint32_t id) {
    float v = smooth(id ^ 0x70661e, on ? 1.0f : 0.0f, 18);
    float w = 52, h = 30;
    Rect tr{r.right() - w, r.cy() - h / 2, w, h};
    fillRect(tr, mix(t.light ? hex(0xC9CED8) : t.surfaceHi, t.accent, v), h / 2);
    float kx = tr.x + h / 2 + (w - h) * v;
    nvgBeginPath(vg);
    nvgCircle(vg, kx, tr.cy() + 1, h / 2 - 3);
    nvgFillColor(vg, hex(0, 0.25f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, kx, tr.cy(), h / 2 - 4);
    nvgFillColor(vg, hex(0xFFFFFF));
    nvgFill(vg);
}

bool UI::toggle(uint32_t id, Rect r, bool on) {
    toggleKnob(r, on, id);
    return tapped(r) || (focused(id) && confirm());
}

bool UI::slider(uint32_t id, Rect r, float& v, float lo, float hi, float step, bool focusedRow) {
    bool changed = false;
    Rect hit = r.inset(0, -14);
    if (!blocked) {
        if (in->touchBegan && hit.contains(in->touchX, in->touchY)) sliderDrag_ = id;
        if (sliderDrag_ == id) {
            float tx = -1;
            if (in->pan.active) {
                if (in->pan.owner == 0 && std::fabs(in->pan.x - in->pan.startX) > std::fabs(in->pan.y - in->pan.startY))
                    in->pan.owner = id;
                if (in->pan.owner == id) tx = in->touchX;
                else if (in->pan.owner != 0) sliderDrag_ = 0;
            } else if (in->tap && hit.contains(in->tapX, in->tapY)) {
                tx = in->tapX;
                in->consumeTap();
            }
            if (tx >= 0) {
                float np = std::clamp((tx - r.x) / r.w, 0.0f, 1.0f);
                float nv = std::round((lo + np * (hi - lo)) / step) * step;
                if (nv != v) {
                    v = nv;
                    changed = true;
                }
            }
            if (!in->touching) sliderDrag_ = 0;
        }
        if (focusedRow) {
            if (in->rep(BTN_LEFT)) { v = std::max(lo, v - step); changed = true; }
            if (in->rep(BTN_RIGHT)) { v = std::min(hi, v + step); changed = true; }
            consumeNav(BTN_LEFT | BTN_RIGHT);
        }
    }
    float p = (hi > lo) ? (v - lo) / (hi - lo) : 0;
    float sp = smooth(id ^ 0x511de, p, 22);
    float th = 6;
    fillRect({r.x, r.cy() - th / 2, r.w, th}, withAlpha(t.textFaint, 0.35f), th / 2);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.cy() - th / 2, r.w * sp, th, th / 2);
    nvgFillPaint(vg, nvgLinearGradient(vg, r.x, 0, r.right(), 0, t.accent, t.accent2));
    nvgFill(vg);
    float kx = r.x + r.w * sp;
    bool active = sliderDrag_ == id || focusedRow;
    float kr = smooth(id ^ 0x6b0b, active ? 11.0f : 8.0f, 18);
    nvgBeginPath(vg);
    nvgCircle(vg, kx, r.cy() + 1, kr + 1);
    nvgFillColor(vg, hex(0, 0.3f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, kx, r.cy(), kr);
    nvgFillColor(vg, hex(0xFFFFFF));
    nvgFill(vg);
    return changed;
}

void UI::toast(const std::string& msg, Icon ic) {
    if (!toasts_.empty() && toasts_.back().msg == msg) {
        toasts_.back().age = std::min(toasts_.back().age, 0.3f);
        return;
    }
    toasts_.push_back({msg, ic, 0});
    if (toasts_.size() > 3) toasts_.erase(toasts_.begin());
}

void UI::drawToasts() {
    const float life = 2.6f;
    float y = 28;
    for (auto it = toasts_.begin(); it != toasts_.end();) {
        it->age += dt;
        if (it->age > life) {
            it = toasts_.erase(it);
            continue;
        }
        float fadeIn = ease(it->age / 0.25f);
        float fadeOut = 1 - ease((it->age - (life - 0.3f)) / 0.3f);
        float a = std::min(fadeIn, fadeOut);
        std::string msg = ellipsize(it->msg, W - 160, 19, true);
        float tw = textWidth(msg, 19, true);
        float w = tw + 74, h = 48;
        Rect r{W / 2 - w / 2, y - 20 * (1 - a), w, h};
        nvgGlobalAlpha(vg, a);
        shadow(r, h / 2, 24, 0.45f, 8);
        fillRect(r, t.light ? hex(0x1A1D24) : mix(t.surface2, hex(0xFFFFFF), 0.06f), h / 2);
        strokeRect(r.inset(0.5f), hex(0xFFFFFF, 0.08f), 1, h / 2);
        nvgBeginPath(vg);
        nvgCircle(vg, r.x + 26, r.cy(), 14);
        nvgFillColor(vg, t.accent);
        nvgFill(vg);
        icon(it->ic, r.x + 26, r.cy(), 17, t.onAccent);
        text(r.x + 50, r.cy(), msg, 19, hex(0xFFFFFF), LEFT, true);
        nvgGlobalAlpha(vg, 1);
        y += (h + 10) * a;
        ++it;
    }
}
