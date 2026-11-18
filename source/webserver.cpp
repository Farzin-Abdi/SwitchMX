#include "webserver.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <zlib.h>

#include "fileops.hpp"
#include "library.hpp"
#include "names.hpp"

extern const char* const kWebUi;

namespace {

constexpr size_t kIoBuffer = 512 * 1024;

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

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

std::string jsonEscape(const std::string& s) {
    std::string out;
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
    return out;
}

bool sendAll(int fd, const char* data, size_t n) {
    while (n) {
        ssize_t k = send(fd, data, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        data += k;
        n -= (size_t)k;
    }
    return true;
}
bool sendAll(int fd, const std::string& s) { return sendAll(fd, s.data(), s.size()); }

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 507: return "Insufficient Storage";
        default: return "OK";
    }
}

void respond(int fd, int code, const std::string& type, const std::string& body, const std::string& extra = "") {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\nCache-Control: no-store\r\n" + extra + "\r\n";
    sendAll(fd, h);
    sendAll(fd, body);
}

void json(int fd, int code, const std::string& body) { respond(fd, code, "application/json; charset=utf-8", body); }
void fail(int fd, int code, const std::string& msg) { json(fd, code, "{\"error\":\"" + jsonEscape(msg) + "\"}"); }

std::string mimeFor(const std::string& path) {
    static const std::map<std::string, std::string> types = {
        {"mp4", "video/mp4"},   {"m4v", "video/mp4"},   {"webm", "video/webm"}, {"mkv", "video/x-matroska"},
        {"mov", "video/quicktime"}, {"mp3", "audio/mpeg"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"},
        {"flac", "audio/flac"}, {"ogg", "audio/ogg"},   {"opus", "audio/ogg"},  {"wav", "audio/wav"},
        {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"}, {"png", "image/png"},   {"gif", "image/gif"},
        {"webp", "image/webp"}, {"bmp", "image/bmp"},   {"txt", "text/plain; charset=utf-8"},
        {"srt", "text/plain; charset=utf-8"}, {"ass", "text/plain; charset=utf-8"}, {"log", "text/plain; charset=utf-8"},
        {"ini", "text/plain; charset=utf-8"}, {"json", "application/json"}, {"pdf", "application/pdf"}, {"epub", "application/epub+zip"},
    };
    auto it = types.find(lower(fileExtension(path)));
    return it != types.end() ? it->second : "application/octet-stream";
}

// Any name a person would type works: what the drive can't store is kept in encoded form (see
// names.hpp). Only an empty name, "." / "..", a path separator or one too long are refused.
bool validName(const std::string& n) {
    if (n.empty() || n == "." || n == ".." || n.find('/') != std::string::npos || n.find('\0') != std::string::npos)
        return false;
    return toDiskName(n).size() <= 255;
}

// The temporary file an upload is written to until it's complete.
bool isUploadPart(const char* n) {
    size_t l = strlen(n);
    return !strncmp(n, ".upload-", 8) && l > 13 && !strcmp(n + l - 5, ".part");
}

std::string withSlash(std::string p) {
    if (!p.empty() && p.back() != '/') p += '/';
    return p;
}

bool isDirPath(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool removeAll(const std::string& path) {
    struct stat st;
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (stat(p.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(p.c_str()) == 0;
    bool ok = true;
    if (DIR* d = opendir(p.c_str())) {
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
        closedir(d);
        for (auto& n : names) ok = removeAll(p + "/" + n) && ok;
    }
    return rmdir(p.c_str()) == 0 && ok;
}

// "name.ext" -> "name (2).ext" until it's free.
// The on-disk name for `name` (a real name) in `dir` that isn't taken yet: "name (2).ext", ...
std::string freeDiskName(const std::string& dir, const std::string& name) {
    struct stat st;
    std::string disk = toDiskName(name);
    if (stat((dir + disk).c_str(), &st) != 0) return disk;
    std::string stem = stripExtension(name), ext = name.substr(stem.size());
    for (int i = 2; i < 1000; i++) {
        std::string n = toDiskName(stem + " (" + std::to_string(i) + ")" + ext);
        if (stat((dir + n).c_str(), &st) != 0) return n;
    }
    return disk;
}


std::string trimSlash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}
std::string baseOf(const std::string& p) {
    std::string t = trimSlash(p);
    return t.substr(t.find_last_of('/') + 1);
}
std::string driveOf(const std::string& p) { return p.substr(0, p.find(':') == std::string::npos ? 0 : p.find(':')); }

bool copyFileTo(const std::string& from, const std::string& to, std::string& err) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) {
        err = "Can't read " + fromDiskName(baseOf(from));
        return false;
    }
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) {
        fclose(in);
        err = "Can't create " + fromDiskName(baseOf(to));
        return false;
    }
    std::vector<char> buf(kIoBuffer);
    bool ok = true;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
        if (fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            err = errno == EFBIG || ftello(out) >= 0xFFF00000LL ? "Files over 4 GB can't go on a FAT32 drive"
                                                               : "Writing " + fromDiskName(baseOf(to)) + " failed";
            break;
        }
    if (ferror(in)) {
        ok = false;
        err = "Reading " + fromDiskName(baseOf(from)) + " failed";
    }
    fclose(in);
    if (fclose(out) != 0 && ok) {
        ok = false;
        err = "Writing " + fromDiskName(baseOf(to)) + " failed";
    }
    if (!ok) unlink(to.c_str());
    return ok;
}

bool copyTreeTo(const std::string& from, const std::string& to, std::string& err) {
    struct stat st;
    if (stat(from.c_str(), &st) != 0) {
        err = "Can't find " + fromDiskName(baseOf(from));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) return copyFileTo(from, to, err);
    if (mkdir(to.c_str(), 0777) != 0 && errno != EEXIST) {
        err = "Can't create folder " + fromDiskName(baseOf(to));
        return false;
    }
    DIR* d = opendir(from.c_str());
    if (!d) {
        err = "Can't open " + fromDiskName(baseOf(from));
        return false;
    }
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
    closedir(d);
    for (auto& n : names)
        if (!copyTreeTo(from + "/" + n, to + "/" + n, err)) return false;
    return true;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
        i = e + 1;
    }
    return out;
}

