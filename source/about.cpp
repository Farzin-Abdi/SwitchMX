// Settings → About: the app, how to reach its author, and ways to support it (with QR codes).
#include <cmath>
#include <cstring>

#include "app.hpp"
#include "qr.hpp"

namespace {
constexpr uint32_t G_LIST = 3;  // same focus group as home.cpp

struct Donation {
    const char* name;     // what it is
    const char* ticker;   // on the badge
    const char* network;  // where to send it
    const char* address;
    const char* warning;  // shown with the large code
    unsigned color, color2;
};

// Each address was checked against its network's own checksum (bech32, base58, base58check and
// EIP-55) before going in here.
const Donation kDonations[] = {
    {"Bitcoin", "BTC", "Bitcoin network", "bc1qt43jxtsxg5uufs6nf6kz0hdmnrl2cyud3thwkv",
     "Send only Bitcoin (BTC) on the Bitcoin network to this address.", 0xF7931A, 0xFFB547},
    {"Solana", "SOL", "Solana network", "3FraMpj5Y5DTPVmGc1TEnfcujwnmaiZdbmYd1qUbAZNm",
     "Send only SOL or Solana tokens on the Solana network to this address.", 0x9945FF, 0x14F195},
    {"Tether (USDT)", "USDT", "Tron network · TRC-20", "TCy7Snky3gjGydMYjuJXzZb1WJgCYUsS1r",
     "Send only USDT on the Tron network (TRC-20). Other networks would be lost.", 0x26A17B, 0x50D2A6},
    {"Ethereum", "ETH", "Ethereum network · ETH and ERC-20", "0x79F64Fbe027d658c5727dE4c7fa59835E4eC1dB2",
     "Send only ETH or ERC-20 tokens on the Ethereum network to this address.", 0x627EEA, 0x9DB0FF},
};

const char* kGithub = "https://github.com/Farzin-Abdi/SwitchMX";
const char* kGithubShort = "github.com/Farzin-Abdi/SwitchMX";
const char* kEmail = "farzinabdi.debian@gmail.com";

// Long addresses on two lines, split in the middle.
void addressLines(UI& ui, float x, float y, const std::string& a, float size, NVGcolor c, int align = LEFT) {
    size_t half = (a.size() + 1) / 2;
    ui.text(x, y, a.substr(0, half), size, c, align, true);
    ui.text(x, y + size * 1.35f, a.substr(half), size, c, align, true);
}

void coinBadge(UI& ui, float cx, float cy, float r, const Donation& d) {
    nvgBeginPath(ui.vg);
    nvgCircle(ui.vg, cx, cy, r);
    nvgFillPaint(ui.vg, nvgLinearGradient(ui.vg, cx - r, cy - r, cx + r, cy + r, hex(d.color), hex(d.color2)));
    nvgFill(ui.vg);
    float size = strlen(d.ticker) > 3 ? r * 0.62f : r * 0.74f;
    ui.text(cx, cy, d.ticker, size, hex(0xFFFFFF), CENTER, true);
}

}  // namespace

void HomeScreen::openQr(const std::string& title, const std::string& text, const std::string& shown,
                        const std::string& note) {
    qrOpen_ = true;
    qrTitle_ = title;
    qrText_ = text;
    qrShown_ = shown;
    qrNote_ = note;
    qrTime_ = 0;
}

