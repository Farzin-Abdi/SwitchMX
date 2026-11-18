// The Playlists section: the lists people make themselves. The automatic Liked list lives in
// Favorites, next to pinned files; it opens with the same list view.
#include <sys/stat.h>

#include <algorithm>
#include <random>

#include "app.hpp"
#include "platform.hpp"

namespace {
constexpr float ROW_H = 78;
constexpr uint32_t G_HEADER = 2, G_LIST = 3;  // same groups as home.cpp

std::string countLabel(size_t n) { return std::to_string(n) + (n == 1 ? " file" : " files"); }

bool fileExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
}  // namespace

const std::vector<std::string>* HomeScreen::listItems(int list) const {
    if (list == kLikedList) return &app.lib.liked;
    if (list >= 0 && list < (int)app.lib.playlists.size()) return &app.lib.playlists[list].items;
    return nullptr;
}

void HomeScreen::openPlaylist(int list) {
    int previous = playlistOpen_;
    playlistOpen_ = list;
    scroll_.reset();
    listTime_ = 0;
    listRevision_ = app.lib.revision - 1;  // rebuilds the rows on the next frame
    // Coming back, focus returns to the list that was open.
    if (list == kNoList) {
        app.ui.setFocus(uid("pl.row", previous + 1));
    } else if (const auto* items = listItems(list)) {
        if (!items->empty()) app.ui.setFocus(uid(items->front().c_str(), 78));
    }
}

// Files on a drive that isn't plugged in are skipped, but stay in the list.
void HomeScreen::playList(int list, int start, bool shuffle) {
    const auto* items = listItems(list);
    if (!items) return;
    std::vector<std::string> files;
    int index = 0;
    for (size_t i = 0; i < items->size(); i++) {
        if (!fileExists((*items)[i])) continue;
        if ((int)i == start) index = (int)files.size();
        files.push_back((*items)[i]);
    }
    if (files.empty()) {
        app.ui.toast("None of these files are available", Icon::Info);
        return;
    }
    if (shuffle) {
        std::mt19937 rng((uint32_t)(platform::nowSeconds() * 1000));
        std::shuffle(files.begin(), files.end(), rng);
        index = 0;
    }
    app.playFiles(files, index);
}

void HomeScreen::newPlaylist() {
    std::string name;
    if (!app.askText("Playlist name", "", name) || name.empty()) return;
    app.lib.createPlaylist(name);
    app.ui.toast(trf("Created {0}. Press X on any file to add it", {name}), Icon::Playlist);
}

void HomeScreen::playlistMenu(int list) {
    const auto* items = listItems(list);
    if (!items) return;
    std::string title = list == kLikedList ? "Liked" : app.lib.playlists[list].name;
    std::vector<MenuItem> menu = {{"Play", Icon::Play}, {"Shuffle", Icon::Shuffle}};
    std::vector<std::function<void()>> actions = {[this, list] { playList(list, 0, false); },
                                                  [this, list] { playList(list, 0, true); }};
    if (list != kLikedList) {
        menu.push_back({"Rename", Icon::Rename});
        actions.push_back([this, list, title] {
            std::string name;
            if (app.askText("Playlist name", title, name) && !name.empty()) app.lib.renamePlaylist(list, name);
        });
        menu.push_back({"Remove missing files", Icon::Refresh});
        actions.push_back([this, list] {
            size_t removed = 0;
            auto& files = app.lib.playlists[list].items;
            for (size_t i = files.size(); i-- > 0;) {
                if (!fileExists(files[i])) {
                    app.lib.removeFromPlaylist(list, i);
                    removed++;
                }
            }
            app.ui.toast(removed ? "Removed " + countLabel(removed) : "Nothing is missing", Icon::Check);
        });
        menu.push_back({"Delete playlist", Icon::Trash, "", true});
        actions.push_back([this, list, title] {
            app.confirm(trf("Delete \"{0}\"?", {title}), "The files themselves stay where they are.", "Delete",
                        [this, list] {
                            if (playlistOpen_ == list) openPlaylist(kNoList);
                            app.lib.deletePlaylist(list);
                        },
                        true);
        });
    }
    app.openMenu(title, menu, [actions](int i) {
        if (i >= 0 && i < (int)actions.size()) actions[i]();
    }, countLabel(items->size()));
}

