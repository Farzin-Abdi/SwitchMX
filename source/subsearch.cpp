#include "subsearch.hpp"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
}

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

#include "library.hpp"
#include "net.hpp"

namespace {

const char* kHost = "api.opensubtitles.com";
using net::Json;
using net::Response;
using net::get;
using net::parseJson;
using net::request;


std::string urlEncode(const std::string& s) {
    static const char* hexd = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else {
            out += '%';
            out += hexd[c >> 4];
            out += hexd[c & 15];
        }
    }
    return out;
}

// OpenSubtitles redirects anything but its canonical form: parameters sorted, lower case, "+" for spaces.
std::string apiPath(const std::string& base, std::vector<std::pair<std::string, std::string>> params) {
    std::sort(params.begin(), params.end());
    std::string out = base;
    char sep = '?';
    for (auto& kv : params) {
        std::string v;
        for (unsigned char c : kv.second) v += (char)tolower(c);
        std::string enc = urlEncode(v);
        for (size_t i; (i = enc.find("%20")) != std::string::npos;) enc.replace(i, 3, "+");
        out += sep + kv.first + "=" + enc;
        sep = '&';
    }
    return out;
}

std::string apiError(const Response& r) {
    if (!r.error.empty()) return r.error;
    Json j;
    std::string msg;
    if (parseJson(r.body, j)) {
        if (j["message"].type == Json::Str) msg = j["message"].str;
        else if (j["errors"].type == Json::Arr && !j["errors"].arr.empty() && j["errors"].arr[0].type == Json::Str)
            msg = j["errors"].arr[0].str;
    }
    if (r.status == 401 || r.status == 403)
        return "OpenSubtitles refused the API key" + (msg.empty() ? std::string(".") : ": " + msg);
    if (r.status == 406 || r.status == 429) return msg.empty() ? "Today's download limit is used up" : msg;
    return msg.empty() ? "OpenSubtitles answered " + std::to_string(r.status) : msg;
}

// OpenSubtitles' file hash: the size plus the 64-bit words of the first and last 64 KB.
std::string movieHash(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    fseeko(f, 0, SEEK_END);
    long long size = ftello(f);
    if (size < 131072) {
        fclose(f);
        return "";
    }
    unsigned long long hash = (unsigned long long)size, word;
    fseeko(f, 0, SEEK_SET);
    for (int i = 0; i < 8192 && fread(&word, 8, 1, f) == 1; i++) hash += word;
    fseeko(f, size - 65536, SEEK_SET);
    for (int i = 0; i < 8192 && fread(&word, 8, 1, f) == 1; i++) hash += word;
    fclose(f);
    char buf[24];
    snprintf(buf, sizeof buf, "%016llx", hash);
    return buf;
}

}  // namespace

struct SubSearch::Shared {
    mutable std::mutex m;
    State state = Idle;
    std::string message, saved, dlError;
    std::vector<SubResult> results;
};

SubSearch::~SubSearch() = default;

// Runs `fn` on its own thread. If the console can't start one, or `fn` throws, the search simply
// fails with a message instead of taking the app down.
static std::atomic<int> g_running{0};
int SubSearch::running() { return g_running; }

void SubSearch::runDetached(std::function<void()> fn, std::shared_ptr<Shared> s) {
    auto* job = new std::function<void()>([fn, s] {
        try {
            fn();
        } catch (...) {
            std::lock_guard<std::mutex> lk(s->m);
            s->state = Failed;
            s->message = "Something went wrong while talking to OpenSubtitles";
        }
    });
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t t;
    g_running++;
    int rc = pthread_create(&t, &attr, [](void* p) -> void* {
        auto* f = (std::function<void()>*)p;
        (*f)();
        delete f;
        g_running--;
        return nullptr;
    }, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        g_running--;
        delete job;
        std::lock_guard<std::mutex> lk(s->m);
        s->state = Failed;
        s->message = "The console is too busy to search right now";
        return;
    }
    pthread_detach(t);
}  // a running thread keeps its own reference to the shared state

