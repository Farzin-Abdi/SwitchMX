#include "library.hpp"
#include "netfs.hpp"
#include <mutex>
#include <unordered_map>
#include <regex>
#include "i18n.hpp"
#include "names.hpp"

#include <unistd.h>

#include "archiver.hpp"
#include "platform.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Natural, case-insensitive comparison so "Ep 2" sorts before "Ep 10".
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char ca = a[i], cb = b[j];
        if (std::isdigit(ca) && std::isdigit(cb)) {
            size_t si = i, sj = j;
            while (si < a.size() && a[si] == '0') si++;
            while (sj < b.size() && b[sj] == '0') sj++;
            size_t ei = si, ej = sj;
            while (ei < a.size() && std::isdigit((unsigned char)a[ei])) ei++;
            while (ej < b.size() && std::isdigit((unsigned char)b[ej])) ej++;
            if (ei - si != ej - sj) return ei - si < ej - sj;
            int c = a.compare(si, ei - si, b, sj, ej - sj);
            if (c != 0) return c < 0;
            i = ei;
            j = ej;
            continue;
        }
        int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        i++;
        j++;
    }
    return a.size() - i < b.size() - j;
}

// Folders on the Switch SD card that never contain user media.
bool isSystemFolder(const char* n) {
    static const char* names[] = {"Nintendo", "atmosphere", "bootloader", "emuMMC", "System Volume Information",
                                  "warmboot_mariko", "exosphere", "sept", "config", "switch"};
    for (auto s : names)
        if (!strcasecmp(n, s)) return true;
    return false;
}

std::string escapeField(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '\t' || c == '\n') r += ' ';
        else r += c;
    }
    return r;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = line.find('\t', start);
        out.push_back(line.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

bool readLines(const std::string& path, std::vector<std::string>& lines) {
    FILE* f = fopen(path.c_str(), "r");
    // The temporary copy only survives alone if a save was cut off between its last two steps.
    if (!f) f = fopen((path + ".tmp").c_str(), "r");
    if (!f) return false;
    char buf[4096];
    while (fgets(buf, sizeof buf, f)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (n) lines.emplace_back(buf);
    }
    fclose(f);
    return true;
}
}  // namespace

const char* sortModeName(int mode) {
    static const char* n[] = {"Name (A–Z)", "Name (Z–A)", "Newest first", "Largest first"};
    return (mode >= 0 && mode < SORT_COUNT) ? n[mode] : n[0];
}

std::string fileExtension(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    return lower(name.substr(dot + 1));
}

