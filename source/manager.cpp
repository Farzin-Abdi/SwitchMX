// File manager: the actions, background job display and text viewer of HomeScreen's Manager section.
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cerrno>

#include "app.hpp"
#include "platform.hpp"
#include "subtext.hpp"

namespace {
std::string trimmed(std::string p) {
    while (p.size() > 1 && p.back() == '/' && p[p.size() - 2] != ':') p.pop_back();
    return p;
}

bool samePath(const std::string& a, const std::string& b) { return trimmed(a) == trimmed(b); }

// FAT and exFAT, used by SD cards and most USB drives, refuse these; say so before trying.
// Any name works (what the drive can't hold is stored encoded, see names.hpp), except a slash.
std::string nameProblem(const std::string& name) {
    if (name.find('/') != std::string::npos) return "A name can't contain /";
    return "";
}

bool anyProtected(const std::vector<std::string>& paths) {
    for (auto& p : paths)
        if (isProtectedPath(trimmed(p))) return true;
    return false;
}

std::string countLabel(size_t n) { return std::to_string(n) + (n == 1 ? " item" : " items"); }

bool anyExists(const std::vector<std::string>& paths, const std::string& dest) {
    std::string d = dest;
    if (d.empty() || d.back() != '/') d += '/';
    struct stat st;
    for (auto& p : paths) {
        std::string target = d + baseName(trimmed(p));
        if (samePath(target, p)) continue;  // copying onto itself always keeps both
        if (stat(target.c_str(), &st) == 0) return true;
    }
    return false;
}
}  // namespace

bool HomeScreen::isMarked(const std::string& path) const {
    return std::find(marked_.begin(), marked_.end(), path) != marked_.end();
}

void HomeScreen::toggleMark(const std::string& path) {
    auto it = std::find(marked_.begin(), marked_.end(), path);
    if (it != marked_.end()) marked_.erase(it);
    else marked_.push_back(path);
}

std::vector<std::string> HomeScreen::actionTargets(const FileEntry& e) const {
    if (!marked_.empty()) return marked_;
    return {e.path};
}

bool HomeScreen::managerBack() {
    if (selectMode()) {
        endSelect();
        return true;
    }
    if (searching_) {
        searching_ = false;
        refresh();
        return true;
    }
    return false;
}

void HomeScreen::managerButtons(const FileEntry* focused) {
    Input& in = *app.ui.in;
    if (in.down(BTN_Y) && focused && !focused->isDrive) {
        selecting_ = true;
        toggleMark(focused->path);
    }
    if (in.down(BTN_R) && (!clipboard_.empty() || !extractPending_.empty())) paste();
    // An empty folder has no row to open the Actions menu from; by touch, Actions means the folder.
    if (in.down(BTN_X) && marked_.empty() && !focused && !dir_.empty()) {
        folderMenu();
        in.pressed &= ~BTN_X;  // drawFiles must not open the item menu as well
    }
}

// Leaving the app mid-copy stops the copy (removing the half-written file) instead of waiting for it.
void HomeScreen::cancelJob() {
    if (!job_) return;
    job_->progress.cancel = true;
    job_->submitPassword("");  // wakes an archive job waiting for a password
}

void HomeScreen::selectAll() {
    selecting_ = true;
    marked_.clear();
    for (auto& f : entries_)
        if (!f.isDrive) marked_.push_back(f.path);
}

void HomeScreen::setClipboard(const std::vector<std::string>& paths, bool move) {
    clipboard_ = paths;
    clipboardMove_ = move;
    extractPending_.clear();
    endSelect();
    std::string what = countLabel(paths.size());
    app.ui.toast(what + (move ? " cut" : " copied") + ". Open a folder, then tap Paste (or press R)",
                 move ? Icon::Cut : Icon::Copy);
}

