#include "names.hpp"

#include <cctype>
#include <cstdint>
#include <string>

namespace {
const char* kPrefix = "xn--";

// Characters FAT drives refuse travel as private-use code points, so they come back exactly.
bool forbidden(uint32_t c) {
    return c < 0x20 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
           c == '>' || c == '|';
}
constexpr uint32_t kEscape = 0xE000;

std::u32string decodeUtf8(const std::string& s, bool& ok) {
    std::u32string out;
    ok = true;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > s.size()) {
            ok = false;
            return out;
        }
        uint32_t cp = len == 1 ? c : c & (0xFF >> (len + 1));
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out += cp;
        i += len;
    }
    return out;
}

std::string encodeUtf8(const std::u32string& s) {
    std::string out;
    for (uint32_t c : s) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 63));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 63));
            out += (char)(0x80 | (c & 63));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 63));
            out += (char)(0x80 | ((c >> 6) & 63));
            out += (char)(0x80 | (c & 63));
        }
    }
    return out;
}

// ---- Punycode (RFC 3492)
constexpr uint32_t kBase = 36, kTMin = 1, kTMax = 26, kSkew = 38, kDamp = 700, kBias = 72, kInitialN = 128;

uint32_t adapt(uint32_t delta, uint32_t points, bool first) {
    delta = first ? delta / kDamp : delta / 2;
    delta += delta / points;
    uint32_t k = 0;
    while (delta > ((kBase - kTMin) * kTMax) / 2) {
        delta /= kBase - kTMin;
        k += kBase;
    }
    return k + (kBase - kTMin + 1) * delta / (delta + kSkew);
}
char digit(uint32_t d) { return (char)(d < 26 ? 'a' + d : '0' + d - 26); }
int value(char c) {
    if (c >= '0' && c <= '9') return c - '0' + 26;
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= 'A' && c <= 'Z') return c - 'A';
    return -1;
}

bool punyEncode(const std::u32string& in, std::string& out) {
    uint32_t n = kInitialN, delta = 0, bias = kBias;
    size_t basic = 0;
    for (uint32_t c : in)
        if (c < 0x80) {
            out += (char)c;
            basic++;
        }
    size_t h = basic;
    if (basic) out += '-';
    while (h < in.size()) {
        uint32_t m = UINT32_MAX;
        for (uint32_t c : in)
            if (c >= n && c < m) m = c;
        if ((uint64_t)(m - n) * (h + 1) + delta > UINT32_MAX) return false;
        delta += (m - n) * (uint32_t)(h + 1);
        n = m;
        for (uint32_t c : in) {
            if (c < n && ++delta == 0) return false;
            if (c != n) continue;
            uint32_t q = delta;
            for (uint32_t k = kBase;; k += kBase) {
                uint32_t t = k <= bias ? kTMin : (k >= bias + kTMax ? kTMax : k - bias);
                if (q < t) break;
                out += digit(t + (q - t) % (kBase - t));
                q = (q - t) / (kBase - t);
            }
            out += digit(q);
            bias = adapt(delta, (uint32_t)h + 1, h == basic);
            delta = 0;
            h++;
        }
        delta++;
        n++;
    }
    return true;
}

bool punyDecode(const std::string& in, std::u32string& out) {
    uint32_t n = kInitialN, i = 0, bias = kBias;
    size_t b = in.rfind('-');
    if (b == std::string::npos) b = 0;
    for (size_t j = 0; j < b; j++) {
        if ((unsigned char)in[j] >= 0x80) return false;
        out += (uint32_t)(unsigned char)in[j];
    }
    for (size_t p = b > 0 ? b + 1 : 0; p < in.size();) {
        uint32_t old = i, w = 1;
        for (uint32_t k = kBase;; k += kBase) {
            if (p >= in.size()) return false;
            int d = value(in[p++]);
            if (d < 0 || (uint64_t)d * w + i > UINT32_MAX) return false;
            i += (uint32_t)d * w;
            uint32_t t = k <= bias ? kTMin : (k >= bias + kTMax ? kTMax : k - bias);
            if ((uint32_t)d < t) break;
            if ((uint64_t)w * (kBase - t) > UINT32_MAX) return false;
            w *= kBase - t;
        }
        uint32_t len = (uint32_t)out.size() + 1;
        bias = adapt(i - old, len, old == 0);
        if (i / len > UINT32_MAX - n) return false;
        n += i / len;
        i %= len;
        if (n > 0x10FFFF) return false;
        out.insert(out.begin() + i, n);
        i++;
    }
    return true;
}

bool storable(const std::string& name) {
    if (name.empty()) return true;
    for (unsigned char c : name)
        if (c >= 0x80 || forbidden(c)) return false;
    return name.back() != '.' && name.back() != ' ';  // Windows-style drives drop these
}

// The extension, when it is plain ASCII and short ("mp3"), stays outside the encoding.
size_t extensionAt(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || name.size() - dot > 8) return std::string::npos;
    for (size_t i = dot + 1; i < name.size(); i++) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c))) return std::string::npos;
    }
    return dot;
}
}  // namespace

std::string toDiskName(const std::string& name) {
    if (storable(name)) return name;
    size_t dot = extensionAt(name);
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    bool ok;
    std::u32string cps = decodeUtf8(stem, ok);
    if (!ok) return name;  // not text at all: leave it to the drive to accept or refuse
    for (auto& c : cps)
        if (c < 0x80 && forbidden(c)) c = kEscape + c;
    if (!cps.empty() && (cps.back() == '.' || cps.back() == ' ')) cps.back() = kEscape + cps.back();
    std::string enc;
    if (!punyEncode(cps, enc)) return name;
    return kPrefix + enc + ext;
}

std::string fromDiskName(const std::string& name) {
    if (name.size() <= 4 || (name.compare(0, 4, kPrefix) != 0 && name.compare(0, 4, "XN--") != 0)) return name;
    size_t dot = extensionAt(name);
    std::string body = name.substr(4, dot == std::string::npos ? std::string::npos : dot - 4);
    std::string ext = dot == std::string::npos ? "" : name.substr(dot);
    std::u32string cps;
    if (!punyDecode(body, cps)) return name;
    for (auto& c : cps)
        if (c >= kEscape && c < kEscape + 0x80) c -= kEscape;
    std::string real = encodeUtf8(cps) + ext;
    // Only a name this code would have made: anything else that happens to start "xn--" stays.
    return toDiskName(real) == name ? real : name;
}

std::string fromDiskPath(const std::string& path) {
    if (path.find("xn--") == std::string::npos && path.find("XN--") == std::string::npos) return path;
    std::string out;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        out += fromDiskName(part);
        if (slash == std::string::npos) break;
        out += '/';
        start = slash + 1;
    }
    return out;
}
