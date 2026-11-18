// Network places on the Home screen: servers on the Storage page, their folders listed on another
// thread (a slow or vanished server never freezes the app), and adding or changing a server.
#include <cstdlib>
#include <mutex>
#include <functional>
#include <pthread.h>

#include "app.hpp"
#include "netfs.hpp"

struct HomeScreen::NetListing {
    std::mutex m;
    bool done = false;
    std::string dir, key, error;
    std::vector<FileEntry> entries;
};

bool HomeScreen::netDir() const { return netfs::isNetPath(dir_); }

void HomeScreen::startNetListing(bool force) {
    netError_.clear();
    // Cached per folder and per filter and sort, so changing either lists it again.
    std::string cacheKey = dir_ + "|" + std::to_string(app.cfg.s.showHidden) + std::to_string(app.cfg.s.mediaOnly) +
                           std::to_string(app.cfg.s.sortMode);
    auto cached = netCache_.find(cacheKey);
    if (!force && cached != netCache_.end() && app.ui.time - cached->second.first < 30) {
        entries_ = cached->second.second;
        netLoading_ = false;
        return;
    }
    entries_.clear();
    netLoading_ = true;
    netJustOpened_ = true;
    auto job = std::make_shared<NetListing>();
    job->dir = dir_;
    job->key = cacheKey;
    netJob_ = job;  // a listing still running for another folder is simply forgotten
    bool showHidden = app.cfg.s.showHidden, mediaOnly = app.cfg.s.mediaOnly != 0;
    int sort = app.cfg.s.sortMode;
    // A plain thread with room on its stack (getaddrinfo and the listing buffers), detached; any
    // error inside it is reported as text, never thrown out of it.
    auto* task = new std::function<void()>([job, showHidden, mediaOnly, sort] {
            std::vector<netfs::Item> items;
            std::string err;
            std::vector<FileEntry> out;
            if (netfs::list(job->dir, items, err)) {
                for (auto& it : items) {
                    if (!showHidden && !it.name.empty() && it.name[0] == '.') continue;
                    FileEntry fe;
                    fe.name = it.name;
                    fe.path = job->dir + netfs::encode(it.name) + (it.dir ? "/" : "");
                    fe.size = it.size;
                    fe.mtime = it.mtime;
                    if (it.dir) {
                        fe.kind = MediaKind::Folder;
                    } else {
                        fe.ext = fileExtension(fe.name);
                        fe.kind = kindForExtension(fe.ext);
                        if (mediaOnly && fe.kind != MediaKind::Video && fe.kind != MediaKind::Audio) continue;
                    }
                    out.push_back(std::move(fe));
                }
                sortEntries(out, sort);
            }
            std::lock_guard<std::mutex> lk(job->m);
            job->entries = std::move(out);
            job->error = err;
            job->done = true;
    });
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_t t;
    int rc = pthread_create(&t, &attr, [](void* p) -> void* {
        auto* f = (std::function<void()>*)p;
        try {
            (*f)();
        } catch (...) {
        }
        delete f;
        return nullptr;
    }, task);
    pthread_attr_destroy(&attr);
    if (rc == 0) pthread_detach(t);
    if (rc != 0) {
        delete task;
        netLoading_ = false;
        netError_ = "The console is busy. Try again in a moment";
    }
}

void HomeScreen::pollNetListing() {
    if (!netJob_) return;
    auto job = netJob_;
    std::lock_guard<std::mutex> lk(job->m);
    if (!job->done) return;
    netJob_.reset();
    if (job->dir != dir_) return;
    netLoading_ = false;
    netError_ = job->error;
    if (!netError_.empty()) return;
    entries_ = job->entries;
    netCache_[job->key] = {app.ui.time, entries_};
    auto it = dirFocus_.find(dir_);
    pendingFocus_ = it != dirFocus_.end() ? uid(it->second.c_str(), 77)
                    : !entries_.empty()   ? uid(entries_[0].path.c_str(), 77)
                                          : 0;
    if (pendingFocus_) app.ui.setFocus(pendingFocus_);
    if (entries_.empty()) netError_.clear();  // shown as an empty folder
}

// Adding a server: first what kind it is, then its details.
void HomeScreen::askServer(int index) {
    auto& list = netfs::servers();
    if (index >= 0 && index < (int)list.size()) {
        askServerDetails(index, list[index].type);
        return;
    }
    app.openMenu("Add network server",
                 {{"SMB (shared folders)", Icon::Server, "Windows, Mac, NAS"}, {"FTP", Icon::Server, "FTP server apps"}},
                 [this](int i) {
                     if (i == 0) askServerDetails(-1, "smb");
                     if (i == 1) askServerDetails(-1, "ftp");
                 });
}