std::string SubSearch::queryFor(const std::string& path) {
    std::string n = stripExtension(baseName(path));
    // Channel tags like "@MoboMoviesBot-" or "[Group]" at the start.
    if (!n.empty() && n[0] == '@') {
        size_t cut = n.find_first_of("-_ .");
        if (cut != std::string::npos) n = n.substr(cut + 1);
    }
    while (!n.empty() && n[0] == '[') {
        size_t e = n.find(']');
        if (e == std::string::npos) break;
        n = n.substr(e + 1);
    }
    for (auto& c : n)
        if (c == '.' || c == '_' || c == '-') c = ' ';
    // Site numbers glued to the title: "1726181The Matrix".
    {
        size_t d = 0;
        while (d < n.size() && isdigit((unsigned char)n[d])) d++;
        if (d >= 3 && d < n.size() && isalpha((unsigned char)n[d])) n = n.substr(d);
    }
    // Keep the words up to the first release detail, but keep the episode (S01E02 / S01 E02).
    std::vector<std::string> words, out;
    size_t i = 0;
    while (i < n.size()) {
        while (i < n.size() && n[i] == ' ') i++;
        size_t j = i;
        while (j < n.size() && n[j] != ' ') j++;
        if (j > i) words.push_back(n.substr(i, j - i));
        i = j;
    }
    auto lower = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    static const char* stops[] = {"720p", "1080p", "2160p", "480p", "4k", "bluray", "brrip", "bdrip", "web", "webrip",
                                  "web-dl", "webdl", "hdtv", "x264", "x265", "h264", "h265", "hevc", "10bit", "8bit",
                                  "dubbed", "softsub", "hardsub", "remastered", "extended", "proper", "dvdrip", "hdrip"};
    for (size_t k = 0; k < words.size(); k++) {
        std::string w = lower(words[k]);
        bool stop = false;
        for (auto s : stops) stop |= w == s;
        bool year = w.size() == 4 && isdigit((unsigned char)w[0]) && (w.rfind("19", 0) == 0 || w.rfind("20", 0) == 0) && k > 0;
        if (stop) break;
        out.push_back(words[k]);
        if (year) break;
        // "S01" followed by "E04": join them.
        if (w.size() == 3 && w[0] == 's' && isdigit((unsigned char)w[1]) && k + 1 < words.size()) {
            std::string nx = lower(words[k + 1]);
            if (nx.size() >= 2 && nx[0] == 'e' && isdigit((unsigned char)nx[1])) {
                out.back() += words[k + 1];
                k++;
                break;
            }
        }
        if (w.size() >= 6 && w[0] == 's' && isdigit((unsigned char)w[1]) && w.find('e') != std::string::npos) break;
    }
    std::string q;
    for (auto& w : out) q += (q.empty() ? "" : " ") + w;
    return q.empty() ? stripExtension(baseName(path)) : q;
}

