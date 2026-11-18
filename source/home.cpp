#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include <cstring>

#include "app.hpp"
#include "netfs.hpp"
#include "platform.hpp"
#include "qr.hpp"
#include "i18n.hpp"

namespace {
constexpr float SIDEBAR_W = 250;
constexpr float ROW_H = 78;
constexpr uint32_t G_SIDEBAR = 1, G_HEADER = 2, G_LIST = 3, G_TABS = 4;

struct NavItem {
    const char* label;
    Icon icon;
};
const NavItem kNav[] = {{"Files", Icon::SdCard},
                        {"Recent", Icon::Clock},
                        {"Favorites", Icon::Star},
                        {"Playlists", Icon::Playlist},
                        {"Streams", Icon::Globe},
                        {"Network", Icon::Server},
                        {"File Manager", Icon::Folder},
                        {"Settings", Icon::Gear}};

// Files and the File Manager are one place in two views (media / manage): the sidebar shows one
// row for both, and section switching skips the manage view.
int navRow(int section) { return section < HomeScreen::MANAGER ? section : section == HomeScreen::MANAGER ? 0 : section - 1; }
constexpr int kNavRows = HomeScreen::SECTION_COUNT - 1;

const Icon kSettingsIcons[] = {Icon::Palette, Icon::Play, Icon::Gesture, Icon::Captions,
                               Icon::Gamepad, Icon::Chip,  Icon::Waveform, Icon::Info};

bool dirExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string relativeDate(time_t t) {
    if (!t) return "";
    time_t now = time(nullptr);
    long d = (long)((now - t) / 86400);
    if (now - t < 3600) return "Just now";
    if (d < 1) return "Today";
    if (d < 2) return "Yesterday";
    if (d < 7) return std::to_string(d) + " days ago";
    return formatDate(t);
}

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

std::string dateString() {
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    if (!tm) return "";
    static const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* months[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
    // "Saturday, 7 November" in the order and digits of the interface language.
    return trf("{0}, {1} {2}", {tr(days[tm->tm_wday % 7]), i18n::localDigits(std::to_string(tm->tm_mday)),
                                tr(months[tm->tm_mon % 12])});
}

NVGcolor kindColor(const UI& ui, MediaKind k) {
    switch (k) {
        case MediaKind::Archive: return hex(0xF0943C);
        case MediaKind::Text: return hex(0x8FA3BF);
        case MediaKind::Audio: return hex(0xB06CFF);
        case MediaKind::Playlist: return hex(0x16C4A8);
        case MediaKind::Subtitle: return hex(0xF5A524);
        case MediaKind::Image: return hex(0x33B5FF);
        case MediaKind::Video: return ui.t.accent;
        case MediaKind::Document: return hex(0xEF5350);
        default: return ui.t.textDim;
    }
}

Icon kindIcon(MediaKind k) {
    switch (k) {
        case MediaKind::Folder: return Icon::Folder;
        case MediaKind::Video: return Icon::Film;
        case MediaKind::Audio: return Icon::Music;
        case MediaKind::Playlist: return Icon::Playlist;
        case MediaKind::Subtitle: return Icon::Captions;
        case MediaKind::Image: return Icon::Image;
        case MediaKind::Archive: return Icon::Archive;
        case MediaKind::Document: return Icon::Book;
        default: return Icon::File;
    }
}

FileEntry entryFromPath(const std::string& p) {
    FileEntry e;
    e.path = p;
    e.name = displayName(p);
    if (!p.empty() && p.back() == '/') {
        e.kind = MediaKind::Folder;
    } else {
        e.ext = fileExtension(p);
        e.kind = isUrl(p) ? MediaKind::Video : kindForExtension(e.ext);
        struct stat st;
        if (stat(p.c_str(), &st) == 0) {
            e.size = st.st_size;
            e.mtime = st.st_mtime;
        }
    }
    return e;
}
}  // namespace

// ------------------------------------------------------------------ lifecycle

void HomeScreen::listsChanged() {
    listRevision_ = app.lib.revision - 1;
    favRevision_ = app.lib.revision - 1;
}

void HomeScreen::enter() {
    listsChanged();
    // The saved folder is only for the app's start. Later, an empty dir is a page of its own (the
    // Storage page, or Network's server list): returning from the player must not jump elsewhere.
    if (dir_.empty() && !openedOnce_ && section_ != NETWORK) {
        openedOnce_ = true;
        std::string root = platform::rootDir();
        std::string start = app.cfg.lastDir;
        if (start.empty() || start.rfind(root, 0) != 0 || !dirExists(start)) start = root;
        openDir(start, false);
    } else {
        openedOnce_ = true;
        refresh();
    }
    // Back from the player: highlight the file that was playing last (the queue may have moved on
    // from the one that was opened) and bring it into view.
    if (!app.lastPlayed.empty() && (section_ == FILES || section_ == MANAGER || section_ == NETWORK) && !dir_.empty() &&
        parentDir(app.lastPlayed) == dir_) {
        for (auto& e : entries_)
            if (e.path == app.lastPlayed) {
                pendingFocus_ = uid(e.path.c_str(), 77);
                app.ui.setFocus(pendingFocus_);
                dirFocus_[dir_] = e.path;
                break;
            }
    }
    app.lastPlayed.clear();
    scroll_.id = uid("home.scroll");
    settingsScroll_.id = uid("settings.scroll");
}

void HomeScreen::setSection(Section s) {
    if (section_ == SETTINGS && s != SETTINGS) app.cfg.save();
    if (s == section_) return;
    // Network keeps its own folder: going in or out swaps it with the Files / File Manager one.
    if (section_ == NETWORK && s != NETWORK) {
        netPlace_ = dir_;
        dir_ = localDir_ != "\x01" ? localDir_ : (app.cfg.lastDir.empty() ? "sdmc:/" : app.cfg.lastDir);
    } else if (s == NETWORK && section_ != NETWORK) {
        localDir_ = dir_;
        dir_ = netPlace_;
    }
    section_ = s;
    sectionTime_ = 0;
    scroll_.reset();
    settingsScroll_.reset();
    listTime_ = 0;
    searching_ = false;
    endSelect();
    playlistOpen_ = kNoList;
    listsChanged();
    if (s == FILES || s == MANAGER || s == NETWORK) refresh();
}

void HomeScreen::refresh() {
    if (dir_.empty()) {
        // The Storage page: SD card plus any mounted USB drives; in Network, the servers instead.
        entries_.clear();
        for (auto& r : platform::storageRoots(true)) {
            if (section_ == NETWORK) break;
            FileEntry e;
            e.path = r.path;
            e.name = r.label;
            e.kind = MediaKind::Folder;
            e.isDrive = true;
            e.removable = r.removable;
            e.detail = r.detail;
            entries_.push_back(e);
        }
        // Network: the saved servers, and a tile to add one.
        if (section_ == NETWORK) {
            auto& list = netfs::servers();
            for (size_t i = 0; i < list.size(); i++) {
                FileEntry e;
                e.path = netfs::rootUrl(list[i]);
                e.name = list[i].name;
                e.kind = MediaKind::Folder;
                e.isDrive = true;
                e.server = (int)i;
                e.detail = (list[i].type == "smb" ? "SMB · " : "FTP · ") + list[i].host;
                entries_.push_back(e);
            }
            FileEntry add;
            add.path = "netfs:add";
            add.name = tr("Add network server");
            add.kind = MediaKind::Folder;
            add.isDrive = true;
            add.addServer = true;
            add.detail = tr("Shared folders (SMB) or FTP");
            entries_.push_back(add);
        }
        return;
    }
    if (searching_) return;  // search results stay until the user leaves them
    if (netDir()) {
        startNetListing(false);
        return;
    }
    entries_ = app.lib.list(dir_, app.cfg.s.showHidden, managerMode() ? false : app.cfg.s.mediaOnly != 0,
                            app.cfg.s.sortMode);
}

void HomeScreen::openDir(const std::string& dir, bool remember) {
    if (remember && !dir_.empty()) {
        for (auto& e : entries_)
            if (app.ui.focus == uid(e.path.c_str(), 77)) dirFocus_[dir_] = e.path;
    }
    dir_ = dir;
    if (!dir_.empty() && dir_.back() != '/') dir_ += '/';
    refresh();
    scroll_.reset();
    listTime_ = 0;
    if (!dir_.empty() && !netDir() && app.cfg.lastDir != dir_) {
        app.cfg.lastDir = dir_;
        app.saveSoon();  // the place to come back to, even if the app is closed from HOME
    }
    pendingFocus_ = 0;
    auto it = dirFocus_.find(dir_);
    if (it != dirFocus_.end()) pendingFocus_ = uid(it->second.c_str(), 77);
    else if (!entries_.empty()) pendingFocus_ = uid(entries_[0].path.c_str(), 77);
    if (pendingFocus_) app.ui.setFocus(pendingFocus_);
}

void HomeScreen::drivesChanged() {
    listsChanged();  // files in playlists may have come back or gone away
    auto roots = platform::storageRoots(false);
    if (dir_.empty()) {
        refresh();
        return;
    }
    for (auto& r : roots)
        if (dir_.rfind(r.path, 0) == 0) return;
    // The drive being browsed was unplugged.
    openDir(roots.size() == 1 ? roots[0].path : "", false);
}

void HomeScreen::goUp() {
    auto roots = platform::storageRoots(false);
    bool atRoot = dir_.empty();
    for (auto& r : roots)
        if (dir_ == r.path) atRoot = true;
    if (netDir()) {  // the folder a server opens in, or the server's top: back to the Storage page
        int i = netfs::serverFor(dir_);
        if ((i >= 0 && dir_ == netfs::rootUrl(netfs::servers()[i])) || parentDir(dir_).find('/', dir_.find("://") + 3) == std::string::npos)
            atRoot = true;
    }
    if (atRoot) {
        // The Storage page when there's more than one drive; in Network, the list of servers.
        if (!dir_.empty() && (roots.size() > 1 || section_ == NETWORK)) {
            dirFocus_[""] = dir_;
            openDir("", false);
        } else {
            app.ui.setFocus(uid("nav", (int)FILES));
        }
        return;
    }
    std::string child = dir_;
    std::string parent = parentDir(dir_);
    dirFocus_[parent] = child;
    openDir(parent, false);
}

// ------------------------------------------------------------------ frame

void HomeScreen::frame() {
    UI& ui = app.ui;
    sectionTime_ += ui.dt;
    listTime_ += ui.dt;
    bool parentBlocked = ui.blocked;
    ui.blocked = parentBlocked || job_ || textOpen_ || transferOpen_ || qrOpen_ || setupOpen_;
    ui.background(app.cfg.s.background);
    drawSidebar();

    Rect content{SIDEBAR_W + 32, 24, ui.W - SIDEBAR_W - 64, ui.H - 24 - 64};
    float a = ui.ease(sectionTime_ / 0.3f);
    if (ui.animMul <= 0) a = 1;
    nvgSave(ui.vg);
    ui.pushAlpha(a);
    nvgTranslate(ui.vg, 0, 18 * (1 - a));
    switch (section_) {
        case FILES:
        case NETWORK: drawFiles(content); break;
        case RECENT: drawRecent(content); break;
        case FAVORITES: drawFavorites(content); break;
        case PLAYLISTS: drawPlaylists(content); break;
        case STREAMS: drawStreams(content); break;
        case MANAGER: drawFiles(content); break;
        case SETTINGS: drawSettings(content); break;
        default: break;
    }
    ui.popAlpha();
    nvgRestore(ui.vg);
    drawHints();

    ui.blocked = parentBlocked;
    if (!parentBlocked) {
        if (textOpen_) drawTextViewer();
        if (job_) drawJob();
        else if (transferOpen_) drawTransfer();
        else if (qrOpen_) drawQrViewer();
        else if (setupOpen_) drawSetup();
    }
    if (ui.blocked || job_ || textOpen_ || transferOpen_ || qrOpen_ || setupOpen_) return;

    // Global buttons
    Input& in = *ui.in;
    if (in.down(BTN_PLUS)) app.running = false;
    bool managerUsesR = (section_ == MANAGER || section_ == FILES) && (!clipboard_.empty() || !extractPending_.empty()) &&
                        in.down(BTN_R);
    if (managerUsesR && section_ == FILES) paste();
    int cur = section_ == MANAGER ? (int)FILES : (int)section_;  // the manage view counts as Files
    if (section_ != SETTINGS && !managerUsesR && (in.down(BTN_L) || in.down(BTN_R))) {
        int s = cur + (in.down(BTN_R) ? 1 : -1);
        if (s == MANAGER) s += in.down(BTN_R) ? 1 : -1;
        if (s >= 0 && s < (int)SETTINGS) {
            setSection((Section)s);
            ui.setFocus(uid("nav", s));
        }
    }
    if (in.down(BTN_ZL) || in.down(BTN_ZR)) {
        int step = in.down(BTN_ZR) ? 1 : SECTION_COUNT - 1;
        int s = (cur + step) % SECTION_COUNT;
        if (s == MANAGER) s = (s + step) % SECTION_COUNT;
        setSection((Section)s);
        ui.setFocus(uid("nav", s));
    }
    if (ui.peekBack()) {
        ui.back();
        if ((section_ == MANAGER || section_ == FILES) && managerBack()) return;
        if (section_ == FILES || section_ == MANAGER || section_ == NETWORK) goUp();
        else if ((section_ == PLAYLISTS || section_ == FAVORITES) && playlistOpen_ != kNoList) openPlaylist(kNoList);
        else ui.setFocus(uid("nav", (int)section_));
    }
}

void HomeScreen::drawSidebar() {
    UI& ui = app.ui;
    NVGcontext* vg = ui.vg;
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, SIDEBAR_W, ui.H);
    nvgFillColor(vg, withAlpha(ui.t.surface, ui.t.light ? 0.7f : 0.5f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, SIDEBAR_W - 1, 0, 1, ui.H);
    nvgFillColor(vg, ui.t.border);
    nvgFill(vg);

    // Logo
    Rect logo{24, 30, 44, 44};
    ui.shadow(logo, 12, 16, 0.35f, 5);
    ui.beginUnmirrored(logo);
    drawLogo(vg, logo.x, logo.y, logo.w);
    ui.endUnmirrored();
    // The name in the logo's face (drawn directly: ui.text only knows the text fonts).
    // Kept readable when the layout is mirrored for right-to-left languages.
    nvgFontFace(vg, "display");
    nvgFontSize(vg, 19);
    nvgTextLetterSpacing(vg, 2.5f);
    float nameW = nvgTextBounds(vg, 0, 0, "SWITCHMX", nullptr, nullptr);
    ui.beginUnmirrored({80, 30, nameW, 30});
    nvgFontFace(vg, "display");
    nvgFontSize(vg, 19);
    nvgTextLetterSpacing(vg, 2.5f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, ui.t.text);
    nvgText(vg, 80, 45, "SWITCHMX", nullptr);
    ui.endUnmirrored();
    // After the un-mirroring, not before: in a right-to-left language endUnmirrored() restores the
    // canvas state saved above, which would otherwise bring the wide spacing back and stretch every
    // word drawn after this one.
    nvgTextLetterSpacing(vg, 0);
    ui.text(80, 66, "Media Player", 14, ui.t.textFaint);

    // Navigation
    // Large text leaves the sidebar short: then the date is left out and the rows close up, so
    // Wi-Fi / Exit always keep a clear gap above the clock.
    bool compact = ui.H < 620;
    float y0 = compact ? 92 : 104;
    float clockBlock = compact ? 74 : 112;  // clock (and date) at the bottom, with its margin
    const float pillGap = 12;  // extra room between Settings and the Wi-Fi / Exit buttons
    float step = std::min(58.0f, (ui.H - y0 - clockBlock - 18 - 14 - pillGap) / (kNavRows + 1));
    float gap = 4, itemH = step - gap;
    auto itemY = [&](int row) { return y0 + row * step + (row >= navRow(SETTINGS) ? 18 : 0); };
    float selY = ui.smooth(uid("nav.sel"), itemY(navRow(section_)), 16);
    Rect sel{14, selY, SIDEBAR_W - 28, itemH};
    ui.fillRect(sel, withAlpha(ui.t.accent, ui.t.light ? 0.14f : 0.16f));
    // Inset so the focus outline, drawn on the item's edge, never runs over it.
    ui.fillRect({sel.x + 8, sel.y + 15, 4, sel.h - 30}, ui.t.accent, 2);

    // Separator above settings
    nvgBeginPath(vg);
    nvgRect(vg, 30, itemY(navRow(SETTINGS)) - 11, SIDEBAR_W - 60, 1);
    nvgFillColor(vg, ui.t.border);
    nvgFill(vg);

    for (int i = 0; i < SECTION_COUNT; i++) {
        if (i == MANAGER) continue;  // a view of Files (its header button)
        Rect r{14, itemY(navRow(i)), SIDEBAR_W - 28, itemH};
        uint32_t id = uid("nav", i);
        ui.focusable(id, r, G_SIDEBAR);
        bool active = i == section_ || (i == FILES && section_ == MANAGER);
        bool foc = ui.focused(id) && ui.showFocus();
        float fv = ui.smooth(id ^ 0xf0c, foc ? 1.0f : 0.0f, 16);
        if (ui.pressedVisual(r)) ui.fillRect(r, withAlpha(ui.t.surfaceHi, 0.7f));
        ui.focusRing(r, ui.t.radius, fv);
        NVGcolor c = active ? ui.t.accent : ui.t.textDim;
        Icon ic = kNav[i].icon;
        if (i == FAVORITES && active) ic = Icon::StarFilled;
        ui.icon(ic, r.x + 30, r.cy(), 24, c);
        float labelW = r.w - 56 - (i == RECENT && !app.lib.history.empty() ? 52 : 12);
        ui.text(r.x + 56, r.cy(), ui.ellipsize(kNav[i].label, labelW, 19, active), 19, active ? ui.t.text : ui.t.textDim,
                LEFT, active);
        if (i == RECENT && !app.lib.history.empty()) {
            std::string n = std::to_string(std::min<size_t>(app.lib.history.size(), 99));
            float w = std::max(26.0f, ui.textWidth(n, 13, true) + 14);
            ui.fillRect({r.right() - w - 12, r.cy() - 11, w, 22}, withAlpha(ui.t.textFaint, 0.25f), 11);
            ui.text(r.right() - 12 - w / 2, r.cy(), n, 13, ui.t.textDim, CENTER, true);
        }
        if (ui.tapped(r) || (ui.focused(id) && ui.confirm())) {
            if (!(i == FILES && section_ == MANAGER)) setSection((Section)i);  // Files keeps its view
            ui.setFocus(id);
        }
    }

    // Wi-Fi transfer and Exit: two rounded buttons side by side under Settings, big enough to tap
    // (without a controller there is no + button), a little apart from the list above.
    {
        float rowY = itemY(kNavRows) + pillGap, gap = 10, half = (SIDEBAR_W - 28 - gap) / 2;
        float ph = std::min(itemH, 46.0f);
        Rect wr{14, rowY + (itemH - ph) / 2, half, ph}, er{14 + half + gap, wr.y, half, ph};
        bool on = app.web.running();
        auto pill = [&](Rect r, uint32_t id, Icon ic, const char* label, NVGcolor tint, bool lit) {
            ui.focusable(id, r, G_SIDEBAR);
            float fv = ui.smooth(id ^ 0xf0c, ui.focused(id) && ui.showFocus() ? 1.0f : 0.0f, 16);
            float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
            NVGcolor bg = lit ? withAlpha(tint, 0.18f + 0.1f * pv) : withAlpha(mix(ui.t.surface2, tint, 0.25f * pv), 0.55f + 0.3f * pv);
            ui.fillRect(r, bg, ph / 2);
            ui.strokeRect(r, withAlpha(lit ? tint : ui.t.border, lit ? 0.5f : 0.9f), 1, ph / 2);
            ui.focusRing(r, ph / 2, fv);
            NVGcolor fg = lit ? ui.t.text : ui.t.textDim;
            std::string shown = ui.ellipsize(label, r.w - 48, 16, true);
            float tw = ui.textWidth(shown, 16, true), total = 20 + 8 + tw;
            float cx = r.cx() - total / 2;
            ui.icon(ic, cx + 10, r.cy(), 20, lit ? tint : ui.t.textDim);
            ui.text(cx + 28, r.cy(), shown, 16, fg, LEFT, true);
            return ui.tapped(r) || (ui.focused(id) && ui.confirm());
        };
        uint32_t wid = uid("nav.wifi");
        if (pill(wr, wid, Icon::Wifi, "Wi-Fi", ui.t.success, on)) {
            transferOpen_ = true;
            ui.setFocus(uid("xfer.toggle"));
        }
        if (on) {  // a live dot in the corner
            float pulse = 0.5f + 0.5f * std::sin(ui.time * 3);
            nvgBeginPath(vg);
            nvgCircle(vg, wr.right() - 12, wr.y + 10, 3 + pulse);
            nvgFillColor(vg, withAlpha(ui.t.success, 0.6f + 0.4f * pulse));
            nvgFill(vg);
        }
        if (pill(er, uid("nav.exit"), Icon::Power, "Exit", ui.t.danger, false))
            app.openMenu("Exit SwitchMX?", {{"Exit", Icon::Power}, {"Cancel", Icon::Close}}, [this](int i) {
                if (i == 0) app.running = false;
            });
    }

    // Clock & battery
    float clockY = compact ? ui.H - 40 : ui.H - 86;
    ui.text(26, clockY, clockString(app.cfg.s.clock24), compact ? 30 : 34, ui.t.text, LEFT, true);
    if (!compact) ui.text(27, ui.H - 54, dateString(), 15, ui.t.textDim);
    int pct = platform::batteryPercent();
    if (pct >= 0) {
        float bx = SIDEBAR_W - 46;
        ui.battery(bx, clockY, pct, platform::charging());
        if (app.cfg.s.batteryPct) ui.text(bx - 8, clockY + 1, std::to_string(pct) + "%", 15, ui.t.textDim, RIGHT);
    }
}

// Factory reset: two confirmations, then everything SwitchMX saved is deleted and the app closes.
void HomeScreen::askFactoryReset() {
    app.confirm("Erase all SwitchMX data?",
                "Deletes your settings, watch history and resume points, playlists, likes, favourites, streams, "
                "cover art and caches, and logs. Your films, music and pictures are not touched, nor are your "
                "screenshots or the app itself.",
                "Continue", [this] {
                    app.confirm("Are you sure?", "This can't be undone. SwitchMX will close when it's done, and start "
                                                 "fresh next time.",
                                "Erase everything", [this] {
                                    std::string err;
                                    bool ok = app.eraseAllData(err);
                                    app.openMenu(ok ? "SwitchMX was reset" : "Reset finished with problems",
                                                 {{"Close SwitchMX", Icon::Power}},
                                                 [this](int) { app.running = false; },
                                                 ok ? "Everything it saved is gone. Open it again for a fresh start."
                                                    : err + ". " + tr("Open SwitchMX again; you can retry from Settings."));
                                }, true);
                }, true);
}

// First start: the language (default English), then a comfortable text size. Each choice applies at
// once, so the whole screen behind is the preview.
void HomeScreen::drawSetup() {
    UI& ui = app.ui;
    NVGcontext* vg = ui.vg;
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, ui.W, ui.H);
    nvgFillColor(vg, hex(0, 0.62f));
    nvgFill(vg);
    const float M = 28, gap = 12;
    float w = std::min(760.0f, ui.W - 48);
    const auto& langs = i18n::languages();
    const int cols = 4, rowsN = ((int)langs.size() + cols - 1) / cols;
    const float langH = 54;
    float bodyH = setupStep_ == 0 ? rowsN * langH + (rowsN - 1) * 10 : 132;
    float h = M + 52 + 16 + 40 + 18 + bodyH + 22 + 50 + M;
    Rect card{ui.W / 2 - w / 2, ui.H / 2 - h / 2, w, h};
    ui.panel(card, 1, ui.t.radius + 10);
    float x = card.x + M, y = card.y + M;
    ui.beginUnmirrored({x, y, 52, 52});
    drawLogo(vg, x, y, 52);
    ui.endUnmirrored();
    ui.text(x + 68, y + 16, "Welcome to SwitchMX", 24, ui.t.text, LEFT, true);
    ui.text(x + 68, y + 40,
            ui.ellipsize(setupStep_ == 0 ? "Choose your language. You can change it any time in Settings."
                                         : "Choose a text size. You can change it any time in Settings.",
                         w - 2 * M - 68, 15),
            15, ui.t.textDim);
    y += 52 + 16;
    ui.text(x, y + 10, setupStep_ == 0 ? "LANGUAGE" : "TEXT SIZE", 12, ui.t.textFaint, LEFT, true);
    // Step dots
    for (int i = 0; i < 2; i++) {
        nvgBeginPath(vg);
        nvgCircle(vg, card.right() - M - 30 + i * 16, y + 10, i == setupStep_ ? 4.5f : 3.5f);
        nvgFillColor(vg, i == setupStep_ ? ui.t.accent : withAlpha(ui.t.textFaint, 0.6f));
        nvgFill(vg);
    }
    y += 40;
    if (setupStep_ == 0) {
        float optW = (w - 2 * M - (cols - 1) * gap) / cols;
        for (int i = 0; i < (int)langs.size(); i++) {
            Rect r{x + (i % cols) * (optW + gap), y + (i / cols) * (langH + 10), optW, langH};
            uint32_t id = uid("setup.lang", i);
            ui.focusable(id, r);
            bool foc = ui.focused(id), sel = app.cfg.s.language == i;
            float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
            ui.fillRect(r, sel ? withAlpha(ui.t.accent, 0.22f) : withAlpha(ui.t.surface2, 0.9f), ui.t.radius);
            if (sel) ui.strokeRect(r, ui.t.accent, 2, ui.t.radius);
            ui.focusRing(r, ui.t.radius, fv);
            ui.text(r.cx(), r.cy(), langs[i].name, 18, sel ? ui.t.accent : ui.t.text, CENTER, true);
            if ((ui.tapped(r) || (foc && ui.confirm())) && !sel) {
                app.cfg.s.language = i;
                app.settingsChanged();
            }
        }
        y += bodyH + 22;
    } else {
        static const char* names[] = {"Small", "Normal", "Large", "Extra large"};
        static const float previewSize[] = {15, 18, 21, 24};
        float optW = (w - 2 * M - 3 * gap) / 4, optH = 132;
        for (int i = 0; i < 4; i++) {
            Rect r{x + i * (optW + gap), y, optW, optH};
            uint32_t id = uid("setup.size", i);
            ui.focusable(id, r);
            bool foc = ui.focused(id), sel = app.cfg.s.uiScale == i;
            float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
            ui.fillRect(r, sel ? withAlpha(ui.t.accent, 0.22f) : withAlpha(ui.t.surface2, 0.9f), ui.t.radius);
            if (sel) ui.strokeRect(r, ui.t.accent, 2, ui.t.radius);
            ui.focusRing(r, ui.t.radius, fv);
            ui.text(r.cx(), r.y + 46, "Aa", previewSize[i] * 1.6f, ui.t.text, CENTER, true);
            ui.text(r.cx(), r.y + 92, ui.ellipsize(names[i], r.w - 12, 16, true), 16, sel ? ui.t.accent : ui.t.textDim,
                    CENTER, true);
            if (sel) ui.icon(Icon::Check, r.right() - 18, r.y + 18, 16, ui.t.accent);
            if ((ui.tapped(r) || (foc && ui.confirm())) && !sel) {
                app.cfg.s.uiScale = i;
                app.settingsChanged();
            }
        }
        y += optH + 22;
    }
    Rect go{card.cx() - 110, y, 220, 50};
    ui.setDefaultFocus(setupStep_ == 0 ? uid("setup.lang", app.cfg.s.language) : uid("setup.size", app.cfg.s.uiScale));
    bool next = ui.button(uid("setup.go"), go, setupStep_ == 0 ? "Next" : "Continue",
                          setupStep_ == 0 ? Icon::ChevronRight : Icon::Check, UI::BTN_FILLED);
    if (setupStep_ == 1 && ui.back()) {  // B on the second step goes back to the languages
        setupStep_ = 0;
        ui.setFocus(uid("setup.lang", app.cfg.s.language));
        return;
    }
    if (setupStep_ == 0 && (next || ui.back())) {
        setupStep_ = 1;
        ui.setFocus(uid("setup.size", app.cfg.s.uiScale));
        return;
    }
    if (next) {
        app.cfg.s.setupDone = 1;
        app.cfg.save();
        setupOpen_ = false;
        setupStep_ = 0;
        ui.setFocus(uid("nav", 0));
    }
}