// What can be done in the open folder itself.
void HomeScreen::folderMenu() {
    if (section_ == NETWORK && dir_.empty()) {
        askServer(-1);
        return;
    }
    if (netDir()) {  // a server's folder: nothing to create or select there
        app.openMenu(displayName(dir_), {{"Refresh", Icon::Refresh}, {"Sort & filter", Icon::Sort}}, [this](int i) {
            if (i == 0) startNetListing(true);
            if (i == 1) sortMenu();
        });
        return;
    }
    if (dir_.empty()) return;
    std::vector<MenuItem> items;
    std::vector<std::function<void()>> actions;
    auto add = [&](const std::string& label, Icon ic, std::function<void()> fn) {
        items.push_back({label, ic});
        actions.push_back(std::move(fn));
    };
    bool canPaste = !clipboard_.empty() || !extractPending_.empty();
    if (canPaste)
        add(!extractPending_.empty() ? "Extract here" : (clipboardMove_ ? "Move here" : "Paste here"), Icon::Paste,
            [this] { paste(); });
    if (!entries_.empty()) {
        add("Select files", Icon::Check, [this] {
            selecting_ = true;
            app.ui.toast("Tap files to select them", Icon::Check);
        });
        add("Select all", Icon::Layers, [this] { selectAll(); });
    }
    add("New folder", Icon::FolderPlus, [this] { askNewFolder(); });
    add("Sort & filter", Icon::Sort, [this] { sortMenu(); });
    if (canPaste) add("Clear clipboard", Icon::Close, [this] {
        clipboard_.clear();
        extractPending_.clear();
    });
    std::string name = dir_;
    while (!name.empty() && name.back() == '/') name.pop_back();
    std::string title = fromDiskName(baseName(name));
    if (title.empty() || title.back() == ':') title = "This folder";
    app.openMenu(title, items, [actions](int i) {
        if (i >= 0 && i < (int)actions.size()) actions[i]();
    });
}

// Replaces the hint bar while files are selected: big buttons for what applies to all of them.
void HomeScreen::drawSelectBar() {
    UI& ui = app.ui;
    float y = ui.H - 32;
    float x = 250 + 32;  // after the sidebar, as the hint bar
    std::string count = marked_.empty() ? "Tap files to select" : countLabel(marked_.size()) + " selected";
    ui.text(x, y, count, 17, ui.t.text, LEFT, true);
    x += ui.textWidth(count, 17, true) + 18;
    std::vector<std::string> targets = marked_;
    bool any = !targets.empty();
    struct B { const char* label; Icon ic; bool on; std::function<void()> fn; };
    std::vector<B> bs = {
        {"All", Icon::Layers, true, [this] { selectAll(); }},
        {"Copy", Icon::Copy, any, [this, targets] { setClipboard(targets, false); }},
        {"Cut", Icon::Cut, any, [this, targets] { setClipboard(targets, true); }},
        {"Delete", Icon::Trash, any, [this, targets] { askDelete(targets); }},
        {"More", Icon::Sliders, any, [this] {
             for (auto& e : entries_)
                 if (isMarked(e.path)) return managerMenu(e);
             FileEntry e;  // selection is all in other folders
             e.path = marked_.front();
             e.name = displayName(e.path);
             managerMenu(e);
         }},
        {"Done", Icon::Close, true, [this] { endSelect(); }},
    };
    float right = ui.W - 32;
    float h = 40;
    // Widths, shrunk to icons only if the labels don't fit.
    float total = 0;
    for (auto& b : bs) total += 44 + ui.textWidth(b.label, 16) + 8;
    bool labels = x + total <= right;
    for (auto& b : bs) {
        float w = labels ? 44 + ui.textWidth(b.label, 16) : 48;
        Rect r{x, y - h / 2, w, h};
        float alpha = b.on ? 1.0f : 0.35f;
        bool down = b.on && ui.pressedVisual(r);
        ui.fillRect(r, withAlpha(b.label == bs.back().label ? ui.t.accent : ui.t.surface2, (down ? 1.0f : 0.8f) * alpha),
                    h / 2);
        NVGcolor fg = withAlpha(b.label == bs.back().label ? ui.t.onAccent : ui.t.text, alpha);
        ui.icon(b.ic, labels ? r.x + 22 : r.cx(), r.cy(), 18, fg);
        if (labels) ui.text(r.x + 38, r.cy(), b.label, 16, fg);
        if (b.on && ui.tapped(r)) b.fn();
        x += w + 8;
    }
}