// ---- Streaming ZIP (stored, ZIP64 where needed): folders and selections download as one file.
struct ZipEntry {
    std::string fsPath, name;  // name inside the archive; ends with '/' for folders
    unsigned long long size = 0;
    time_t mtime = 0;
};

void zipCollect(const std::string& fsPath, const std::string& name, std::vector<ZipEntry>& out, int depth = 0) {
    struct stat st;
    if (depth > 32 || stat(fsPath.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        out.push_back({fsPath, name + "/", 0, st.st_mtime});
        DIR* d = opendir(fsPath.c_str());
        if (!d) return;
        std::vector<std::string> names;
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && !isUploadPart(e->d_name)) names.push_back(e->d_name);
        closedir(d);
        std::sort(names.begin(), names.end());
        for (auto& n : names) zipCollect(fsPath + "/" + n, name + "/" + fromDiskName(n), out, depth + 1);
    } else {
        out.push_back({fsPath, name, (unsigned long long)st.st_size, st.st_mtime});
    }
}

struct ZipOut {
    int fd = -1;            // -1: only count
    unsigned long long n = 0;
    bool ok = true;
    void put(const void* p, size_t len) {
        n += len;
        if (fd >= 0 && ok) ok = sendAll(fd, (const char*)p, len);
    }
    void u16(unsigned v) {
        unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
        put(b, 2);
    }
    void u32(unsigned long v) {
        unsigned char b[4];
        for (int i = 0; i < 4; i++) b[i] = (unsigned char)(v >> (8 * i));
        put(b, 4);
    }
    void u64(unsigned long long v) {
        unsigned char b[8];
        for (int i = 0; i < 8; i++) b[i] = (unsigned char)(v >> (8 * i));
        put(b, 8);
    }
};

// Writes (or, with out.fd < 0, measures) the whole archive. Returns false if a file couldn't be read.
bool zipEmit(const std::vector<ZipEntry>& entries, ZipOut& out) {
    struct Central {
        unsigned long crc;
        unsigned long long size, offset;
        unsigned time, date;
    };
    std::vector<Central> cd;
    std::vector<char> buf(out.fd >= 0 ? kIoBuffer : 0);
    for (auto& e : entries) {
        struct tm t;
        localtime_r(&e.mtime, &t);
        unsigned dtime = (unsigned)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
        unsigned ddate = (unsigned)(std::max(0, t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
        Central c{0, e.size, out.n, dtime, ddate};
        out.u32(0x04034b50);
        out.u16(45);
        out.u16(0x0808);  // sizes follow the data; names are UTF-8
        out.u16(0);
        out.u16(dtime);
        out.u16(ddate);
        out.u32(0);
        out.u32(0);
        out.u32(0);
        // A file of 4 GB or more says so in its local header too, or readers expect 32-bit sizes
        // after its data.
        bool big = e.size >= 0xFFFFFFFFULL;
        out.u16((unsigned)e.name.size());
        out.u16(big ? 20 : 0);
        out.put(e.name.data(), e.name.size());
        if (big) {
            out.u16(1);
            out.u16(16);
            out.u64(0);
            out.u64(0);
        }
        unsigned long crc = crc32(0L, Z_NULL, 0);
        if (e.name.back() != '/') {
            if (out.fd >= 0) {
                FILE* f = fopen(e.fsPath.c_str(), "rb");
                if (!f) return false;
                unsigned long long left = e.size;
                while (left > 0 && out.ok) {
                    size_t k = fread(buf.data(), 1, (size_t)std::min<unsigned long long>(left, buf.size()), f);
                    if (k == 0) break;
                    crc = crc32(crc, (const Bytef*)buf.data(), (uInt)k);
                    out.put(buf.data(), k);
                    left -= k;
                }
                fclose(f);
                if (left) return false;  // shorter than it said: the archive would be broken
            } else {
                out.n += e.size;
            }
        }
        c.crc = crc;
        out.u32(0x08074b50);
        out.u32(crc);
        if (big) {
            out.u64(e.size);
            out.u64(e.size);
        } else {
            out.u32((unsigned long)e.size);
            out.u32((unsigned long)e.size);
        }
        cd.push_back(c);
        if (!out.ok) return false;
    }
    unsigned long long cdStart = out.n;
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        const auto& c = cd[i];
        bool bigSize = c.size >= 0xFFFFFFFFULL, bigOff = c.offset >= 0xFFFFFFFFULL;
        unsigned extra = (bigSize || bigOff) ? 4 + (bigSize ? 16 : 0) + (bigOff ? 8 : 0) : 0;
        out.u32(0x02014b50);
        out.u16(45 | (3 << 8));
        out.u16(45);
        out.u16(0x0808);
        out.u16(0);
        out.u16(c.time);
        out.u16(c.date);
        out.u32(c.crc);
        out.u32(bigSize ? 0xFFFFFFFFUL : (unsigned long)c.size);
        out.u32(bigSize ? 0xFFFFFFFFUL : (unsigned long)c.size);
        out.u16((unsigned)e.name.size());
        out.u16(extra);
        out.u16(0);
        out.u16(0);
        out.u16(0);
        out.u32(e.name.back() == '/' ? (0040755UL << 16) | 0x10 : (0100644UL << 16));
        out.u32(bigOff ? 0xFFFFFFFFUL : (unsigned long)c.offset);
        out.put(e.name.data(), e.name.size());
        if (extra) {
            out.u16(1);
            out.u16(extra - 4);
            if (bigSize) {
                out.u64(c.size);
                out.u64(c.size);
            }
            if (bigOff) out.u64(c.offset);
        }
    }
    unsigned long long cdSize = out.n - cdStart;
    bool z64 = entries.size() >= 0xFFFF || cdStart >= 0xFFFFFFFFULL || cdSize >= 0xFFFFFFFFULL;
    if (z64) {
        unsigned long long z64At = out.n;
        out.u32(0x06064b50);
        out.u64(44);
        out.u16(45);
        out.u16(45);
        out.u32(0);
        out.u32(0);
        out.u64(entries.size());
        out.u64(entries.size());
        out.u64(cdSize);
        out.u64(cdStart);
        out.u32(0x07064b50);
        out.u32(0);
        out.u64(z64At);
        out.u32(1);
    }
    out.u32(0x06054b50);
    out.u16(0);
    out.u16(0);
    out.u16(z64 ? 0xFFFF : (unsigned)entries.size());
    out.u16(z64 ? 0xFFFF : (unsigned)entries.size());
    out.u32(z64 ? 0xFFFFFFFFUL : (unsigned long)cdSize);
    out.u32(z64 ? 0xFFFFFFFFUL : (unsigned long)cdStart);
    out.u16(0);
    return out.ok;
}

// Writes uploads to the card on its own thread, so receiving from the network never waits for the
// card (and the reverse). Two 1 MB buffers swap between the two sides. If the thread can't start,
// it writes inline.
class PipeWriter {
public:
    explicit PipeWriter(FILE* f) : f_(f) {
        for (auto& b : bufs_) b.resize(1 << 20);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        threaded_ = pthread_create(&t_, &attr, main, this) == 0;
        pthread_attr_destroy(&attr);
    }
    ~PipeWriter() { finish(); }
    char* buffer() { return bufs_[cur_].data(); }
    size_t capacity() const { return bufs_[0].size(); }
    // Hands over the current buffer holding n bytes; returns false once writing has failed.
    bool submit(size_t n) {
        if (!threaded_) return write(bufs_[cur_].data(), n);
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return pending_ < 0 || failed_; });
        if (failed_) return false;
        pending_ = cur_;
        pendingLen_ = n;
        cur_ ^= 1;
        cv_.notify_all();
        return true;
    }
    bool finish() {
        if (threaded_) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return pending_ < 0 || failed_; });
                done_ = true;
            }
            cv_.notify_all();
            pthread_join(t_, nullptr);
            threaded_ = false;
        }
        return !failed_;
    }
    int error() const { return errno_; }
    bool tooBig() const { return tooBig_; }