MediaKind kindForExtension(const std::string& ext) {
    static const char* video[] = {"mkv",  "mk3d", "mp4", "m4v", "avi",  "mov",  "qt",   "webm", "wmv",  "asf",  "flv",
                                  "f4v",  "ts",   "m2ts", "mts", "m2t", "trp",  "tp",   "mpg",  "mpeg", "mpe",  "m1v",
                                  "m2v",  "m2p",  "mod", "tod", "vob",  "evo",  "3gp",  "3g2",  "ogv",  "ogm",  "rm",
                                  "rmvb", "divx", "xvid", "dv",  "mxf",  "nut",  "wtv",  "dvr-ms", "amv", "nsv", "y4m",
                                  "h264", "264",  "avc", "hevc", "h265", "265",  "vc1",  "ivf",  "av1",  "obu",  "vvc",
                                  "mjpeg", "mjpg", "bik",  "smk",  "fli",  "flc",  "roq",  "thp",  "dav",  "vro",
                                  "lrv",  "insv", "mp4v", "3gpp", "ismv", "mjp",  "ivr",  "drc",  "usm",  "gxf",
                                  "nuv"};
    static const char* audio[] = {"mp3", "mp2",  "mp1", "flac", "aac",  "m4a",  "m4b", "m4r", "ogg", "oga", "opus",
                                  "spx", "wav",  "w64", "wma",  "ac3",  "eac3", "ec3", "dts", "dtshd", "thd", "truehd",
                                  "mlp", "mka",  "alac", "ape", "wv",   "tta",  "tak", "mpc", "aif", "aiff", "aifc",
                                  "caf", "amr",  "awb", "au",   "snd",  "dsf",  "ra",  "shn", "weba", "m2a", "mpga",
                                  "8svx", "voc", "oma", "aa3", "at3", "at9", "aea", "hca", "adx", "brstm", "bcstm",
                                  "bfstm", "xwma", "qoa", "vqf", "sln", "gsm", "rka", "osq", "dff", "3ga", "ast",
                                  "vag", "msf", "fsb", "bwf", "rf64"};
    static const char* subs[] = {"srt", "ass", "ssa", "vtt", "sub", "idx", "sup", "smi", "sami", "rt", "jss", "aqt",
                                 "pjs", "stl", "mpl"};
    static const char* lists[] = {"m3u", "m3u8", "pls", "cue"};
    static const char* images[] = {"jpg", "jpeg", "jfif", "png", "apng", "bmp", "gif", "webp", "tif", "tiff", "tga",
                                   "qoi", "avif", "heic", "heif", "ppm", "pgm", "pbm", "pcx", "exr", "hdr", "dds",
                                   "jp2", "j2k", "ico", "psd", "dpx", "sgi", "ras", "sun", "xpm", "xbm", "xwd", "pam",
                                   "pfm", "pnm", "jls", "pcd"};
    // Opened by the document reader (MuPDF).
    static const char* docs[] = {"pdf", "xps", "oxps", "epub", "mobi", "prc", "fb2", "cbz", "cbr", "cb7", "cbt",
                                 "svg", "docx", "xlsx", "pptx", "hwpx", "htm", "html", "xhtml"};
    static const char* texts[] = {"txt", "log", "ini", "cfg", "conf", "json", "xml", "md", "nfo", "csv", "yaml", "yml"};
    for (auto e : video) if (ext == e) return MediaKind::Video;
    for (auto e : audio) if (ext == e) return MediaKind::Audio;
    for (auto e : subs) if (ext == e) return MediaKind::Subtitle;
    for (auto e : lists) if (ext == e) return MediaKind::Playlist;
    for (auto e : images) if (ext == e) return MediaKind::Image;
    for (auto e : texts) if (ext == e) return MediaKind::Text;
    for (auto e : docs) if (ext == e) return MediaKind::Document;
    return MediaKind::Other;
}