void HomeScreen::managerOpenFile(const FileEntry& e) {
    if (e.kind == MediaKind::Archive || isArchiveName(e.name)) {
        managerMenu(e);
        return;
    }
    if (e.kind == MediaKind::Text || e.kind == MediaKind::Subtitle) {
        openTextViewer(e.path);
        return;
    }
    showProperties({e.path});
}

void HomeScreen::managerMenu(const FileEntry& e) {
    std::vector<MenuItem> items;
    std::vector<std::function<void()>> actions;
    auto add = [&](const std::string& label, Icon ic, std::function<void()> fn, bool danger = false,
                   const std::string& detail = "") {
        MenuItem m{label, ic, detail};
        m.danger = danger;
        items.push_back(m);
        actions.push_back(std::move(fn));
    };
    std::vector<std::string> targets = actionTargets(e);
    bool single = targets.size() == 1;
    FileEntry entry = e;
    std::string name = e.name;
    while (!name.empty() && name.back() == '/') name.pop_back();
    bool isFolder = e.kind == MediaKind::Folder;
    std::string title = single ? name : countLabel(targets.size()) + " selected";

    if (single) {
        bool archive = !isFolder && (e.kind == MediaKind::Archive || isArchiveName(e.name));
        bool playable = e.kind == MediaKind::Video || e.kind == MediaKind::Audio || e.kind == MediaKind::Playlist ||
                        e.kind == MediaKind::Image;
        if (isFolder) add("Open", Icon::Folder, [this, entry] { openDir(entry.path); });
        if (playable) add("Open", Icon::Play, [this, entry] { app.playPath(entry.path); });
        if (e.kind == MediaKind::Document) add("Read", Icon::Book, [this, entry] { app.playPath(entry.path); });
        if (e.kind == MediaKind::Text || e.kind == MediaKind::Subtitle)
            add("View as text", Icon::File, [this, entry] { openTextViewer(entry.path); });
        if (archive) {
            std::string stem = stripExtension(name);
            std::string folder = parentDir(e.path);
            add(trf("Extract to \"{0}\"", {stem}), Icon::FolderPlus, [this, entry, folder, stem] {
                std::string dest = uniquePath(folder + toDiskName(stem));  // a name the drive can store
                askExtract(entry.path, dest);
            });
            add("Extract here", Icon::Archive, [this, entry, folder] { askExtract(entry.path, folder); });
            add("Extract to...", Icon::Paste, [this, entry] {
                extractPending_ = entry.path;
                clipboard_.clear();
                app.ui.toast("Open a folder and press R to extract there", Icon::Archive);
            });
            add("View contents", Icon::List, [this, entry, name] {
                startJob(FileJob::listContents(entry.path), [this, name](FileJob& job) {
                    if (!job.ok) {
                        if (!job.cancelled) app.ui.toast(job.message, Icon::Info);
                        return;
                    }
                    std::vector<MenuItem> rows;
                    size_t shown = std::min<size_t>(job.entries.size(), 300);
                    for (size_t i = 0; i < shown; i++) {
                        const ArcEntry& a = job.entries[i];
                        rows.push_back({a.name, a.isDir ? Icon::Folder : Icon::File, a.isDir ? "" : formatSize(a.size)});
                    }
                    std::string msg = tr(std::to_string(job.entries.size()) + " entries");
                    if (shown < job.entries.size()) msg += " " + tr("(first " + std::to_string(shown) + " shown)");
                    app.openMenu(name, rows, [](int) {}, msg);
                });
            });
        }
    }
    add(isMarked(e.path) ? "Deselect" : "Select", Icon::Check, [this, entry] {
        selecting_ = true;
        toggleMark(entry.path);
    });
    add("Select all", Icon::Layers, [this] { selectAll(); });
    if (selectMode()) add("Clear selection", Icon::Close, [this] { endSelect(); });
    add("Add to playlist", Icon::Playlist, [this, targets] {
        endSelect();
        app.addToPlaylistMenu(targets);
    });
    add("Copy", Icon::Copy, [this, targets] { setClipboard(targets, false); });
    add("Cut", Icon::Cut, [this, targets] { setClipboard(targets, true); });
    if (!clipboard_.empty() || !extractPending_.empty())
        add(extractPending_.empty() ? "Paste here" : "Extract here", Icon::Paste, [this] { paste(); });
    if (single) add("Rename", Icon::Rename, [this, entry] { askRename(entry); });
    add("Compress...", Icon::Archive, [this, targets] { askCompress(targets); });
    add("New folder", Icon::FolderPlus, [this] { askNewFolder(); });
    add("Properties", Icon::Info, [this, targets] { showProperties(targets); });
    add("Delete", Icon::Trash, [this, targets] { askDelete(targets); }, true);

    app.openMenu(title, items, [actions](int i) {
        if (i >= 0 && i < (int)actions.size()) actions[i]();
    });
}