void HomeScreen::filesChanged() {
    if (job_ || searching_ || dir_.empty() || (section_ != FILES && section_ != MANAGER)) return;
    refresh();
}

// The Wi-Fi transfer panel: where to point a browser, the PIN, and the switches. Laid out top to
// bottom on a fixed grid (28 px margins, 20 px between blocks); the card is as tall as its content.
void HomeScreen::drawTransfer() {
    UI& ui = app.ui;
    NVGcontext* vg = ui.vg;
    WebServer& web = app.web;
    bool on = web.running();
    std::string address = on ? app.webAddress() : "";
    const float M = 28, GAP = 20, BTN = 50;
    float w = std::min(on && !address.empty() ? 800.0f : 620.0f, ui.W - 80);
    float bodyH = on ? (address.empty() ? 70 : 24 + 64 + GAP + 24 + 62 + GAP + 18) : 3 * 36 + 10 + 18;
    float h = M + 52 + GAP + 1 + GAP + bodyH + GAP + BTN + M;
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, ui.W, ui.H);
    nvgFillColor(vg, hex(0, 0.6f));
    nvgFill(vg);
    Rect card{ui.W / 2 - w / 2, ui.H / 2 - h / 2, w, h};
    ui.panel(card, 1, ui.t.radius + 10);
    float x = card.x + M, right = card.right() - M, y = card.y + M;

    // Header: icon tile, title and line under it, status pill.
    Rect tile{x, y, 52, 52};
    nvgBeginPath(vg);
    nvgRoundedRect(vg, tile.x, tile.y, tile.w, tile.h, 15);
    NVGcolor c1 = on ? ui.t.success : ui.t.accent;
    NVGcolor c2 = on ? nvgLerpRGBA(ui.t.success, ui.t.accent2, 0.6f) : ui.t.accent2;
    nvgFillPaint(vg, nvgLinearGradient(vg, tile.x, tile.y, tile.right(), tile.bottom(), c1, c2));
    nvgFill(vg);
    ui.icon(Icon::Wifi, tile.cx(), tile.cy() + 1, 30, hex(0xFFFFFF));
    const char* st = on ? "ON" : "OFF";
    float pw = ui.textWidth(st, 13, true) + 28;
    Rect pill{right - pw, tile.cy() - 13, pw, 26};
    ui.fillRect(pill, withAlpha(on ? ui.t.success : ui.t.textFaint, 0.22f), 13);
    ui.text(pill.cx(), pill.cy(), st, 13, on ? ui.t.success : ui.t.textDim, CENTER, true);
    float tx = tile.right() + 16, tw = pill.x - 12 - tx;
    ui.text(tx, tile.y + 16, "Wi-Fi transfer", 24, ui.t.text, LEFT, true);
    ui.text(tx, tile.y + 40, ui.ellipsize("Manage files from a computer or phone", tw, 15), 15, ui.t.textDim);
    y = tile.bottom() + GAP;
    ui.fillRect({x, y, right - x, 1}, ui.t.border);
    y += 1 + GAP;

    if (on && !address.empty()) {
        // A code beside the address: a phone's camera opens the page straight away.
        float qs = 24 + 64 + GAP + 24 + 62;
        ui.beginUnmirrored({right - qs, y, qs, qs});  // a mirrored code doesn't scan
        drawQr(vg, {right - qs, y, qs, qs}, address, 12);
        ui.endUnmirrored();
        right -= qs + 18;
        ui.text(x, y + 9, ui.ellipsize("OPEN IN A BROWSER, OR SCAN THE CODE", right - x, 12, true), 12, ui.t.textFaint,
                LEFT, true);
        y += 24;
        Rect box{x, y, right - x, 64};
        ui.fillRect(box, withAlpha(ui.t.accent, 0.14f), 16);
        ui.strokeRect(box, withAlpha(ui.t.accent, 0.45f), 1.5f, 16);
        float as = 28;
        while (as > 16 && ui.textWidth(address, as, true) > box.w - 24) as -= 1;
        ui.text(box.cx(), box.cy(), address, as, ui.t.text, CENTER, true);
        y = box.bottom() + GAP;
        ui.text(x, y + 9, "PIN", 12, ui.t.textFaint, LEFT, true);
        y += 24;
        const std::string& pin = web.pin();
        float dw = 54, dg = 10;
        ui.keepDigits = true;  // typed into a browser: the digits on any keyboard
        for (size_t i = 0; i < pin.size(); i++) {
            Rect d{x + i * (dw + dg), y, dw, 62};
            ui.fillRect(d, ui.t.surface2, 14);
            ui.strokeRect(d, ui.t.border, 1.5f, 14);
            ui.text(d.cx(), d.cy(), std::string(1, pin[i]), 30, ui.t.text, CENTER, true);
        }
        ui.keepDigits = false;
        // Status beside the PIN, in its own rounded box of the same height.
        float sx = x + pin.size() * (dw + dg) + 6;
        Rect status{sx, y, right - sx, 62};
        ui.fillRect(status, withAlpha(ui.t.success, 0.10f), 14);
        int n = web.activeTransfers();
        float icx = status.x + 26, icy = status.cy();
        if (n > 0) {
            ui.spinner(icx, icy, 10, ui.t.success, 3);
        } else {
            nvgBeginPath(vg);
            nvgCircle(vg, icx, icy, 5 + std::sin(ui.time * 3));
            nvgFillColor(vg, ui.t.success);
            nvgFill(vg);
        }
        std::string line1 = n > 0 ? (n == 1 ? "1 transfer running" : std::to_string(n) + " transfers running")
                                  : "Ready";
        std::string line2 = n > 0 ? "Keep this open until it finishes" : "Waiting for a browser";
        float lx = status.x + 46, lw = status.right() - 14 - lx;
        ui.text(lx, icy - 10, ui.ellipsize(line1, lw, 17, true), 17, ui.t.text, LEFT, true);
        ui.text(lx, icy + 12, ui.ellipsize(line2, lw, 14), 14, ui.t.textDim);
        y += 62 + GAP;
        right = card.right() - M;
        ui.text(x, y + 8, "The console stays awake while this is on.", 14, ui.t.textFaint);
        y += 18;
    } else if (on) {
        ui.text(x, y + 14, "The Switch isn't connected to Wi-Fi.", 18, ui.t.text, LEFT, true);
        ui.text(x, y + 44, ui.ellipsize("Connect it in System Settings → Internet, then come back here.", right - x, 15),
                15, ui.t.textDim);
        y += 70;
    } else {
        static const char* lines[] = {"Upload films, music and pictures from a browser",
                                      "Download or play anything on the SD card and USB drives",
                                      "Create folders, rename and delete"};
        for (int i = 0; i < 3; i++) {
            float ly = y + i * 36 + 14;
            nvgBeginPath(vg);
            nvgCircle(vg, x + 12, ly, 12);
            nvgFillColor(vg, withAlpha(ui.t.success, 0.16f));
            nvgFill(vg);
            ui.icon(Icon::Check, x + 12, ly, 15, ui.t.success);
            ui.text(x + 36, ly, ui.ellipsize(lines[i], right - x - 36, 16), 16, ui.t.text);
        }
        y += 3 * 36 + 10;
        ui.text(x, y + 8, ui.ellipsize("Only your network can connect, and it needs the PIN shown here.", right - x, 14), 14,
                ui.t.textFaint);
        y += 18;
    }

    // Buttons: three equal widths across the card.
    float bw = (right - x - 2 * 12) / 3, by = card.bottom() - M - BTN;
    Rect b1{x, by, bw, BTN}, b2{b1.right() + 12, by, bw, BTN}, b3{b2.right() + 12, by, bw, BTN};
    ui.setDefaultFocus(uid("xfer.toggle"));
    if (ui.button(uid("xfer.toggle"), b1, on ? "Turn off" : "Turn on", on ? Icon::Close : Icon::Wifi,
                  on ? UI::BTN_TONAL : UI::BTN_FILLED)) {
        if (on) app.webStop();
        else app.webStart();
    }
    std::string auto_ = trf("At start: {0}", {tr(app.cfg.s.webAutostart ? "On" : "Off")});
    if (ui.button(uid("xfer.auto"), b2, auto_.c_str(), app.cfg.s.webAutostart ? Icon::Check : Icon::Power)) {
        app.cfg.s.webAutostart = !app.cfg.s.webAutostart;
        app.cfg.save();
        app.ui.toast(app.cfg.s.webAutostart ? "Wi-Fi transfer will start with SwitchMX"
                                            : "Wi-Fi transfer starts only when you turn it on",
                     Icon::Wifi);
    }
    if (ui.button(uid("xfer.close"), b3, "Close", Icon::Back) || ui.back()) {
        transferOpen_ = false;
        ui.setFocus(uid("nav.wifi"));
    }
}