std::string baseName(const std::string& path) {
    std::string p = path;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string parentDir(const std::string& path) {
    std::string p = path;
    if (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t s = p.rfind('/');
    if (s == std::string::npos) return p;
    return p.substr(0, s + 1);
}

std::string stripExtension(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return name;
    return name.substr(0, dot);
}

std::string formatSize(uint64_t b) {
    // The number and unit in the interface language ("30.2 MB", "۳۰٫۲ مگابایت", "30,2 Mo").
    char num[32];
    const char* unit;
    if (b >= (1ull << 30)) snprintf(num, sizeof num, "%.2f", b / (double)(1ull << 30)), unit = "GB";
    else if (b >= (1ull << 20)) snprintf(num, sizeof num, "%.1f", b / (double)(1ull << 20)), unit = "MB";
    else if (b >= 1024) snprintf(num, sizeof num, "%.0f", b / 1024.0), unit = "KB";
    else snprintf(num, sizeof num, "%llu", (unsigned long long)b), unit = "B";
    std::string u = trf(std::string("size:") + unit, {});
    if (u.rfind("size:", 0) == 0) u.erase(0, 5);  // English: the unit as it is
    return i18n::localDigits(num) + " " + u;
}

std::string displayName(const std::string& path) {
    if (netfs::isNetPath(path)) return netfs::decode(baseName(path));  // "Film%20One.mkv" -> "Film One.mkv"
    return fromDiskName(baseName(path));
}

std::string shownTitle(const std::string& path, bool tidy) {
    std::string name = displayName(path);
    MediaKind k = kindForExtension(fileExtension(name));
    if (tidy && k == MediaKind::Video) return tidyTitle(name);
    if (tidy && k == MediaKind::Audio) return tidySong(name);
    return stripExtension(name);
}

std::string tidySpaces(std::string s) {
    for (auto& c : s)
        if (c == '_') c = ' ';
    return s;
}

std::string tidySong(const std::string& fileName) {
    std::string s = tidySpaces(stripExtension(fileName));
    size_t i = 0;
    while (i < s.size() && i < 3 && isdigit((unsigned char)s[i])) i++;
    // "07 Name", "07. Name", "07 - Name": the number goes (a name that is only a number stays)
    if (i > 0 && i < s.size() && (s[i] == ' ' || s[i] == '.' || s[i] == '-')) {
        size_t j = i;
        while (j < s.size() && (s[j] == ' ' || s[j] == '.' || s[j] == '-')) j++;
        if (j < s.size()) s = s.substr(j);
    }
    // Downloads often join a song's words with hyphens ("Har-Bai-Kimayo"). A hyphen between two
    // letters becomes a space; one with spaces around it ("Artist - Song") is left as it is, and
    // so is one inside a word a person would write that way ("Hip-Hop" keeps its shape only when
    // the parts are short), which a letter-count rule can't tell apart — so only the joined form.
    bool joined = s.find(' ') == std::string::npos && std::count(s.begin(), s.end(), '-') >= 2;
    if (joined)
        for (auto& c : s)
            if (c == '-') c = ' ';
    return s.empty() ? stripExtension(fileName) : s;
}

std::string tidyTitle(const std::string& fileName) {
    static std::mutex m;
    static std::unordered_map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lk(m);
        auto it = cache.find(fileName);
        if (it != cache.end()) return it->second;
    }
    using std::regex;
    const auto icase = std::regex::icase | std::regex::ECMAScript;
    std::string stem = stripExtension(fileName), s = stem;
    try {
        static const regex channel("^@[^-_. ]+[-_. ]+"), groups("\\[[^\\]]*\\]|\\{[^}]*\\}"),
            gluedId("^\\d{5,}(?=[A-Za-z])"), seps("[._]+"), gluedYear("([A-Za-z])((?:19|20)\\d\\d)(?![0-9])"),
            gluedQuality("([A-Za-z])(\\d{3,4}p)", icase), episode("\\bS(\\d{1,2})\\s*E(\\d{1,3})\\b", icase),
            stop("(^|[\\s\\-(])(\\d{3,4}p|4k|uhd|hdr|x264|x265|h\\.?264|h\\.?265|hevc|avc|web[- ]?dl|web[- ]?rip|webrip|"
                 "blu[- ]?ray|br[- ]?rip|bd[- ]?rip|hdtv|dvdrip|hdrip|remux|10[- ]?bit|8[- ]?bit|dubbed|softsub|hardsub|"
                 "dual[- ]?audio|yify|aac|ac3|proper|repack|extended|unrated|imax)\\b",
                 icase),
            emptyParens("\\(\\s*\\)"), trailing("[\\s\\-(]+$"), spaces("\\s{2,}");
        s = std::regex_replace(s, channel, "");
        s = std::regex_replace(s, groups, "");
        s = std::regex_replace(s, gluedId, "");
        s = std::regex_replace(s, seps, " ");
        s = std::regex_replace(s, gluedYear, "$1 $2");
        s = std::regex_replace(s, gluedQuality, "$1 $2");
        // "S1 E2" / "S01_E02" -> "S01E02"
        std::string out;
        auto begin = std::sregex_iterator(s.begin(), s.end(), episode);
        size_t last = 0;
        for (auto it = begin; it != std::sregex_iterator(); ++it) {
            char ep[24];
            snprintf(ep, sizeof ep, "S%02dE%02d", atoi((*it)[1].str().c_str()), atoi((*it)[2].str().c_str()));
            out += s.substr(last, it->position() - last) + ep;
            last = it->position() + it->length();
        }
        s = out + s.substr(last);
        std::smatch mt;
        if (std::regex_search(s, mt, stop) && mt.position() > 0) s = s.substr(0, mt.position());
        s = std::regex_replace(s, emptyParens, "");
        s = std::regex_replace(s, trailing, "");
        s = std::regex_replace(s, spaces, " ");
        while (!s.empty() && (s.front() == ' ' || s.front() == '-')) s.erase(0, 1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '-')) s.pop_back();
    } catch (const std::exception&) {
        s = stem;
    }
    if (s.size() < 2) s = stem;
    std::lock_guard<std::mutex> lk(m);
    if (cache.size() > 4096) cache.clear();
    cache[fileName] = s;
    return s;
}

