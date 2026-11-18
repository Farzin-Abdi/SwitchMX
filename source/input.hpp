#pragma once
#include <cstdint>

// Logical button bits, platform independent.
enum Btn : uint32_t {
    BTN_A = 1u << 0,
    BTN_B = 1u << 1,
    BTN_X = 1u << 2,
    BTN_Y = 1u << 3,
    BTN_L = 1u << 4,
    BTN_R = 1u << 5,
    BTN_ZL = 1u << 6,
    BTN_ZR = 1u << 7,
    BTN_PLUS = 1u << 8,
    BTN_MINUS = 1u << 9,
    BTN_UP = 1u << 10,
    BTN_DOWN = 1u << 11,
    BTN_LEFT = 1u << 12,
    BTN_RIGHT = 1u << 13,
    BTN_LSTICK = 1u << 14,
    BTN_RSTICK = 1u << 15,
};
constexpr uint32_t BTN_DIRS = BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT;

// Raw per-frame input sampled by the platform layer. Touch coordinates are in
// the 1280x720 logical UI space.
struct RawInput {
    uint32_t buttons = 0;
    int touches = 0;
    float tx[4] = {}, ty[4] = {};
    float wheel = 0;     // mouse wheel (desktop), positive = scroll up
    float pinchWheel = 0; // ctrl+wheel (desktop) emulates pinch
    float lx = 0, ly = 0, rx = 0, ry = 0;
    bool quit = false;
};

struct PanState {
    bool active = false;
    bool began = false;  // first frame of the pan
    bool ended = false;  // finger lifted this frame
    float startX = 0, startY = 0;
    float x = 0, y = 0;
    float dx = 0, dy = 0;  // movement this frame
    float vx = 0, vy = 0;  // smoothed velocity, px/s
    uint32_t owner = 0;    // widget that captured this pan
};

struct PinchState {
    bool active = false, began = false, ended = false;
    float scale = 1;       // relative to pinch start
    float frameScale = 1;  // relative to previous frame
    float cx = 0, cy = 0;
};

class Input {
public:
    void update(const RawInput& raw, float dt);

    // Buttons
    uint32_t held = 0, pressed = 0, released = 0;
    uint32_t repeat = 0;  // pressed + auto-repeat (directions and shoulder buttons)
    float holdTime(Btn b) const;
    bool down(uint32_t b) const { return (pressed & b) != 0; }
    // Acts as if the button was pressed on the next update; lets touch reach every button action.
    void inject(uint32_t b) { injected_ |= b; }
    bool rep(uint32_t b) const { return (repeat & b) != 0; }
    bool isHeld(uint32_t b) const { return (held & b) != 0; }
    float lx = 0, ly = 0, rx = 0, ry = 0;
    bool usingButtons = false;  // last interaction came from a controller

    // Touch: primary finger
    bool touching = false;
    bool touchBegan = false;
    float touchX = 0, touchY = 0;
    float touchStartX = 0, touchStartY = 0;
    bool touchMoved = false;  // moved past slop since going down

    // Discrete gesture events for this frame (valid for one frame).
    bool tap = false;          // fired on release, immediately
    bool doubleTap = false;    // second tap within the window
    bool tapConfirmed = false; // single tap, after the double-tap window elapsed
    float tapX = 0, tapY = 0;
    float confirmedX = 0, confirmedY = 0;
    bool longPress = false;    // fired once when held still long enough
    bool longPressActive = false;
    bool longPressEnded = false;
    float wheel = 0;

    PanState pan;
    PinchState pinch;

    // Consumption so overlapping widgets don't both react.
    void consumeTap() {
        tap = doubleTap = tapConfirmed = false;
        pendingSingle_ = false;
        lastTapTime_ = -10;
    }
    // Flips everything sideways for a mirrored (right-to-left) screen: positions become W - x and
    // left/right swap. Calling it again undoes it.
    void mirrorX(float W);
    bool tapIn(float x, float y, float w, float h) const {
        return tap && tapX >= x && tapX < x + w && tapY >= y && tapY < y + h;
    }
    bool touchDownIn(float x, float y, float w, float h) const {
        return touching && !touchMoved && !pinch.active && touchStartX >= x && touchStartX < x + w &&
               touchStartY >= y && touchStartY < y + h && touchX >= x && touchX < x + w && touchY >= y &&
               touchY < y + h;
    }
    bool quit = false;

private:
    uint32_t injected_ = 0;
    float holdTimes_[16] = {};
    float repeatTimers_[16] = {};
    bool wasTouching_ = false;
    int prevTouches_ = 0;
    float downTime_ = 0;
    float lastTapTime_ = -10, lastTapX_ = 0, lastTapY_ = 0;
    bool pendingSingle_ = false;
    float pendingTime_ = 0, pendingX_ = 0, pendingY_ = 0;
    float time_ = 0;
    float pinchStartDist_ = 1, pinchPrevDist_ = 1;
    bool gestureCancelled_ = false;
    uint32_t stickButtons_ = 0;
};