void HomeScreen::drawHints() {
    UI& ui = app.ui;
    float y = ui.H - 32;
    float x = SIDEBAR_W + 32;
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, SIDEBAR_W + 32, ui.H - 62, ui.W - SIDEBAR_W - 64, 1);
    nvgFillColor(ui.vg, ui.t.border);
    nvgFill(ui.vg);
    if ((section_ == FILES || section_ == MANAGER) && selectMode()) {
        drawSelectBar();
        return;
    }
    // Each hint does exactly what its button does, whether tapped or pressed.
    switch (section_) {
        case FILES:
            x += ui.hint(x, y, BTN_A, "Open");
            x += ui.hint(x, y, BTN_B, "Back");
            x += ui.hint(x, y, BTN_X, "Options");
            x += ui.hint(x, y, BTN_Y, "Sort & filter");
            x += ui.hint(x, y, BTN_MINUS, app.cfg.s.browserLayout ? "List view" : "Grid view");
            if (!clipboard_.empty() && !dir_.empty()) x += ui.hint(x, y, BTN_R, clipboardMove_ ? "Move here" : "Paste");
            break;
        case NETWORK:
            x += ui.hint(x, y, BTN_A, "Open");
            if (!dir_.empty()) x += ui.hint(x, y, BTN_B, "Back");
            x += ui.hint(x, y, BTN_X, "Options");
            if (!dir_.empty()) x += ui.hint(x, y, BTN_Y, "Sort & filter");
            x += ui.hint(x, y, BTN_MINUS, app.cfg.s.browserLayout ? "List view" : "Grid view");
            break;
        case MANAGER:
            x += ui.hint(x, y, BTN_A, "Open");
            x += ui.hint(x, y, BTN_B, searching_ ? "Close search" : "Back");
            x += ui.hint(x, y, BTN_X, "Actions");
            x += ui.hint(x, y, BTN_Y, "Select");
            if (!clipboard_.empty() || !extractPending_.empty())
                x += ui.hint(x, y, BTN_R, extractPending_.empty() ? "Paste" : "Extract here");
            break;
        case RECENT:
            x += ui.hint(x, y, BTN_A, "Resume");
            x += ui.hint(x, y, BTN_X, "Options");
            break;
        case FAVORITES:
            x += ui.hint(x, y, BTN_A, playlistOpen_ == kLikedList ? "Play" : "Open");
            if (playlistOpen_ == kLikedList) x += ui.hint(x, y, BTN_B, "Back");
            x += ui.hint(x, y, BTN_X, "Options");
            if (playlistOpen_ == kLikedList) x += ui.hint(x, y, BTN_Y, "Shuffle");
            break;
        case PLAYLISTS:
            x += ui.hint(x, y, BTN_A, playlistOpen_ == kNoList ? "Open" : "Play");
            if (playlistOpen_ != kNoList) x += ui.hint(x, y, BTN_B, "Back");
            x += ui.hint(x, y, BTN_X, "Options");
            x += ui.hint(x, y, BTN_Y, playlistOpen_ == kNoList ? "New playlist" : "Shuffle");
            break;
        case STREAMS:
            x += ui.hint(x, y, BTN_A, "Play");
            x += ui.hint(x, y, BTN_X, "Options");
            break;
        case SETTINGS:
            x += ui.hint(x, y, BTN_A, "Change");
            x += ui.hint(x, y, BTN_B, "Back");
            x += ui.hint(x, y, BTN_L, "");
            x -= 22;
            x += ui.hint(x, y, BTN_R, "Category");
            break;
        default: break;
    }
    float rx = ui.W - 32 - ui.textWidth("Exit", 17) - 32;
    if (rx > x) ui.hint(rx, y, BTN_PLUS, "Exit");
}

void HomeScreen::drawEmpty(Rect area, Icon ic, const char* title, const char* sub) {
    UI& ui = app.ui;
    float cy = area.cy() - 20;
    nvgBeginPath(ui.vg);
    nvgCircle(ui.vg, area.cx(), cy - 40, 52);
    nvgFillColor(ui.vg, withAlpha(ui.t.accent, 0.12f));
    nvgFill(ui.vg);
    ui.icon(ic, area.cx(), cy - 40, 48, ui.t.accent);
    ui.text(area.cx(), cy + 42, title, 24, ui.t.text, CENTER, true);
    ui.text(area.cx(), cy + 74, sub, 17, ui.t.textDim, CENTER);
}

// ------------------------------------------------------------------ files

void HomeScreen::drawHeader(Rect area) {
    UI& ui = app.ui;
    const auto& roots = app.drives();
    std::string title;
    bool atRoot = dir_.empty();  // nothing above this level to go back to
    for (auto& r : roots)
        if (dir_ == r.path) {
            title = r.label;
            atRoot = roots.size() == 1;
        }
    if (dir_.empty()) title = section_ == NETWORK ? "Network" : "Storage";
    if (searching_) title = trf("Search: {0}", {searchTerm_});
    if (netDir()) {  // a server's starting folder (or its top) is called by the server's name
        int si = netfs::serverFor(dir_);
        bool top = parentDir(dir_).find('/', dir_.find("://") + 3) == std::string::npos;
        if (si >= 0 && (top || dir_ == netfs::rootUrl(netfs::servers()[si]))) title = netfs::servers()[si].name;
        else title = displayName(dir_);
    }
    if (title.empty()) {
        title = dir_;
        while (!title.empty() && title.back() == '/') title.pop_back();
        title = fromDiskName(baseName(title));
    }

    // Header buttons, right aligned.
    bool mgr = managerMode();
    struct HB { const char* key; Icon ic; std::function<void()> action; };
    std::vector<HB> btns;
    // The two views of Files: media (what plays, with covers) and manage (every file, every action).
    if (section_ == FILES || section_ == MANAGER)
        btns.push_back({"hdr.mode", mgr ? Icon::Film : Icon::Sliders, [this, mgr] {
            setSection(mgr ? FILES : MANAGER);
            app.ui.setFocus(uid("hdr.mode"));
            app.ui.toast(mgr ? "Media view" : "Manage view: every file, with copy, move, rename and more",
                         mgr ? Icon::Film : Icon::Sliders);
        }});
    btns.push_back({"hdr.view", app.cfg.s.browserLayout ? Icon::List : Icon::Grid, [this] {
        app.cfg.s.browserLayout = !app.cfg.s.browserLayout;
        app.saveSoon();
        listTime_ = 0;
    }});
    btns.push_back({"hdr.sort", Icon::Sort, [this] { sortMenu(); }});
    if (mgr) {
        btns.push_back({"hdr.hidden", app.cfg.s.showHidden ? Icon::Eye : Icon::EyeOff, [this] {
            app.cfg.s.showHidden = !app.cfg.s.showHidden;
            app.saveSoon();
            refresh();
            app.ui.toast(app.cfg.s.showHidden ? "Showing hidden files" : "Hiding hidden files", Icon::Eye);
        }});
        if (!dir_.empty()) {
            btns.push_back({"hdr.search", Icon::Search, [this] { askSearch(); }});
            btns.push_back({"hdr.newfolder", Icon::FolderPlus, [this] { askNewFolder(); }});
            if (!clipboard_.empty() || !extractPending_.empty())
                btns.push_back({"hdr.paste", Icon::Paste, [this] { paste(); }});
        }
    } else {
        if (!dir_.empty() && !netDir() && (!clipboard_.empty() || !extractPending_.empty()))
            btns.push_back({"hdr.paste", Icon::Paste, [this] { paste(); }});
        btns.push_back({"hdr.filter", app.cfg.s.mediaOnly ? Icon::EyeOff : Icon::Eye, [this] {
            app.cfg.s.mediaOnly = !app.cfg.s.mediaOnly;
            app.saveSoon();
            refresh();
            app.ui.toast(app.cfg.s.mediaOnly ? "Showing media only" : "Showing all files",
                         app.cfg.s.mediaOnly ? Icon::Film : Icon::File);
        }});
    }
    if (mgr && !dir_.empty() && !entries_.empty() && !netDir())  // selecting is for managing files
        btns.push_back({"hdr.select", selectMode() ? Icon::Close : Icon::Check, [this] {
            if (selectMode()) endSelect();
            else {
                selecting_ = true;
                app.ui.toast("Tap files to select them", Icon::Check);
            }
        }});
    float bx = area.right() - 24;
    float by = area.y + 30;
    for (auto& hb : btns) {
        if (ui.iconButton(uid(hb.key), bx, by, 48, hb.ic, ui.t.text, G_HEADER, false)) hb.action();
        bx -= 54;
    }
    float textRight = bx - 10;

    float tx = area.x;
    if (!atRoot) {
        if (ui.iconButton(uid("hdr.up"), area.x + 22, by, 48, Icon::Back, ui.t.text, G_HEADER, true)) {
            if (!managerBack()) goUp();
        }
        tx += 60;
    }
    ui.text(tx, by - 2, ui.ellipsize(title, textRight - tx, 30, true), 30, ui.t.text, LEFT, true);
    size_t drives = 0;
    for (auto& e : entries_) drives += !e.addServer;
    // trf() doesn't touch its arguments, and the line it ends up in also holds Latin words, so the
    // number would stay in Latin digits beside Persian or Arabic text unless it is converted here.
    auto num = [](size_t n) { return i18n::localDigits(std::to_string(n)); };
    std::string count = entries_.size() == 1 ? tr("1 item") : trf("{0} items", {num(entries_.size())});
    // U+200E after the path: without it, a folder whose name ends in Persian or Arabic drags the
    // following "  ·  " into its own right-to-left run, and the separator lands on the wrong side.
    std::string sub = dir_.empty() && section_ == NETWORK ? tr("Shared folders (SMB) and FTP servers on your network")
                      : dir_.empty() ? (drives == 1 ? tr("1 drive") : trf("{0} drives", {num(drives)}))
                                   : platform::displayPath(dir_) + "‎" + "  ·  " + count;
    ui.text(tx + 1, by + 30, ui.ellipsize(sub, textRight - tx, 15), 15, ui.t.textFaint);
}

