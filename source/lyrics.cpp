#include "lyrics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "library.hpp"
#include "subtext.hpp"

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// "[mm:ss]", "[mm:ss.xx]" or "[mm:ss:xx]" at `i`; returns the seconds and moves `i` past it.
bool timeTag(const std::string& s, size_t& i, double& out) {
    if (i >= s.size() || s[i] != '[') return false;
    size_t close = s.find(']', i);
    if (close == std::string::npos) return false;
    std::string in = s.substr(i + 1, close - i - 1);
    size_t colon = in.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t k = 0; k < colon; k++)
        if (!isdigit((unsigned char)in[k])) return false;
    std::string rest = in.substr(colon + 1);
    if (rest.empty() || !isdigit((unsigned char)rest[0])) return false;
    for (auto& c : rest)
        if (c == ':') c = '.';  // some files write mm:ss:xx
    out = atoi(in.substr(0, colon).c_str()) * 60.0 + atof(rest.c_str());
    i = close + 1;
    return true;
}
}  // namespace

int Lyrics::lineAt(double t) const {
    if (!synced) return -1;
    auto it = std::upper_bound(lines.begin(), lines.end(), t, [](double v, const Line& l) { return v < l.time; });
    return (int)(it - lines.begin()) - 1;
}

Lyrics parseLyrics(const std::string& text) {
    Lyrics out;
    double offset = 0;
    std::vector<std::string> plain;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        // Header tags such as [ar:Artist] or [offset:+250]
        if (t.size() > 2 && t[0] == '[' && t.back() == ']' && isalpha((unsigned char)t[1])) {
            if (t.compare(0, 8, "[offset:") == 0) offset = atof(t.substr(8).c_str()) / 1000.0;
            continue;
        }
        size_t i = 0;
        std::vector<double> times;
        double v;
        while (timeTag(t, i, v)) {
            times.push_back(v);
            while (i < t.size() && t[i] == ' ') i++;
        }
        if (times.empty()) {
            plain.push_back(t);
            continue;
        }
        // Word-timing tags <mm:ss.xx> inside the line are dropped.
        std::string words;
        for (size_t k = i; k < t.size(); k++) {
            if (t[k] == '<') {
                size_t e = t.find('>', k);
                if (e != std::string::npos) {
                    k = e;
                    continue;
                }
            }
            words += t[k];
        }
        for (double tm : times) out.lines.push_back({tm, trim(words)});
    }
    if (!out.lines.empty()) {
        out.synced = true;
        // A positive offset shows the words earlier.
        for (auto& l : out.lines) l.time = std::max(0.0, l.time - offset);
        std::stable_sort(out.lines.begin(), out.lines.end(),
                         [](const Lyrics::Line& a, const Lyrics::Line& b) { return a.time < b.time; });
        // Nothing to show at all (only timing marks): treat as no lyrics.
        bool any = false;
        for (auto& l : out.lines) any |= !l.text.empty();
        if (!any) out.lines.clear();
        return out;
    }
    // Plain: keep the verses apart, but never more than one blank line in a row.
    while (!plain.empty() && plain.back().empty()) plain.pop_back();
    size_t first = 0;
    while (first < plain.size() && plain[first].empty()) first++;
    for (size_t k = first; k < plain.size(); k++) {
        if (plain[k].empty() && !out.lines.empty() && out.lines.back().text.empty()) continue;
        out.lines.push_back({0, plain[k]});
    }
    return out;
}

Lyrics findLyrics(const std::string& songPath, const std::map<std::string, std::string>& tags, int langHint) {
    if (!songPath.empty() && !isUrl(songPath)) {
        std::string stem = stripExtension(songPath);
        for (const char* ext : {".lrc", ".LRC", ".Lrc"}) {
            FILE* f = fopen((stem + ext).c_str(), "rb");
            if (!f) continue;
            std::string data(256 * 1024, '\0');
            data.resize(fread(&data[0], 1, data.size(), f));
            fclose(f);
            Lyrics l = parseLyrics(subtitleToUtf8(data, 0, langHint));
            if (!l.empty()) return l;
        }
    }
    // Tags: "lyrics", "unsyncedlyrics", "lyrics-eng", ... (keys are lower case)
    for (auto& kv : tags) {
        if (kv.first.compare(0, 6, "lyrics") != 0 && kv.first.compare(0, 14, "unsyncedlyrics") != 0) continue;
        Lyrics l = parseLyrics(kv.second);
        if (!l.empty()) return l;
    }
    return {};
}
