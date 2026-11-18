// Developer mode (see devtools.hpp).

#include "devtools.hpp"
#include "net.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <vector>

#include <mpv/client.h>

#include "app.hpp"
#include "gl.h"
#include "readahead.hpp"
#include "reader.hpp"

namespace {

// ---- log lines, numbered so a client can ask for what's new
struct Line {
    long long n;
    double t;
    std::string src, text;
};
std::mutex g_logMutex;
std::deque<Line> g_log;
long long g_logNext = 1;
constexpr size_t kLogKeep = 4000;

double nowSecs() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string jsonStr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + "\"";
}

std::string num(double v, int prec = 2) {
    char b[64];
    snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

// ---- work only the app's own thread can do, handed over and waited for
struct Job {
    std::string what;
    std::map<std::string, std::string> q;
    std::string body;
    int code = 200;
    std::string type = "application/json; charset=utf-8", out;
    bool done = false;
};
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::deque<std::shared_ptr<Job>> g_jobs;

// ---- scripted input, played back a frame at a time
struct Step {
    uint32_t buttons = 0;
    bool touch = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // a swipe moves from 0 to 1
    float rx = 0, ry = 0;                   // right stick
    float secs = 0.1f;
};
std::mutex g_inputMutex;
std::deque<Step> g_steps;
float g_stepTime = 0;
bool g_gap = false;  // one frame with nothing pressed between steps, so repeats register

std::atomic<double> g_lastRequest{-1};
std::atomic<int> g_requests{0};

uint32_t buttonBit(const std::string& n) {
    static const std::map<std::string, uint32_t> m = {
        {"A", BTN_A},       {"B", BTN_B},        {"X", BTN_X},         {"Y", BTN_Y},       {"L", BTN_L},
        {"R", BTN_R},       {"ZL", BTN_ZL},      {"ZR", BTN_ZR},       {"PLUS", BTN_PLUS}, {"+", BTN_PLUS},
        {"MINUS", BTN_MINUS}, {"-", BTN_MINUS}, {"UP", BTN_UP},       {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT},
        {"RIGHT", BTN_RIGHT}, {"LS", BTN_LSTICK}, {"RS", BTN_RSTICK},
    };
    std::string u = n;
    for (auto& c : u) c = (char)toupper((unsigned char)c);
    auto it = m.find(u);
    return it == m.end() ? 0 : it->second;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find(sep, i);
        if (e == std::string::npos) e = s.size();
        out.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    return out;
}

// "A", "R:1500", "ZL+ZR:300", "wait:400", "tap:640/360", "hold:x/y:ms", "swipe:x0/y0/x1/y1:ms",
// "rstick:x/y:ms"; separated by commas.
bool parseSteps(const std::string& keys, std::vector<Step>& out, std::string& err) {
    for (auto& raw : split(keys, ',')) {
        std::string s = raw;
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (s.empty()) continue;
        auto parts = split(s, ':');
        std::string head = parts[0];
        for (auto& c : head) c = (char)tolower((unsigned char)c);
        Step st;
        auto ms = [&](size_t i, float def) { return parts.size() > i ? (float)atof(parts[i].c_str()) / 1000 : def; };
        if (head == "wait") {
            st.secs = ms(1, 0.3f);
        } else if (head == "tap" || head == "hold") {
            auto xy = split(parts.size() > 1 ? parts[1] : "", '/');
            if (xy.size() != 2) return err = "tap needs x/y: " + s, false;
            st.touch = true;
            st.x0 = st.x1 = (float)atof(xy[0].c_str());
            st.y0 = st.y1 = (float)atof(xy[1].c_str());
            st.secs = head == "tap" ? 0.05f : ms(2, 0.8f);
        } else if (head == "swipe") {
            auto v = split(parts.size() > 1 ? parts[1] : "", '/');
            if (v.size() != 4) return err = "swipe needs x0/y0/x1/y1: " + s, false;
            st.touch = true;
            st.x0 = (float)atof(v[0].c_str());
            st.y0 = (float)atof(v[1].c_str());
            st.x1 = (float)atof(v[2].c_str());
            st.y1 = (float)atof(v[3].c_str());
            st.secs = ms(2, 0.3f);
        } else if (head == "rstick") {
            auto v = split(parts.size() > 1 ? parts[1] : "", '/');
            if (v.size() != 2) return err = "rstick needs x/y: " + s, false;
            st.rx = (float)atof(v[0].c_str());
            st.ry = (float)atof(v[1].c_str());
            st.secs = ms(2, 0.5f);
        } else {
            for (auto& b : split(parts[0], '+')) {
                uint32_t bit = buttonBit(b);
                if (!bit) return err = "unknown button: " + b, false;
                st.buttons |= bit;
            }
            st.secs = ms(1, 0.1f);
        }
        st.secs = std::max(0.02f, std::min(st.secs, 30.0f));
        out.push_back(st);
    }
    return true;
}

// ---- a PNG of what's on screen
void put32(std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; i--) s += (char)(v >> (i * 8));
}
void chunk(std::string& png, const char* type, const std::string& data) {
    put32(png, (uint32_t)data.size());
    std::string td = std::string(type, 4) + data;
    png += td;
    put32(png, (uint32_t)crc32(0, (const Bytef*)td.data(), (uInt)td.size()));
}
std::string screenshotPng(int w, int h, bool half) {
    // Read and compressed a strip at a time: when memory is tight one big buffer may not be had.
    int step = half ? 2 : 1, ow = w / step, oh = h / step;
    z_stream zs{};
    if (deflateInit(&zs, 3) != Z_OK) throw std::runtime_error("zlib");
    std::string z;
    std::vector<unsigned char> strip, row((size_t)ow * 3 + 1), out(64 << 10);
    const int rowsPer = 16 * step;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    auto pump = [&](int flush) {
        do {
            zs.next_out = out.data();
            zs.avail_out = (uInt)out.size();
            deflate(&zs, flush);
            z.append((const char*)out.data(), out.size() - zs.avail_out);
        } while (zs.avail_out == 0);
    };
    for (int y0 = 0; y0 < oh; y0 += rowsPer / step) {
        int rows = std::min(rowsPer / step, oh - y0);  // output rows in this strip
        int srcTop = h - y0 * step;                    // GL rows go bottom-up
        int srcRows = rows * step;
        strip.resize((size_t)w * srcRows * 4);
        glReadPixels(0, srcTop - srcRows, w, srcRows, GL_RGBA, GL_UNSIGNED_BYTE, strip.data());
        for (int r = 0; r < rows; r++) {
            const unsigned char* src = &strip[(size_t)(srcRows - 1 - r * step) * w * 4];
            row[0] = 0;
            for (int x = 0; x < ow; x++) {
                const unsigned char* p = src + (size_t)x * step * 4;
                row[1 + x * 3] = p[0];
                row[2 + x * 3] = p[1];
                row[3 + x * 3] = p[2];
            }
            zs.next_in = row.data();
            zs.avail_in = (uInt)row.size();
            pump(Z_NO_FLUSH);
        }
    }
    pump(Z_FINISH);
    deflateEnd(&zs);
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32(ihdr, (uint32_t)ow);
    put32(ihdr, (uint32_t)oh);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    return png;
}