std::string formatDate(time_t t) {
    if (!t) return "";
    struct tm* tm = localtime(&t);
    if (!tm) return "";
    // The month in the interface language (trf only reads the table: safe on any thread).
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // "7 Nov 2026", or in the order the interface language writes dates ("2026年11月7日").
    std::string d = trf("@date {0} {1} {2}", {i18n::localDigits(std::to_string(tm->tm_mday)),
                                               trf(months[tm->tm_mon % 12], {}),
                                               i18n::localDigits(std::to_string(tm->tm_year + 1900))});
    if (d.rfind("@date ", 0) == 0) d.erase(0, 6);  // English: no entry, the pattern itself
    return d;
}

std::string formatTime(double secs, bool forceHours) {
    if (secs < 0 || secs != secs) secs = 0;
    long s = (long)secs;
    char buf[32];
    if (s >= 3600 || forceHours) snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, sizeof buf, "%ld:%02ld", s / 60, s % 60);
    return buf;
}

bool isUrl(const std::string& path) { return path.find("://") != std::string::npos && path.rfind("sdmc:", 0) != 0; }

bool writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    unlink(path.c_str());  // the Switch's file systems won't rename over a file
    return rename(tmp.c_str(), path.c_str()) == 0;
}

namespace {
// The path after a move, when `path` is the moved item or inside it.
bool movedTo(const std::string& path, const std::string& from, const std::string& to, std::string& out) {
    if (path.compare(0, from.size(), from) != 0) return false;
    if (path.size() == from.size()) {
        out = to;
        return true;
    }
    if (path[from.size()] != '/') return false;
    out = to + path.substr(from.size());
    return true;
}

bool isInside(const std::string& path, const std::string& root) {
    return path.compare(0, root.size(), root) == 0 && (path.size() == root.size() || path[root.size()] == '/');
}

std::string withoutSlash(std::string p) {
    while (p.size() > 1 && p.back() == '/' && p[p.size() - 2] != ':') p.pop_back();
    return p;
}
}  // namespace

void Library::load(const std::string& dataDir) {
    dir_ = dataDir;
    std::vector<std::string> lines;
    if (readLines(dir_ + "history.tsv", lines)) {
        for (auto& l : lines) {
            auto f = splitTabs(l);
            if (f.size() < 4) continue;
            HistoryItem h;
            h.path = f[0];
            h.position = atof(f[1].c_str());
            h.duration = atof(f[2].c_str());
            h.lastPlayed = (time_t)atoll(f[3].c_str());
            if (f.size() > 4) h.title = f[4];
            history.push_back(h);
        }
    }
    lines.clear();
    if (readLines(dir_ + "favorites.txt", lines)) favorites = lines;
    lines.clear();
    if (readLines(dir_ + "streams.tsv", lines)) {
        for (auto& l : lines) {
            auto f = splitTabs(l);
            if (f.size() >= 2) streams.push_back({f[0], f[1]});
        }
    }
    loadPlaylists();
    lines.clear();
    if (readLines(dir_ + "tracks.tsv", lines)) {
        for (auto& l : lines) {
            auto f = splitTabs(l);
            if (f.size() >= 3) trackPrefs_[f[0]] = {f[1], f[2]};
        }
    }
}

std::string Library::trackPref(const std::string& path, const char* type) const {
    auto it = trackPrefs_.find(path);
    if (it == trackPrefs_.end()) return "";
    return strcmp(type, "sub") == 0 ? it->second.second : it->second.first;
}

