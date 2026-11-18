#include "net.hpp"

#include <chrono>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
}

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "version.hpp"

#ifdef __SWITCH__
extern "C" void switchmx_tls_strict(int on);
#endif

namespace net {
namespace {
const char* kAgent = "SwitchMX/" SWITCHMX_VERSION;
}  // namespace

// ---------------------------------------------------------------- a small JSON reader

namespace {

struct Parser {
    const char* p;
    const char* end;
    void ws() {
        while (p < end && isspace((unsigned char)*p)) p++;
    }
    bool lit(const char* s) {
        size_t n = strlen(s);
        if ((size_t)(end - p) < n || strncmp(p, s, n)) return false;
        p += n;
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 63));
            out += (char)(0x80 | (cp & 63));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 63));
            out += (char)(0x80 | ((cp >> 6) & 63));
            out += (char)(0x80 | (cp & 63));
        }
    }
    bool string(std::string& out) {
        if (p >= end || *p != '"') return false;
        p++;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                char c = *p++;
                switch (c) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (end - p < 4) return false;
                        unsigned cp = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16);
                        p += 4;
                        if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            unsigned lo = (unsigned)strtoul(std::string(p + 2, 4).c_str(), nullptr, 16);
                            p += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(out, cp);
                        break;
                    }
                    default: out += c;
                }
            } else {
                out += *p++;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool value(Json& v, int depth = 0) {
        if (depth > 64) return false;
        ws();
        if (p >= end) return false;
        if (*p == '{') {
            v.type = Json::Obj;
            p++;
            ws();
            if (p < end && *p == '}') return p++, true;
            for (;;) {
                ws();
                std::string k;
                if (!string(k)) return false;
                ws();
                if (p >= end || *p != ':') return false;
                p++;
                Json child;
                if (!value(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                ws();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                if (p < end && *p == '}') return p++, true;
                return false;
            }
        }
        if (*p == '[') {
            v.type = Json::Arr;
            p++;
            ws();
            if (p < end && *p == ']') return p++, true;
            for (;;) {
                Json child;
                if (!value(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                if (p < end && *p == ']') return p++, true;
                return false;
            }
        }
        if (*p == '"') {
            v.type = Json::Str;
            return string(v.str);
        }
        if (lit("true")) return v.type = Json::Bool, v.b = true, true;
        if (lit("false")) return v.type = Json::Bool, true;
        if (lit("null")) return true;
        char* e = nullptr;
        v.num = strtod(p, &e);
        if (e == p) return false;
        v.type = Json::Num;
        p = e;
        return true;
    }
};

}  // namespace

bool parseJson(const std::string& text, Json& out) {
    Parser ps{text.data(), text.data() + text.size()};
    return ps.value(out);
}

// ---------------------------------------------------------------- HTTPS

namespace {
// Gives up after 20 s without progress: looking up the name, connecting or the TLS handshake have
// no limit of their own, and once kept "Checking for updates" spinning for good.
struct Deadline {
    double until = 0, start = now();
    Sink* sink = nullptr;
    static double now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void extend() { until = now() + 20; }
    // 20 s without data ends any request; a request kept in memory (an API call, a page) also ends
    // after 45 s in all, so a server trickling bytes can't hold it (and its thread) for minutes.
    // Downloads to a file (an update) may take as long as they keep moving.
    static int expired(void* self) {
        auto* d = (Deadline*)self;
        double t = now();
        return (d->sink && d->sink->cancel) || t > d->until || (!(d->sink && d->sink->file) && t - d->start > 45);
    }
};
}  // namespace

Response request(const std::string& method, const std::string& host, const std::string& path,
                 const std::vector<std::string>& headers, const std::string& body, Sink* sink) {
    Response r;
    AVIOContext* io = nullptr;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rw_timeout", "15000000", 0);  // 15 s without data
#ifdef __SWITCH__
    av_dict_set(&opts, "tls_verify", "1", 0);
#endif
    std::string url = "tls://" + host + ":443";
    Deadline deadline;
    deadline.sink = sink;
    deadline.extend();
    AVIOInterruptCB interrupt{Deadline::expired, &deadline};
#ifdef __SWITCH__
    switchmx_tls_strict(1);  // a certificate for this host, from a trusted CA (ffmpeg_protocols.c)
#endif
    int rc = avio_open2(&io, url.c_str(), AVIO_FLAG_READ_WRITE, &interrupt, &opts);
#ifdef __SWITCH__
    switchmx_tls_strict(0);
#endif
    av_dict_free(&opts);
    if (rc < 0 || !io) {
        r.error = "Couldn't reach " + host + ". Check the Switch is online.";
        return r;
    }
    // HTTP/1.0 so the answer is never chunked and ends when the connection closes.
    std::string req = method + " " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: " + kAgent +
                      "\r\nAccept: */*\r\nConnection: close\r\n";
    for (auto& h : headers) req += h + "\r\n";
    if (!body.empty() || method == "POST")
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    avio_write(io, (const unsigned char*)req.data(), (int)req.size());
    avio_flush(io);
    std::string data;
    std::vector<unsigned char> buf(64 * 1024);
    bool headDone = false, toFile = false;
    for (;;) {
        if (sink && sink->cancel) break;
        int n = avio_read_partial(io, buf.data(), (int)buf.size());
        if (n <= 0) break;
        deadline.extend();
        if (toFile) {
            if (fwrite(buf.data(), 1, (size_t)n, sink->file) != (size_t)n) {
                sink->failed = true;
                break;
            }
            sink->done += n;
            if (sink->total > 0 && sink->done >= sink->total) break;  // all there: don't wait for the close
            continue;
        }
        data.append((const char*)buf.data(), (size_t)n);
        if (!headDone) {
            size_t he = data.find("\r\n\r\n");
            if (he == std::string::npos) {
                if (data.size() > 64 * 1024) break;
                continue;
            }
            headDone = true;
            std::string head = data.substr(0, he);
            data.erase(0, he + 4);
            size_t sp = head.find(' ');
            r.status = sp == std::string::npos ? 0 : atoi(head.c_str() + sp + 1);
            size_t pos = head.find("\r\n");
            while (pos != std::string::npos && pos < head.size()) {
                size_t e = head.find("\r\n", pos + 2);
                std::string line = head.substr(pos + 2, (e == std::string::npos ? head.size() : e) - pos - 2);
                size_t c = line.find(':');
                if (c != std::string::npos) {
                    std::string k = line.substr(0, c), v = line.substr(c + 1);
                    for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
                    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
                    r.headers[k] = v;
                }
                pos = e;
            }
            // A successful download goes to the file from here on.
            if (sink && sink->file && r.status == 200) {
                toFile = true;
                if (r.headers.count("content-length")) sink->total = atoll(r.headers["content-length"].c_str());
                if (!data.empty()) {
                    if (fwrite(data.data(), 1, data.size(), sink->file) != data.size()) sink->failed = true;
                    sink->done += (long long)data.size();
                    data.clear();
                }
                if (sink->failed) break;
                continue;
            }
        }
        // The whole answer has come: servers may keep the connection open, so stop here.
        if (headDone && r.headers.count("content-length") &&
            data.size() >= (size_t)atoll(r.headers["content-length"].c_str()))
            break;
        if (data.size() > (8u << 20)) break;  // nothing kept in memory is this big
    }
    avio_closep(&io);
    if (!headDone) {
        r.error = "No answer from " + host;
        return r;
    }
    r.body = std::move(data);
    return r;
}

Response get(std::string url, const std::vector<std::string>& headers, Sink* sink) {
    for (int hop = 0; hop < 5; hop++) {
        if (url.rfind("https://", 0) != 0) {
            Response r;
            r.error = "Unexpected download address";
            return r;
        }
        std::string rest = url.substr(8);
        size_t slash = rest.find('/');
        std::string host = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
        Response r = request("GET", host, path, headers, "", sink);
        if (r.status >= 300 && r.status < 400 && r.headers.count("location")) {
            std::string loc = r.headers["location"];
            url = loc.rfind("/", 0) == 0 ? "https://" + host + loc : loc;
            continue;
        }
        return r;
    }
    Response r;
    r.error = "Too many redirects";
    return r;
}

}  // namespace net