void HomeScreen::startJob(FileJob* job, std::function<void(FileJob&)> onDone) {
    job_.reset(job);
    jobDone_ = std::move(onDone);
    platform::setMediaPlayback(true);  // don't let the console sleep in the middle of a copy
}

void HomeScreen::startCopy(const std::vector<std::string>& paths, const std::string& dest, bool move) {
    auto run = [this, paths, dest, move](ConflictPolicy policy) {
        startJob(FileJob::copy(paths, dest, move, policy), [this, move, paths, dest, policy](FileJob& job) {
            if (job.ok) {
                if (move) clipboard_.clear();
                // Playlists, likes and resume points follow the files. With "keep both" the
                // target may be a different file of the same name, so those are left alone.
                if (move && policy != ConflictPolicy::KeepBoth) {
                    std::string d = dest.empty() || dest.back() == '/' ? dest : dest + "/";
                    struct stat st;
                    for (auto& p : paths) {
                        std::string src = trimmed(p), target = d + baseName(src);
                        if (stat(src.c_str(), &st) != 0 && stat(target.c_str(), &st) == 0)
                            app.lib.pathMoved(src, target);
                    }
                    app.lib.save();
                }
                std::string msg = countLabel(paths.size()) + (move ? " moved" : " copied");
                if (!job.message.empty()) msg += " (" + job.message + ")";
                app.ui.toast(msg, move ? Icon::Cut : Icon::Copy);
            } else if (!job.cancelled) {
                app.openMenu("Couldn't finish", {{"Close", Icon::Close}}, [](int) {}, job.message);
            }
        });
    };
    if (!anyExists(paths, dest)) {
        run(ConflictPolicy::Replace);
        return;
    }
    app.openMenu("Some items already exist",
                 {{"Replace", Icon::Refresh}, {"Keep both", Icon::Copy}, {"Skip existing", Icon::ChevronRight},
                  {"Cancel", Icon::Close}},
                 [run](int i) {
                     if (i == 0) run(ConflictPolicy::Replace);
                     else if (i == 1) run(ConflictPolicy::KeepBoth);
                     else if (i == 2) run(ConflictPolicy::Skip);
                 });
}

void HomeScreen::paste() {
    if (dir_.empty() || searching_) {
        app.ui.toast("Open a folder first", Icon::Info);
        return;
    }
    if (!extractPending_.empty()) {
        std::string archive = extractPending_;
        extractPending_.clear();
        askExtract(archive, dir_);
        return;
    }
    if (clipboard_.empty()) return;
    if (clipboardMove_ && anyProtected(clipboard_)) {
        auto paths = clipboard_;
        std::string dest = dir_;
        app.confirm("Move system files?", "These items are needed to boot your console or homebrew.", "Move anyway",
                    [this, paths, dest] { startCopy(paths, dest, true); }, true);
        return;
    }
    startCopy(clipboard_, dir_, clipboardMove_);
}

