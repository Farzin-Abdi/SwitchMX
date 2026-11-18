#include "input.hpp"

#include <cmath>
#include <utility>

namespace {
constexpr float kTouchSlop = 14.0f;
constexpr float kLongPressTime = 0.45f;
constexpr float kTapMaxTime = 0.35f;
constexpr float kDoubleTapWindow = 0.28f;
constexpr float kDoubleTapRadius = 90.0f;
constexpr float kRepeatDelay = 0.38f;
constexpr float kStickThreshold = 0.5f;

int bitIndex(uint32_t b) {
    int i = 0;
    while (b > 1) { b >>= 1; i++; }
    return i;
}
}  // namespace

float Input::holdTime(Btn b) const { return holdTimes_[bitIndex(b)]; }

void Input::mirrorX(float W) {
    auto swapLR = [](uint32_t& v) {
        uint32_t l = v & BTN_LEFT, r = v & BTN_RIGHT;
        v &= ~(BTN_LEFT | BTN_RIGHT);
        if (l) v |= BTN_RIGHT;
        if (r) v |= BTN_LEFT;
    };
    swapLR(pressed);
    swapLR(released);
    swapLR(held);
    swapLR(repeat);
    swapLR(injected_);
    swapLR(stickButtons_);
    std::swap(holdTimes_[bitIndex(BTN_LEFT)], holdTimes_[bitIndex(BTN_RIGHT)]);
    std::swap(repeatTimers_[bitIndex(BTN_LEFT)], repeatTimers_[bitIndex(BTN_RIGHT)]);
    lx = -lx;
    rx = -rx;
    for (float* x : {&tapX, &touchX, &touchStartX, &confirmedX, &pan.x, &pan.startX, &pinch.cx, &lastTapX_, &pendingX_})
        *x = W - *x;
    pan.dx = -pan.dx;
    pan.vx = -pan.vx;
}