// Fills the tile when the shapes are close; otherwise fits the whole picture (a poster in a wide tile).
void HomeScreen::drawCover(Rect tile, const CoverArt::Art& art, float radius) {
    UI& ui = app.ui;
    NVGcontext* vg = ui.vg;
    float alpha = std::min(1.0f, art.age / 0.25f);
    float artAspect = (float)art.w / art.h, tileAspect = tile.w / tile.h;
    float ratio = artAspect / tileAspect;
    bool square = std::fabs(tileAspect - 1) < 0.05f;
    // Wide films are cropped to fill too (a row of letterboxed and full tiles looked uneven).
    bool fill = square || (ratio > 0.72f && ratio < 2.0f);
    float w = tile.w, h = tile.h;
    Rect shape = tile;
    if (fill) {
        if (ratio > 1) w = h * artAspect;
        else h = w / artAspect;
    } else {
        ui.fillRect(tile, hex(0x000000, 0.3f * alpha), radius);
        if (ratio > 1) h = w / artAspect;
        else w = h * artAspect;
        shape = Rect{tile.cx() - w / 2, tile.cy() - h / 2, w, h};
    }
    nvgBeginPath(vg);
    nvgRoundedRect(vg, shape.x, shape.y, shape.w, shape.h, std::min(radius, std::min(shape.w, shape.h) / 2));
    ui.beginUnmirrored(tile);  // pictures keep their own left and right
    nvgFillPaint(vg, nvgImagePattern(vg, tile.cx() - w / 2, tile.cy() - h / 2, w, h, 0, art.image, alpha));
    nvgFill(vg);
    ui.endUnmirrored();
}

void HomeScreen::fileTileIcon(Rect tile, const FileEntry& e, float iconSize) {
    UI& ui = app.ui;
    NVGcontext* vg = ui.vg;
    float rad = std::min(ui.t.radius, tile.h * 0.3f);
    const CoverArt::Art* art = e.isDrive ? nullptr : app.covers.get(e.path, e.kind);
    if (art && e.kind == MediaKind::Folder) {
        drawCover(tile, *art, rad);
        // Keeps a folder recognisable as one.
        float s = std::clamp(tile.h * 0.3f, 18.0f, 30.0f);
        Rect badge{tile.right() - s - 3, tile.bottom() - s - 3, s, s};
        ui.fillRect(badge, withAlpha(ui.t.accent, 0.95f), s * 0.28f);
        ui.icon(Icon::Folder, badge.cx(), badge.cy(), s * 0.66f, hex(0xFFFFFF));
        return;
    }
    if (art) {
        NVGcolor kc = kindColor(ui, e.kind);
        ui.fillRect(tile, mix(ui.t.surface2, kc, ui.t.light ? 0.12f : 0.16f), rad);
        drawCover(tile, *art, rad);
        return;
    }
    if (e.kind == MediaKind::Folder) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tile.x, tile.y, tile.w, tile.h, rad);
        nvgFillPaint(vg, nvgLinearGradient(vg, tile.x, tile.y, tile.right(), tile.bottom(), ui.t.accent,
                                           ui.t.accent2));
        nvgFill(vg);
        Icon ic = !e.isDrive ? Icon::Folder
                  : e.addServer ? Icon::Plus
                  : e.server >= 0 ? Icon::Server
                  : (e.removable ? Icon::Drive : Icon::SdCard);
        ui.icon(ic, tile.cx(), tile.cy(), iconSize, hex(0xFFFFFF, 0.95f));
        return;
    }
    if (e.kind == MediaKind::Audio) {
        // Songs without a cover get the same generated art as in the player, by album folder
        ui.generatedArt(tile, parentDir(e.path), stripExtension(e.name), rad);
        return;
    }
    NVGcolor kc = kindColor(ui, e.kind);
    ui.fillRect(tile, mix(ui.t.surface2, kc, ui.t.light ? 0.12f : 0.16f), rad);
    bool showExt = !e.ext.empty() && tile.h >= 44;
    float iy = showExt ? tile.cy() - tile.h * 0.1f : tile.cy();
    ui.icon(kindIcon(e.kind), tile.cx(), iy, iconSize, kc);
    if (showExt) {
        std::string ext = e.ext;
        for (auto& c : ext) c = (char)toupper((unsigned char)c);
        ui.text(tile.cx(), tile.bottom() - tile.h * 0.17f, ext, std::clamp(tile.h * 0.2f, 10.0f, 15.0f), withAlpha(kc, 0.9f),
                CENTER, true);
    }
}

bool HomeScreen::fileRow(uint32_t id, Rect r, const FileEntry& e, int index) {
    UI& ui = app.ui;
    ui.focusable(id, r, G_LIST);
    bool foc = ui.focused(id);
    if (foc) contextIndex_ = index;
    if (r.bottom() < scroll_.view.y - 10 || r.y > scroll_.view.bottom() + 10) {
        return !ui.blocked && foc && ui.confirm();
    }

    float appear = ui.animMul > 0 ? ui.ease((listTime_ - std::min(index, 14) * 0.03f) / 0.3f) : 1;
    nvgSave(ui.vg);
    ui.pushAlpha(appear * scroll_.edgeFade(r.y, r.h));
    nvgTranslate(ui.vg, 24 * (1 - appear), 0);

    bool showFoc = foc && ui.showFocus();
    float fv = ui.smooth(id ^ 0xf0c, showFoc ? 1.0f : 0.0f, 16);
    float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
    Rect card = r.inset(0, 4);
    if (fv > 0.01f || pv > 0.01f) ui.fillRect(card, withAlpha(ui.t.surface2, std::max(fv, pv)));
    ui.focusRing(card, ui.t.radius, fv);

    Rect tile{card.x + 12, card.cy() - 25, 50, 50};
    fileTileIcon(tile, e, 26);
    if (isMarked(e.path)) {
        ui.strokeRect(card, ui.t.accent, 2, ui.t.radius);
        nvgBeginPath(ui.vg);
        nvgCircle(ui.vg, tile.right() - 2, tile.y + 2, 11);
        nvgFillColor(ui.vg, ui.t.accent);
        nvgFill(ui.vg);
        ui.icon(Icon::Check, tile.right() - 2, tile.y + 2, 14, ui.t.onAccent);
    } else if (selectMode() && !e.isDrive && (section_ == FILES || section_ == MANAGER)) {
        // An empty circle to tick, in the theme's colours (a dark blob on light backgrounds otherwise).
        nvgBeginPath(ui.vg);
        nvgCircle(ui.vg, tile.right() - 2, tile.y + 2, 10);
        nvgFillColor(ui.vg, withAlpha(ui.t.surface, 0.9f));
        nvgFill(ui.vg);
        nvgStrokeColor(ui.vg, withAlpha(ui.t.textDim, 0.9f));
        nvgStrokeWidth(ui.vg, 1.6f);
        nvgStroke(ui.vg);
    }

    float x = tile.right() + 18;
    float right = card.right() - 20;
    // Trailing indicators
    float prog = e.kind == MediaKind::Folder ? -1 : app.lib.progress(e.path);
    if (e.kind == MediaKind::Folder) {
        ui.icon(Icon::ChevronRight, right - 8, card.cy(), 22, ui.t.textFaint);
        right -= 32;
    } else if (prog >= 1.0f) {
        nvgBeginPath(ui.vg);
        nvgCircle(ui.vg, right - 12, card.cy(), 13);
        nvgFillColor(ui.vg, withAlpha(ui.t.success, 0.18f));
        nvgFill(ui.vg);
        ui.icon(Icon::Check, right - 12, card.cy(), 16, ui.t.success);
        right -= 38;
    } else if (prog >= 0) {
        // Time left (or the page) over a short bar: a ring with a sliver of colour looked like a
        // loading spinner.
        std::string left;
        if (const HistoryItem* h = app.lib.find(e.path); h && h->duration > 0) {
            left = e.kind == MediaKind::Document
                       ? "Page " + std::to_string((int)h->position + 1)
                       : "−" + formatTime(std::max(0.0, h->duration - h->position));
        }
        const float bw = 72;
        Rect bar{right - bw, card.cy() + 9, bw, 4};
        ui.fillRect(bar, withAlpha(ui.t.textFaint, 0.25f), 2);
        ui.fillRect({bar.x, bar.y, std::max(4.0f, bw * prog), bar.h}, ui.t.accent, 2);
        if (!left.empty()) ui.text(right, card.cy() - 8, left, 15, ui.t.textDim, RIGHT, true);
        right -= bw + 16;
    }
    if (app.lib.isFavorite(e.path)) {
        ui.icon(Icon::StarFilled, right - 10, card.cy(), 18, ui.t.warning);
        right -= 30;
    }
    if (app.lib.isLiked(e.path)) {
        ui.icon(Icon::HeartFilled, right - 10, card.cy(), 18, ui.t.danger);
        right -= 30;
    }

    std::string name = e.name;
    if (e.kind == MediaKind::Folder) {
        while (!name.empty() && name.back() == '/') name.pop_back();
        if (app.cfg.s.tidyNames && !managerMode() && !e.isDrive) name = tidySpaces(name);
    } else if ((e.kind == MediaKind::Video || e.kind == MediaKind::Audio) && app.cfg.s.tidyNames && !managerMode()) {
        // The File Manager keeps names exactly as they are.
        name = e.kind == MediaKind::Video ? tidyTitle(name) : tidySong(name);
    } else if (!app.cfg.s.showExtensions) {
        name = stripExtension(name);
    }
    bool info = app.cfg.s.showFileInfo;
    float ny = info ? card.cy() - 11 : card.cy() + 1;
    // The highlighted row shows its whole name, gliding if it's too long.
    if (showFoc) ui.marquee(id ^ 0x3a9, x, ny, right - x - 8, name, 20, ui.t.text, LEFT, true);
    else ui.text(x, ny, ui.ellipsize(name, right - x - 8, 20, foc), 20, ui.t.text, LEFT, foc);
    if (info) {
        std::string detail;
        if (e.kind == MediaKind::Folder) detail = e.isDrive ? e.detail : tr("Folder");
        else detail = formatSize(e.size);
        std::string d = formatDate(e.mtime);
        if (!d.empty()) detail += "  ·  " + d;
        if (!e.isDrive && !e.detail.empty()) detail = e.detail;
        if (prog > 0 && prog < 1) {
            auto h = app.lib.find(e.path);
            if (h) detail += "  ·  " + tr(formatTime(h->position) + " watched");
        }
        if (showFoc) ui.marquee(id ^ 0x3aa, x, card.cy() + 14, right - x - 8, detail, 15, ui.t.textFaint, LEFT, false);
        else ui.text(x, card.cy() + 14, ui.ellipsize(detail, right - x - 8, 15), 15, ui.t.textFaint);
    }
    ui.popAlpha();
    nvgRestore(ui.vg);

    if (ui.longPressIn(card)) {
        ui.setFocus(id);
        // The same menu as X: inside a playlist (or Liked) it's the playlist's own.
        if (section_ == PLAYLISTS || (section_ == FAVORITES && playlistOpen_ == kLikedList))
            playlistItemMenu((size_t)index);
        else entryMenu(e);
        return false;
    }
    return ui.tapped(card) || (foc && ui.confirm());
}