void HomeScreen::askDelete(const std::vector<std::string>& paths) {
    std::string what = paths.size() == 1 ? "\"" + displayName(trimmed(paths[0])) + "\"" : tr(countLabel(paths.size()));
    std::string msg = trf("{0} will be permanently deleted. This can't be undone.", {what});
    if (anyProtected(paths))
        msg = tr("WARNING: this includes system files. Your console or homebrew may stop booting.") + "\n\n" + msg;
    auto run = [this, paths] {
        startJob(FileJob::remove(paths), [this, paths](FileJob& job) {
            if (job.ok) {
                for (auto& p : paths) app.lib.forgetFile(p);
                app.lib.save();
                endSelect();
                app.ui.toast(countLabel(paths.size()) + " deleted", Icon::Trash);
            } else if (!job.cancelled) {
                app.openMenu("Couldn't delete", {{"Close", Icon::Close}}, [](int) {}, job.message);
            }
        });
    };
    app.confirm("Delete?", msg, "Delete", run, true);
}

void HomeScreen::askRename(const FileEntry& e) {
    std::string oldPath = trimmed(e.path);
    std::string oldName = displayName(oldPath);
    auto apply = [this, oldPath, oldName] {
        std::string name;
        if (!app.askText("New name", oldName, name) || name.empty() || name == oldName) return;
        std::string problem = nameProblem(name);
        if (!problem.empty()) {
            app.ui.toast(problem, Icon::Info);
            return;
        }
        std::string target = parentDir(oldPath) + toDiskName(name);
        // SD cards and most USB drives ignore letter case: "movie" and "Movie" are the same name
        // there, so changing only the case goes through a temporary name.
        bool caseOnly = name.size() == oldName.size() &&
                        std::equal(name.begin(), name.end(), oldName.begin(), [](char a, char b) {
                            return tolower((unsigned char)a) == tolower((unsigned char)b);
                        });
        struct stat st;
        if (!caseOnly && stat(target.c_str(), &st) == 0) {
            app.ui.toast("That name is already used here", Icon::Info);
            return;
        }
        int rc;
        if (caseOnly) {
            std::string temp = oldPath + ".renaming";
            rc = rename(oldPath.c_str(), temp.c_str());
            if (rc == 0 && (rc = rename(temp.c_str(), target.c_str())) != 0) rename(temp.c_str(), oldPath.c_str());
        } else {
            rc = rename(oldPath.c_str(), target.c_str());
        }
        if (rc != 0) {
            int e = errno;
            app.logEvent("rename failed: " + oldPath + " -> " + name + ": " + strerror(e));
            app.ui.toast(trf("Couldn't rename: {0}", {strerror(e)}), Icon::Info);
            return;
        }
        app.lib.pathMoved(oldPath, target);
        app.lib.save();
        refresh();
        app.ui.toast("Renamed", Icon::Rename);
    };
    if (isProtectedPath(oldPath))
        app.confirm("Rename a system item?", "Your console or homebrew may stop booting if this is renamed.",
                    "Rename anyway", apply, true);
    else
        apply();
}

void HomeScreen::askNewFolder() {
    if (dir_.empty() || searching_) {
        app.ui.toast("Open a folder first", Icon::Info);
        return;
    }
    std::string name;
    if (!app.askText("Folder name", "New folder", name) || name.empty()) return;
    std::string problem = nameProblem(name);
    if (!problem.empty()) {
        app.ui.toast(problem, Icon::Info);
        return;
    }
    if (mkdir((dir_ + toDiskName(name)).c_str(), 0777) != 0) {
        app.ui.toast("Couldn't create the folder", Icon::Info);
        return;
    }
    refresh();
    app.ui.setFocus(uid((dir_ + toDiskName(name) + "/").c_str(), 77));
}

