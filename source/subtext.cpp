#include "subtext.hpp"

#include <cstdint>
#include <cstdio>

#include "library.hpp"

extern "C" const unsigned short g_codepageTables[7][128];

namespace {
enum Enc { E_AUTO, E_UTF8, E_1256, E_1251, E_1252, E_1250, E_1253, E_1254, E_1255 };

// Index into g_codepageTables (cp1250 .. cp1256).
int tableFor(int enc) {
    switch (enc) {
        case E_1250: return 0;
        case E_1251: return 1;
        case E_1252: return 2;
        case E_1253: return 3;
        case E_1254: return 4;
        case E_1255: return 5;
        case E_1256: return 6;
        default: return 2;
    }
}

void putUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += (char)c;
    } else if (c < 0x800) {
        out += (char)(0xC0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3F));
        out += (char)(0x80 | ((c >> 6) & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

bool validUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return false;
        for (int k = 1; k < len; k++)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

std::string fromUtf16(const std::string& s, bool bigEndian) {
    std::string out;
    for (size_t i = 2; i + 1 < s.size(); i += 2) {
        uint32_t u = bigEndian ? ((unsigned char)s[i] << 8 | (unsigned char)s[i + 1])
                               : ((unsigned char)s[i + 1] << 8 | (unsigned char)s[i]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < s.size()) {
            uint32_t lo = bigEndian ? ((unsigned char)s[i + 2] << 8 | (unsigned char)s[i + 3])
                                    : ((unsigned char)s[i + 3] << 8 | (unsigned char)s[i + 2]);
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        }
        putUtf8(out, u);
    }
    return out;
}

std::string fromCodepage(const std::string& s, int enc) {
    const unsigned short* table = g_codepageTables[tableFor(enc)];
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (c < 0x80) out += (char)c;
        else putUtf8(out, table[c - 0x80]);
    }
    return out;
}

// Guesses a single-byte code page from letter frequencies.
int guessCodepage(const std::string& s, int langHint) {
    // Language hints use the order of the language list in config.cpp.
    switch (langHint) {
        case 8: return E_1251;   // Russian
        case 11: case 12: return E_1256;  // Arabic, Persian
        case 14: return E_1254;  // Turkish
    }
    size_t count[256] = {};
    size_t high = 0, ascii = 0;
    for (unsigned char c : s) {
        count[c]++;
        if (c >= 0x80) high++;
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ascii++;
    }
    // Western languages use accented letters sparingly; Arabic/Cyrillic text is mostly high bytes.
    if (high < (high + ascii) * 0.3) return E_1252;
    // Alef (C7) and lam (E1) dominate Arabic script; o (EE) and a (E0) dominate Russian.
    size_t arabic = count[0xC7] + count[0xE1] + count[0xC8] + count[0xD1];
    size_t cyrillic = count[0xEE] + count[0xE0] + count[0xF2] + count[0xE8];
    return arabic >= cyrillic ? E_1256 : E_1251;
}
}  // namespace

const std::vector<std::string>& subtitleEncodingNames() {
    static const std::vector<std::string> names = {
        "Automatic",           "UTF-8",          "Persian / Arabic (1256)", "Cyrillic (1251)", "Western (1252)",
        "Central Europe (1250)", "Greek (1253)", "Turkish (1254)",          "Hebrew (1255)"};
    return names;
}

std::string subtitleToUtf8(const std::string& data, int encoding, int langHint) {
    if (data.size() >= 2 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE) return fromUtf16(data, false);
    if (data.size() >= 2 && (unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF) return fromUtf16(data, true);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        return data.substr(3);
    if (encoding == E_UTF8 || (encoding == E_AUTO && validUtf8(data))) return data;
    int enc = encoding == E_AUTO ? guessCodepage(data, langHint) : encoding;
    return fromCodepage(data, enc);
}

std::string prepareSubtitle(const std::string& path, int encoding, int langHint, const std::string& cacheDir) {
    // Picture-based formats are binary; "converting" them would corrupt them.
    std::string ext = fileExtension(path);
    if (ext == "sup" || ext == "idx" || ext == "sub") return path;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return path;
    std::string data;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && data.size() < (16u << 20)) data.append(buf, n);
    fclose(f);

    std::string utf8 = subtitleToUtf8(data, encoding, langHint);
    if (utf8 == data) return path;

    // Name the copy after the original so mpv's track list shows a sensible title.
    uint32_t h = 2166136261u;
    for (char c : path) h = (h ^ (unsigned char)c) * 16777619u;
    char prefix[16];
    snprintf(prefix, sizeof prefix, "%08x-", h);
    std::string out = cacheDir + prefix + baseName(path);
    FILE* o = fopen(out.c_str(), "wb");
    if (!o) return path;
    fwrite(utf8.data(), 1, utf8.size(), o);
    fclose(o);
    return out;
}