bool readFile(const std::string& path, long long from, size_t max, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseeko(f, 0, SEEK_END);
    long long size = ftello(f);
    long long start = from < 0 ? std::max(0LL, size + from) : std::min(from, size);
    fseeko(f, start, SEEK_SET);
    size_t want = (size_t)std::min<long long>(size - start, (long long)max);
    out.resize(want);
    size_t got = want ? fread(&out[0], 1, want, f) : 0;
    out.resize(got);
    fclose(f);
    return true;
}

}  // namespace

void DevTools::log(const char* src, const std::string& line) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::string text = line;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    g_log.push_back({g_logNext++, nowSecs(), src, text});
    while (g_log.size() > kLogKeep) g_log.pop_front();
}

double DevTools::lastRequest() const {
    double t = g_lastRequest;
    return t < 0 ? -1 : nowSecs() - t;
}

int DevTools::requests() const { return g_requests; }

std::string DevTools::newToken() {
    static const char* chars = "abcdefghjkmnpqrstuvwxyz23456789";
    std::random_device rd;
    std::mt19937 rng(rd() ^ (unsigned)time(nullptr) ^ (unsigned)(uintptr_t)this);
    std::string t;
    for (int i = 0; i < 10; i++) t += chars[rng() % strlen(chars)];
    app.cfg.devToken = t;
    app.cfg.save();
    token_.clear();  // handed to the server again on the next update
    return t;
}