void Library::setTrackPref(const std::string& path, const char* type, const std::string& value) {
    if (path.empty()) return;
    if (!trackPrefs_.count(path) && trackPrefs_.size() >= 3000) return;  // plenty; never grows without end
    auto& v = trackPrefs_[path];
    std::string& slot = strcmp(type, "sub") == 0 ? v.second : v.first;
    if (slot == value) return;
    slot = value;
    saveTrackPrefs();
}

void Library::saveTrackPrefs() {
    if (dir_.empty()) return;
    std::string text;
    for (auto& kv : trackPrefs_)
        text += escapeField(kv.first) + "\t" + escapeField(kv.second.first) + "\t" + escapeField(kv.second.second) + "\n";
    writeFileAtomic(dir_ + "tracks.tsv", text);
}

// One entry per line, because a path can be nearly as long as the line reader's buffer:
// "L<tab>path" for a like, "P<tab>name" to start a playlist, "F<tab>path" for its files.
void Library::loadPlaylists() {
    std::vector<std::string> lines;
    readLines(dir_ + "playlists.txt", lines);
    for (auto& l : lines) {
        auto f = splitTabs(l);
        if (f.size() < 2) continue;
        if (f[0] == "L") {
            if (likedSet_.insert(f[1]).second) liked.push_back(f[1]);
        } else if (f[0] == "P") {
            playlists.push_back({f[1], {}});
        } else if (f[0] == "F" && !playlists.empty()) {
            playlists.back().items.push_back(f[1]);
        }
    }
}

void Library::savePlaylists() {
    if (dir_.empty()) return;
    std::string text;
    for (auto& p : liked) text += "L\t" + escapeField(p) + "\n";
    for (auto& pl : playlists) {
        text += "P\t" + escapeField(pl.name) + "\n";
        for (auto& p : pl.items) text += "F\t" + escapeField(p) + "\n";
    }
    writeFileAtomic(dir_ + "playlists.txt", text);
}

bool Library::toggleLiked(const std::string& path) {
    revision++;
    bool nowLiked = likedSet_.insert(path).second;
    if (nowLiked) {
        liked.insert(liked.begin(), path);
    } else {
        likedSet_.erase(path);
        liked.erase(std::remove(liked.begin(), liked.end(), path), liked.end());
    }
    savePlaylists();
    return nowLiked;
}

int Library::createPlaylist(const std::string& name) {
    for (size_t i = 0; i < playlists.size(); i++)
        if (playlists[i].name == name) return (int)i;
    playlists.push_back({name, {}});
    revision++;
    savePlaylists();
    return (int)playlists.size() - 1;
}

void Library::deletePlaylist(int index) {
    if (index < 0 || index >= (int)playlists.size()) return;
    playlists.erase(playlists.begin() + index);
    revision++;
    savePlaylists();
}

void Library::renamePlaylist(int index, const std::string& name) {
    if (index < 0 || index >= (int)playlists.size()) return;
    playlists[index].name = name;
    revision++;
    savePlaylists();
}

bool Library::addToPlaylist(int index, const std::string& path) {
    if (index < 0 || index >= (int)playlists.size()) return false;
    auto& items = playlists[index].items;
    if (std::find(items.begin(), items.end(), path) != items.end()) return false;
    items.push_back(path);
    revision++;
    savePlaylists();
    return true;
}

void Library::removeFromPlaylist(int index, size_t item) {
    if (index < 0 || index >= (int)playlists.size() || item >= playlists[index].items.size()) return;
    playlists[index].items.erase(playlists[index].items.begin() + (long)item);
    revision++;
    savePlaylists();
}

void Library::movePlaylistItem(int index, size_t item, int delta) {
    if (index < 0 || index >= (int)playlists.size()) return;
    auto& items = playlists[index].items;
    long target = (long)item + delta;
    if (item >= items.size() || target < 0 || target >= (long)items.size()) return;
    std::swap(items[item], items[(size_t)target]);
    revision++;
    savePlaylists();
}