// Under the app's name: whether this is the newest release, and the way to get a newer one.
void HomeScreen::drawUpdatePill(float x, float y, float maxW, float contentTop, Rect list) {
    UI& ui = app.ui;
    Updater& up = app.updater;
    Updater::State st = up.state();
    if (st == Updater::Idle && !updateAsked_) {  // once per visit to the page
        updateAsked_ = true;
        up.check();
        st = up.state();
    }
    std::string label;
    Icon ic = Icon::Refresh;
    NVGcolor tint = ui.t.textDim;
    bool filled = false;
    switch (st) {
        case Updater::Idle:
        case Updater::Checking: label = "Checking for updates…"; break;
        case Updater::UpToDate:
            label = "You have the latest version";
            ic = Icon::Check;
            tint = ui.t.success;
            break;
        case Updater::Available:
            label = trf("Update to v{0}", {up.latest()});
            ic = Icon::Download;
            filled = true;
            break;
        case Updater::NoRelease: label = "No release published yet · check again"; break;
        case Updater::Failed:
            label = tr(up.message()) + " · " + tr("try again");
            ic = Icon::Wifi;
            tint = ui.t.warning;
            break;
        case Updater::Downloading: {
            float p = up.progress();
            label = p >= 0 ? tr("Downloading…") + " " + std::to_string((int)(p * 100)) + "%" : "Downloading…";
            ic = Icon::Download;
            break;
        }
        case Updater::Installed:
            label = trf("Updated to v{0}", {up.latest()}) + " · " + tr("restart to use it");
            ic = Icon::Power;
            tint = ui.t.success;
            filled = true;
            break;
        case Updater::DownloadFailed:
            label = tr(up.message()) + " · " + tr("try again");
            ic = Icon::Info;
            tint = ui.t.warning;
            break;
    }
    float h = 34, w = std::min(maxW, ui.textWidth(label, 15, true) + 56);
    Rect r{x, y, w, h};
    uint32_t id = uid("about.update");
    ui.focusable(id, r, 3);
    bool foc = ui.focused(id);
    if (foc && ui.focusMoved) settingsScroll_.ensureVisible(contentTop - list.y + settingsScroll_.offset, r.bottom() - contentTop);
    float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
    float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
    NVGcolor bg = filled ? (st == Updater::Installed ? ui.t.success : ui.t.accent) : withAlpha(tint, 0.14f + 0.1f * pv);
    ui.fillRect(r, bg, h / 2);
    if (st == Updater::Downloading) {
        float p = std::max(0.0f, up.progress());
        ui.fillRect({r.x, r.y, r.w * p, r.h}, withAlpha(ui.t.accent, 0.45f), h / 2);
    }
    ui.focusRing(r, h / 2, fv);
    NVGcolor fg = filled ? ui.t.onAccent : (st == Updater::UpToDate ? ui.t.success : ui.t.text);
    if (st == Updater::Checking || st == Updater::Idle) ui.spinner(r.x + 20, r.cy(), 7, ui.t.accent, 2.5f);
    else ui.icon(ic, r.x + 20, r.cy(), 17, filled ? ui.t.onAccent : tint);
    ui.text(r.x + 38, r.cy(), ui.ellipsize(label, r.w - 50, 15, true), 15, fg, LEFT, true);
    if (!(ui.tapped(r) || (foc && ui.confirm()))) return;
    switch (st) {
        case Updater::Available: {
            std::string notes = up.notes();
            if (notes.size() > 600) notes = notes.substr(0, 600) + "…";
            app.confirm(trf("Update to v{0}?", {up.latest()}),
                        (notes.empty() ? std::string() : notes + "\n\n") +
                            tr("It downloads from GitHub and replaces this copy of SwitchMX. Your settings are kept."),
                        "Download and install", [this] { app.updater.install(app.nroPath()); });
            break;
        }
        case Updater::Downloading:
            app.confirm("Stop the download?", "", "Stop", [this] { app.updater.cancel(); }, true);
            break;
        case Updater::Installed:
            app.confirm("Restart SwitchMX?", "Close it now and open it again to use the new version.", "Close SwitchMX",
                        [this] { app.running = false; });
            break;
        case Updater::DownloadFailed: app.updater.install(app.nroPath()); break;
        case Updater::Checking:
        case Updater::Idle: break;
        default: app.updater.check(); break;
    }
}