void Input::update(const RawInput& raw, float dt) {
    time_ += dt;
    quit = raw.quit;
    lx = raw.lx; ly = raw.ly; rx = raw.rx; ry = raw.ry;

    // Left stick acts as a d-pad, with hysteresis.
    uint32_t stick = 0;
    float th = kStickThreshold;
    if (raw.ly > th || ((stickButtons_ & BTN_UP) && raw.ly > th * 0.6f)) stick |= BTN_UP;
    if (raw.ly < -th || ((stickButtons_ & BTN_DOWN) && raw.ly < -th * 0.6f)) stick |= BTN_DOWN;
    if (raw.lx < -th || ((stickButtons_ & BTN_LEFT) && raw.lx < -th * 0.6f)) stick |= BTN_LEFT;
    if (raw.lx > th || ((stickButtons_ & BTN_RIGHT) && raw.lx > th * 0.6f)) stick |= BTN_RIGHT;
    stickButtons_ = stick;

    uint32_t now = raw.buttons | stick;
    pressed = now & ~held;
    released = held & ~now;
    held = now;
    repeat = pressed;
    // A tapped hint is the same as pressing its button, highlight included, so Open and Options
    // visibly act on the highlighted item.
    pressed |= injected_;
    if (pressed) usingButtons = true;
    repeat |= injected_;
    injected_ = 0;

    for (int i = 0; i < 16; i++) {
        uint32_t bit = 1u << i;
        if (held & bit) {
            float prev = holdTimes_[i];
            holdTimes_[i] += dt;
            if (prev == 0) {
                repeatTimers_[i] = kRepeatDelay;
            } else {
                // Accelerate repeat the longer a button is held.
                repeatTimers_[i] -= dt;
                if (repeatTimers_[i] <= 0) {
                    repeat |= bit;
                    float rate = holdTimes_[i] > 2.0f ? 0.035f : (holdTimes_[i] > 1.0f ? 0.055f : 0.085f);
                    repeatTimers_[i] += rate;
                }
            }
        } else {
            holdTimes_[i] = 0;
        }
    }

    // ---- Touch gestures ----
    tap = doubleTap = tapConfirmed = longPress = longPressEnded = false;
    touchBegan = false;
    pan.began = pan.ended = false;
    pan.dx = pan.dy = 0;
    pinch.began = pinch.ended = false;
    pinch.frameScale = 1;
    wheel = raw.wheel;

    int n = raw.touches;
    if (n > 0) usingButtons = false;

    // Two fingers: pinch (cancels any single-finger gesture).
    if (n >= 2) {
        float dx = raw.tx[1] - raw.tx[0], dy = raw.ty[1] - raw.ty[0];
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < 1) dist = 1;
        float cx = (raw.tx[0] + raw.tx[1]) * 0.5f, cy = (raw.ty[0] + raw.ty[1]) * 0.5f;
        if (!pinch.active) {
            pinch.active = true;
            pinch.began = true;
            pinchStartDist_ = pinchPrevDist_ = dist;
            if (pan.active) { pan.active = false; pan.ended = true; }
            if (longPressActive) { longPressActive = false; longPressEnded = true; }
            gestureCancelled_ = true;
        }
        pinch.scale = dist / pinchStartDist_;
        pinch.frameScale = dist / pinchPrevDist_;
        pinchPrevDist_ = dist;
        pinch.cx = cx; pinch.cy = cy;
    } else if (pinch.active) {
        pinch.active = false;
        pinch.ended = true;
    }

    // Desktop: ctrl+wheel emulates a pinch step.
    if (raw.pinchWheel != 0 && n < 2) {
        pinch.began = true;
        pinch.ended = true;
        pinch.frameScale = raw.pinchWheel > 0 ? 1.08f : 1 / 1.08f;
        pinch.scale = pinch.frameScale;
        pinch.cx = 640; pinch.cy = 360;
    }

    bool isTouching = n > 0;
    if (isTouching && !wasTouching_) {
        touching = true;
        touchBegan = true;
        touchX = touchStartX = raw.tx[0];
        touchY = touchStartY = raw.ty[0];
        touchMoved = false;
        downTime_ = time_;
        gestureCancelled_ = n >= 2;
        pan.vx = pan.vy = 0;
    } else if (isTouching) {
        float nx = raw.tx[0], ny = raw.ty[0];
        // While pinching, finger 0 may change identity; don't treat as motion.
        if (n == 1 && prevTouches_ == 1 && !gestureCancelled_) {
            float mdx = nx - touchX, mdy = ny - touchY;
            float fromStartX = nx - touchStartX, fromStartY = ny - touchStartY;
            if (!touchMoved && std::sqrt(fromStartX * fromStartX + fromStartY * fromStartY) > kTouchSlop &&
                !longPressActive) {
                touchMoved = true;
                pan.active = true;
                pan.began = true;
                pan.owner = 0;
                pan.startX = touchStartX;
                pan.startY = touchStartY;
                mdx = fromStartX;
                mdy = fromStartY;
            }
            if (pan.active) {
                pan.dx = mdx;
                pan.dy = mdy;
                if (dt > 0) {
                    float a = 1 - std::exp(-dt * 18.0f);
                    pan.vx += (mdx / dt - pan.vx) * a;
                    pan.vy += (mdy / dt - pan.vy) * a;
                }
            }
            if (!touchMoved && !longPressActive && time_ - downTime_ > kLongPressTime) {
                longPress = true;
                longPressActive = true;
            }
        }
        touchX = nx;
        touchY = ny;
        pan.x = nx;
        pan.y = ny;
    } else if (wasTouching_) {
        touching = false;
        float touchTime = time_ - downTime_;
        if (pan.active) {
            pan.active = false;
            pan.ended = true;
        } else if (longPressActive) {
            longPressActive = false;
            longPressEnded = true;
        } else if (!gestureCancelled_ && !touchMoved && touchTime < kTapMaxTime) {
            tap = true;
            tapX = touchX;
            tapY = touchY;
            float ddx = tapX - lastTapX_, ddy = tapY - lastTapY_;
            if (time_ - lastTapTime_ < kDoubleTapWindow + kTapMaxTime &&
                std::sqrt(ddx * ddx + ddy * ddy) < kDoubleTapRadius) {
                doubleTap = true;
                pendingSingle_ = false;
                lastTapTime_ = -10;
            } else {
                pendingSingle_ = true;
                pendingTime_ = time_;
                pendingX_ = tapX;
                pendingY_ = tapY;
                lastTapTime_ = time_;
                lastTapX_ = tapX;
                lastTapY_ = tapY;
            }
        }
        gestureCancelled_ = false;
    }

    if (pendingSingle_ && !touching && time_ - pendingTime_ > kDoubleTapWindow) {
        pendingSingle_ = false;
        tapConfirmed = true;
        confirmedX = pendingX_;
        confirmedY = pendingY_;
    }
    // A second touch that turns into a drag or pinch cancels the pending tap.
    if (pendingSingle_ && (pan.active || pinch.active || longPressActive)) pendingSingle_ = false;

    wasTouching_ = isTouching;
    prevTouches_ = n;
}