void DevTools::update() {
    bool on = app.cfg.s.devMode != 0;
    if (on && app.cfg.devToken.empty()) newToken();
    if (on && (!installed_ || token_ != app.cfg.devToken)) {
        token_ = app.cfg.devToken;
        app.web.setDev(token_, [this](const std::string& p, const std::map<std::string, std::string>& q,
                                      const std::string& body, int& code, std::string& type, std::string& out) {
            return serve(p, q, body, code, type, out);
        });
        app.logEvent(installed_ || announced_ ? "developer token changed" : "developer mode on");
        installed_ = announced_ = true;
    } else if (!on && installed_) {
        app.web.setDev("", nullptr);
        installed_ = announced_ = false;
        token_.clear();
        app.logEvent("developer mode off");
    }
}

// Runs on a server thread; anything touching the app is handed to the app's thread.
bool DevTools::serve(const std::string& path, const std::map<std::string, std::string>& q,
                     const std::string& body, int& code, std::string& type, std::string& out) {
    g_lastRequest = nowSecs();
    g_requests++;
    auto get = [&](const char* k) {
        auto it = q.find(k);
        return it == q.end() ? std::string() : it->second;
    };
    if (path == "log") {
        long long since = atoll(get("since").c_str());
        std::string src = get("src");
        bool json = get("format") == "json";
        std::lock_guard<std::mutex> lk(g_logMutex);
        type = json ? "application/json; charset=utf-8" : "text/plain; charset=utf-8";
        if (json) out = "{\"next\":" + std::to_string(g_logNext) + ",\"lines\":[";
        bool first = true;
        for (auto& l : g_log) {
            if (l.n < since || (!src.empty() && src != "all" && src != l.src)) continue;
            if (json) {
                out += std::string(first ? "" : ",") + "{\"n\":" + std::to_string(l.n) + ",\"t\":" + num(l.t, 3) +
                       ",\"src\":" + jsonStr(l.src) + ",\"text\":" + jsonStr(l.text) + "}";
            } else {
                out += std::to_string(l.n) + " " + num(l.t, 3) + " [" + l.src + "] " + l.text + "\n";
            }
            first = false;
        }
        if (json) out += "]}";
        else out += "# next " + std::to_string(g_logNext) + "\n";
        return true;
    }
    if (path == "ls") {
        std::string dir = get("path");
        if (!dir.empty() && dir.back() != '/') dir += '/';
        DIR* d = opendir(dir.c_str());
        if (!d) {
            code = 404;
            out = "{\"error\":\"can't open folder\"}";
            return true;
        }
        out = "[";
        bool first = true;
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            struct stat st;
            std::string full = dir + e->d_name;
            bool ok = stat(full.c_str(), &st) == 0;
            out += std::string(first ? "" : ",") + "{\"name\":" + jsonStr(e->d_name) + ",\"shown\":" +
                   jsonStr(fromDiskName(e->d_name)) + ",\"dir\":" + (ok && S_ISDIR(st.st_mode) ? "true" : "false") +
                   ",\"size\":" + std::to_string(ok ? (long long)st.st_size : -1) +
                   ",\"mtime\":" + std::to_string(ok ? (long long)st.st_mtime : 0) + "}";
            first = false;
        }
        closedir(d);
        out += "]";
        return true;
    }
    if (path == "file") {
        std::string from = get("from");
        size_t max = get("max").empty() ? (16u << 20) : (size_t)std::min(atoll(get("max").c_str()), 64LL << 20);
        if (!readFile(get("path"), from.empty() ? 0 : atoll(from.c_str()), max, out)) {
            code = 404;
            out = "{\"error\":\"can't read file\"}";
            return true;
        }
        type = "application/octet-stream";
        return true;
    }
    if (path == "dns") {
        // How long the console takes to look a name up, and what it answers (network checks).
        auto t0 = nowSecs();
        struct addrinfo hints {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        int rc = getaddrinfo(get("host").c_str(), "443", &hints, &res);
        std::string ip;
        if (rc == 0 && res) {
            char b[64] = "";
            inet_ntop(AF_INET, &((struct sockaddr_in*)res->ai_addr)->sin_addr, b, sizeof b);
            ip = b;
            freeaddrinfo(res);
        }
        out = "{\"rc\":" + std::to_string(rc) + ",\"ip\":" + jsonStr(ip) + ",\"seconds\":" + num(nowSecs() - t0, 2) + "}";
        return true;
    }
    if (path == "fetch") {
        // An https address fetched over the console's own network (for working out how a web page
        // finds its stream). headers: one "Name: value" per line; with a body, it's a POST.
        std::string url = get("url");
        std::vector<std::string> hdrs;
        std::string h = get("headers");
        size_t i = 0;
        while (i < h.size()) {
            size_t e = h.find('\n', i);
            if (e == std::string::npos) e = h.size();
            if (e > i) hdrs.push_back(h.substr(i, e - i));
            i = e + 1;
        }
        net::Response r;
        if (body.empty()) {
            r = net::get(url, hdrs);
        } else if (url.rfind("https://", 0) == 0) {
            std::string rest = url.substr(8);
            size_t slash = rest.find('/');
            r = net::request("POST", rest.substr(0, slash), slash == std::string::npos ? "/" : rest.substr(slash), hdrs, body);
        }
        std::string hs = "{";
        bool first = true;
        for (auto& kv : r.headers) {
            hs += std::string(first ? "" : ",") + jsonStr(kv.first) + ":" + jsonStr(kv.second);
            first = false;
        }
        hs += "}";
        out = "{\"status\":" + std::to_string(r.status) + ",\"error\":" + jsonStr(r.error) + ",\"headers\":" + hs +
              ",\"size\":" + std::to_string(r.body.size()) + ",\"body\":" + jsonStr(r.body.substr(0, 300000)) + "}";
        return true;
    }
    if (path == "input") {
        std::vector<Step> steps;
        std::string err;
        if (!parseSteps(get("keys").empty() ? body : get("keys"), steps, err)) {
            code = 400;
            out = "{\"error\":" + jsonStr(err) + "}";
            return true;
        }
        float total = 0;
        {
            std::lock_guard<std::mutex> lk(g_inputMutex);
            for (auto& s : steps) {
                g_steps.push_back(s);
                total += s.secs + 0.034f;
            }
        }
        out = "{\"ok\":true,\"steps\":" + std::to_string(steps.size()) + ",\"seconds\":" + num(total) + "}";
        return true;
    }
    // Everything else needs the app's thread.
    static const char* mainThread[] = {"status", "screenshot", "open", "mpv", "mpvlog", "back", "config", "exit", "dump", "clock"};
    bool known = false;
    for (auto m : mainThread) known |= path == m;
    if (!known) return false;
    auto job = std::make_shared<Job>();
    job->what = path;
    job->q = q;
    job->body = body;
    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        g_jobs.push_back(job);
    }
    std::unique_lock<std::mutex> lk(g_jobMutex);
    if (!g_jobCv.wait_for(lk, std::chrono::seconds(8), [&] { return job->done; })) {
        code = 504;
        out = "{\"error\":\"the app didn't answer in 8 seconds (busy or stuck)\"}";
        return true;
    }
    code = job->code;
    type = job->type;
    out = std::move(job->out);
    return true;
}