void HomeScreen::askSearch() {
    if (dir_.empty()) return;
    std::string term;
    if (!app.askText("Search in this folder and below", searchTerm_, term) || term.empty()) return;
    searchTerm_ = term;
    startJob(FileJob::search(dir_, term), [this](FileJob& job) {
        if (job.cancelled) return;
        entries_.clear();
        for (auto& p : job.foundPaths) {
            FileEntry fe;
            fe.path = p;
            bool folder = p.back() == '/';
            fe.name = displayName(trimmed(p));
            fe.kind = folder ? MediaKind::Folder : kindForExtension(fileExtension(fe.name));
            if (!folder) {
                fe.ext = fileExtension(fe.name);
                if (fe.kind == MediaKind::Other && isArchiveName(fe.name)) fe.kind = MediaKind::Archive;
                struct stat st;
                if (stat(p.c_str(), &st) == 0) {
                    fe.size = (uint64_t)st.st_size;
                    fe.mtime = st.st_mtime;
                }
            }
            fe.detail = platform::displayPath(parentDir(trimmed(p)));
            entries_.push_back(std::move(fe));
        }
        searching_ = true;
        marked_.clear();
        scroll_.reset();
        listTime_ = 0;
    });
}

void HomeScreen::askExtract(const std::string& archive, const std::string& dest) {
    auto run = [this, archive, dest](ConflictPolicy policy) {
        startJob(FileJob::extract(archive, dest, policy), [this, dest](FileJob& job) {
            if (job.ok) {
                app.ui.toast(trf("Extracted to {0}", {displayName(trimmed(dest))}), Icon::Archive);
            } else if (!job.cancelled) {
                app.openMenu("Couldn't extract", {{"Close", Icon::Close}}, [](int) {}, job.message);
            }
        });
    };
    struct stat st;
    if (stat(trimmed(dest).c_str(), &st) != 0) {
        run(ConflictPolicy::Replace);  // a new folder can't conflict with anything
        return;
    }
    app.openMenu("If a file already exists",
                 {{"Replace", Icon::Refresh}, {"Keep both", Icon::Copy}, {"Skip it", Icon::ChevronRight},
                  {"Cancel", Icon::Close}},
                 [run](int i) {
                     if (i == 0) run(ConflictPolicy::Replace);
                     else if (i == 1) run(ConflictPolicy::KeepBoth);
                     else if (i == 2) run(ConflictPolicy::Skip);
                 });
}

void HomeScreen::askCompress(const std::vector<std::string>& paths) {
    std::string first = trimmed(paths[0]);
    std::string base = fromDiskName(paths.size() == 1 ? stripExtension(baseName(first)) : baseName(trimmed(parentDir(first))));
    if (base.empty()) base = "Archive";
    std::string folder = searching_ ? parentDir(first) : dir_;
    app.openMenu("Compress " + countLabel(paths.size()),
                 {{"ZIP", Icon::Archive}, {"ZIP with password (AES-256)", Icon::Lock}, {"7z", Icon::Archive},
                  {"TAR.GZ", Icon::Archive}},
                 [this, paths, base, folder](int i) {
                     if (i < 0) return;
                     const char* ext = i <= 1 ? ".zip" : (i == 2 ? ".7z" : ".tar.gz");
                     ArcFormat fmt = i <= 1 ? ArcFormat::Zip : (i == 2 ? ArcFormat::SevenZip : ArcFormat::TarGz);
                     std::string name, password;
                     if (!app.askText("Archive name", base + ext, name) || name.empty()) return;
                     if (name.find('/') != std::string::npos) {
                         app.ui.toast("A name can't contain /", Icon::Info);
                         return;
                     }
                     if (i == 1 && (!app.askText("Password", "", password) || password.empty())) return;
                     std::string out = uniquePath(folder + toDiskName(name));
                     startJob(FileJob::compress(paths, out, fmt, password), [this, out](FileJob& job) {
                         if (job.ok) app.ui.toast(trf("Created {0}", {displayName(out)}), Icon::Archive);
                         else if (!job.cancelled)
                             app.openMenu("Couldn't compress", {{"Close", Icon::Close}}, [](int) {}, job.message);
                     });
                 });
}