private:
    bool write(const char* p, size_t n) {
        if (fwrite(p, 1, n, f_) == n) return true;
        errno_ = errno;
        tooBig_ = errno == EFBIG || ftello(f_) >= 0xFFF00000LL;
        failed_ = true;
        return false;
    }
    static void* main(void* self) {
        auto* w = (PipeWriter*)self;
        std::unique_lock<std::mutex> lk(w->m_);
        for (;;) {
            w->cv_.wait(lk, [&] { return w->pending_ >= 0 || w->done_; });
            if (w->pending_ < 0) return nullptr;  // done, nothing left
            int i = w->pending_;
            size_t n = w->pendingLen_;
            lk.unlock();
            bool ok = w->write(w->bufs_[i].data(), n);
            lk.lock();
            w->pending_ = -1;
            w->cv_.notify_all();
            if (!ok) return nullptr;
        }
    }
    FILE* f_;
    std::vector<char> bufs_[2];
    int cur_ = 0, pending_ = -1;
    size_t pendingLen_ = 0;
    bool threaded_ = false, done_ = false;
    std::atomic<bool> failed_{false};
    bool tooBig_ = false;
    int errno_ = 0;
    pthread_t t_{};
    std::mutex m_;
    std::condition_variable cv_;
};

}  // namespace

struct WebRequest {
    std::string method, path;
    std::map<std::string, std::string> query, headers;
    std::string body;  // bytes after the headers that arrived with them
};

