#include "updater.hpp"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "net.hpp"
#include "version.hpp"

namespace {
std::atomic<int> g_running{0};

// False if no thread could be started (memory short): the caller says so instead of waiting forever.
bool runDetached(std::function<void()> fn) {
    auto* job = new std::function<void()>(std::move(fn));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    g_running++;
    int rc = pthread_create(&t, &attr, [](void* p) -> void* {
        auto* f = (std::function<void()>*)p;
        try {
            (*f)();
        } catch (...) {
        }
        delete f;
        g_running--;
        return nullptr;
    }, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        g_running--;
        delete job;
        return false;
    }
    pthread_detach(t);
    return true;
}

std::vector<long> versionParts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<long> out;
    size_t i = 0;
    while (i < v.size() && isdigit((unsigned char)v[i])) {
        out.push_back(strtol(v.c_str() + i, nullptr, 10));
        while (i < v.size() && isdigit((unsigned char)v[i])) i++;
        if (i < v.size() && v[i] == '.') i++;
        else break;
    }
    return out;
}

bool isNro(const std::string& path, long long expected) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size < 0x100 || (expected > 0 && st.st_size != expected)) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char head[0x14] = {};
    bool ok = fread(head, 1, sizeof head, f) == sizeof head && memcmp(head + 0x10, "NRO0", 4) == 0;
    fclose(f);
    return ok;
}
}  // namespace

struct Updater::Shared {
    mutable std::mutex m;
    State state = Idle;
    std::string latest, notes, message, assetUrl;
    long long assetSize = 0;
    net::Sink sink;
};

int Updater::running() { return g_running; }

int Updater::compareVersions(const std::string& a, const std::string& b) {
    auto x = versionParts(a), y = versionParts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

void Updater::check() {
    auto s = std::make_shared<Shared>();
    s->state = Checking;
    s_ = s;
    bool started = runDetached([s] {
        std::string repo = kRepo;
#ifndef __SWITCH__
        if (const char* e = getenv("SWITCHMX_UPDATE_REPO")) repo = e;  // dev aid: test against another project
#endif
        net::Response r = net::request("GET", "api.github.com", "/repos/" + repo + "/releases/latest",
                                       {"Accept: application/vnd.github+json"}, "");
        std::lock_guard<std::mutex> lk(s->m);
        if (!r.error.empty()) {
            s->state = Failed;
            s->message = "No internet connection";
            return;
        }
        if (r.status == 404) {
            s->state = NoRelease;
            return;
        }
        net::Json j;
        if (r.status != 200 || !net::parseJson(r.body, j)) {
            s->state = Failed;
            s->message = r.status == 403 || r.status == 429 ? "GitHub is busy. Try again later" : "Couldn't check for updates";
            return;
        }
        std::string tag = j["tag_name"].str;
        s->latest = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
        s->notes = j["body"].str;
        for (auto& a : j["assets"].arr) {
            std::string name = a["name"].str;
            if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".nro") == 0) {
                s->assetUrl = a["browser_download_url"].str;
                s->assetSize = (long long)a["size"].num;
                if (strcasecmp(name.c_str(), "SwitchMX.nro") == 0) break;  // the app itself, if there are several
            }
        }
        if (s->latest.empty()) s->state = NoRelease;
        else if (compareVersions(s->latest, SWITCHMX_VERSION) > 0) s->state = Available;
        else s->state = UpToDate;
    });
    if (!started) {
        std::lock_guard<std::mutex> lk(s->m);
        s->state = Failed;
        s->message = "Couldn't check for updates";
    }
}

void Updater::install(const std::string& nroPath) {
    auto s = s_;
    if (!s) return;
    std::string url;
    long long size;
    {
        std::lock_guard<std::mutex> lk(s->m);
        if (s->state != Available && s->state != DownloadFailed) return;
        if (s->assetUrl.empty()) {
            s->state = DownloadFailed;
            s->message = "This release has no SwitchMX.nro to download";
            return;
        }
        s->state = Downloading;
        s->sink.done = 0;
        s->sink.total = s->assetSize;
        s->sink.cancel = false;
        url = s->assetUrl;
        size = s->assetSize;
    }
    bool started = runDetached([s, url, size, nroPath] {
        auto fail = [&](const std::string& why) {
            std::lock_guard<std::mutex> lk(s->m);
            s->state = DownloadFailed;
            s->message = why;
        };
        std::string part = nroPath + ".download", old = nroPath + ".old";
        FILE* f = fopen(part.c_str(), "wb");
        if (!f) return fail("Can't write to the SD card");
        s->sink.file = f;
        s->sink.failed = false;
        net::Response r = net::get(url, {"Accept: application/octet-stream"}, &s->sink);
        s->sink.file = nullptr;
        bool written = fclose(f) == 0 && !s->sink.failed;
        if (s->sink.cancel) {
            unlink(part.c_str());
            std::lock_guard<std::mutex> lk(s->m);
            s->state = Available;
            return;
        }
        if (!r.error.empty() || r.status != 200 || !written || !isNro(part, size)) {
            unlink(part.c_str());
            return fail(!r.error.empty() ? "The connection was lost" : !written ? "The SD card is full or read-only"
                                                                                 : "The download was incomplete");
        }
        // Swap in the new file; the old one comes back if anything goes wrong.
        unlink(old.c_str());
        bool hadOld = access(nroPath.c_str(), F_OK) == 0;
        if (hadOld && rename(nroPath.c_str(), old.c_str()) != 0) {
            unlink(part.c_str());
            return fail("Couldn't replace the app file");
        }
        if (rename(part.c_str(), nroPath.c_str()) != 0) {
            if (hadOld) rename(old.c_str(), nroPath.c_str());
            unlink(part.c_str());
            return fail("Couldn't replace the app file");
        }
        unlink(old.c_str());
        std::lock_guard<std::mutex> lk(s->m);
        s->state = Installed;
    });
    if (!started) {
        std::lock_guard<std::mutex> lk(s->m);
        s->state = DownloadFailed;
        s->message = "Couldn't start the download. Try again";
    }
}

void Updater::cancel() {
    if (s_) s_->sink.cancel = true;
}

Updater::State Updater::state() const {
    if (!s_) return Idle;
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->state;
}
std::string Updater::latest() const {
    if (!s_) return "";
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->latest;
}
std::string Updater::notes() const {
    if (!s_) return "";
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->notes;
}
std::string Updater::message() const {
    if (!s_) return "";
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->message;
}
float Updater::progress() const {
    if (!s_) return -1;
    long long t = s_->sink.total, d = s_->sink.done;
    return t > 0 ? std::min(1.0f, (float)d / (float)t) : -1;
}