bool HomeScreen::fileTile(uint32_t id, Rect r, const FileEntry& e, int index) {
    UI& ui = app.ui;
    ui.focusable(id, r, G_LIST);
    bool foc = ui.focused(id);
    if (foc) contextIndex_ = index;
    if (r.bottom() < scroll_.view.y - 10 || r.y > scroll_.view.bottom() + 10) {
        return !ui.blocked && foc && ui.confirm();
    }
    float appear = (ui.animMul > 0 ? ui.ease((listTime_ - std::min(index, 16) * 0.025f) / 0.3f) : 1) *
                   scroll_.edgeFade(r.y, r.h);
    bool showFoc = foc && ui.showFocus();
    float fv = ui.smooth(id ^ 0xf0c, showFoc ? 1.0f : 0.0f, 16);
    float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
    float s = (0.92f + 0.08f * appear) * (1 + 0.03f * fv - 0.03f * pv);

    nvgSave(ui.vg);
    ui.pushAlpha(appear);
    nvgTranslate(ui.vg, r.cx(), r.cy());
    nvgScale(ui.vg, s, s);
    nvgTranslate(ui.vg, -r.cx(), -r.cy());

    Rect card = r.inset(7);
    ui.fillRect(card, withAlpha(ui.t.surface, ui.t.light ? 1.0f : 0.75f));
    if (fv > 0.01f) ui.fillRect(card, withAlpha(ui.t.surface2, fv));
    Rect art{card.x + 8, card.y + 8, card.w - 16, card.h - 66};
    fileTileIcon(art, e, std::min(56.0f, art.h * 0.5f));
    if (isMarked(e.path)) {
        ui.strokeRect(card, ui.t.accent, 3, ui.t.radius);
        nvgBeginPath(ui.vg);
        nvgCircle(ui.vg, art.x + 18, art.y + 18, 13);
        nvgFillColor(ui.vg, ui.t.accent);
        nvgFill(ui.vg);
        ui.icon(Icon::Check, art.x + 18, art.y + 18, 16, ui.t.onAccent);
    } else if (selectMode() && !e.isDrive && (section_ == FILES || section_ == MANAGER)) {
        nvgBeginPath(ui.vg);
        nvgCircle(ui.vg, art.x + 18, art.y + 18, 12);
        nvgFillColor(ui.vg, withAlpha(ui.t.surface, 0.9f));
        nvgFill(ui.vg);
        nvgStrokeColor(ui.vg, withAlpha(ui.t.textDim, 0.9f));
        nvgStrokeWidth(ui.vg, 1.8f);
        nvgStroke(ui.vg);
    }
    float prog = e.kind == MediaKind::Folder ? -1 : app.lib.progress(e.path);
    if (prog > 0) {
        float pw = (art.w - 20) * std::min(prog, 1.0f);
        ui.fillRect({art.x + 10, art.bottom() - 12, art.w - 20, 4}, hex(0, 0.3f), 2);
        ui.fillRect({art.x + 10, art.bottom() - 12, pw, 4}, prog >= 1 ? ui.t.success : ui.t.accent, 2);
    }
    float badgeX = art.right() - 16;
    if (app.lib.isFavorite(e.path)) {
        ui.icon(Icon::StarFilled, badgeX, art.y + 16, 18, ui.t.warning);
        badgeX -= 24;
    }
    if (app.lib.isLiked(e.path)) ui.icon(Icon::HeartFilled, badgeX, art.y + 16, 18, ui.t.danger);

    std::string name = e.name;
    if (e.kind == MediaKind::Folder) {
        while (!name.empty() && name.back() == '/') name.pop_back();
        if (app.cfg.s.tidyNames && !managerMode() && !e.isDrive) name = tidySpaces(name);
    } else if ((e.kind == MediaKind::Video || e.kind == MediaKind::Audio) && app.cfg.s.tidyNames && !managerMode()) {
        // The File Manager keeps names exactly as they are.
        name = e.kind == MediaKind::Video ? tidyTitle(name) : tidySong(name);
    } else if (!app.cfg.s.showExtensions) {
        name = stripExtension(name);
    }
    if (foc && ui.showFocus()) ui.marquee(id ^ 0x3a9, card.x + 14, card.bottom() - 40, card.w - 28, name, 17, ui.t.text, LEFT, true);
    else ui.text(card.x + 14, card.bottom() - 40, ui.ellipsize(name, card.w - 28, 17, true), 17, ui.t.text, LEFT, true);
    std::string detail = e.isDrive ? e.detail : (e.kind == MediaKind::Folder ? tr("Folder") : formatSize(e.size));
    if (foc && ui.showFocus())
        ui.marquee(id ^ 0x3aa, card.x + 14, card.bottom() - 17, card.w - 28, detail, 14, ui.t.textFaint, LEFT, false);
    else ui.text(card.x + 14, card.bottom() - 17, ui.ellipsize(detail, card.w - 28, 14), 14, ui.t.textFaint);
    ui.focusRing(card, ui.t.radius, fv);
    ui.popAlpha();
    nvgRestore(ui.vg);

    if (ui.longPressIn(card)) {
        ui.setFocus(id);
        entryMenu(e);
        return false;
    }
    return ui.tapped(card) || (foc && ui.confirm());
}

void HomeScreen::drawFiles(Rect area) {
    UI& ui = app.ui;
    drawHeader(area);
    Rect list{area.x - 12, area.y + 84, area.w + 24, area.h - 84};
    contextIndex_ = -1;
    pollNetListing();
    if (netDir() && entries_.empty() && (netLoading_ || !netError_.empty())) {
        // Focus stays here while the folder loads, so a button pressed meanwhile can't land on the
        // sidebar (Exit included); on an error, A tries again too.
        uint32_t holdId = uid("net.hold");
        ui.focusable(holdId, list);
        if (ui.focus == 0 || !ui.focused(holdId)) {
            if (ui.focus == 0 || ui.focus == uid("nav", (int)NETWORK) || netJustOpened_) ui.setFocus(holdId);
        }
        netJustOpened_ = false;
        if (!netLoading_ && ui.focused(holdId) && ui.confirm()) startNetListing(true);
        if (netLoading_) {
            ui.spinner(list.cx(), list.cy() - 30, 22, ui.t.accent, 4);
            ui.text(list.cx(), list.cy() + 22, tr("Connecting to the server…"), 18, ui.t.textDim, CENTER);
        } else {
            drawEmpty(list, Icon::Globe, netError_.c_str(), "Press Y to try again, or B to go back");
            if (!ui.blocked && ui.in->down(BTN_Y)) startNetListing(true);
        }
        return;
    }
    if (entries_.empty()) {
        bool filtered = !managerMode() && app.cfg.s.mediaOnly;
        drawEmpty(list, searching_ ? Icon::Search : (filtered ? Icon::Film : Icon::Folder),
                  searching_ ? "Nothing found" : (filtered ? "No media here" : "This folder is empty"),
                  filtered ? "Tap the eye icon to show all files" : "Press B to go back");
        // New folder and paste still work in an empty folder.
        if (managerMode() && !ui.blocked) managerButtons(nullptr);
        return;
    }

    bool grid = app.cfg.s.browserLayout == 1;
    // Grid tiles about 160 px wide: five or six across on the Switch screen.
    int cols = grid ? std::max(3, (int)((list.w - 12) / 160)) : 1;
    float tileW = (list.w - 12) / cols;
    float tileH = grid ? tileW * 0.66f + 64 : ROW_H;
    int rows = ((int)entries_.size() + cols - 1) / cols;
    float contentH = rows * tileH + 12;
    scroll_.update(ui, list, contentH);
    scroll_.begin(ui);
    int open = -1;
    for (size_t i = 0; i < entries_.size(); i++) {
        FileEntry& e = entries_[i];
        int row = (int)i / cols, col = (int)i % cols;
        float ly = row * tileH + 6;
        Rect r{list.x + 6 + col * tileW, list.y + ly - scroll_.offset, grid ? tileW : list.w - 12, tileH};
        if (!e.uiId) e.uiId = uid(e.path.c_str(), 77);
        uint32_t id = e.uiId;
        // Sizes and dates are read as rows come into view, or when the focused row is used.
        if (!e.detailsKnown && (ui.focused(id) || (r.bottom() >= list.y && r.y <= list.bottom())))
            Library::fillDetails(e);
        if ((ui.focused(id) && ui.focusMoved && ui.showFocus()) || id == pendingFocus_) {
            scroll_.ensureVisible(ly, tileH, grid ? 12 : 40);
            if (id == pendingFocus_) {
                scroll_.offset = scroll_.target >= 0 ? scroll_.target : scroll_.offset;
                pendingFocus_ = 0;
            }
        }
        bool activated = grid ? fileTile(id, r, e, (int)i) : fileRow(id, r, e, (int)i);
        if (activated) open = (int)i;
    }
    scroll_.end(ui);
    scroll_.drawScrollbar(ui);

    if (open >= 0) {
        // While items are selected in the manager, activating an item toggles it.
        if (selectMode() && !entries_[open].isDrive) toggleMark(entries_[open].path);
        else openEntry(entries_[open]);
        return;
    }
    if (ui.blocked) return;
    Input& in = *ui.in;
    if (managerMode()) managerButtons(contextIndex_ >= 0 ? &entries_[contextIndex_] : nullptr);
    if (in.down(BTN_X)) {
        const FileEntry* sel = nullptr;
        for (auto& f : entries_)
            if (!marked_.empty() && isMarked(f.path)) sel = &f;
        if (sel) managerMenu(*sel);  // acts on everything selected
        else if (contextIndex_ >= 0) entryMenu(entries_[contextIndex_]);
        else folderMenu();
    }
    if (!managerMode() && in.down(BTN_Y)) sortMenu();
    if (in.down(BTN_MINUS)) {
        app.cfg.s.browserLayout = !app.cfg.s.browserLayout;
        app.saveSoon();
        listTime_ = 0;
    }
}

void HomeScreen::openEntry(const FileEntry& e) {
    if (e.addServer) {
        askServer(-1);
        return;
    }
    if (netDir() && e.kind != MediaKind::Folder) {
        // A server's files play as a queue in the order shown (the next episode follows), with the
        // same kind of files as the one chosen.
        if (e.kind != MediaKind::Video && e.kind != MediaKind::Audio) {
            app.ui.toast("Only videos and music play from a network place", Icon::Info);
            return;
        }
        std::vector<std::string> files;
        int idx = 0;
        for (auto& f : entries_) {
            if (f.kind != e.kind) continue;
            if (f.path == e.path) idx = (int)files.size();
            files.push_back(f.path);
        }
        if (!app.cfg.s.autoplayNext) files = {e.path}, idx = 0;
        app.playFiles(files, idx);
        return;
    }
    if (e.kind == MediaKind::Folder) {
        openDir(e.path);
        return;
    }
    if (e.kind == MediaKind::Video || e.kind == MediaKind::Audio || e.kind == MediaKind::Playlist) {
        app.playPath(e.path);
        return;
    }
    if (e.kind == MediaKind::Image || e.kind == MediaKind::Document) {
        app.playPath(e.path);
        return;
    }
    managerOpenFile(e);
}

void HomeScreen::entryMenu(const FileEntry& e) {
    std::vector<MenuItem> items;
    std::vector<std::function<void()>> actions;
    // Files that don't play (archives, text, subtitles...) get the File Manager's menu: Extract,
    // View as text and the rest, instead of a Play that does nothing.
    if (e.server >= 0 || e.addServer) {
        if (e.server >= 0) serverMenu(e);
        else askServer(-1);
        return;
    }
    if (netDir()) {  // files on a server: play and favourites only (nothing there can be changed)
        if (e.kind == MediaKind::Folder) {
            app.openMenu(e.name, {{"Open", Icon::Folder}, {"Refresh", Icon::Refresh}}, [this, e](int i) {
                if (i == 0) openDir(e.path);
                if (i == 1) {
                    netCache_.clear();
                    openDir(e.path);
                }
            });
        } else {
            FileEntry f = e;
            bool fav = app.lib.isFavorite(e.path);
            app.openMenu(e.name, {{"Play", Icon::Play}, {"Play only this file", Icon::Film},
                                  {fav ? "Remove from favorites" : "Add to favorites", fav ? Icon::Star : Icon::StarFilled}},
                         [this, f, fav](int i) {
                             if (i == 0) openEntry(f);
                             if (i == 1) app.playFiles({f.path}, 0);
                             if (i == 2) {
                                 app.lib.toggleFavorite(f.path);
                                 app.lib.save();
                                 app.ui.toast(fav ? "Removed from favorites" : "Added to favorites", Icon::StarFilled);
                             }
                         },
                         formatSize(e.size));
        }
        return;
    }
    bool media = e.kind == MediaKind::Folder || e.kind == MediaKind::Video || e.kind == MediaKind::Audio ||
                 e.kind == MediaKind::Playlist || e.kind == MediaKind::Image || e.kind == MediaKind::Document;
    if (!e.isDrive && (managerMode() || (section_ == FILES && !marked_.empty()) || !media)) {
        managerMenu(e);
        return;
    }
    if (e.isDrive) {
        if (!e.removable) return;
        FileEntry drive = e;
        app.openMenu(e.name, {{"Open", Icon::Folder}, {"Safely remove", Icon::Close}}, [this, drive](int i) {
            if (i == 0) {
                openDir(drive.path);
            } else if (i == 1) {
                app.ui.toast(platform::ejectDrive(drive.path) ? trf("Safe to unplug {0}", {drive.name})
                                                               : "Couldn't remove the drive",
                             Icon::Drive);
                drivesChanged();
            }
        }, e.detail);
        return;
    }
    bool fav = app.lib.isFavorite(e.path);
    std::string name = e.name;
    while (!name.empty() && name.back() == '/') name.pop_back();
    FileEntry entry = e;

    if (e.kind == MediaKind::Folder) {
        items.push_back({"Open", Icon::Folder});
        actions.push_back([this, entry] { openDir(entry.path); });
        items.push_back({"Play all", Icon::Play});
        actions.push_back([this, entry] {
            auto list = app.lib.list(entry.path, false, true, SORT_NAME);
            std::vector<std::string> files;
            for (auto& f : list)
                if (f.kind == MediaKind::Video || f.kind == MediaKind::Audio) files.push_back(f.path);
            if (files.empty()) app.ui.toast("No playable files in this folder", Icon::Info);
            else app.playFiles(files, 0);
        });
    } else {
        double resume = app.lib.resumePosition(e.path);
        bool image = e.kind == MediaKind::Image, doc = e.kind == MediaKind::Document;
        items.push_back({image ? "Open" : doc ? (resume > 0 ? "Continue reading" : "Read") : (resume > 0 ? "Resume" : "Play"),
                         image ? Icon::Image : doc ? Icon::Book : Icon::Play, resume > 0 && !doc ? formatTime(resume) : ""});
        actions.push_back([this, entry] { app.playPath(entry.path); });
        if (resume > 0 && !doc) {
            items.push_back({"Play from beginning", Icon::Refresh});
            actions.push_back([this, entry] {
                auto h = app.lib.find(entry.path);
                app.lib.updateHistory(entry.path, 0, h ? h->duration : 0);
                app.playPath(entry.path);
            });
        }
        if (!image && !doc) {
            items.push_back({"Play only this file", Icon::Film});
            actions.push_back([this, entry] { app.playFiles({entry.path}, 0); });
        }
    }
    if (app.canCollect(e.path)) {
        bool liked = app.lib.isLiked(e.path);
        items.push_back({liked ? "Unlike" : "Like", liked ? Icon::Heart : Icon::HeartFilled});
        actions.push_back([this, entry] { app.toggleLike(entry.path); });
    }
    if (app.canCollect(e.path) || e.kind == MediaKind::Folder) {
        items.push_back({"Add to playlist", Icon::Playlist});
        actions.push_back([this, entry] { app.addToPlaylistMenu({entry.path}); });
    }
    // Copying, moving, renaming, deleting and the rest: the manage view's menu, for this item.
    if ((section_ == FILES || section_ == FAVORITES) && !netfs::isNetPath(e.path)) {
        items.push_back({"File actions…", Icon::Sliders, "copy, move, rename, delete"});
        actions.push_back([this, entry] { managerMenu(entry); });
    }
    items.push_back({fav ? "Remove from favorites" : "Add to favorites", fav ? Icon::Star : Icon::StarFilled});
    actions.push_back([this, entry, fav] {
        app.lib.toggleFavorite(entry.path);
        app.lib.save();
        app.ui.toast(fav ? "Removed from favorites" : "Added to favorites", Icon::StarFilled);
    });
    if (e.kind != MediaKind::Folder) {
        items.push_back({"Details", Icon::Info});
        actions.push_back([this, entry, name] {
            std::string msg = trf("Location: {0}", {platform::displayPath(parentDir(entry.path))}) + "\n" +
                              trf("Size: {0}", {formatSize(entry.size)}) + "\n" +
                              trf("Modified: {0}", {formatDate(entry.mtime)});
            auto h = app.lib.find(entry.path);
            if (h && h->duration > 0)
                msg += "\n" + trf("Length: {0}", {formatTime(h->duration)}) + "\n" +
                       trf("Last played: {0}", {tr(relativeDate(h->lastPlayed))});
            app.openMenu(name, {{"Close", Icon::Close}}, [](int) {}, msg);
        });
        if (app.cfg.s.showStats) {
            items.push_back({"Test read speed", Icon::Waveform});
            actions.push_back([this, entry, name] { readBenchmark(entry.path, name); });
        }
    }
    app.openMenu(name, items, [actions](int i) {
        if (i >= 0 && i < (int)actions.size()) actions[i]();
    });
}