// Asks for a server's details, one keyboard at a time; index -1 adds a new one.
void HomeScreen::askServerDetails(int index, const std::string& type) {
    auto& list = netfs::servers();
    netfs::Server s = index >= 0 && index < (int)list.size() ? list[index] : netfs::Server{};
    s.type = type;
    bool smb = type == "smb";
    int defPort = netfs::defaultPort(type);
    std::string addr = s.host.empty() ? "" : s.host + (s.port && s.port != defPort ? ":" + std::to_string(s.port) : "");
    if (!app.askText(smb ? "Computer or NAS address (like 192.168.1.10 or MYPC)"
                         : "FTP server address (like 192.168.1.10 or 192.168.1.10:2121)",
                     addr, addr) ||
        addr.empty())
        return;
    // A whole address pasted in (smb://user:pass@host/Share/Folder, \\PC\Share, ftp://...)
    // fills the other fields too.
    std::string a = addr;
    for (auto& c : a)
        if (c == '\\') c = '/';
    if (a.rfind("ftp://", 0) == 0 || a.rfind("smb://", 0) == 0) a = a.substr(6);
    while (!a.empty() && a[0] == '/') a.erase(0, 1);
    size_t slash = a.find('/');
    bool pathGiven = slash != std::string::npos;
    if (pathGiven) {
        s.path = netfs::decode(a.substr(slash));
        a = a.substr(0, slash);
    }
    size_t at = a.rfind('@');
    if (at != std::string::npos) {
        std::string cred = a.substr(0, at);
        a = a.substr(at + 1);
        size_t c = cred.find(':');
        s.user = netfs::decode(cred.substr(0, c));
        if (c != std::string::npos) s.pass = netfs::decode(cred.substr(c + 1));
    }
    size_t colon = a.rfind(':');
    s.port = 0;
    if (colon != std::string::npos) {
        s.port = atoi(a.c_str() + colon + 1);
        a = a.substr(0, colon);
    }
    while (!a.empty() && a.back() == ' ') a.pop_back();
    while (!a.empty() && a[0] == ' ') a.erase(0, 1);
    if (a.empty()) return;
    s.host = a;
    if (at == std::string::npos) {
        if (!app.askText("User name (leave empty to sign in as a guest)", s.user, s.user)) return;
        if (!s.user.empty() && !app.askText("Password", s.pass, s.pass)) return;
        if (s.user.empty()) s.pass.clear();
    }
    if (s.path.empty()) s.path = "/";
    if (!pathGiven) {
        if (smb) {
            // The share, optionally with a folder in it; empty shows the server's shared folders.
            std::string sh = s.path == "/" ? "" : s.path.substr(1);
            if (!app.askText("Shared folder (leave empty to choose from the server's)", sh, sh)) return;
            for (auto& c : sh)
                if (c == '\\') c = '/';
            while (!sh.empty() && sh[0] == '/') sh.erase(0, 1);
            s.path = "/" + sh;
        } else if (!app.askText("Folder to open (/ for the top)", s.path, s.path)) {
            return;
        }
    }
    if (s.path.empty() || s.path[0] != '/') s.path = "/" + s.path;
    std::string name = s.name.empty() ? s.host : s.name;
    if (!app.askText("Name for this server", name, name)) return;
    s.name = name.empty() ? s.host : name;
    if (index >= 0 && index < (int)list.size()) list[index] = s;
    else list.push_back(s);
    netfs::save();
    netCache_.clear();
    app.logEvent("network place " + std::string(index >= 0 ? "changed" : "added") + ": " + type + " " + s.host);
    openDir(netfs::rootUrl(s));  // straight in: a wrong detail shows right away
}

void HomeScreen::serverMenu(const FileEntry& e) {
    int i = e.server;
    if (i < 0 || i >= (int)netfs::servers().size()) return;
    std::string path = e.path, name = e.name;
    app.openMenu(name, {{"Open", Icon::Folder}, {"Change…", Icon::Rename}, {"Remove", Icon::Trash, "", true}},
                 [this, i, path, name](int k) {
                     if (k == 0) openDir(path);
                     if (k == 1) askServer(i);
                     if (k == 2)
                         app.confirm("Remove this server?", trf("“{0}” is removed from SwitchMX. Nothing on the server changes.", {name}),
                                     "Remove", [this, i] {
                                         auto& list = netfs::servers();
                                         if (i < (int)list.size()) list.erase(list.begin() + i);
                                         netfs::save();
                                         netCache_.clear();
                                         refresh();
                                     });
                 },
                 e.detail);
}