void HomeScreen::playlistItemMenu(size_t index) {
    if (index >= listEntries_.size()) return;
    int list = playlistOpen_;
    FileEntry entry = listEntries_[index];
    std::vector<MenuItem> menu = {{"Play from here", Icon::Play}};
    std::vector<std::function<void()>> actions = {[this, list, index] { playList(list, (int)index, false); }};
    bool liked = app.lib.isLiked(entry.path);
    menu.push_back({liked ? "Unlike" : "Like", liked ? Icon::Heart : Icon::HeartFilled});
    actions.push_back([this, entry] { app.toggleLike(entry.path); });
    menu.push_back({"Add to another playlist", Icon::Playlist});
    actions.push_back([this, entry] { app.addToPlaylistMenu({entry.path}); });
    if (list != kLikedList) {
        auto keepFocus = [this, entry] { app.ui.setFocus(uid(entry.path.c_str(), 78)); };
        if (index > 0) {
            menu.push_back({"Move up", Icon::ChevronUp});
            actions.push_back([this, list, index, keepFocus] {
                app.lib.movePlaylistItem(list, index, -1);
                keepFocus();
            });
        }
        if (index + 1 < listEntries_.size()) {
            menu.push_back({"Move down", Icon::ChevronDown});
            actions.push_back([this, list, index, keepFocus] {
                app.lib.movePlaylistItem(list, index, 1);
                keepFocus();
            });
        }
        menu.push_back({"Remove from playlist", Icon::Close});
        actions.push_back([this, list, index] { app.lib.removeFromPlaylist(list, index); });
    }
    menu.push_back({"Open containing folder", Icon::Folder});
    actions.push_back([this, entry] {
        std::string dir = parentDir(entry.path);
        setSection(FILES);
        dirFocus_[dir] = entry.path;
        openDir(dir, false);
        app.ui.setFocus(uid(entry.path.c_str(), 77));
    });
    app.openMenu(stripExtension(entry.name), menu, [actions](int i) {
        if (i >= 0 && i < (int)actions.size()) actions[i]();
    });
}

void HomeScreen::drawPlaylists(Rect area) {
    if (playlistOpen_ != kNoList && !listItems(playlistOpen_)) playlistOpen_ = kNoList;
    if (playlistOpen_ == kNoList) drawPlaylistIndex(area);
    else drawPlaylistItems(area);
}

void HomeScreen::drawPlaylistIndex(Rect area) {
    UI& ui = app.ui;
    auto& lists = app.lib.playlists;
    ui.text(area.x, area.y + 28, "Playlists", 30, ui.t.text, LEFT, true);
    ui.text(area.x + 1, area.y + 60, "Your music and videos, in the order you choose", 15, ui.t.textFaint);
    if (ui.button(uid("pl.new"), {area.right() - 196, area.y + 8, 196, 48}, "New playlist", Icon::Plus, UI::BTN_TONAL,
                  G_HEADER)) {
        newPlaylist();
    }
    Rect list{area.x - 12, area.y + 84, area.w + 24, area.h - 84};
    if (lists.empty()) {
        drawEmpty(list, Icon::Playlist, "No playlists yet",
                  "Tap New playlist, or press X on a song or video and choose Add to playlist");
        if (!ui.blocked && ui.in->down(BTN_Y)) newPlaylist();
        return;
    }
    size_t count = lists.size();
    float rowH = 84;
    scroll_.update(ui, list, count * rowH + 12);
    scroll_.begin(ui);
    int open = kNoList, focused = kNoList;
    bool menuPressed = false;
    for (size_t i = 0; i < count; i++) {
        int which = (int)i;
        const auto* items = listItems(which);
        Rect r{list.x + 6, list.y + 6 + i * rowH - scroll_.offset, list.w - 12, rowH};
        uint32_t id = uid("pl.row", (int)i);
        ui.focusable(id, r, G_LIST);
        bool foc = ui.focused(id);
        if (foc) focused = which;
        if (foc && ui.focusMoved) scroll_.ensureVisible(6 + i * rowH, rowH);
        if (r.bottom() < list.y || r.y > list.bottom()) continue;

        ui.pushAlpha(scroll_.edgeFade(r.y, r.h));
        float fv = ui.smooth(id ^ 0xf0c, foc && ui.showFocus() ? 1.0f : 0.0f, 16);
        float pv = ui.smooth(id ^ 0x9e3, ui.pressedVisual(r) ? 1.0f : 0.0f, 25);
        Rect card = r.inset(0, 4);
        if (fv > 0.01f || pv > 0.01f) ui.fillRect(card, withAlpha(ui.t.surface2, std::max(fv, pv)));
        ui.focusRing(card, ui.t.radius, fv);

        Rect tile{card.x + 12, card.cy() - 27, 54, 54};
        float rad = std::min(ui.t.radius, 16.0f);
        const CoverArt::Art* art = nullptr;
        if (which != kLikedList && !items->empty())
            art = app.covers.get(items->front(), kindForExtension(fileExtension(items->front())));
        if (art) {
            drawCover(tile, *art, rad);
        } else {
            nvgBeginPath(ui.vg);
            nvgRoundedRect(ui.vg, tile.x, tile.y, tile.w, tile.h, rad);
            NVGcolor from = which == kLikedList ? ui.t.danger : ui.t.accent;
            NVGcolor to = which == kLikedList ? mix(ui.t.danger, ui.t.accent2, 0.5f) : ui.t.accent2;
            nvgFillPaint(ui.vg, nvgLinearGradient(ui.vg, tile.x, tile.y, tile.right(), tile.bottom(), from, to));
            nvgFill(ui.vg);
            ui.icon(which == kLikedList ? Icon::HeartFilled : Icon::Playlist, tile.cx(), tile.cy(), 26, hex(0xFFFFFF));
        }
        float x = tile.right() + 18, right = card.right() - 48;
        std::string name = which == kLikedList ? "Liked" : lists[which].name;
        if (foc && ui.showFocus()) ui.marquee(id ^ 0x3a9, x, card.cy() - 11, right - x, name, 20, ui.t.text, LEFT, true);
        else ui.text(x, card.cy() - 11, ui.ellipsize(name, right - x, 20, foc), 20, ui.t.text, LEFT, foc);
        ui.text(x, card.cy() + 14, countLabel(items->size()), 15, ui.t.textFaint);
        ui.icon(Icon::ChevronRight, card.right() - 28, card.cy(), 22, ui.t.textFaint);
        ui.popAlpha();

        if (ui.longPressIn(card)) {
            ui.setFocus(id);
            focused = which;
            menuPressed = true;
        } else if (ui.tapped(card) || (foc && ui.confirm())) {
            open = which;
        }
    }
    scroll_.end(ui);
    scroll_.drawScrollbar(ui);
    if (ui.blocked) return;
    if (open != kNoList) {
        openPlaylist(open);
        return;
    }
    if (focused != kNoList && (menuPressed || ui.in->down(BTN_X))) playlistMenu(focused);
    else if (ui.in->down(BTN_Y)) newPlaylist();
}