void SubSearch::search(const std::string& apiKey, const std::string& query, const std::string& languages,
                       const std::string& videoPath) {
    s_ = std::make_shared<Shared>();
    s_->state = Busy;
    downloading_ = false;
    auto s = s_;
    runDetached([s, apiKey, query, languages, videoPath] {
        std::vector<std::pair<std::string, std::string>> ps = {{"query", query}};
        if (!languages.empty()) ps.push_back({"languages", languages});
        std::string path = apiPath("/api/v1/subtitles", ps);
        std::string hash = movieHash(videoPath);
        // With the hash, versions made for exactly this file come first.
        Response r = request("GET", kHost, path, {"Api-Key: " + apiKey}, "");
        if ((r.status == 301 || r.status == 302) && r.headers.count("location") && r.headers["location"][0] == '/')
            r = request("GET", kHost, r.headers["location"], {"Api-Key: " + apiKey}, "");
        Response rh;
        if (!hash.empty() && r.status == 200) {
            std::vector<std::pair<std::string, std::string>> hs = {{"moviehash", hash}};
            if (!languages.empty()) hs.push_back({"languages", languages});
            std::string hp = apiPath("/api/v1/subtitles", hs);
            rh = request("GET", kHost, hp, {"Api-Key: " + apiKey}, "");
        }
        std::vector<SubResult> results;
        auto collect = [&](const Response& resp, bool exact) {
            Json j;
            if (resp.status != 200 || !parseJson(resp.body, j)) return;
            for (auto& item : j["data"].arr) {
                const Json& a = item["attributes"];
                if (a["files"].arr.empty()) continue;
                SubResult sr;
                sr.fileId = (long long)a["files"].arr[0]["file_id"].num;
                sr.release = a["release"].str;
                if (sr.release.empty()) sr.release = a["files"].arr[0]["file_name"].str;
                sr.language = a["language"].str;
                sr.downloads = (int)a["download_count"].num;
                sr.uploader = a["uploader"]["name"].str;
                sr.hashMatch = exact || a["moviehash_match"].b;
                if (!sr.fileId) continue;
                bool dup = false;
                for (auto& o : results) dup |= o.fileId == sr.fileId;
                if (!dup) results.push_back(sr);
            }
        };
        collect(rh, true);
        collect(r, false);
        std::stable_sort(results.begin(), results.end(), [](const SubResult& a, const SubResult& b) {
            if (a.hashMatch != b.hashMatch) return a.hashMatch;
            return a.downloads > b.downloads;
        });
        std::lock_guard<std::mutex> lk(s->m);
        if (r.status != 200) {
            s->state = Failed;
            s->message = apiError(r);
            return;
        }
        s->results = std::move(results);
        s->state = Done;
        if (s->results.empty()) s->message.clear();  // the player says "Nothing found for ..." itself
    }, s);
}

void SubSearch::download(const std::string& apiKey, long long fileId, const std::string& outPath) {
    auto s = s_ ? s_ : std::make_shared<Shared>();
    s_ = s;
    downloading_ = true;
    {
        std::lock_guard<std::mutex> lk(s->m);
        s->state = Busy;
        s->saved.clear();
        s->dlError.clear();
    }
    runDetached([s, apiKey, fileId, outPath] {
        Response r = request("POST", kHost, "/api/v1/download", {"Api-Key: " + apiKey},
                             "{\"file_id\":" + std::to_string(fileId) + "}");
        Json j;
        std::string link, note;
        if (r.status == 200 && parseJson(r.body, j)) {
            link = j["link"].str;
            if (j["remaining"].type == Json::Num) note = std::to_string((int)j["remaining"].num) + " downloads left today";
        }
        if (link.empty()) {
            std::lock_guard<std::mutex> lk(s->m);
            s->state = Done;  // the results stay usable
            s->dlError = apiError(r);
            return;
        }
        Response f = get(link);
        bool ok = f.error.empty() && f.status == 200 && !f.body.empty();
        if (ok) {
            FILE* out = fopen(outPath.c_str(), "wb");
            ok = out && fwrite(f.body.data(), 1, f.body.size(), out) == f.body.size();
            if (out) ok = fclose(out) == 0 && ok;
            if (!ok) unlink(outPath.c_str());
        }
        std::lock_guard<std::mutex> lk(s->m);
        s->state = Done;
        if (ok) {
            s->message = note;
            s->saved = outPath;
        } else {
            s->dlError = f.error.empty() ? "The download failed (" + std::to_string(f.status) + ")" : f.error;
        }
    }, s);
}

SubSearch::State SubSearch::state() const {
    if (!s_) return Idle;
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->state;
}

std::string SubSearch::message() const {
    if (!s_) return "";
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->message;
}

std::vector<SubResult> SubSearch::results() const {
    if (!s_) return {};
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->results;
}

std::string SubSearch::takeDownloadError() {
    if (!s_) return "";
    std::lock_guard<std::mutex> lk(s_->m);
    std::string e = s_->dlError;
    s_->dlError.clear();
    if (!e.empty()) downloading_ = false;
    return e;
}

std::string SubSearch::savedPath() const {
    if (!s_) return "";
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->saved;
}