// Measures raw SD-card reads of one file: sequential from the start and scattered 1 MB reads.
// Slow results on one file but not others usually mean the file is fragmented on the card.
void HomeScreen::readBenchmark(const std::string& path, const std::string& name) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        app.ui.toast("Couldn't open file", Icon::Info);
        return;
    }
    setvbuf(f, nullptr, _IONBF, 0);
    const size_t chunk = 1 << 20;
    std::vector<char> buf(chunk);
    double t0 = platform::nowSeconds(), worst = 0;
    size_t total = 0;
    while (total < (48u << 20)) {
        double a = platform::nowSeconds();
        size_t n = fread(buf.data(), 1, chunk, f);
        worst = std::max(worst, platform::nowSeconds() - a);
        total += n;
        if (n < chunk) break;
    }
    double seqTime = platform::nowSeconds() - t0;

    fseek(f, 0, SEEK_END);
    long long size = ftell(f);
    double r0 = platform::nowSeconds(), rworst = 0;
    int reads = 0;
    for (int i = 1; i <= 12 && size > (long long)chunk * 2; i++) {
        long long off = (size - (long long)chunk) * i / 13;
        double a = platform::nowSeconds();
        fseek(f, (long)off, SEEK_SET);
        if (fread(buf.data(), 1, chunk, f) == chunk) reads++;
        rworst = std::max(rworst, platform::nowSeconds() - a);
    }
    double rndTime = platform::nowSeconds() - r0;
    fclose(f);

    char msg[400];
    snprintf(msg, sizeof msg,
             "Sequential: %.1f MB/s (%.0f MB, slowest 1 MB took %.0f ms)\n"
             "Scattered: %.1f MB/s (%d reads, slowest %.0f ms)\n\n"
             "A 1080p movie needs about 1 MB/s. Much slower numbers for some files only usually mean "
             "they are fragmented on the SD card; copying them off and back on fixes that.",
             seqTime > 0 ? total / 1048576.0 / seqTime : 0, total / 1048576.0, worst * 1000,
             rndTime > 0 ? reads / rndTime : 0, reads, rworst * 1000);
    app.openMenu(trf("Read speed: {0}", {name}), {{"Close", Icon::Close}}, [](int) {}, msg);
}

void HomeScreen::sortMenu() {
    std::vector<MenuItem> items;
    for (int i = 0; i < SORT_COUNT; i++) {
        MenuItem m{sortModeName(i), Icon::Sort};
        m.checked = app.cfg.s.sortMode == i;
        items.push_back(m);
    }
    MenuItem all{"Show all file types", Icon::File};
    all.checked = !app.cfg.s.mediaOnly;
    items.push_back(all);
    MenuItem hidden{"Show hidden files", Icon::Eye};
    hidden.checked = app.cfg.s.showHidden;
    items.push_back(hidden);
    app.openMenu("Sort & filter", items, [this](int i) {
        if (i < 0) return;
        if (i < SORT_COUNT) app.cfg.s.sortMode = i;
        else if (i == SORT_COUNT) app.cfg.s.mediaOnly = !app.cfg.s.mediaOnly;
        else app.cfg.s.showHidden = !app.cfg.s.showHidden;
        app.saveSoon();
        refresh();
        listTime_ = 0;
    });
}

// ------------------------------------------------------------------ recent

void HomeScreen::drawRecent(Rect area) {
    UI& ui = app.ui;
    ui.text(area.x, area.y + 28, "Continue watching", 30, ui.t.text, LEFT, true);
    ui.text(area.x + 1, area.y + 60, "Your recently played files", 15, ui.t.textFaint);
    Rect list{area.x - 12, area.y + 84, area.w + 24, area.h - 84};
    auto& hist = app.lib.history;
    if (hist.empty()) {
        drawEmpty(list, Icon::Clock, "Nothing played yet", "Music and videos you play will show up here");
        return;
    }
    float rowH = 84;
    scroll_.update(ui, list, hist.size() * rowH + 12);
    scroll_.begin(ui);
    int open = -1, menu = -1;
    for (size_t i = 0; i < hist.size(); i++) {
        const HistoryItem& h = hist[i];
        Rect r{list.x + 6, list.y + 6 + i * rowH - scroll_.offset, list.w - 12, rowH};
        uint32_t id = uid(h.path.c_str(), 91);
        ui.focusable(id, r, G_LIST);
        bool foc = ui.focused(id);
        if (foc) menu = (int)i;
        if (foc && ui.focusMoved) scroll_.ensureVisible(6 + i * rowH, rowH);
        if (r.bottom() < list.y || r.y > list.bottom()) continue;

        float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
        float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
        Rect card = r.inset(0, 4);
        if (fv > 0.01f || pv > 0.01f) ui.fillRect(card, withAlpha(ui.t.surface2, std::max(fv, pv)));
        ui.focusRing(card, ui.t.radius, fv);

        // Cover art with a play badge on top, or just the badge
        Rect tile{card.x + 12, card.cy() - 27, 54, 54};
        float tileRad = std::min(ui.t.radius, 16.0f);
        const CoverArt::Art* art = isUrl(h.path) ? nullptr : app.covers.get(h.path, kindForExtension(fileExtension(h.path)));
        if (art) {
            drawCover(tile, *art, tileRad);
            nvgBeginPath(ui.vg);
            nvgCircle(ui.vg, tile.cx(), tile.cy(), 15);
            nvgFillColor(ui.vg, hex(0x000000, 0.55f));
            nvgFill(ui.vg);
            ui.icon(Icon::Play, tile.cx() + 1, tile.cy(), 16, hex(0xFFFFFF));
        } else {
            nvgBeginPath(ui.vg);
            nvgRoundedRect(ui.vg, tile.x, tile.y, tile.w, tile.h, tileRad);
            nvgFillPaint(ui.vg, nvgLinearGradient(ui.vg, tile.x, tile.y, tile.right(), tile.bottom(),
                                                  withAlpha(ui.t.accent, 0.9f), withAlpha(ui.t.accent2, 0.9f)));
            nvgFill(ui.vg);
            bool book = kindForExtension(fileExtension(h.path)) == MediaKind::Document;
            ui.icon(isUrl(h.path) ? Icon::Globe : book ? Icon::Book : Icon::Play,
                    tile.cx() + (isUrl(h.path) || book ? 0 : 2), tile.cy(), 24, hex(0xFFFFFF));
        }

        float x = tile.right() + 18, right = card.right() - 110;
        std::string name = shownTitle(h.path, app.cfg.s.tidyNames);
        std::string named = app.streamName(h.path);  // a stream from the Streams list, by its name
        if (!named.empty()) name = named;
        else if (!h.title.empty()) name = h.title;
        else if (isUrl(h.path) && !netfs::isNetPath(h.path)) {
            // A stream: its file name when the address ends in one ("…/Film%20One.mkv" -> "Film One"),
            // otherwise the address itself (a live channel, a playlist without a name).
            std::string base = h.path.substr(0, h.path.find_first_of("?#"));
            std::string file = netfs::decode(baseName(base));
            MediaKind k = kindForExtension(fileExtension(file));
            name = (k == MediaKind::Video || k == MediaKind::Audio) ? (app.cfg.s.tidyNames && k == MediaKind::Video ? tidyTitle(file) : stripExtension(file))
                                                                    : h.path;
        }
        for (auto& s : app.lib.streams)
            if (s.url == h.path) name = s.name;
        if (foc && ui.showFocus()) ui.marquee(id ^ 0x3a9, x, card.cy() - 14, right - x, name, 20, ui.t.text, LEFT, true);
        else ui.text(x, card.cy() - 14, ui.ellipsize(name, right - x, 20, foc), 20, ui.t.text, LEFT, foc);
        std::string detail;
        float p = 0;
        bool doc = kindForExtension(fileExtension(h.path)) == MediaKind::Document;
        if (h.position < 0) {
            detail = tr("Finished");
            p = 1;
        } else if (doc && h.duration > 0) {
            detail = "Page " + std::to_string((int)h.position + 1) + " of " + std::to_string((int)h.duration);
            p = (float)((h.position + 1) / h.duration);
        } else if (h.duration > 0) {
            detail = formatTime(h.position) + " / " + formatTime(h.duration);
            p = (float)(h.position / h.duration);
        }
        std::string when = relativeDate(h.lastPlayed);
        if (!when.empty()) detail += (detail.empty() ? "" : "  ·  ") + tr(when);
        ui.text(x, card.cy() + 9, detail, 15, ui.t.textFaint);
        float bw = std::min(260.0f, right - x);
        if (p > 0) ui.fillRect({x, card.cy() + 24, bw, 4}, withAlpha(ui.t.textFaint, 0.25f), 2);  // only once started
        if (p > 0) ui.fillRect({x, card.cy() + 24, bw * std::min(p, 1.0f), 4}, p >= 1 ? ui.t.success : ui.t.accent, 2);
        // On the right: what's left to watch, or a tick when it's done.
        if (p >= 1) {
            nvgBeginPath(ui.vg);
            nvgCircle(ui.vg, card.right() - 36, card.cy(), 14);
            nvgFillColor(ui.vg, withAlpha(ui.t.success, 0.18f));
            nvgFill(ui.vg);
            ui.icon(Icon::Check, card.right() - 36, card.cy(), 16, ui.t.success);
        } else if (h.duration > 0 && h.position > 0 && !doc) {
            ui.text(card.right() - 24, card.cy(), "−" + formatTime(std::max(0.0, h.duration - h.position)), 17,
                    ui.t.textDim, RIGHT, true);
        }

        if (ui.longPressIn(card)) {
            ui.setFocus(id);
            menu = (int)i;
            ui.in->longPress = false;
            contextIndex_ = -2;
        }
        if (ui.tapped(card) || (foc && ui.confirm())) open = (int)i;
    }
    scroll_.end(ui);
    scroll_.drawScrollbar(ui);

    if (open >= 0) {
        std::string p = hist[open].path;
        if (isUrl(p)) app.playFiles({p}, 0);
        else app.playPath(p);
        return;
    }
    bool wantMenu = contextIndex_ == -2 || (!ui.blocked && ui.in->down(BTN_X));
    contextIndex_ = -1;
    if (wantMenu && menu >= 0) {
        std::string path = hist[menu].path;
        std::vector<MenuItem> items = {{"Play from beginning", Icon::Refresh},
                                       {"Open containing folder", Icon::Folder},
                                       {"Remove from history", Icon::Close},
                                       {"Clear all history", Icon::Trash, "", true}};
        bool url = isUrl(path) && !netfs::isNetPath(path);  // a stream: no folder to show (a server's file has one)
        if (url) items.erase(items.begin() + 1);
        app.openMenu(url ? path : shownTitle(path, app.cfg.s.tidyNames), items, [this, path, url](int i) {
            if (i < 0) return;
            if (url && i >= 1) i++;
            if (i == 0) {
                auto h = app.lib.find(path);
                app.lib.updateHistory(path, 0, h ? h->duration : 0);
                if (url) app.playFiles({path}, 0);
                else app.playPath(path);
            } else if (i == 1) {
                setSection(netfs::isNetPath(path) ? NETWORK : FILES);
                dirFocus_[parentDir(path)] = path;
                openDir(parentDir(path), false);
                app.ui.setFocus(uid(path.c_str(), 77));
            } else if (i == 2) {
                app.lib.removeHistory(path);
                app.lib.save();
            } else if (i == 3) {
                app.confirm("Clear history?", "This also forgets all resume positions.", "Clear", [this] {
                    app.lib.clearHistory();
                    app.lib.save();
                }, true);
            }
        });
    }
}

// ------------------------------------------------------------------ favorites

void HomeScreen::drawFavorites(Rect area) {
    UI& ui = app.ui;
    // The Liked list opens here, in the same view as a playlist.
    if (playlistOpen_ == kLikedList) {
        drawPlaylistItems(area);
        return;
    }
    ui.text(area.x, area.y + 28, "Favorites", 30, ui.t.text, LEFT, true);
    ui.text(area.x + 1, area.y + 60, "Songs and videos you like, and pinned folders and files", 15, ui.t.textFaint);
    Rect list{area.x - 12, area.y + 84, area.w + 24, area.h - 84};
    // Looked up when the list changes; asking the SD card about every file 60 times a second adds up.
    if (favRevision_ != app.lib.revision) {
        favRevision_ = app.lib.revision;
        favEntries_.clear();
        for (auto& p : app.lib.favorites) favEntries_.push_back(entryFromPath(p));
    }
    std::vector<FileEntry>& favs = favEntries_;
    const float likedH = 88, headH = favs.empty() ? 0 : 40;
    float contentH = likedH + headH + favs.size() * ROW_H + 12 + (favs.empty() ? 150 : 0);
    scroll_.update(ui, list, contentH);
    scroll_.begin(ui);
    contextIndex_ = -1;
    int open = -1;
    bool openLiked = false;
    {
        // Liked: always first, like a playlist the app keeps for you.
        Rect r{list.x + 6, list.y + 6 - scroll_.offset, list.w - 12, likedH};
        uint32_t id = uid("fav.liked");
        ui.focusable(id, r, G_LIST);
        bool foc = ui.focused(id);
        if (foc && ui.focusMoved) scroll_.ensureVisible(6, likedH);
        float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
        float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
        Rect card = r.inset(0, 4);
        ui.fillRect(card, withAlpha(mix(ui.t.surface, ui.t.surface2, std::max(fv, pv)), 0.8f), ui.t.radius);
        ui.focusRing(card, ui.t.radius, fv);
        Rect tile{card.x + 12, card.cy() - 28, 56, 56};
        nvgBeginPath(ui.vg);
        nvgRoundedRect(ui.vg, tile.x, tile.y, tile.w, tile.h, std::min(ui.t.radius, 16.0f));
        nvgFillPaint(ui.vg, nvgLinearGradient(ui.vg, tile.x, tile.y, tile.right(), tile.bottom(), ui.t.danger,
                                              mix(ui.t.danger, ui.t.accent2, 0.5f)));
        nvgFill(ui.vg);
        ui.icon(Icon::HeartFilled, tile.cx(), tile.cy(), 28, hex(0xFFFFFF));
        size_t n = app.lib.liked.size();
        ui.text(tile.right() + 18, card.cy() - 11, "Liked", 20, ui.t.text, LEFT, true);
        ui.text(tile.right() + 18, card.cy() + 14,
                n ? tr(std::to_string(n) + (n == 1 ? " file" : " files")) + "  ·  " + tr("tap the heart in the player to add more")
                  : "Tap the heart in the player to like a song or video",
                15, ui.t.textFaint);
        ui.icon(Icon::ChevronRight, card.right() - 28, card.cy(), 22, ui.t.textFaint);
        if (ui.tapped(card) || (foc && ui.confirm())) openLiked = true;
    }
    float y0 = 6 + likedH;
    if (!favs.empty()) {
        ui.text(list.x + 18, list.y + y0 + 22 - scroll_.offset, "PINNED", 13, ui.t.textFaint, LEFT, true);
        y0 += headH;
    }
    for (size_t i = 0; i < favs.size(); i++) {
        Rect r{list.x + 6, list.y + y0 + i * ROW_H - scroll_.offset, list.w - 12, ROW_H};
        uint32_t id = uid(favs[i].path.c_str(), 77);
        if (ui.focused(id) && ui.focusMoved) scroll_.ensureVisible(y0 + i * ROW_H, ROW_H);
        if (fileRow(id, r, favs[i], (int)i)) open = (int)i;
    }
    if (favs.empty()) {
        float ey = list.y + y0 + 30 - scroll_.offset;
        ui.icon(Icon::Star, list.cx(), ey + 24, 34, ui.t.textFaint);
        ui.text(list.cx(), ey + 70, "Nothing pinned yet", 18, ui.t.textDim, CENTER, true);
        ui.text(list.cx(), ey + 98, "Press X on a file or folder (or hold it) and choose Add to favorites", 15,
                ui.t.textFaint, CENTER);
    }
    scroll_.end(ui);
    scroll_.drawScrollbar(ui);
    if (openLiked) {
        openPlaylist(kLikedList);
        return;
    }
    if (open >= 0) {
        FileEntry e = favs[open];
        if (e.kind == MediaKind::Folder) {
            setSection(netfs::isNetPath(e.path) ? NETWORK : FILES);
            openDir(e.path);
        } else {
            openEntry(e);
        }
        return;
    }
    if (!ui.blocked && ui.in->down(BTN_X) && contextIndex_ >= 0) entryMenu(favs[contextIndex_]);
}