void HomeScreen::showProperties(const std::vector<std::string>& paths) {
    startJob(FileJob::measure(paths), [this, paths](FileJob& job) {
        if (!job.ok) return;
        std::string title = paths.size() == 1 ? displayName(trimmed(paths[0])) : countLabel(paths.size());
        std::string msg;
        if (paths.size() == 1) {
            msg += trf("Location: {0}", {platform::displayPath(parentDir(trimmed(paths[0])))}) + "\n";
            struct stat st;
            if (stat(trimmed(paths[0]).c_str(), &st) == 0) msg += trf("Modified: {0}", {formatDate(st.st_mtime)}) + "\n";
        }
        msg += trf("Size: {0}", {formatSize(job.totalBytes)});
        if (job.folderCount || paths.size() > 1)
            msg += "\n" + trf("Contains: {0} files, {1} folders", {std::to_string(job.fileCount), std::to_string(job.folderCount)});
        app.openMenu(title, {{"Close", Icon::Close}}, [](int) {}, msg);
    });
}

void HomeScreen::openTextViewer(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        app.ui.toast("Couldn't open the file", Icon::Info);
        return;
    }
    const size_t limit = 512 * 1024;
    std::string data(limit, '\0');
    data.resize(fread(&data[0], 1, limit, f));
    bool truncated = !feof(f) && fgetc(f) != EOF;
    fclose(f);
    // Same encoding detection as subtitles, so old Persian/Arabic text files read correctly.
    std::string text = subtitleToUtf8(data, 0, app.cfg.s.slang);
    if (text.find('\0') != std::string::npos) {
        app.ui.toast("This doesn't look like a text file", Icon::Info);
        return;
    }
    if (truncated) text += "\n\n(Only the first 512 KB are shown)";

    // Wrap once up front; the row width only depends on the interface size.
    UI& ui = app.ui;
    float width = ui.W - 120;
    ui.setFont(false);
    nvgFontSize(ui.vg, 17);
    textRows_.clear();
    size_t pos = 0;
    while (pos <= text.size() && textRows_.size() < 50000) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            textRows_.push_back("");
        } else {
            const char* s = line.c_str();
            const char* e = s + line.size();
            NVGtextRow rows[16];
            int n;
            while (s < e && (n = nvgTextBreakLines(ui.vg, s, e, width, rows, 16)) > 0) {
                for (int i = 0; i < n; i++) textRows_.emplace_back(rows[i].start, rows[i].end);
                s = rows[n - 1].next;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    textTitle_ = displayName(path);
    textScroll_.reset();
    textScroll_.id = uid("text.scroll");
    textOpen_ = true;
}

void HomeScreen::drawTextViewer() {
    UI& ui = app.ui;
    Input& in = *ui.in;
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, 0, 0, ui.W, ui.H);
    nvgFillColor(ui.vg, ui.t.bg);
    nvgFill(ui.vg);
    ui.text(34, 40, ui.ellipsize(textTitle_, ui.W - 140, 24, true), 24, ui.t.text, LEFT, true);
    bool close = ui.iconButton(uid("text.close"), ui.W - 44, 40, 48, Icon::Close, ui.t.text, 0, false);
    const float rowH = 24;
    Rect view{30, 76, ui.W - 60, ui.H - 92};
    textScroll_.update(ui, view, textRows_.size() * rowH + 16);
    // D-pad reads like a document: up/down scroll a few lines, left/right a page.
    float by = 0;
    if (in.rep(BTN_DOWN)) by = rowH * 3;
    if (in.rep(BTN_UP)) by = -rowH * 3;
    if (in.rep(BTN_RIGHT)) by = view.h - rowH;
    if (in.rep(BTN_LEFT)) by = -(view.h - rowH);
    ui.consumeNav(BTN_DIRS);
    if (by != 0) {
        float base = textScroll_.target >= 0 ? textScroll_.target : textScroll_.offset;
        textScroll_.target = std::clamp(base + by, 0.0f, textScroll_.maxOffset());
        textScroll_.idleTime = 0;
    }
    textScroll_.begin(ui);
    int first = std::max(0, (int)(textScroll_.offset / rowH));
    for (int i = first; i < (int)textRows_.size(); i++) {
        float y = view.y + 8 + i * rowH - textScroll_.offset;
        if (y > view.bottom()) break;
        ui.text(view.x + 4, y + rowH / 2, textRows_[i], 17, ui.t.text);
    }
    textScroll_.end(ui);
    textScroll_.drawScrollbar(ui);
    if (close || ui.back()) {
        textOpen_ = false;
        textRows_.clear();
    }
}

