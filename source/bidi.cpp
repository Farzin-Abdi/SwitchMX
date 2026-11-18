#include "bidi.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {
struct Forms {
    uint32_t base, isolated, final, initial, medial;  // initial/medial 0 = right-joining only
};

// Presentation forms for Arabic and Persian letters.
const Forms kForms[] = {
    {0x0621, 0xFE80, 0, 0, 0},           {0x0622, 0xFE81, 0xFE82, 0, 0},      {0x0623, 0xFE83, 0xFE84, 0, 0},
    {0x0624, 0xFE85, 0xFE86, 0, 0},      {0x0625, 0xFE87, 0xFE88, 0, 0},      {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},
    {0x0627, 0xFE8D, 0xFE8E, 0, 0},      {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, {0x0629, 0xFE93, 0xFE94, 0, 0},
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98}, {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, {0x062F, 0xFEA9, 0xFEAA, 0, 0},  {0x0630, 0xFEAB, 0xFEAC, 0, 0},
    {0x0631, 0xFEAD, 0xFEAE, 0, 0},      {0x0632, 0xFEAF, 0xFEB0, 0, 0},      {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC},
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},
    {0x0648, 0xFEED, 0xFEEE, 0, 0},      {0x0649, 0xFEEF, 0xFEF0, 0, 0},      {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59}, {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D},
    {0x0698, 0xFB8A, 0xFB8B, 0, 0},      {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91},
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95}, {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF},
};

const Forms* formsFor(uint32_t c) {
    for (auto& f : kForms)
        if (f.base == c) return &f;
    return nullptr;
}

bool isTransparent(uint32_t c) { return (c >= 0x064B && c <= 0x065F) || c == 0x0670; }
bool isTatweel(uint32_t c) { return c == 0x0640; }
bool joinsForward(uint32_t c) {  // can connect to the following letter
    if (isTatweel(c)) return true;
    const Forms* f = formsFor(c);
    return f && f->initial;
}
bool joinsBackward(uint32_t c) {  // can connect to the preceding letter
    if (isTatweel(c)) return true;
    const Forms* f = formsFor(c);
    return f && f->final;
}

bool isRtl(uint32_t c) {
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF);
}
bool isLtrStrong(uint32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c < 0x0590 && c != 0xD7 && c != 0xF7) ||
           (c >= 0x0900 && c < 0xFB1D) || c >= 0x10000;
}
// Digits in any script (and the Arabic decimal and thousands marks) run left to right, even inside
// right-to-left text, so they're tested before the right-to-left letters they sit among.
bool isDigit(uint32_t c) {
    return (c >= '0' && c <= '9') || (c >= 0x06F0 && c <= 0x06F9) || (c >= 0x0660 && c <= 0x066C);
}

uint32_t mirror(uint32_t c) {
    switch (c) {
        case '(': return ')';
        case ')': return '(';
        case '[': return ']';
        case ']': return '[';
        case '{': return '}';
        case '}': return '{';
        case '<': return '>';
        case '>': return '<';
        case 0xAB: return 0xBB;
        case 0xBB: return 0xAB;
    }
    return c;
}