// ------------------------------------------------------------------ streams

void HomeScreen::drawStreams(Rect area) {
    UI& ui = app.ui;
    ui.text(area.x, area.y + 28, "Streams", 30, ui.t.text, LEFT, true);
    ui.text(area.x + 1, area.y + 60, "HTTP(S), HLS, RTMP, RTSP and IPTV .m3u links", 15, ui.t.textFaint);
    Rect list{area.x - 12, area.y + 84, area.w + 24, area.h - 84};
    auto& streams = app.lib.streams;
    float rowH = ROW_H;
    scroll_.update(ui, list, (streams.size() + 1) * rowH + 12);
    scroll_.begin(ui);
    int open = -2, menu = -1;
    for (int i = -1; i < (int)streams.size(); i++) {
        Rect r{list.x + 6, list.y + 6 + (i + 1) * rowH - scroll_.offset, list.w - 12, rowH};
        uint32_t id = i < 0 ? uid("stream.add") : uid(streams[i].url.c_str(), 1000 + i);
        ui.focusable(id, r, G_LIST);
        bool foc = ui.focused(id);
        if (foc && ui.focusMoved) scroll_.ensureVisible(6 + (i + 1) * rowH, rowH);
        if (foc) menu = i;
        float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
        float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
        Rect card = r.inset(0, 4);
        if (i < 0) {
            nvgBeginPath(ui.vg);
            nvgRoundedRect(ui.vg, card.x + 1, card.y + 1, card.w - 2, card.h - 2, ui.t.radius);
            nvgStrokeColor(ui.vg, withAlpha(ui.t.accent, 0.5f + 0.5f * fv));
            nvgStrokeWidth(ui.vg, 1.5f);
            nvgStroke(ui.vg);
        }
        if (fv > 0.01f || pv > 0.01f) ui.fillRect(card, withAlpha(ui.t.surface2, std::max(fv, pv)));
        ui.focusRing(card, ui.t.radius, fv);
        Rect tile{card.x + 12, card.cy() - 25, 50, 50};
        if (i < 0) {
            ui.fillRect(tile, withAlpha(ui.t.accent, 0.18f), std::min(ui.t.radius, 15.0f));
            ui.icon(Icon::Plus, tile.cx(), tile.cy(), 26, ui.t.accent);
            ui.text(tile.right() + 18, card.cy() - 11, "Add a stream", 20, ui.t.accent, LEFT, true);
            ui.text(tile.right() + 18, card.cy() + 14, "Enter a URL with the on-screen keyboard", 15, ui.t.textFaint);
        } else {
            ui.fillRect(tile, mix(ui.t.surface2, hex(0x16C4A8), 0.18f), std::min(ui.t.radius, 15.0f));
            ui.icon(Icon::Globe, tile.cx(), tile.cy(), 26, hex(0x16C4A8));
            float x = tile.right() + 18, right = card.right() - 20;
            ui.text(x, card.cy() - 11, ui.ellipsize(streams[i].name, right - x, 20, foc), 20, ui.t.text, LEFT, foc);
            ui.text(x, card.cy() + 14, ui.ellipsize(streams[i].url, right - x, 15), 15, ui.t.textFaint);
        }
        if (i >= 0 && ui.longPressIn(card)) {
            ui.setFocus(id);
            contextIndex_ = -2;
            menu = i;
        }
        if (ui.tapped(card) || (foc && ui.confirm())) open = i;
    }
    scroll_.end(ui);
    scroll_.drawScrollbar(ui);

    auto editStream = [this](int idx) {
        std::string url = idx >= 0 ? app.lib.streams[idx].url : "https://";
        if (!app.askText("Stream URL", url, url) || url.empty() || url == "https://") return;
        std::string name = idx >= 0 ? app.lib.streams[idx].name : baseName(url);
        if (name.empty()) name = url;
        if (!app.askText("Name", name, name)) name = baseName(url);
        if (idx >= 0) app.lib.streams[idx] = {name, url};
        else app.lib.streams.push_back({name, url});
        app.lib.save();
    };

    if (open == -1) {
        editStream(-1);
        return;
    }
    if (open >= 0) {
        app.playStream(streams[open].url, streams[open].name);
        return;
    }
    bool wantMenu = contextIndex_ == -2 || (!ui.blocked && ui.in->down(BTN_X));
    contextIndex_ = -1;
    if (wantMenu && menu >= 0) {
        std::vector<MenuItem> items = {{"Play", Icon::Play}, {"Edit", Icon::Sliders}, {"Delete", Icon::Trash, "", true}};
        app.openMenu(streams[menu].name, items, [this, menu, editStream](int i) {
            if (i == 0) app.playStream(app.lib.streams[menu].url, app.lib.streams[menu].name);
            else if (i == 1) editStream(menu);
            else if (i == 2) {
                app.lib.streams.erase(app.lib.streams.begin() + menu);
                app.lib.save();
            }
        });
    }
}

// ------------------------------------------------------------------ settings

void HomeScreen::settingChanged(const SettingDef& d) {
    (void)d;
    app.settingsChanged();
    if (d.field == &Settings::showHidden || d.field == &Settings::mediaOnly) refresh();
}

// The HOME Menu icon as an ordinary on/off setting: on adds it (now, or once the keys turn up).
void HomeScreen::homeIconRow(Rect r, uint32_t id) {
    UI& ui = app.ui;
    ui.focusable(id, r, G_LIST);
    bool foc = ui.focused(id);
    float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
    float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
    ui.pushAlpha(settingsScroll_.edgeFade(r.y, r.h));
    Rect card = r.inset(0, 4);
    if (fv > 0.01f || pv > 0.01f) ui.fillRect(card, withAlpha(ui.t.surface2, std::max(fv, pv)));
    ui.focusRing(card, ui.t.radius, fv);
    bool installed = app.forwarderInstalled(), pending = !installed && app.forwarderPending();
    bool outdated = installed && app.forwarderOutdated();
    const char* desc = outdated ? "An update for the icon is ready: open SwitchMX once from the Album to install it"
                       : installed ? "SwitchMX has its own icon on the HOME Menu"
                       : pending ? "Waiting for prod.keys on the SD card; it's added by itself"
                                 : "Start SwitchMX from its own icon instead of the Album";
    float x = card.x + 22, labelW = card.w - 120;
    ui.icon(Icon::Home, x + 12, card.cy(), 24, installed ? ui.t.accent : ui.t.textDim);
    x += 40;
    ui.text(x, card.cy() - 11, "HOME Menu icon", 20, ui.t.text, LEFT, true);
    ui.text(x, card.cy() + 14, ui.ellipsize(desc, labelW - 40, 15), 15, pending || outdated ? ui.t.warning : ui.t.textFaint);
    Rect ctrl{card.right() - 80, card.y, 60, card.h};
    ui.toggleKnob(ctrl, installed || pending, id);
    ui.popAlpha();
    if (!(ui.tapped(card) || (foc && ui.confirm()))) return;
    if (pending) {
        app.setForwarderPending(false);
        ui.toast("The icon won't be added", Icon::Close);
    } else if (installed && app.forwarderRunningFromIcon()) {
        app.openMenu("Open SwitchMX from the Album first", {{"OK", Icon::Check}}, [](int) {},
                     "SwitchMX is running from this icon right now, so it can't remove it. Close the app, open it "
                     "from the Album (hbmenu), and turn this off there.");
    } else if (installed) {
        app.confirm("Remove the HOME Menu icon?", "The app itself stays on your SD card.", "Remove",
                    [this] { app.forwarderRemove(); }, true);
    } else {
        app.forwarderInstall();
    }
}

bool HomeScreen::settingRow(const SettingDef& d, Rect r, uint32_t id) {
    UI& ui = app.ui;
    Input& in = *ui.in;
    int& v = app.cfg.s.*(d.field);
    ui.focusable(id, r, G_LIST);
    bool foc = ui.focused(id);
    bool showFoc = foc && ui.showFocus();
    float fv = ui.smooth(id ^ 0xf0c, showFoc ? 1.0f : 0.0f, 16);
    float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) && d.type != SettingType::Slider ? 1.0f : 0.0f, 25);
    ui.pushAlpha(settingsScroll_.edgeFade(r.y, r.h));
    Rect card = r.inset(0, 4);
    if (fv > 0.01f || pv > 0.01f) ui.fillRect(card, withAlpha(ui.t.surface2, std::max(fv, pv)));
    ui.focusRing(card, ui.t.radius, fv);

    float ctrlW = 0;
    switch (d.type) {
        case SettingType::Toggle: ctrlW = 60; break;
        case SettingType::Choice: ctrlW = 270; break;
        case SettingType::Slider: ctrlW = 320; break;
        case SettingType::Color: ctrlW = std::min(card.w * 0.5f, d.choices.size() * 32.0f + 8); break;
    }
    float x = card.x + 22, labelW = card.w - ctrlW - 60;
    // The highlighted row glides its label and description, so a long one can be read in full.
    if (showFoc) {
        ui.marquee(id ^ 0x5a1, x, card.cy() - 11, labelW, d.label, 20, ui.t.text, LEFT, true);
        ui.marquee(id ^ 0x5a2, x, card.cy() + 14, labelW, d.desc, 15, ui.t.textFaint, LEFT, false);
    } else {
        ui.text(x, card.cy() - 11, ui.ellipsize(d.label, labelW, 20, true), 20, ui.t.text, LEFT, true);
        ui.text(x, card.cy() + 14, ui.ellipsize(d.desc, labelW, 15), 15, ui.t.textFaint);
    }
    Rect ctrl{card.right() - ctrlW - 20, card.y, ctrlW, card.h};
    bool changed = false;
    bool blocked = ui.blocked;

    switch (d.type) {
        case SettingType::Toggle:
            ui.toggleKnob(ctrl, v != 0, id);
            if (ui.tapped(card) || (foc && ui.confirm())) {
                v = !v;
                changed = true;
            }
            break;
        case SettingType::Choice: {
            // The font row's names depend on the language on screen, which can change while the app
            // runs; the settings list itself is built once, so they are read fresh here.
            std::vector<std::string> live;
            if (std::strcmp(d.key, "ui_font") == 0) {
                live.push_back(tr("Automatic"));
                for (int k = 0; k < App::uiFontCount(); k++) live.push_back(App::uiFontName(k));
            }
            const std::vector<std::string>& choices = live.empty() ? d.choices : live;
            int n = (int)choices.size();
            Rect lt{ctrl.x, ctrl.cy() - 22, 44, 44}, rt{ctrl.right() - 44, ctrl.cy() - 22, 44, 44};
            ui.fillRect({ctrl.x, ctrl.cy() - 22, ctrl.w, 44}, withAlpha(ui.t.surfaceHi, 0.6f), 22);
            ui.icon(Icon::ChevronLeft, lt.cx(), lt.cy(), 20, v > 0 ? ui.t.text : ui.t.textFaint);
            ui.icon(Icon::ChevronRight, rt.cx(), rt.cy(), 20, v < n - 1 ? ui.t.text : ui.t.textFaint);
            ui.text(ctrl.cx(), ctrl.cy(), ui.ellipsize(choices[std::clamp(v, 0, n - 1)], ctrl.w - 100, 17, true), 17,
                    foc ? ui.t.accent : ui.t.text, CENTER, true);
            if (ui.tapped(lt.inset(-6))) { v = std::max(0, v - 1); changed = true; }
            else if (ui.tapped(rt.inset(-6))) { v = std::min(n - 1, v + 1); changed = true; }
            if (foc && !blocked) {
                if (in.rep(BTN_LEFT) && v > 0) { v--; changed = true; }
                if (in.rep(BTN_RIGHT) && v < n - 1) { v++; changed = true; }
                ui.consumeNav(BTN_LEFT | BTN_RIGHT);
            }
            if (ui.tapped(card) || (foc && ui.confirm())) {
                // Full picker for long lists, cycling for short ones.
                if (n > 3) {
                    std::vector<MenuItem> items;
                    // The names the picker shows have to be the live ones too: the font row's list
                    // follows the language, so reading d.choices here would show the set that was
                    // current when the settings list was built (and could be a different length).
                    for (int i = 0; i < n; i++) {
                        MenuItem m{choices[i]};
                        m.checked = i == v;
                        items.push_back(m);
                    }
                    const SettingDef* dp = &d;
                    app.openMenu(d.label, items, [this, dp](int i) {
                        if (i < 0) return;
                        app.cfg.s.*(dp->field) = i;
                        settingChanged(*dp);
                    });
                } else {
                    v = (v + 1) % n;
                    changed = true;
                }
            }
            break;
        }
        case SettingType::Slider: {
            float fvv = (float)v;
            Rect sr{ctrl.x + 8, ctrl.y, ctrl.w - 90, ctrl.h};
            if (ui.slider(id, sr, fvv, (float)d.min, (float)d.max, (float)d.step, foc)) {
                v = (int)std::lround(fvv);
                changed = true;
            }
            ui.text(ctrl.right(), ctrl.cy(), std::to_string(v) + d.unit, 18, ui.t.text, RIGHT, true);
            break;
        }
        case SettingType::Color: {
            int n = (int)d.choices.size();
            for (int i = 0; i < n; i++) {
                float spacing = (ctrl.w - 8) / n;
                float cx = ctrl.x + 4 + spacing * (i + 0.5f), cy = ctrl.cy();
                bool selc = i == v;
                float sv = ui.smooth(id ^ (0xc0 + i), selc ? 1.0f : 0.0f, 18);
                nvgBeginPath(ui.vg);
                float base = std::min(11.0f, spacing * 0.36f);
                nvgCircle(ui.vg, cx, cy, base + 2 * sv);
                nvgFillColor(ui.vg, accentColor(i));
                nvgFill(ui.vg);
                if (sv > 0.01f) {
                    nvgBeginPath(ui.vg);
                    nvgCircle(ui.vg, cx, cy, base + 6);
                    nvgStrokeColor(ui.vg, withAlpha(ui.t.text, sv));
                    nvgStrokeWidth(ui.vg, 2);
                    nvgStroke(ui.vg);
                    ui.icon(Icon::Check, cx, cy, 14 * sv, hex(0xFFFFFF));
                }
                if (ui.tapped({cx - spacing / 2, cy - 22, spacing, 44})) {
                    v = i;
                    changed = true;
                }
            }
            if (foc && !blocked) {
                if (in.rep(BTN_LEFT)) { v = (v + n - 1) % n; changed = true; }
                if (in.rep(BTN_RIGHT)) { v = (v + 1) % n; changed = true; }
                ui.consumeNav(BTN_LEFT | BTN_RIGHT);
            }
            break;
        }
    }
    ui.popAlpha();
    if (changed) settingChanged(d);
    return foc;
}