namespace {

bool readRequest(int fd, WebRequest& r) {
    std::string buf;
    char tmp[4096];
    size_t end;
    for (;;) {
        end = buf.find("\r\n\r\n");
        if (end != std::string::npos) break;
        if (buf.size() > 32 * 1024) return false;
        ssize_t k = recv(fd, tmp, sizeof tmp, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        buf.append(tmp, (size_t)k);
    }
    r.body = buf.substr(end + 4);
    std::string head = buf.substr(0, end);
    size_t lineEnd = head.find("\r\n");
    std::string line = head.substr(0, lineEnd);
    size_t a = line.find(' '), b = line.rfind(' ');
    if (a == std::string::npos || b == a) return false;
    r.method = line.substr(0, a);
    std::string target = line.substr(a + 1, b - a - 1);
    size_t q = target.find('?');
    r.path = urlDecode(target.substr(0, q));
    if (q != std::string::npos) {
        std::string qs = target.substr(q + 1);
        size_t i = 0;
        while (i <= qs.size()) {
            size_t amp = qs.find('&', i);
            if (amp == std::string::npos) amp = qs.size();
            std::string kv = qs.substr(i, amp - i);
            size_t eq = kv.find('=');
            if (!kv.empty()) r.query[urlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : urlDecode(kv.substr(eq + 1));
            i = amp + 1;
        }
    }
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        std::string h = head.substr(pos, e - pos);
        size_t c = h.find(':');
        if (c != std::string::npos) {
            std::string v = h.substr(c + 1);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            r.headers[lower(h.substr(0, c))] = v;
        }
        pos = e + 2;
    }
    return true;
}

// The rest of a small body (a list of paths), after what arrived with the headers.
bool readBody(int fd, WebRequest& r, size_t max = 1 << 20) {
    auto it = r.headers.find("content-length");
    size_t want = it == r.headers.end() ? 0 : (size_t)atoll(it->second.c_str());
    if (want > max) return false;
    char tmp[4096];
    while (r.body.size() < want) {
        ssize_t k = recv(fd, tmp, std::min(sizeof tmp, want - r.body.size()), 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        r.body.append(tmp, (size_t)k);
    }
    r.body.resize(want);
    return true;
}

std::string param(const WebRequest& r, const char* k) {
    auto it = r.query.find(k);
    return it != r.query.end() ? it->second : "";
}

}  // namespace

namespace {
bool spawn(pthread_t& t, void* (*fn)(void*), void* arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    bool ok = pthread_create(&t, &attr, fn, arg) == 0;
    pthread_attr_destroy(&attr);
    return ok;
}
}  // namespace

// Threads are detached and carry the generation they belong to: stopping never waits more than a
// moment for them (a connection stuck on a vanished network can't freeze the app), and a quick
// restart can't be confused by threads of the previous run still on their way out.
struct WebServer::ThreadArg {
    WebServer* self;
    int gen, listenFd;
};

void* WebServer::acceptMain(void* p) {
    auto* a = (ThreadArg*)p;
    a->self->acceptLoop(a->gen, a->listenFd);
    a->self->alive_--;
    delete a;
    return nullptr;
}

void* WebServer::workerMain(void* p) {
    auto* a = (ThreadArg*)p;
    a->self->workerLoop(a->gen);
    a->self->alive_--;
    delete a;
    return nullptr;
}

bool WebServer::start(int port) {
    if (running_) return true;
#ifndef __SWITCH__
    signal(SIGPIPE, SIG_IGN);
#endif
    auto failWith = [&](const char* msg) {
        std::lock_guard<std::mutex> lk(m_);
        error_ = msg;
        return false;
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return failWith("Couldn't open a network socket. Is Wi-Fi on?");
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int bound = -1;
    for (int p = port; p < port + 10; p++) {  // the usual port may be taken by another app
        addr.sin_port = htons((uint16_t)p);
        if (bind(fd, (sockaddr*)&addr, sizeof addr) == 0) {
            bound = p;
            break;
        }
    }
    if (bound < 0 || listen(fd, 8) != 0) {
        close(fd);
        return failWith("Couldn't listen on the network");
    }
    listenFd_ = fd;
    port_ = bound;
    std::mt19937 rng((unsigned)std::chrono::steady_clock::now().time_since_epoch().count() ^ (unsigned)time(nullptr));
    pin_ = std::to_string(1000 + rng() % 9000);
#ifndef __SWITCH__
    if (const char* p = getenv("SWITCHMX_WEBPIN")) pin_ = p;  // dev aid for scripted tests
#endif
    quit_ = false;
    badPins_ = 0;
    lockedUntil_ = 0;
    int g = ++gen_;
    auto launch = [&](void* (*fn)(void*)) {
        auto* arg = new ThreadArg{this, g, fd};
        pthread_t t;
        alive_++;
        if (!spawn(t, fn, arg)) {
            alive_--;
            delete arg;
            return false;
        }
        pthread_detach(t);
        return true;
    };
    int workers = 0;
    for (int i = 0; i < kWorkers; i++) workers += launch(workerMain);
    if (!workers || !launch(acceptMain)) {
        quit_ = true;
        qcv_.notify_all();
        close(listenFd_);
        listenFd_ = -1;
        return failWith("The console couldn't start the transfer service. Close other apps and try again.");
    }
    running_ = true;
    return true;
}

void WebServer::stop() {
    if (!running_) return;
    quit_ = true;
    gen_++;
    {
        // Wake the workers, and cut off connections still being served so none waits out its timeout.
        std::lock_guard<std::mutex> lk(qm_);
        for (int fd : queue_) close(fd);
        queue_.clear();
        for (int fd : active_) shutdown(fd, SHUT_RDWR);
    }
    qcv_.notify_all();
    // Give them a moment to finish; any still busy (a dead connection) end on their own later.
    for (int i = 0; i < 100 && alive_ > 0; i++) usleep(20000);
    if (listenFd_ >= 0) close(listenFd_);
    listenFd_ = -1;
    running_ = false;
}

std::vector<std::pair<std::string, std::string>> WebServer::takeMoves() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(moves_);
    return out;
}

std::string WebServer::error() const {
    std::lock_guard<std::mutex> lk(m_);
    return error_;
}

void WebServer::setRoots(const std::vector<Root>& roots) {
    std::lock_guard<std::mutex> lk(m_);
    roots_ = roots;
}

std::vector<WebServer::Root> WebServer::roots() {
    std::lock_guard<std::mutex> lk(m_);
    return roots_;
}

bool WebServer::allowed(const std::string& path) {
    if (path.find("/../") != std::string::npos || path.find('\\') != std::string::npos) return false;
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0) return false;
    for (auto& r : roots())
        if (path.rfind(r.path, 0) == 0) return true;
    return false;
}

void WebServer::acceptLoop(int g, int listenFd) {
    while (!quit_ && gen_ == g) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(listenFd, &set);
        timeval tv{0, 250000};
        int ready = select(listenFd + 1, &set, nullptr, nullptr, &tv);
        if (ready < 0) usleep(200000);  // the network went away: don't spin
        if (ready <= 0) continue;
        int c = accept(listenFd, nullptr, nullptr);
        if (c < 0) continue;
        // A browser that goes quiet (Wi-Fi gone) frees its worker after 15 s.
        timeval to{15, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
        int rcv = 1 << 20, snd = 512 << 10;  // big windows: uploads keep flowing while the card writes
        setsockopt(c, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        bool queued = false;
        {
            std::lock_guard<std::mutex> lk(qm_);
            if (queue_.size() < 16) {
                queue_.push_back(c);
                queued = true;
            }
        }
        if (queued) {
            qcv_.notify_one();
        } else {
            fail(c, 503, "Busy, try again in a moment");
            close(c);
        }
    }
}

void WebServer::workerLoop(int g) {
    for (;;) {
        int c;
        {
            std::unique_lock<std::mutex> lk(qm_);
            qcv_.wait(lk, [&] { return quit_ || gen_ != g || !queue_.empty(); });
            if (quit_ || gen_ != g) return;
            c = queue_.front();
            queue_.pop_front();
            active_.insert(c);
        }
        // Nothing a browser sends may bring the app down.
        try {
            handle(c);
        } catch (const std::bad_alloc&) {
            // Said plainly instead of a dropped connection (seen while a film filled the memory).
            try {
                fail(c, 503, "The Switch is short of memory right now. Close the player and try again");
            } catch (...) {
            }
        } catch (...) {
        }
        {
            std::lock_guard<std::mutex> lk(qm_);
            active_.erase(c);
        }
        close(c);
    }
}

// Developer mode: the token from Settings → Debug, the same lockout as wrong PINs.
void WebServer::handleDev(int fd, WebRequest& r) {
    std::string token;
    DevHandler handler;
    {
        std::lock_guard<std::mutex> lk(m_);
        token = devToken_;
        handler = dev_;
    }
    if (token.empty() || !handler) {
        fail(fd, 404, "Developer mode is off (Settings → Debug)");
        return;
    }
    long long now = (long long)time(nullptr);
    if (now < lockedUntil_) {
        fail(fd, 429, "Too many wrong tokens. Wait a minute");
        return;
    }
    std::string given = r.headers.count("x-dev-token") ? r.headers["x-dev-token"] : param(r, "token");
    unsigned diff = given.size() != token.size();
    for (size_t i = 0; i < given.size() && i < token.size(); i++) diff |= (unsigned char)(given[i] ^ token[i]);
    if (diff) {
        if (++badPins_ >= 8) {
            badPins_ = 0;
            lockedUntil_ = now + 60;
        }
        fail(fd, 401, "Wrong developer token");
        return;
    }
    badPins_ = 0;
    if (r.method == "POST" && !readBody(fd, r)) {
        fail(fd, 400, "Bad request");
        return;
    }
    int code = 200;
    std::string type = "application/json; charset=utf-8", out;
    try {
        if (!handler(r.path.substr(5), r.query, r.body, code, type, out)) {
            fail(fd, 404, "Unknown developer request");
            return;
        }
    } catch (const std::exception& e) {
        fail(fd, 500, e.what());
        return;
    }
    respond(fd, code, type, out);
}

void WebServer::handle(int fd) {
    WebRequest r;
    if (!readRequest(fd, r)) return;
    if (r.path == "/" || r.path == "/index.html") {
        respond(fd, 200, "text/html; charset=utf-8", kWebUi);
        return;
    }
    if (r.path.rfind("/dev/", 0) == 0) {
        handleDev(fd, r);
        return;
    }
    if (r.path.rfind("/api/", 0) != 0) {
        fail(fd, 404, "Not found");
        return;
    }
    // Everything under /api needs the PIN shown on the console.
    // After 8 wrong PINs in a row, nothing is accepted for a minute: four digits would otherwise
    // be quick to guess by trying them all.
    long long now = (long long)time(nullptr);
    if (now < lockedUntil_) {
        fail(fd, 429, "Too many wrong PINs. Wait a minute, then try again");
        return;
    }
    std::string given = r.headers.count("x-pin") ? r.headers["x-pin"] : param(r, "pin");
    if (given != pin_) {
        if (++badPins_ >= 8) {
            badPins_ = 0;
            lockedUntil_ = now + 60;
            log("wifi transfer: 8 wrong PINs, locked for a minute");
        }
        fail(fd, 401, "Wrong PIN");
        return;
    }
    badPins_ = 0;
    std::string api = r.path.substr(5);
    std::string path = param(r, "path");

    if (api == "check") {
        json(fd, 200, "{\"ok\":true}");
        return;
    }
    if (api == "roots") {
        std::string out = "[";
        bool first = true;
        for (auto& root : roots()) {
            struct statvfs v;
            unsigned long long freeB = 0, total = 0;
            if (statvfs(root.path.c_str(), &v) == 0) {
                unsigned long long bs = v.f_frsize ? v.f_frsize : v.f_bsize;
                freeB = (unsigned long long)v.f_bavail * bs;
                total = (unsigned long long)v.f_blocks * bs;
            }
            out += std::string(first ? "" : ",") + "{\"path\":\"" + jsonEscape(root.path) + "\",\"name\":\"" +
                   jsonEscape(root.name) + "\",\"free\":" + std::to_string(freeB) + ",\"total\":" + std::to_string(total) + "}";
            first = false;
        }
        json(fd, 200, out + "]");
        return;
    }
    if (api != "zip" && !allowed(path)) {  // a zip checks each of its paths itself
        fail(fd, 403, "That location isn't available");
        return;
    }

    if (api == "zip" && r.method == "GET") {
        // ?paths=<one per line>&name=<archive name>; every path must be reachable.
        std::vector<ZipEntry> entries;
        for (auto& p : splitLines(param(r, "paths"))) {
            if (!allowed(p)) {
                fail(fd, 403, "That location isn't available");
                return;
            }
            zipCollect(trimSlash(p), fromDiskName(baseOf(p)), entries);
        }
        if (entries.empty()) {
            fail(fd, 404, "Nothing to download");
            return;
        }
        std::string name = param(r, "name");
        if (!validName(name)) name = "SwitchMX.zip";
        ZipOut measure;
        zipEmit(entries, measure);
        std::string h = "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: " +
                        std::to_string(measure.n) + "\r\nConnection: close\r\nContent-Disposition: attachment; filename*=UTF-8''" +
                        urlEncode(name) + "\r\n\r\n";
        if (!sendAll(fd, h)) return;
        transfers_++;
        ZipOut out;
        out.fd = fd;
        zipEmit(entries, out);
        transfers_--;
        return;
    }

    if (api == "exists" && r.method == "GET") {
        // Whether `name` ("Folder/Sub/file.ext" for a folder upload) is already in `path`, so the
        // browser can skip it without sending it.
        std::string p = withSlash(path), name = param(r, "name");
        size_t i = 0;
        while (i <= name.size()) {
            size_t e = name.find('/', i);
            if (e == std::string::npos) e = name.size();
            p += toDiskName(name.substr(i, e - i)) + (e < name.size() ? "/" : "");
            i = e + 1;
        }
        json(fd, 200, std::string("{\"exists\":") + (access(p.c_str(), F_OK) == 0 ? "true" : "false") + "}");
        return;
    }

    if (api == "du" && r.method == "GET") {
        // Size of a folder (or file) and what's in it; stops after 10 seconds.
        auto t0 = std::chrono::steady_clock::now();
        unsigned long long bytes = 0, files = 0, folders = 0;
        bool partial = false;
        std::vector<std::string> stack = {trimSlash(path)};
        while (!stack.empty()) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) {
                partial = true;
                break;
            }
            std::string p = stack.back();
            stack.pop_back();
            struct stat st;
            if (stat(p.c_str(), &st) != 0) continue;
            if (!S_ISDIR(st.st_mode)) {
                bytes += st.st_size;
                files++;
                continue;
            }
            if (p != trimSlash(path)) folders++;
            if (DIR* d = opendir(p.c_str())) {
                while (dirent* e = readdir(d))
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) stack.push_back(p + "/" + e->d_name);
                closedir(d);
            }
        }
        json(fd, 200, "{\"bytes\":" + std::to_string(bytes) + ",\"files\":" + std::to_string(files) +
                          ",\"folders\":" + std::to_string(folders) + ",\"partial\":" + (partial ? "true" : "false") + "}");
        return;
    }

    if (api == "cleanup" && r.method == "POST") {
        // The files a Mac leaves on other drives: "._name" copies of extended attributes and
        // .DS_Store, everywhere under `path`. Nothing else is touched.
        unsigned long long bytes = 0, count = 0;
        std::vector<std::string> stack = {trimSlash(path)};
        while (!stack.empty()) {
            std::string p = stack.back();
            stack.pop_back();
            DIR* d = opendir(p.c_str());
            if (!d) continue;
            std::vector<std::string> names;
            while (dirent* e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
            closedir(d);
            for (auto& n : names) {
                std::string full = p + "/" + n;
                struct stat st;
                if (stat(full.c_str(), &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    if (n != ".Spotlight-V100" && n != ".Trashes" && n != ".fseventsd") stack.push_back(full);
                    else if (removeAll(full)) count++;
                    continue;
                }
                bool junk = (n.size() > 2 && n[0] == '.' && n[1] == '_') || n == ".DS_Store";
                if (junk && unlink(full.c_str()) == 0) {
                    bytes += st.st_size;
                    count++;
                }
            }
        }
        if (count) changed_ = true;
        json(fd, 200, "{\"removed\":" + std::to_string(count) + ",\"bytes\":" + std::to_string(bytes) + "}");
        return;
    }

    if (api == "search" && r.method == "GET") {
        // Names containing the words, anywhere under `path`; stops after 400 matches or 6 seconds.
        std::string q = lower(param(r, "q"));
        if (q.empty()) {
            json(fd, 200, "{\"results\":[]}");
            return;
        }
        auto t0 = std::chrono::steady_clock::now();
        std::string out = "{\"results\":[";
        int found = 0;
        bool truncated = false;
        std::vector<std::pair<std::string, int>> stack = {{withSlash(path), 0}};
        while (!stack.empty() && found < 400) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(6)) {
                truncated = true;
                break;
            }
            auto [dir, depth] = stack.back();
            stack.pop_back();
            DIR* d = opendir(dir.c_str());
            if (!d) continue;
            while (dirent* e = readdir(d)) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || isUploadPart(e->d_name)) continue;
                std::string full = dir + e->d_name;
                struct stat st;
                bool isDir = stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
                if (isDir && depth < 12 && e->d_name[0] != '.') stack.push_back({full + "/", depth + 1});
                std::string shown = fromDiskName(e->d_name);  // what the name really is
                if (lower(shown).find(q) == std::string::npos) continue;
                out += std::string(found ? "," : "") + "{\"path\":\"" + jsonEscape(isDir ? full + "/" : full) +
                       "\",\"name\":\"" + jsonEscape(shown) + "\",\"where\":\"" + jsonEscape(fromDiskPath(dir)) + "\",\"dir\":" + (isDir ? "true" : "false") +
                       ",\"size\":" + std::to_string(isDir ? 0 : (unsigned long long)st.st_size) +
                       ",\"mtime\":" + std::to_string((long long)st.st_mtime) + "}";
                if (++found >= 400) {
                    truncated = true;
                    break;
                }
            }
            closedir(d);
        }
        json(fd, 200, out + "],\"truncated\":" + (truncated ? "true" : "false") + "}");
        return;
    }

    if ((api == "move" || api == "copy") && r.method == "POST") {
        // Body: one path per line. `path` is the destination folder.
        if (!readBody(fd, r)) {
            fail(fd, 400, "Bad request");
            return;
        }
        std::string dest = withSlash(path);
        if (!isDirPath(dest)) {
            fail(fd, 404, "The destination folder doesn't exist");
            return;
        }
        bool move = api == "move";
        int done = 0;
        std::string err;
        transfers_++;
        for (auto& src : splitLines(r.body)) {
            std::string from = trimSlash(src);
            if (!allowed(from) || isProtectedPath(from)) {
                err = "\"" + fromDiskName(baseOf(from)) + "\" can't be changed from here";
                break;
            }
            bool rootItem = false;
            for (auto& root : roots()) rootItem |= withSlash(from) == root.path;
            if (rootItem) {
                err = "A whole drive can't be moved";
                break;
            }
            if (isDirPath(from) && withSlash(dest).rfind(withSlash(from), 0) == 0) {
                err = "A folder can't go inside itself";
                break;
            }
            std::string name = baseOf(from);
            std::string to = dest + name;
            if (trimSlash(to) == from) {  // same place: a copy gets a new name, a move does nothing
                if (!move) to = dest + freeDiskName(dest, fromDiskName(name));
                else {
                    done++;
                    continue;
                }
            } else if (access(to.c_str(), F_OK) == 0) {
                to = dest + freeDiskName(dest, fromDiskName(name));
            }
            bool ok;
            if (move && driveOf(from) == driveOf(dest)) {
                ok = rename(from.c_str(), to.c_str()) == 0;
                if (!ok) err = "Couldn't move " + fromDiskName(name) + ": " + strerror(errno);
            } else {
                ok = copyTreeTo(from, to, err);
                if (ok && move && !removeAll(from)) err = "Copied, but couldn't remove the original " + fromDiskName(name);
            }
            if (ok && move) noteMove(from, trimSlash(to));
            if (!ok) break;
            done++;
        }
        transfers_--;
        if (done) changed_ = true;
        if (!err.empty()) {
            fail(fd, 500, err);
            return;
        }
        json(fd, 200, "{\"ok\":true,\"done\":" + std::to_string(done) + "}");
        return;
    }

    if (api == "list" && r.method == "GET") {
        std::string dir = withSlash(path);
        DIR* d = opendir(dir.c_str());
        if (!d) {
            fail(fd, 404, "Can't open this folder");
            return;
        }
        // Names as they really are; each entry's own path is the one on disk, for every request.
        std::string out = "{\"path\":\"" + jsonEscape(dir) + "\",\"shown\":\"" + jsonEscape(fromDiskPath(dir)) +
                          "\",\"entries\":[";
        bool first = true;
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            struct stat st;
            std::string full = dir + e->d_name;
            if (isUploadPart(e->d_name)) {  // an upload still arriving, or one cut off long ago
                if (stat(full.c_str(), &st) == 0 && time(nullptr) - st.st_mtime > 6 * 3600) unlink(full.c_str());
                continue;
            }
            bool isDir = false;
            unsigned long long size = 0;
            long long mtime = 0;
            if (stat(full.c_str(), &st) == 0) {
                isDir = S_ISDIR(st.st_mode);
                size = isDir ? 0 : (unsigned long long)st.st_size;
                mtime = (long long)st.st_mtime;
            }
            out += std::string(first ? "" : ",") + "{\"name\":\"" + jsonEscape(fromDiskName(e->d_name)) +
                   "\",\"path\":\"" + jsonEscape(isDir ? full + "/" : full) + "\",\"dir\":" +
                   (isDir ? "true" : "false") + ",\"size\":" + std::to_string(size) + ",\"mtime\":" + std::to_string(mtime) + "}";
            first = false;
        }
        closedir(d);
        json(fd, 200, out + "]}");
        return;
    }

    if (api == "file" && r.method == "GET") {
        // The size comes from the path: USB drives' file handles can't report it.
        struct stat st;
        FILE* f = stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode) ? fopen(path.c_str(), "rb") : nullptr;
        if (!f) {
            if (f) fclose(f);
            fail(fd, 404, "File not found");
            return;
        }
        long long size = st.st_size, from = 0, to = size - 1;
        bool partial = false;
        auto rh = r.headers.find("range");
        if (rh != r.headers.end() && rh->second.rfind("bytes=", 0) == 0) {
            std::string spec = rh->second.substr(6);
            size_t dash = spec.find('-');
            if (dash != std::string::npos) {
                std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
                if (a.empty()) from = std::max(0LL, size - atoll(b.c_str()));
                else {
                    from = atoll(a.c_str());
                    if (!b.empty()) to = std::min(size - 1, atoll(b.c_str()));
                }
                partial = true;
            }
            if (from > to || from >= size) {
                fclose(f);
                respond(fd, 416, "text/plain", "", "Content-Range: bytes */" + std::to_string(size) + "\r\n");
                return;
            }
        }
        long long len = size ? to - from + 1 : 0;
        std::string name = fromDiskName(path.substr(path.find_last_of('/') + 1));
        std::string h = "HTTP/1.1 " + std::string(partial ? "206 Partial Content" : "200 OK") +
                        "\r\nContent-Type: " + mimeFor(path) + "\r\nContent-Length: " + std::to_string(len) +
                        "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n";
        if (partial)
            h += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size) + "\r\n";
        h += std::string("Content-Disposition: ") + (param(r, "dl") == "1" ? "attachment" : "inline") +
             "; filename*=UTF-8''" + urlEncode(name) + "\r\n\r\n";
        if (!sendAll(fd, h) || r.method == "HEAD") {
            fclose(f);
            return;
        }
        transfers_++;
        fseeko(f, from, SEEK_SET);
        std::vector<char> buf(kIoBuffer);
        while (len > 0) {
            size_t want = (size_t)std::min<long long>(len, (long long)buf.size());
            size_t got = fread(buf.data(), 1, want, f);
            if (got == 0 || !sendAll(fd, buf.data(), got)) break;
            len -= (long long)got;
        }
        transfers_--;
        fclose(f);
        return;
    }

    if (api == "upload" && (r.method == "PUT" || r.method == "POST")) {
        std::string dir = withSlash(path), name = param(r, "name");
        // A folder upload sends "Folder/Sub/file.ext": make the folders on the way.
        {
            std::vector<std::string> parts;
            size_t i = 0;
            while (i <= name.size()) {
                size_t e = name.find('/', i);
                if (e == std::string::npos) e = name.size();
                parts.push_back(name.substr(i, e - i));
                i = e + 1;
            }
            for (auto& part : parts)
                if (!validName(part)) {
                    fail(fd, 400, "\"" + part + "\" can't be used as a name");
                    return;
                }
            if (isDirPath(dir))
                for (size_t k = 0; k + 1 < parts.size(); k++) {
                    dir += toDiskName(parts[k]) + "/";
                    mkdir(dir.c_str(), 0777);
                }
            name = parts.back();  // the real name; the disk name is worked out below
        }
        if (!isDirPath(dir)) {
            fail(fd, 404, "The folder doesn't exist");
            return;
        }
        auto cl = r.headers.find("content-length");
        if (cl == r.headers.end()) {
            fail(fd, 400, "Missing file size");
            return;
        }
        long long total = atoll(cl->second.c_str());
        if (total < 0) {
            fail(fd, 400, "Bad file size");
            return;
        }
        struct statvfs v;
        if (statvfs(dir.c_str(), &v) == 0 && v.f_blocks) {
            unsigned long long freeB = (unsigned long long)v.f_bavail * (v.f_frsize ? v.f_frsize : v.f_bsize);
            if (freeB && (unsigned long long)total > freeB) {
                fail(fd, 507, "Not enough free space on this drive");
                return;
            }
        }
        // Replacing what the console or its homebrew needs to boot isn't done from a browser; adding
        // new files there (e.g. a homebrew's own folder) is.
        bool ownFolder = lower(dir).rfind("sdmc:/switch/switchmx/", 0) == 0;  // updating the app itself is fine
        // An existing file of that name: keep both (default), replace it, or leave it and skip.
        std::string exists = param(r, "exists");
        if (param(r, "overwrite") == "1") exists = "replace";
        bool replace = exists == "replace";
        if (replace && isDirPath(dir + toDiskName(name))) {
            fail(fd, 409, "A folder called \"" + name + "\" is already here");
            return;
        }
        if (replace && !ownFolder && isProtectedPath(dir + toDiskName(name)) &&
            access((dir + toDiskName(name)).c_str(), F_OK) == 0) {
            fail(fd, 403, "\"" + name + "\" is needed by the console or its homebrew, so it can only be replaced on the Switch");
            return;
        }
        // Stored under the name itself when the drive can hold it, else its encoded form.
        std::string finalName = replace ? toDiskName(name) : freeDiskName(dir, name);
        std::string part = dir + ".upload-" + std::to_string((unsigned long)time(nullptr)) + "-" +
                           std::to_string((unsigned long)(uintptr_t)&r % 100000) + ".part";
        FILE* f = fopen(part.c_str(), "wb");
        if (!f) {
            int e = errno;
            log("web upload failed for \"" + name + "\" (" + finalName + "): " + strerror(e));
            fail(fd, 500, std::string("The Switch couldn't create \"") + name + "\": " + strerror(e));
            return;
        }
        transfers_++;
        setvbuf(f, nullptr, _IONBF, 0);
        long long left = total;
        bool ok = true;
        std::string err;
        if (!r.body.empty()) {
            size_t n = (size_t)std::min<long long>(left, (long long)r.body.size());
            if (fwrite(r.body.data(), 1, n, f) != n) ok = false;
            left -= (long long)n;
        }
        {
            PipeWriter w(f);
            size_t fill = 0;
            char* cur = w.buffer();
            while (ok && left > 0) {
                ssize_t k = recv(fd, cur + fill, (size_t)std::min<long long>(left, (long long)(w.capacity() - fill)), 0);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) {
                    ok = false;
                    err = "The upload was interrupted (the connection dropped)";
                    break;
                }
                fill += (size_t)k;
                left -= k;
                if (fill == w.capacity() || left == 0) {  // big blocks: the card is much faster that way
                    if (!w.submit(fill)) ok = false;
                    fill = 0;
                    cur = w.buffer();
                }
            }
            if (!w.finish() && err.empty()) ok = false;
            if (!ok && err.empty())
                err = w.tooBig() ? "Files over 4 GB can't be stored on a FAT32 drive"
                                 : std::string("Writing failed: ") + strerror(w.error());
        }
        if (fclose(f) != 0 && ok) {
            ok = false;
            err = "Writing failed";
        }
        transfers_--;
        if (!ok) {
            unlink(part.c_str());
            fail(fd, 500, err.empty() ? "Writing failed" : err);
            return;
        }
        std::string target = dir + finalName;
        unlink(target.c_str());  // only exists when overwriting; the Switch won't rename over a file
        if (rename(part.c_str(), target.c_str()) != 0) {
            unlink(part.c_str());
            fail(fd, 500, "Couldn't finish saving the file");
            return;
        }
        changed_ = true;
        json(fd, 200, "{\"ok\":true,\"name\":\"" + jsonEscape(fromDiskName(finalName)) + "\"}");
        return;
    }

    if (r.method != "POST") {
        fail(fd, 400, "Unsupported request");
        return;
    }
    if (api == "mkdir") {
        std::string name = param(r, "name");
        if (!validName(name)) {
            fail(fd, 400, "That folder name can't be used");
            return;
        }
        std::string made = withSlash(path) + toDiskName(name);
        if (mkdir(made.c_str(), 0777) != 0) {
            fail(fd, errno == EEXIST ? 409 : 500, errno == EEXIST ? "That name is already used" : strerror(errno));
            return;
        }
        changed_ = true;
        json(fd, 200, "{\"ok\":true,\"path\":\"" + jsonEscape(made + "/") + "\"}");
        return;
    }
    // Deleting or renaming a whole drive, or the console's system folders, isn't offered here.
    for (auto& root : roots())
        if (withSlash(path) == root.path) {
            fail(fd, 403, "A whole drive can't be changed from here");
            return;
        }
    if (isProtectedPath(path)) {
        fail(fd, 403, "This is needed by the console or its homebrew, so it can only be changed on the Switch");
        return;
    }
    if (api == "delete") {
        noteMove(trimSlash(path), "");
        if (!removeAll(path)) {
            fail(fd, 500, "Couldn't delete everything");
            changed_ = true;
            return;
        }
        changed_ = true;
        json(fd, 200, "{\"ok\":true}");
        return;
    }
    if (api == "rename") {
        std::string name = param(r, "name");
        if (!validName(name)) {
            fail(fd, 400, "That name can't be used");
            return;
        }
        std::string p = path;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        std::string target = p.substr(0, p.find_last_of('/') + 1) + toDiskName(name);
        struct stat st;
        bool caseOnly = lower(target) == lower(p);
        if (!caseOnly && stat(target.c_str(), &st) == 0) {
            fail(fd, 409, "That name is already used here");
            return;
        }
        int rc;
        if (caseOnly) {  // FAT and exFAT ignore case: go through a temporary name
            std::string tmp = p + ".renaming";
            rc = rename(p.c_str(), tmp.c_str());
            if (rc == 0 && (rc = rename(tmp.c_str(), target.c_str())) != 0) rename(tmp.c_str(), p.c_str());
        } else {
            rc = rename(p.c_str(), target.c_str());
        }
        if (rc != 0) {
            fail(fd, 500, std::string("Couldn't rename: ") + strerror(errno));
            return;
        }
        noteMove(p, target);
        changed_ = true;
        json(fd, 200, "{\"ok\":true}");
        return;
    }
    fail(fd, 404, "Unknown request");
}