std::vector<uint32_t> decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); i++; continue; }
        if (i + len > s.size()) break;
        for (int k = 1; k < len; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string encode(const std::vector<uint32_t>& v) {
    std::string out;
    for (uint32_t c : v) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
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
    return out;
}

std::vector<uint32_t> shapeArabic(const std::vector<uint32_t>& in) {
    std::vector<uint32_t> out;
    out.reserve(in.size());
    auto prevJoinable = [&](size_t i) {
        for (size_t k = i; k-- > 0;) {
            if (isTransparent(in[k])) continue;
            return joinsForward(in[k]);
        }
        return false;
    };
    auto nextIndex = [&](size_t i) {
        for (size_t k = i + 1; k < in.size(); k++)
            if (!isTransparent(in[k])) return k;
        return in.size();
    };
    for (size_t i = 0; i < in.size(); i++) {
        uint32_t c = in[i];
        const Forms* f = formsFor(c);
        if (!f) {
            out.push_back(c);
            continue;
        }
        bool joinPrev = f->final && prevJoinable(i);
        size_t ni = nextIndex(i);
        uint32_t next = ni < in.size() ? in[ni] : 0;

        // Lam + alef ligatures.
        if (c == 0x0644 && (next == 0x0622 || next == 0x0623 || next == 0x0625 || next == 0x0627)) {
            uint32_t lig = next == 0x0622 ? 0xFEF5 : next == 0x0623 ? 0xFEF7 : next == 0x0625 ? 0xFEF9 : 0xFEFB;
            out.push_back(joinPrev ? lig + 1 : lig);
            for (size_t k = i + 1; k < ni; k++) out.push_back(in[k]);
            i = ni;
            continue;
        }
        bool joinNext = f->initial && ni < in.size() && joinsBackward(next);
        uint32_t form = f->isolated;
        if (joinPrev && joinNext) form = f->medial;
        else if (joinPrev) form = f->final;
        else if (joinNext) form = f->initial;
        out.push_back(form ? form : c);
    }
    return out;
}

// Minimal bidi: resolves each character to RTL or LTR and reorders runs into visual order.
std::vector<uint32_t> reorder(const std::vector<uint32_t>& v) {
    size_t n = v.size();
    // 1 = RTL, 0 = LTR, -1 = neutral (resolved below)
    std::vector<int> dir(n);
    int paragraph = -1;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = v[i];
        // U+200F / U+200E (right-to-left / left-to-right marks) set the direction without showing.
        dir[i] = c == 0x200F ? 1 : c == 0x200E ? 0 : isDigit(c) ? 0 : isRtl(c) ? 1 : isLtrStrong(c) ? 0 : -1;
        if (paragraph < 0 && dir[i] >= 0 && !isDigit(c)) paragraph = dir[i];
    }
    if (paragraph < 0) paragraph = 0;
    for (size_t i = 0; i < n; i++) {
        if (dir[i] >= 0) continue;
        size_t j = i;
        while (j < n && dir[j] < 0) j++;
        // Numbers count as right-to-left for the spaces and marks around them (Unicode rule N1),
        // though the digits themselves still read left to right.
        auto weight = [&](size_t k) { return isDigit(v[k]) ? 1 : dir[k]; };
        int before = -1, after = -1;
        for (size_t k = i; k-- > 0;) if (dir[k] >= 0) { before = weight(k); break; }
        if (j < n) after = weight(j);
        int d = (before == after && before >= 0) ? before : paragraph;
        for (size_t k = i; k < j; k++) dir[k] = d;
        i = j - 1;
    }

    std::vector<uint32_t> out(v.begin(), v.end());
    for (size_t i = 0; i < n; i++)
        if (dir[i] == 1) out[i] = mirror(out[i]);
    if (paragraph == 1) {
        std::reverse(out.begin(), out.end());
        std::reverse(dir.begin(), dir.end());
        // Restore the order inside embedded left-to-right runs (Latin words, numbers).
        for (size_t i = 0; i < n;) {
            if (dir[i] != 0) { i++; continue; }
            size_t j = i;
            while (j < n && dir[j] == 0) j++;
            std::reverse(out.begin() + i, out.begin() + j);
            i = j;
        }
    } else {
        for (size_t i = 0; i < n;) {
            if (dir[i] != 1) { i++; continue; }
            size_t j = i;
            while (j < n && dir[j] == 1) j++;
            std::reverse(out.begin() + i, out.begin() + j);
            i = j;
        }
    }
    return out;
}
}  // namespace

std::string shapeForDisplay(const std::string& utf8) {
    bool hasRtl = false;
    for (unsigned char c : utf8) {
        // Hebrew and Arabic code points start with UTF-8 lead bytes D6..DF; presentation forms with EF.
        if (c >= 0xD6 && c <= 0xDF) { hasRtl = true; break; }
        if (c == 0xEF) { hasRtl = true; break; }
    }
    if (!hasRtl) return utf8;

    static std::unordered_map<std::string, std::string> cache;
    auto it = cache.find(utf8);
    if (it != cache.end()) return it->second;
    if (cache.size() > 4096) cache.clear();  // long text files would otherwise stay in memory
    std::vector<uint32_t> cps = decode(utf8);
    bool any = false;
    for (uint32_t c : cps)
        if (isRtl(c)) any = true;
    std::string result = utf8;
    if (any) {
        std::vector<uint32_t> visual = reorder(shapeArabic(cps));
        visual.erase(std::remove_if(visual.begin(), visual.end(), [](uint32_t c) { return c == 0x200F || c == 0x200E; }),
                     visual.end());
        result = encode(visual);
    }
    if (cache.size() > 2048) cache.clear();
    cache.emplace(utf8, result);
    return result;
}