void HomeScreen::drawJob() {
    UI& ui = app.ui;
    FileJob& job = *job_;
    if (job.finished) {
        std::unique_ptr<FileJob> done = std::move(job_);
        auto cb = std::move(jobDone_);
        jobDone_ = nullptr;
        platform::setMediaPlayback(false);
        app.logEvent(done->title + (done->ok ? " finished" : done->cancelled ? " cancelled" : " failed: " + done->message));
        refresh();
        if (cb) cb(*done);
        return;
    }
    if (job.needsPassword) {
        std::string password;
        bool got = app.askText(job.wrongPassword ? "Wrong password, try again" : "This archive needs a password",
                                      "", password);
        job.submitPassword(got ? password : "");
        return;
    }
    nvgBeginPath(ui.vg);
    nvgRect(ui.vg, 0, 0, ui.W, ui.H);
    nvgFillColor(ui.vg, hex(0, 0.55f));
    nvgFill(ui.vg);
    float w = std::min(560.0f, ui.W - 80), h = 196;
    Rect card{ui.W / 2 - w / 2, ui.H / 2 - h / 2, w, h};
    ui.panel(card, 1, ui.t.radius + 8);
    ui.text(card.x + 28, card.y + 38, job.title, 24, ui.t.text, LEFT, true);
    std::string current = job.progress.getCurrent();
    ui.text(card.x + 28, card.y + 72, ui.ellipsize(current, card.w - 56, 16), 16, ui.t.textDim);

    uint64_t total = job.progress.total, done = job.progress.done;
    Rect bar{card.x + 28, card.y + 96, card.w - 56, 8};
    ui.fillRect(bar, withAlpha(ui.t.textFaint, 0.3f), 4);
    if (total > 0) {
        float p = std::min(1.0f, (float)((double)done / (double)total));
        nvgBeginPath(ui.vg);
        nvgRoundedRect(ui.vg, bar.x, bar.y, std::max(8.0f, bar.w * p), bar.h, 4);
        nvgFillPaint(ui.vg, nvgLinearGradient(ui.vg, bar.x, 0, bar.right(), 0, ui.t.accent, ui.t.accent2));
        nvgFill(ui.vg);
        char buf[64];
        snprintf(buf, sizeof buf, "%d%%", (int)(p * 100));
        ui.text(card.right() - 28, card.y + 38, buf, 20, ui.t.textDim, RIGHT, true);
    } else {
        // Size unknown: a block slides back and forth.
        float t = 0.5f + 0.5f * std::sin(ui.time * 3);
        ui.fillRect({bar.x + (bar.w - 90) * t, bar.y, 90, bar.h}, ui.t.accent, 4);
    }
    Rect cancel{card.cx() - 80, card.bottom() - 62, 160, 44};
    ui.setDefaultFocus(uid("job.cancel"));
    if (ui.button(uid("job.cancel"), cancel, "Cancel", Icon::Close) || ui.back()) {
        job.progress.cancel = true;
        job.submitPassword("");
    }
}