float HomeScreen::drawAbout(Rect list, float y) {
    UI& ui = app.ui;
    NVGcontext* vg = ui.vg;
    const float M = 6;
    float x = list.x + M, w = list.w - 2 * M;
    float contentTop = y;  // where the page starts, for scrolling all the way back up
    auto card = [&](Rect r, uint32_t id, bool focusable, bool firstRow = false) {
        bool foc = false;
        if (focusable) {
            ui.focusable(id, r, G_LIST);
            foc = ui.focused(id);
            if (foc && ui.focusMoved) {
                // The first row also brings the app card above it back into view: it can't take the
                // focus itself, so moving up would otherwise never reach it.
                float top = firstRow ? contentTop : r.y;
                settingsScroll_.ensureVisible(top - list.y + settingsScroll_.offset, r.bottom() - top);
            }
        }
        float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
        float pv = focusable ? ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25) : 0;
        ui.fillRect(r, withAlpha(mix(ui.t.surface, ui.t.surface2, pv), 0.85f), ui.t.radius + 4);
        ui.focusRing(r, ui.t.radius + 4, fv);
        return focusable && (ui.tapped(r) || (foc && ui.confirm()));
    };

    // ---- The app
    {
        Rect r{x, y, w, 168};
        card(r, uid("about.hero"), false);
        // A soft light behind the logo.
        nvgBeginPath(vg);
        nvgRoundedRect(vg, r.x, r.y, r.w, r.h, ui.t.radius + 4);  // inside the card's corners
        float glow = 0.5f + 0.5f * std::sin(ui.time * 1.4f);
        nvgFillPaint(vg, nvgRadialGradient(vg, r.x + 84, r.cy(), 10, 150, withAlpha(ui.t.accent, 0.18f + 0.08f * glow),
                                           withAlpha(ui.t.accent, 0)));
        nvgFill(vg);
        ui.beginUnmirrored({r.x + 32, r.cy() - 52, 104, 104});
        drawLogo(vg, r.x + 32, r.cy() - 52, 104, true, ui.time);
        ui.endUnmirrored();
        float tx = r.x + 164;
        ui.text(tx, r.y + 48, "SwitchMX", 32, ui.t.text, LEFT, true);
        float vw = ui.textWidth("v" SWITCHMX_VERSION, 13, true) + 20;
        float nameW = ui.textWidth("SwitchMX", 32, true);
        Rect chip{tx + nameW + 12, r.y + 36, vw, 24};
        ui.fillRect(chip, withAlpha(ui.t.accent, 0.2f), 12);
        ui.text(chip.cx(), chip.cy(), "v" SWITCHMX_VERSION, 13, ui.t.accent, CENTER, true);
        float tw = r.right() - 24 - tx;
        ui.text(tx, r.y + 84, ui.ellipsize("A touch-first media player for films, music and pictures", tw, 17), 17, ui.t.textDim);
        ui.text(tx, r.y + 114, "Made by Farzin Abdi", 16, ui.t.text, LEFT, true);
        drawUpdatePill(tx, r.y + 124, tw, contentTop, list);
        y = r.bottom() + 14;
    }

    // ---- Contact
    {
        ui.text(x + 6, y + 12, "CONTACT", 13, ui.t.textFaint, LEFT, true);  // short: always fits
        y += 30;
        float cw = (w - 14) / 2, ch = 132;
        struct Contact { Icon ic; const char* label; const char* shown; const char* qr; const char* key; };
        Contact cs[] = {{Icon::Link, "GitHub", kGithubShort, kGithub, "about.github"},
                        {Icon::Mail, "Email", kEmail, nullptr, "about.mail"}};
        std::string mailto = std::string("mailto:") + kEmail;
        for (int i = 0; i < 2; i++) {
            Rect r{x + i * (cw + 14), y, cw, ch};
            const char* qrText = cs[i].qr ? cs[i].qr : mailto.c_str();
            bool hit = card(r, uid(cs[i].key), true, true);
            Rect q{r.right() - 16 - (ch - 32), r.y + 16, ch - 32, ch - 32};
            ui.beginUnmirrored(q);
            drawQr(vg, q, qrText, 8);
            ui.endUnmirrored();
            float lx = r.x + 22;
            nvgBeginPath(vg);
            nvgCircle(vg, lx + 18, r.y + 36, 18);
            nvgFillColor(vg, withAlpha(ui.t.accent, 0.18f));
            nvgFill(vg);
            ui.icon(cs[i].ic, lx + 18, r.y + 36, 20, ui.t.accent);
            ui.text(lx + 48, r.y + 36, cs[i].label, 18, ui.t.text, LEFT, true);
            ui.text(lx, r.y + 78, ui.ellipsize(cs[i].shown, q.x - lx - 12, 16, true), 16, ui.t.text, LEFT, true);
            ui.text(lx, r.y + 104, ui.ellipsize("Tap to show the code larger", q.x - lx - 12, 13), 13, ui.t.textFaint);
            if (hit) openQr(cs[i].label, qrText, cs[i].shown, cs[i].qr ? "Scan with your phone's camera to open it."
                                                                        : "Scan to write an email.");
        }
        y += ch + 14;
    }

    // ---- Donations
    {
        // "Show some love": a beating heart, and the cards below in the same spirit.
        float beat = std::pow(std::max(0.0f, std::sin(ui.time * 3.2f)), 8.0f);
        Rect band{x, y, w, 88};
        ui.fillRect(band, withAlpha(ui.t.danger, 0.10f), ui.t.radius + 4);
        nvgBeginPath(vg);
        nvgRect(vg, band.x, band.y, band.w, band.h);
        nvgFillPaint(vg, nvgRadialGradient(vg, band.x + 50, band.cy(), 4, 90, withAlpha(ui.t.danger, 0.22f + 0.12f * beat),
                                           withAlpha(ui.t.danger, 0)));
        nvgFill(vg);
        float hs = 38 * (1 + 0.12f * beat);
        ui.icon(Icon::HeartFilled, band.x + 50, band.cy(), hs, ui.t.danger);
        float bw = band.right() - 20 - (band.x + 92);
        ui.text(band.x + 92, band.y + 28, ui.ellipsize("Show SwitchMX some love", bw, 22, true), 22, ui.t.text, LEFT, true);
        // Two lines when it doesn't fit on one.
        ui.textBox(band.x + 92, band.y + 46, bw,
                   "Made with love in spare time. If it makes your Switch better, a gift helps it grow. Thank you!", 14,
                   ui.t.textDim);
        y = band.bottom() + 14;
        float cw = (w - 14) / 2, ch = 176;
        for (int i = 0; i < 4; i++) {
            const Donation& d = kDonations[i];
            Rect r{x + (i % 2) * (cw + 14), y + (i / 2) * (ch + 14), cw, ch};
            bool hit = card(r, uid("about.donate", i), true);
            // A thin band of the coin's colour along the top.
            nvgSave(vg);
            nvgIntersectScissor(vg, r.x, r.y, r.w, 4);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, r.x, r.y, r.w, 20, ui.t.radius + 4);
            nvgFillPaint(vg, nvgLinearGradient(vg, r.x, 0, r.right(), 0, hex(d.color), hex(d.color2)));
            nvgFill(vg);
            nvgRestore(vg);
            Rect q{r.x + 16, r.y + 18, ch - 34, ch - 34};
            ui.beginUnmirrored(q);
            drawQr(vg, q, d.address, 8);
            ui.endUnmirrored();
            float lx = q.right() + 18, lw = r.right() - 16 - lx;
            coinBadge(ui, lx + 18, r.y + 38, 18, d);
            ui.text(lx + 46, r.y + 32, ui.ellipsize(d.name, lw - 46, 18, true), 18, ui.t.text, LEFT, true);
            ui.text(lx + 46, r.y + 52, ui.ellipsize(d.network, lw - 46, 13), 13, ui.t.textDim);
            // The address in full, on two lines, small enough to fit.
            float as = 13.5f;
            size_t half = (strlen(d.address) + 1) / 2;
            while (as > 10 && ui.textWidth(std::string(d.address).substr(0, half), as, true) > lw) as -= 0.5f;
            addressLines(ui, lx, r.y + 92, d.address, as, ui.t.text);
            ui.icon(Icon::HeartFilled, lx + 7, r.y + 146, 13, withAlpha(ui.t.danger, 0.85f));
            ui.text(lx + 20, r.y + 146, ui.ellipsize("Send some love · tap for a larger code", lw - 20, 13), 13, ui.t.textFaint);
            if (hit) openQr(std::string(d.name) + " · " + d.network, d.address, d.address, d.warning);
        }
        y += 2 * ch + 14 + 14;
    }
    return y;
}

