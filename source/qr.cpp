#include "qr.hpp"

#include <unordered_map>
#include <vector>

extern "C" {
#include "qrcodegen.h"
}

namespace {
struct Code {
    int size = 0;
    std::vector<unsigned char> dark;
    int image = 0;  // the modules as a texture, one pixel each (made on first draw)
    NVGcontext* vg = nullptr;
};

const Code& codeFor(const std::string& text) {
    static std::unordered_map<std::string, Code> cache;
    auto it = cache.find(text);
    if (it != cache.end()) return it->second;
    Code c;
    std::vector<uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), tmp(qrcodegen_BUFFER_LEN_MAX);
    // Medium error correction: survives glare on the screen and still keeps the modules large.
    if (qrcodegen_encodeText(text.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                             qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        c.size = qrcodegen_getSize(qr.data());
        c.dark.resize((size_t)c.size * c.size);
        for (int y = 0; y < c.size; y++)
            for (int x = 0; x < c.size; x++) c.dark[(size_t)y * c.size + x] = qrcodegen_getModule(qr.data(), x, y);
    }
    return cache.emplace(text, std::move(c)).first->second;
}
}  // namespace

void drawQr(NVGcontext* vg, Rect r, const std::string& text, float radius) {
    const Code& c = codeFor(text);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, r.x, r.y, r.w, r.h, radius);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgFill(vg);
    if (!c.size) return;
    const int quiet = 3;  // modules of white border (4 is the standard; 3 plus the card's margin is plenty)
    float m = std::min(r.w, r.h) / (c.size + 2 * quiet);
    float ox = r.x + (r.w - m * c.size) / 2, oy = r.y + (r.h - m * c.size) / 2;
    // A texture of one pixel per module, scaled up without smoothing: drawing each module as a
    // shape cost most of a CPU core on the About page (four codes, every frame).
    Code& cc = const_cast<Code&>(c);
    if (!cc.image || cc.vg != vg) {
        std::vector<unsigned char> px((size_t)c.size * c.size * 4);
        for (size_t i = 0; i < (size_t)c.size * c.size; i++) {
            unsigned char v = c.dark[i] ? 12 : 255;
            px[i * 4] = v;
            px[i * 4 + 1] = v;
            px[i * 4 + 2] = c.dark[i] ? 20 : 255;
            px[i * 4 + 3] = 255;
        }
        cc.image = nvgCreateImageRGBA(vg, c.size, c.size, NVG_IMAGE_NEAREST, px.data());
        cc.vg = vg;
    }
    float side = m * c.size;
    nvgBeginPath(vg);
    nvgRect(vg, ox, oy, side, side);
    nvgFillPaint(vg, nvgImagePattern(vg, ox, oy, side, side, 0, cc.image, 1));
    nvgFill(vg);
}