void Library::forgetFile(const std::string& path) {
    std::string root = withoutSlash(path);
    auto gone = [&](const std::string& p) { return isInside(p, root); };
    history.erase(std::remove_if(history.begin(), history.end(), [&](const HistoryItem& h) { return gone(h.path); }),
                  history.end());
    favorites.erase(std::remove_if(favorites.begin(), favorites.end(), gone), favorites.end());
    for (auto& p : liked)
        if (gone(p)) likedSet_.erase(p);
    liked.erase(std::remove_if(liked.begin(), liked.end(), gone), liked.end());
    for (auto& pl : playlists) pl.items.erase(std::remove_if(pl.items.begin(), pl.items.end(), gone), pl.items.end());
    size_t before = trackPrefs_.size();
    for (auto it = trackPrefs_.begin(); it != trackPrefs_.end();) it = gone(it->first) ? trackPrefs_.erase(it) : std::next(it);
    if (trackPrefs_.size() != before) saveTrackPrefs();
    revision++;
    savePlaylists();
}

void Library::pathMoved(const std::string& from, const std::string& to) {
    std::string src = withoutSlash(from), dst = withoutSlash(to), out;
    for (auto& h : history)
        if (movedTo(h.path, src, dst, out)) h.path = out;
    for (auto& p : favorites)
        if (movedTo(p, src, dst, out)) p = out;
    for (auto& p : liked) {
        if (!movedTo(p, src, dst, out)) continue;
        likedSet_.erase(p);
        likedSet_.insert(out);
        p = out;
    }
    for (auto& pl : playlists)
        for (auto& p : pl.items)
            if (movedTo(p, src, dst, out)) p = out;
    {
        std::vector<std::pair<std::string, std::pair<std::string, std::string>>> moved;
        for (auto it = trackPrefs_.begin(); it != trackPrefs_.end();) {
            if (movedTo(it->first, src, dst, out)) {
                moved.push_back({out, it->second});
                it = trackPrefs_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& m : moved) trackPrefs_[m.first] = m.second;
        if (!moved.empty()) saveTrackPrefs();
    }
    revision++;
    savePlaylists();
}

std::string Library::historyText() const {
    std::string text;
    char nums[96];
    for (auto& h : history) {
        snprintf(nums, sizeof nums, "\t%.2f\t%.2f\t%lld", h.position, h.duration, (long long)h.lastPlayed);
        text += escapeField(h.path) + nums;
        if (!h.title.empty()) text += "\t" + escapeField(h.title);  // older versions read the first four
        text += "\n";
    }
    return text;
}

void Library::save() {
    if (dir_.empty()) return;
    {
        // Anything still queued is older than what gets written here.
        std::lock_guard<std::mutex> file(fileMutex_);
        {
            std::lock_guard<std::mutex> lk(saverMutex_);
            saverHasWork_ = false;
        }
        writeFileAtomic(dir_ + "history.tsv", historyText());
    }
    std::string text;
    for (auto& p : favorites) text += escapeField(p) + "\n";
    writeFileAtomic(dir_ + "favorites.txt", text);
    text.clear();
    for (auto& s : streams) text += escapeField(s.name) + "\t" + escapeField(s.url) + "\n";
    writeFileAtomic(dir_ + "streams.tsv", text);
}

void Library::saveHistoryInBackground() {
    if (dir_.empty()) return;
    std::string text = historyText();
    {
        std::lock_guard<std::mutex> lk(saverMutex_);
        saverPending_ = std::move(text);
        saverHasWork_ = true;
    }
    if (!saver_.joinable()) {
        try {
        saver_ = std::thread([this] {
            platform::lowerThreadPriority();
            for (;;) {
                std::unique_lock<std::mutex> file(fileMutex_);
                std::unique_lock<std::mutex> lk(saverMutex_);
                if (!saverHasWork_) {
                    if (saverQuit_) return;
                    file.unlock();
                    saverWake_.wait(lk, [this] { return saverHasWork_ || saverQuit_; });
                    continue;
                }
                std::string data = std::move(saverPending_);
                saverHasWork_ = false;
                lk.unlock();
                writeFileAtomic(dir_ + "history.tsv", data);
            }
        });
        } catch (...) {
            // No thread could be started (the console's limit was reached): save right here instead.
            std::lock_guard<std::mutex> file(fileMutex_);
            std::lock_guard<std::mutex> lk(saverMutex_);
            if (saverHasWork_) writeFileAtomic(dir_ + "history.tsv", saverPending_);
            saverHasWork_ = false;
            return;
        }
    }
    saverWake_.notify_one();
}

void Library::stopBackgroundSaves() {
    if (!saver_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(saverMutex_);
        saverQuit_ = true;
    }
    saverWake_.notify_one();
    saver_.join();
}

void Library::fillDetails(FileEntry& e) {
    if (e.detailsKnown) return;
    e.detailsKnown = true;
    struct stat st;
    if (stat(e.path.c_str(), &st) != 0) return;
    e.size = e.kind == MediaKind::Folder ? 0 : (uint64_t)st.st_size;
    e.mtime = st.st_mtime;
}

std::vector<FileEntry> Library::list(const std::string& dir, bool showHidden, bool mediaOnly, int sort) const {
    std::vector<FileEntry> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    std::string base = dir;
    if (base.empty() || base.back() != '/') base += '/';
    bool atRoot = base == "sdmc:/";
    while (struct dirent* e = readdir(d)) {
        const char* n = e->d_name;
        if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
        if (!showHidden && n[0] == '.') continue;
        if (mediaOnly && atRoot && isSystemFolder(n)) continue;
        FileEntry fe;
        fe.name = fromDiskName(n);  // as it really is (see names.hpp)
        fe.path = base + n;
        // The directory entry already says what it is, which saves a stat per file: on the SD
        // card a folder of a thousand files would otherwise take about a second to open.
        struct stat st;
        bool haveStat = false, isDir = e->d_type == DT_DIR;
        if (e->d_type != DT_DIR && e->d_type != DT_REG) {
            haveStat = stat(fe.path.c_str(), &st) == 0;
            isDir = haveStat ? S_ISDIR(st.st_mode) : false;
        }
        if (isDir) {
            fe.kind = MediaKind::Folder;
            fe.path += '/';
        } else {
            fe.ext = fileExtension(fe.name);
            fe.kind = kindForExtension(fe.ext);
            if (fe.kind == MediaKind::Other && isArchiveName(fe.name)) fe.kind = MediaKind::Archive;
            if (mediaOnly && fe.kind != MediaKind::Video && fe.kind != MediaKind::Audio &&
                fe.kind != MediaKind::Playlist && fe.kind != MediaKind::Image && fe.kind != MediaKind::Document)
                continue;
        }
        if (!haveStat && (sort == SORT_NEWEST || sort == SORT_LARGEST)) haveStat = stat(fe.path.c_str(), &st) == 0;
        if (haveStat) {
            fe.size = isDir ? 0 : (uint64_t)st.st_size;
            fe.mtime = st.st_mtime;
        }
        fe.detailsKnown = haveStat;
        out.push_back(std::move(fe));
    }
    closedir(d);
    sortEntries(out, sort);
    return out;
}

void sortEntries(std::vector<FileEntry>& out, int sort) {
    std::sort(out.begin(), out.end(), [sort](const FileEntry& a, const FileEntry& b) {
        bool da = a.kind == MediaKind::Folder, db = b.kind == MediaKind::Folder;
        if (da != db) return da;
        switch (sort) {
            case SORT_NAME_DESC: return naturalLess(b.name, a.name);
            case SORT_NEWEST:
                if (a.mtime != b.mtime) return a.mtime > b.mtime;
                break;
            case SORT_LARGEST:
                if (a.size != b.size) return a.size > b.size;
                break;
        }
        return naturalLess(a.name, b.name);
    });
}

int Library::siblings(const std::string& path, std::vector<std::string>& out) const {
    out.clear();
    if (isUrl(path)) {
        out.push_back(path);
        return 0;
    }
    MediaKind kind = kindForExtension(fileExtension(path));
    auto entries = list(parentDir(path), false, true, SORT_NAME);
    int idx = -1;
    for (auto& e : entries) {
        if (e.kind != kind) continue;
        if (e.path == path) idx = (int)out.size();
        out.push_back(e.path);
    }
    if (idx < 0) {
        out.clear();
        out.push_back(path);
        idx = 0;
    }
    return idx;
}

std::vector<std::string> Library::subtitlesFor(const std::string& videoPath, bool matchingOnly) const {
    std::vector<std::string> out;
    if (isUrl(videoPath)) return out;
    std::string stem = lower(stripExtension(baseName(videoPath)));
    auto entries = list(parentDir(videoPath), false, false, SORT_NAME);
    for (auto& e : entries) {
        if (e.kind != MediaKind::Subtitle) continue;
        bool match = lower(e.name).rfind(stem, 0) == 0;
        if (match) out.insert(out.begin(), e.path);
        else if (!matchingOnly) out.push_back(e.path);
    }
    // Common release layouts: Subs/<file>.srt and Subs/<video name>/<lang>.srt
    std::string dir = parentDir(videoPath);
    for (const char* sub : {"Subs/", "subs/", "Subtitles/"}) {
        for (auto& e : list(dir + sub, false, false, SORT_NAME)) {
            if (e.kind == MediaKind::Subtitle && (!matchingOnly || lower(e.name).rfind(stem, 0) == 0))
                out.push_back(e.path);
            if (e.kind == MediaKind::Folder && lower(baseName(e.path)) == stem)
                for (auto& f : list(e.path, false, false, SORT_NAME))
                    if (f.kind == MediaKind::Subtitle) out.push_back(f.path);
        }
    }
    return out;
}

const HistoryItem* Library::find(const std::string& path) const {
    for (auto& h : history)
        if (h.path == path) return &h;
    return nullptr;
}

void Library::updateHistory(const std::string& path, double pos, double dur, bool pages, const std::string& title) {
    if (path.empty()) return;
    std::string keptTitle = title;
    for (auto it = history.begin(); it != history.end(); ++it) {
        if (it->path == path) {
            if (keptTitle.empty()) keptTitle = it->title;
            history.erase(it);
            break;
        }
    }
    HistoryItem h;
    h.path = path;
    // The last few percent count as finished (-1) so the next play starts fresh.
    h.position = pages ? pos : (dur > 0 && pos > dur * 0.95) ? -1 : (pos < 5 ? 0 : pos);
    h.duration = dur;
    h.lastPlayed = time(nullptr);
    h.title = keptTitle;
    history.insert(history.begin(), h);
    if (history.size() > 200) history.resize(200);
}

void Library::removeHistory(const std::string& path) {
    history.erase(std::remove_if(history.begin(), history.end(), [&](const HistoryItem& h) { return h.path == path; }),
                  history.end());
}

void Library::clearHistory() { history.clear(); }

double Library::resumePosition(const std::string& path) const {
    auto h = find(path);
    return (h && h->position > 0) ? h->position : 0;
}

float Library::progress(const std::string& path) const {
    auto h = find(path);
    if (!h || h->duration <= 0) return -1;
    // Songs start from the beginning each time (see PlayerScreen::play): no half-heard mark on them.
    if (h->duration < 20 * 60 && kindForExtension(fileExtension(path)) == MediaKind::Audio) return -1;
    if (h->position < 0) return 1.0f;
    if (h->position == 0) return -1;
    return (float)(h->position / h->duration);
}

bool Library::isFavorite(const std::string& path) const {
    return std::find(favorites.begin(), favorites.end(), path) != favorites.end();
}

void Library::toggleFavorite(const std::string& path) {
    auto it = std::find(favorites.begin(), favorites.end(), path);
    if (it != favorites.end()) favorites.erase(it);
    else favorites.push_back(path);
    revision++;
}