void HomeScreen::drawQrViewer() {
    UI& ui = app.ui;
    NVGcontext* vg = ui.vg;
    qrTime_ += ui.dt;
    float a = std::min(1.0f, qrTime_ / 0.18f);
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, ui.W, ui.H);
    nvgFillColor(vg, hex(0, 0.72f * a));
    nvgFill(vg);
    ui.pushAlpha(a);
    float qs = std::min(330.0f, ui.H - 290);
    float w = std::min(620.0f, ui.W - 80), h = 28 + 30 + 16 + qs + 20 + 50 + 30 + 50 + 28;
    Rect card{ui.W / 2 - w / 2, ui.H / 2 - h / 2 + 12 * (1 - a), w, h};
    ui.panel(card, 1, ui.t.radius + 10);
    float y = card.y + 28;
    ui.text(card.cx(), y + 12, ui.ellipsize(qrTitle_, w - 56, 22, true), 22, ui.t.text, CENTER, true);
    y += 30 + 16;
    Rect q{card.cx() - qs / 2, y, qs, qs};
    ui.beginUnmirrored(q);
    drawQr(vg, q, qrText_, 14);
    ui.endUnmirrored();
    y = q.bottom() + 20;
    // The address or link in full, large and on two lines when long.
    float size = 19;
    if (ui.textWidth(qrShown_, size, true) <= w - 60) {
        ui.text(card.cx(), y + 12, qrShown_, size, ui.t.text, CENTER, true);
    } else {
        size_t half = (qrShown_.size() + 1) / 2;
        while (size > 13 && ui.textWidth(qrShown_.substr(0, half), size, true) > w - 60) size -= 0.5f;
        addressLines(ui, card.cx(), y + 4, qrShown_, size, ui.t.text, CENTER);
    }
    y += 50;
    ui.text(card.cx(), y + 8, ui.ellipsize(qrNote_, w - 56, 15), 15, ui.t.warning, CENTER);
    y += 30;
    Rect b{card.cx() - 90, card.bottom() - 28 - 50, 180, 50};
    ui.setDefaultFocus(uid("qr.close"));
    bool close = ui.button(uid("qr.close"), b, "Close", Icon::Close, UI::BTN_TONAL) || ui.back();
    // A tap outside the card closes it too.
    if (!close && ui.in->tap && !card.contains(ui.in->tapX, ui.in->tapY)) {
        ui.in->consumeTap();
        close = true;
    }
    ui.popAlpha();
    if (close) qrOpen_ = false;
}