void HomeScreen::drawPlaylistItems(Rect area) {
    UI& ui = app.ui;
    int which = playlistOpen_;
    const auto& items = *listItems(which);
    if (listRevision_ != app.lib.revision) {
        listRevision_ = app.lib.revision;
        listEntries_.clear();
        listEntries_.reserve(items.size());
        for (auto& p : items) {
            FileEntry e;
            e.path = p;
            e.name = displayName(p);
            e.ext = fileExtension(p);
            e.kind = kindForExtension(e.ext);
            struct stat st;
            if (stat(p.c_str(), &st) == 0) {
                e.size = (uint64_t)st.st_size;
                e.mtime = st.st_mtime;
            } else {
                e.detail = "Not available: deleted, moved outside the app, or on a removed drive";
            }
            listEntries_.push_back(std::move(e));
        }
    }

    if (ui.iconButton(uid("pl.back"), area.x + 22, area.y + 28, 48, Icon::Back, ui.t.text, G_HEADER, true)) {
        openPlaylist(kNoList);
        return;
    }
    float tx = area.x + 56, btnW = 124, btnX = area.right() - btnW * 2 - 12;
    std::string title = which == kLikedList ? "Liked" : app.lib.playlists[which].name;
    ui.text(tx, area.y + 28, ui.ellipsize(title, btnX - tx - 12, 30, true), 30, ui.t.text, LEFT, true);
    ui.text(tx + 1, area.y + 60, countLabel(items.size()), 15, ui.t.textFaint);
    bool any = !items.empty();
    if (any && ui.button(uid("pl.play"), {btnX, area.y + 8, btnW, 48}, "Play", Icon::Play, UI::BTN_FILLED, G_HEADER)) {
        playList(which, 0, false);
        return;
    }
    if (any && ui.button(uid("pl.shuffle"), {btnX + btnW + 12, area.y + 8, btnW, 48}, "Shuffle", Icon::Shuffle,
                         UI::BTN_TONAL, G_HEADER)) {
        playList(which, 0, true);
        return;
    }

    Rect list{area.x - 12, area.y + 84, area.w + 24, area.h - 84};
    if (!any) {
        if (which == kLikedList)
            drawEmpty(list, Icon::Heart, "Nothing liked yet", "Tap the heart in the player, or press X on a file");
        else
            drawEmpty(list, Icon::Playlist, "This playlist is empty", "Press X on a file or folder and choose Add to playlist");
        return;
    }
    scroll_.update(ui, list, listEntries_.size() * ROW_H + 12);
    scroll_.begin(ui);
    contextIndex_ = -1;
    int open = -1;
    for (size_t i = 0; i < listEntries_.size(); i++) {
        Rect r{list.x + 6, list.y + 6 + i * ROW_H - scroll_.offset, list.w - 12, ROW_H};
        uint32_t id = uid(listEntries_[i].path.c_str(), 78);
        if (ui.focused(id) && ui.focusMoved) scroll_.ensureVisible(6 + i * ROW_H, ROW_H);
        if (fileRow(id, r, listEntries_[i], (int)i)) open = (int)i;
    }
    scroll_.end(ui);
    scroll_.drawScrollbar(ui);
    if (ui.blocked) return;
    if (open >= 0) {
        playList(which, open, false);
        return;
    }
    if (ui.in->down(BTN_X) && contextIndex_ >= 0) playlistItemMenu((size_t)contextIndex_);
    else if (ui.in->down(BTN_Y)) playList(which, 0, true);
}