void DevTools::applyInput(RawInput& raw, float dt) {
    std::lock_guard<std::mutex> lk(g_inputMutex);
    if (g_steps.empty()) return;
    if (g_gap) {  // nothing pressed for a frame between steps
        g_gap = false;
        return;
    }
    Step& s = g_steps.front();
    float k = s.secs > 0 ? std::min(1.0f, g_stepTime / s.secs) : 1;
    raw.buttons |= s.buttons;
    if (s.touch) {
        raw.touches = 1;
        raw.tx[0] = s.x0 + (s.x1 - s.x0) * k;
        raw.ty[0] = s.y0 + (s.y1 - s.y0) * k;
    }
    if (s.rx != 0 || s.ry != 0) {
        raw.rx = s.rx;
        raw.ry = s.ry;
    }
    g_stepTime += dt;
    if (g_stepTime >= s.secs) {
        bool needsGap = s.buttons || s.touch;
        g_steps.pop_front();
        g_stepTime = 0;
        g_gap = needsGap;
    }
}

std::string DevTools::status() {
    App& a = app;
    Player& p = a.player;
    std::string o = "{";
    auto kv = [&](const char* k, const std::string& v) { o += (o.size() > 1 ? "," : "") + std::string("\"") + k + "\":" + v; };
    kv("version", jsonStr(SWITCHMX_VERSION));
    kv("built", jsonStr(std::string(__DATE__) + " " + __TIME__));
    kv("system", jsonStr(platform::systemVersion()));
    kv("appletMode", platform::isAppletMode() ? "true" : "false");
    kv("docked", platform::isDocked() ? "true" : "false");
    kv("battery", std::to_string(platform::batteryPercent()));
    kv("charging", platform::charging() ? "true" : "false");
    kv("uptime", num(a.time, 1));
    unsigned long long used = 0, total = 0;
    platform::memoryUsage(used, total);
    kv("memoryUsedMB", num(used / 1048576.0, 1));
    kv("memoryTotalMB", num(total / 1048576.0, 1));
    long long thUsed = 0, thLimit = 0;
    if (platform::threadUsage(thUsed, thLimit)) kv("threads", jsonStr(std::to_string(thUsed) + "/" + std::to_string(thLimit)));
    float avg = 0, worst = 0;
    int n = std::min(a.frameIndex_, App::kFrameHistory);
    for (int i = 0; i < n; i++) {
        avg += a.frameTimes_[i];
        worst = std::max(worst, a.frameTimes_[i]);
    }
    kv("frameMs", num(n ? avg / n * 1000 : 0, 1));
    kv("worstFrameMs", num(worst * 1000, 1));
    if (!a.cfg.s.showStats) a.cpuCores_ = platform::cpuLoad(a.cpu_, 4);  // load since the last request
    kv("cpu", "[" + num(a.cpu_[0] * 100, 0) + "," + num(a.cpu_[1] * 100, 0) + "," + num(a.cpu_[2] * 100, 0) + "," +
                  num(a.cpu_[3] * 100, 0) + "]");
    std::string screen = a.screen == a.home ? "home" : a.screen == a.playerScreen ? "player" : a.screen == a.reader ? "reader" : "other";
    kv("screen", jsonStr(screen));
    kv("modalOpen", a.modalOpen() ? "true" : "false");
    kv("language", jsonStr(i18n::languages()[a.cfg.s.language].code));
    kv("web", jsonStr(a.web.running() ? a.webAddress() : ""));
    kv("focus", std::to_string(a.ui.focus));
    kv("focusShown", a.ui.showFocus() ? "true" : "false");
    kv("webPin", jsonStr(a.web.running() ? a.web.pin() : ""));
    kv("lowMemoryMode", total && total < (1ull << 30) ? "true" : "false");
    if (a.home) {
        kv("homeSection", std::to_string((int)a.home->section()));
        kv("homeDir", jsonStr(a.home->dir()));
    }
    std::string pl = "{";
    auto pk = [&](const char* k, const std::string& v) { pl += (pl.size() > 1 ? "," : "") + std::string("\"") + k + "\":" + v; };
    pk("path", jsonStr(p.path));
    pk("position", num(p.position));
    pk("duration", num(p.duration));
    pk("paused", p.paused ? "true" : "false");
    pk("idle", p.idle ? "true" : "false");
    pk("loaded", p.loaded ? "true" : "false");
    pk("buffering", p.buffering ? "true" : "false");
    pk("speed", num(p.speed));
    pk("playlistPos", std::to_string(p.playlistPos));
    pk("playlistCount", std::to_string(p.playlistCount));
    pk("video", jsonStr(p.videoCodec + (p.width ? " " + std::to_string(p.width) + "x" + std::to_string(p.height) : "")));
    pk("audio", jsonStr(p.audioCodec));
    pk("hwdec", jsonStr(p.hwdecCurrent));
    pk("droppedFrames", std::to_string(p.droppedFrames));
    pk("cacheSeconds", num(p.cacheTime, 1));
    pk("seeks", std::to_string(p.seekCount));
    pk("lastError", jsonStr(p.lastError));
    if (a.playerScreen) {
        pk("osd", a.playerScreen->osdShown() ? "true" : "false");
        pk("panel", std::to_string(a.playerScreen->panelShown()));
        pk("locked", a.playerScreen->lockedNow() ? "true" : "false");
    }
    pl += "}";
    kv("player", pl);
    ReadAheadStats ra = readAheadStats();
    kv("readAhead", "{\"active\":" + std::string(ra.active ? "true" : "false") + ",\"MBps\":" + num(ra.readMBps, 1) +
                        ",\"bufferedMB\":" + num(ra.bufferedMB, 1) + ",\"slowestMs\":" + num(ra.slowestReadMs, 0) + "}");
    if (a.reader)
        kv("reader", "{\"path\":" + jsonStr(a.reader->path()) + ",\"page\":" + std::to_string(a.reader->page()) +
                         ",\"pages\":" + std::to_string(a.reader->pages()) + "}");
    std::string fwd;
    readFile(a.dataDir + "forwarder_build", 0, 64, fwd);
    while (!fwd.empty() && (fwd.back() == '\n' || fwd.back() == '\r')) fwd.pop_back();
    kv("homeIcon", jsonStr(fwd));
    return o + "}";
}