void HomeScreen::drawSettings(Rect area) {
    UI& ui = app.ui;
    auto& sections = Config::sections();
    int tabCount = (int)sections.size() + 1;  // + About
    ui.text(area.x, area.y + 28, "Settings", 30, ui.t.text, LEFT, true);

    // Category chips; collapse inactive labels when space is tight.
    auto tabLabel = [&](int i) -> std::string { return i < (int)sections.size() ? sections[i] : "About"; };
    float full = 0;
    for (int i = 0; i < tabCount; i++) full += ui.textWidth(tabLabel(i), 16, true) + 62;
    bool compact = full > area.w;
    float x = area.x;
    float ty = area.y + 58;
    for (int i = 0; i < tabCount; i++) {
        bool active = i == settingsTab_;
        std::string label = (compact && !active) ? "" : tabLabel(i);
        float w = label.empty() ? 46 : ui.textWidth(label, 16, true) + 62;
        Rect r{x, ty, w, 42};
        uint32_t id = uid("settings.tab", i);
        ui.focusable(id, r, G_TABS);
        float fv = ui.smooth(id ^ 0xf0c, ui.focused(id) && ui.showFocus() ? 1.0f : 0.0f, 16);
        float av = ui.smooth(id ^ 0xac7, active ? 1.0f : 0.0f, 16);
        ui.fillRect(r, mix(withAlpha(ui.t.surface2, 0.8f), ui.t.accent, av), 21);
        ui.focusRing(r, 21, fv);
        NVGcolor fg = mix(ui.t.textDim, ui.t.onAccent, av);
        if (label.empty()) {
            ui.icon(kSettingsIcons[i], r.cx(), r.cy(), 20, fg);
        } else {
            ui.icon(kSettingsIcons[i], r.x + 24, r.cy(), 20, fg);
            ui.text(r.x + 42, r.cy(), label, 16, fg, LEFT, true);
        }
        if (ui.tapped(r) || (ui.focused(id) && ui.confirm())) {
            settingsTab_ = i;
            settingsScroll_.reset();
            sectionTime_ = 0.15f;
        }
        x += w + 8;
    }
    if (!ui.blocked && (ui.in->down(BTN_L) || ui.in->down(BTN_R))) {
        settingsTab_ = (settingsTab_ + (ui.in->down(BTN_R) ? 1 : tabCount - 1)) % tabCount;
        settingsScroll_.reset();
        ui.setFocus(uid("settings.tab", settingsTab_));
    }

    Rect list{area.x - 12, area.y + 112, area.w + 24, area.h - 112};
    if (settingsTab_ == tabCount - 1) {
        // About
        settingsScroll_.update(ui, list, aboutH_);
        settingsScroll_.begin(ui);
        float y = drawAbout(list, list.y + 10 - settingsScroll_.offset);
        Rect tips{list.x + 6, y, list.w - 12, 388};
        // Reachable with the controller, so the page scrolls all the way down too.
        uint32_t tipsId = uid("about.tips");
        ui.focusable(tipsId, tips, G_LIST);
        if (ui.focused(tipsId) && ui.focusMoved)
            settingsScroll_.ensureVisible(tips.y - list.y + settingsScroll_.offset, tips.h);
        ui.focusRing(tips, ui.t.radius, ui.smooth(tipsId ^ 0xf0c, ui.focused(tipsId) && ui.showFocus() ? 1.0f : 0.0f, 16));
        ui.fillRect(tips, withAlpha(ui.t.surface, 0.8f));
        ui.text(tips.x + 24, tips.y + 32, "Player controls", 20, ui.t.text, LEFT, true);
        struct Tip { uint32_t b; const char* t; } tipList[] = {
            {BTN_A, "Play / pause"},           {BTN_B, "Show controls, then exit"},
            {BTN_X, actionName((Action)app.cfg.s.bindX)}, {BTN_Y, actionName((Action)app.cfg.s.bindY)},
            {BTN_L, actionName((Action)app.cfg.s.bindL)}, {BTN_R, actionName((Action)app.cfg.s.bindR)},
            {BTN_ZL, actionName((Action)app.cfg.s.bindZL)}, {BTN_ZR, actionName((Action)app.cfg.s.bindZR)},
            {BTN_PLUS, "Player menu"},         {BTN_MINUS, "Lock controls"},
        };
        for (int i = 0; i < 10; i++) {
            // Pictures of the buttons, not buttons: tapping here mustn't press them.
            float colW = tips.w / 2 - 24;
            float tx = tips.x + 24 + (i % 2) * (tips.w / 2);
            float tyy = tips.y + 72 + (i / 2) * 36;
            bool wide = tipList[i].b == BTN_L || tipList[i].b == BTN_R || tipList[i].b == BTN_ZL || tipList[i].b == BTN_ZR;
            float gw = wide ? 36 : 24;
            ui.glyph(tipList[i].b, tx + gw / 2, tyy, 24);
            ui.text(tx + gw + 8, tyy, ui.ellipsize(tipList[i].t, colW - gw - 16, 17), 17, ui.t.textDim);
        }
        ui.textBox(tips.x + 24, tips.y + 252, tips.w - 48,
                   "Touch: double-tap sides to seek · swipe to scrub · pinch to zoom · hold for fast-forward", 15,
                   ui.t.textFaint);
        ui.text(tips.x + 24, tips.y + 306,
                ui.ellipsize(trf("Power users: put mpv.conf in {0}", {platform::displayPath(app.dataDir) + "mpv/"}), tips.w - 48, 15),
                15, ui.t.textFaint);
        // The AGPL asks that everyone using the app be told the licence and where the source is, so
        // this line stays on the About page as well as on the Wi-Fi transfer page.
        ui.marquee(uid("about.licence"), tips.x + 24, tips.y + 334, tips.w - 48,
                   tr("Free software under the AGPL-3.0 · source at github.com/Farzin-Abdi/SwitchMX"), 14,
                   ui.t.textFaint);
        ui.marquee(uid("about.credits"), tips.x + 24, tips.y + 360, tips.w - 48,
                   tr("Powered by mpv, FFmpeg, libass, MuPDF, libsmb2, libusbhsfs, libarchive, NanoVG and QR Code "
                      "generator (Project Nayuki)"),
                   14, ui.t.textFaint);
        // Up / down move row by row through this page (the general nearest-item rule skips rows that
        // are scrolled out of view); up from the first row also scrolls back to the very top.
        {
            const uint32_t rows[5][2] = {{uid("about.update"), uid("about.update")},
                                         {uid("about.github"), uid("about.mail")},
                                         {uid("about.donate", 0), uid("about.donate", 1)},
                                         {uid("about.donate", 2), uid("about.donate", 3)},
                                         {tipsId, tipsId}};
            Input& in = *ui.in;
            int row = -1, col = 0;
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 2; c++)
                    if (ui.focus == rows[r][c]) row = r, col = c;
            if (row >= 0 && !ui.blocked) {
                if (in.rep(BTN_DOWN) && row < 4) {
                    ui.setFocus(rows[row + 1][col]);
                    in.repeat &= ~BTN_DOWN;
                    in.pressed &= ~BTN_DOWN;
                } else if (in.rep(BTN_UP)) {
                    if (row > 0) {
                        ui.setFocus(rows[row - 1][col]);
                        in.repeat &= ~BTN_UP;
                        in.pressed &= ~BTN_UP;
                    } else {
                        settingsScroll_.target = 0;  // to the tabs, with the whole page from its top
                    }
                }
            }
        }
        aboutH_ = tips.bottom() + 20 - (list.y + 10 - settingsScroll_.offset) + 10;
        settingsScroll_.end(ui);
        settingsScroll_.drawScrollbar(ui);
        return;
    }

    std::vector<const SettingDef*> rows;
    for (auto& d : Config::defs()) {
        if (d.section != settingsTab_) continue;
        if (d.dependsOn && !(app.cfg.s.*(d.dependsOn))) continue;
        rows.push_back(&d);
    }
    bool advanced = settingsTab_ == 5;
    float rowH = 80;
    bool iconRow = advanced && app.forwarderAvailable();
    float top = iconRow ? rowH : 0;
    bool devCard = settingsTab_ == 6 && app.cfg.s.devMode;
    float contentH = top + rows.size() * rowH + (advanced ? 90 : 0) + (devCard ? 214 : 0) + 12;
    settingsScroll_.update(ui, list, contentH);
    settingsScroll_.begin(ui);
    if (iconRow) {
        Rect r{list.x + 6, list.y + 6 - settingsScroll_.offset, list.w - 12, rowH};
        uint32_t id = uid("settings.homeicon");
        if (ui.focused(id) && ui.focusMoved) settingsScroll_.ensureVisible(6, rowH);
        homeIconRow(r, id);
    }
    for (size_t i = 0; i < rows.size(); i++) {
        Rect r{list.x + 6, list.y + 6 + top + i * rowH - settingsScroll_.offset, list.w - 12, rowH};
        uint32_t id = uid(rows[i]->key, 55);
        if (ui.focused(id) && ui.focusMoved) settingsScroll_.ensureVisible(6 + top + i * rowH, rowH);
        settingRow(*rows[i], r, id);
    }
    if (devCard) {
        // Where a computer reaches the app, and the token it needs.
        Rect card{list.x + 10, list.y + 6 + top + rows.size() * rowH + 10 - settingsScroll_.offset, list.w - 20, 196};
        ui.fillRect(card, withAlpha(ui.t.surface, 0.8f));
        ui.strokeRect(card.inset(0.5f), withAlpha(ui.t.accent, 0.35f), 1, ui.t.radius);
        float x = card.x + 24, y = card.y + 34;
        ui.icon(Icon::Chip, x + 10, y, 24, ui.t.accent);
        ui.text(x + 34, y, "Developer access", 20, ui.t.text, LEFT, true);
        std::string addr = app.web.running() ? app.webAddress() : "";
        double ago = app.dev.lastRequest();
        std::string seen = ago < 0 ? tr("No requests yet")
                                   : trf("Last request {0} s ago", {std::to_string((int)ago)});
        ui.text(card.right() - 24, y, seen, 15, ago >= 0 && ago < 10 ? ui.t.success : ui.t.textFaint, RIGHT);
        ui.keepDigits = true;  // an address and a token are typed on a computer as they are
        ui.text(x, y + 44, tr("Address"), 16, ui.t.textFaint);
        ui.text(x + 120, y + 44, addr.empty() ? tr("Waiting for Wi-Fi…") : addr + "/dev/", 18, ui.t.text, LEFT, true);
        ui.text(x, y + 80, tr("Token"), 16, ui.t.textFaint);
        ui.text(x + 120, y + 80, app.cfg.devToken, 22, ui.t.accent, LEFT, true);
        ui.keepDigits = false;
        ui.text(x, y + 118, ui.ellipsize(tr("Anyone with the token can see and control SwitchMX. Turn this off when you're done."),
                                        card.w - 260, 14), 14, ui.t.warning);
        uint32_t nt = uid("settings.devtoken");
        if (ui.focused(nt) && ui.focusMoved) settingsScroll_.ensureVisible(top + rows.size() * rowH + 10, 196);
        if (ui.button(nt, {card.right() - 200, card.bottom() - 66, 176, 48}, "New token", Icon::Refresh, UI::BTN_TONAL,
                      G_LIST)) {
            app.dev.newToken();
            app.ui.toast("The old token no longer works", Icon::Check);
        }
    }
    if (advanced) {
        float y = list.y + 6 + top + rows.size() * rowH + 14 - settingsScroll_.offset;
        float bw = (list.w - 44) / 3;
        uint32_t h1 = uid("settings.clearhist"), h2 = uid("settings.reset"), h3 = uid("settings.factory");
        if (ui.button(h3, {list.x + 34 + 2 * bw, y, bw, 56}, "Erase all data", Icon::Trash, UI::BTN_DANGER, G_LIST))
            askFactoryReset();
        if (ui.focused(h1) || ui.focused(h2) || ui.focused(h3)) {
            if (ui.focusMoved) settingsScroll_.ensureVisible(top + rows.size() * rowH + 14, 70);
        }
        if (ui.button(h1, {list.x + 10, y, bw, 56}, "Clear watch history", Icon::Clock, UI::BTN_TONAL, G_LIST)) {
            app.confirm("Clear watch history?", "Resume positions will be forgotten.", "Clear", [this] {
                app.lib.clearHistory();
                app.lib.save();
                app.ui.toast("History cleared", Icon::Check);
            }, true);
        }
        if (ui.button(h2, {list.x + 22 + bw, y, bw, 56}, "Reset all settings", Icon::Refresh, UI::BTN_TONAL,
                      G_LIST)) {
            app.confirm("Reset settings?", "Every option goes back to its default value.", "Reset", [this] {
                std::string last = app.cfg.lastDir;
                app.cfg.reset();
                app.cfg.lastDir = last;
                app.cfg.save();
                app.settingsChanged();
                app.ui.toast("Settings reset", Icon::Check);
            }, true);
        }
    }
    settingsScroll_.end(ui);
    settingsScroll_.drawScrollbar(ui);
}