void DevTools::endFrame(int fbw, int fbh) {
    std::deque<std::shared_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        jobs.swap(g_jobs);
    }
    if (jobs.empty()) return;
    for (auto& job : jobs) {
        auto get = [&](const char* k) {
            auto it = job->q.find(k);
            return it == job->q.end() ? std::string() : it->second;
        };
        try {
            if (job->what == "status") {
                job->out = status();
            } else if (job->what == "screenshot") {
                job->out = screenshotPng(fbw, fbh, get("half") == "1");
                job->type = "image/png";
            } else if (job->what == "open") {
                std::string path = get("path");
                struct stat st;
                if (path.empty() || stat(path.c_str(), &st) != 0) {
                    job->code = 404;
                    job->out = "{\"error\":\"no such file\"}";
                } else {
                    app.logEvent("developer: open " + path);
                    if (S_ISDIR(st.st_mode)) {
                        if (app.screen != app.home) app.switchTo(app.home);
                        app.home->openDir(path);
                    } else {
                        app.playPath(path);
                    }
                    job->out = "{\"ok\":true}";
                }
            } else if (job->what == "back") {
                if (app.screen == app.playerScreen) app.playerScreen->exit();
                else if (app.screen == app.reader) app.reader->exit();
                job->out = "{\"ok\":true}";
            } else if (job->what == "mpv" || job->what == "mpvlog") {
                mpv_handle* h = app.player.handle();
                if (!h) {
                    job->code = 503;
                    job->out = "{\"error\":\"the player isn't running\"}";
                } else if (job->what == "mpvlog") {
                    std::string lvl = get("level").empty() ? "v" : get("level");
                    app.player.setDevLogLevel(lvl);
                    job->out = "{\"ok\":true}";
                } else if (!get("get").empty()) {
                    char* v = mpv_get_property_string(h, get("get").c_str());
                    job->out = "{\"value\":" + (v ? jsonStr(v) : std::string("null")) + "}";
                    if (v) mpv_free(v);
                } else {
                    std::string cmd = get("cmd").empty() ? job->body : get("cmd");
                    int rc = mpv_command_string(h, cmd.c_str());
                    app.logEvent("developer: mpv " + cmd);
                    job->out = "{\"result\":" + jsonStr(mpv_error_string(rc)) + "}";
                }
            } else if (job->what == "exit") {
                // Closes the app the ordinary way (a new build sent over Wi-Fi starts next time).
                // Starting it again inside the same process isn't done: it crashed every time, as
                // something of the old run went on writing into the memory the new one was given.
                app.logEvent("developer: exit");
                app.running = false;
                job->out = "{\"ok\":true}";
            } else if (job->what == "dump") {
                // Stops the app on purpose so Atmosphère writes a crash report with every thread's
                // call stack: the way to see where something is stuck.
                app.logEvent("developer: dump (the app stops here on purpose)");
                platform::abortForReport();
            } else if (job->what == "clock") {
                // CPU clock in MHz for measuring (cpu=1785; cpu=0 gives the system's back).
                int now = get("cpu").empty() ? platform::setCpuClock(-1) : platform::setCpuClock(atoi(get("cpu").c_str()));
                job->out = "{\"cpuMHz\":" + std::to_string(now) + "}";
            } else if (job->what == "config") {
                std::string text;
                readFile(app.dataDir + "config.ini", 0, 1 << 20, text);
                job->out = text;
                job->type = "text/plain; charset=utf-8";
            }
        } catch (const std::exception& e) {
            job->code = 500;
            job->out = "{\"error\":" + jsonStr(e.what()) + "}";
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_jobMutex);
        for (auto& job : jobs) job->done = true;
    }
    g_jobCv.notify_all();
}
