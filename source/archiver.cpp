#include "archiver.hpp"
#include "i18n.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "library.hpp"
#include "names.hpp"

namespace {
constexpr size_t kBufSize = 1 << 20;

bool pathExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool isDirectory(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

uint64_t fileSize(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

// Turns a name stored in an archive into a safe relative path: no "..", no absolute paths, no
// control characters. Returns "" for names with nothing left. The real name: diskRelative() gives
// what is created on the drive.
std::string safeRelative(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::string out, part;
    size_t i = 0;
    while (i <= name.size()) {
        if (i == name.size() || name[i] == '/') {
            if (!part.empty() && part != "." && part != "..") {
                while (!part.empty() && (part.back() == ' ' || part.back() == '.')) part.pop_back();
                if (!part.empty()) {
                    if (!out.empty()) out += '/';
                    out += part;
                }
            }
            part.clear();
        } else {
            char c = name[i];
            if ((unsigned char)c < 32) c = '_';
            part += c;
        }
        i++;
    }
    return out;
}

// Applies the conflict policy. Returns false when the entry should be skipped.
bool resolveTarget(std::string& target, ConflictPolicy policy) {
    if (!pathExists(target)) return true;
    if (policy == ConflictPolicy::Skip) return false;
    if (policy == ConflictPolicy::KeepBoth) target = uniquePath(target);
    return true;
}

// RAR multi-volume sets, starting at the given first part: "name.part2.rar" or "name.r00".
std::vector<std::string> rarVolumes(const std::string& first) {
    std::vector<std::string> out{first};
    std::string lower = first;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    size_t p = lower.rfind(".part");
    size_t digits = p == std::string::npos || lower.size() < p + 9 ? 0 : lower.size() - 4 - (p + 5);
    bool numbered = digits > 0 && lower.compare(lower.size() - 4, 4, ".rar") == 0 &&
                    std::all_of(lower.begin() + p + 5, lower.end() - 4, [](char c) { return isdigit((unsigned char)c); });
    char num[16];
    if (numbered) {
        for (int i = 2; i < 100000; i++) {
            snprintf(num, sizeof num, "%0*d", (int)digits, i);
            std::string next = first.substr(0, p + 5) + num + first.substr(first.size() - 4);
            if (!pathExists(next)) break;
            out.push_back(next);
        }
        return out;
    }
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".rar") != 0) return out;
    // .r00 to .r99, then .s00 and so on; keep the case of the first part's extension.
    std::string stem = first.substr(0, first.size() - 3);
    char letter = first[first.size() - 3] == 'R' ? 'R' : 'r';
    for (int i = 0; i < 900; i++) {
        snprintf(num, sizeof num, "%c%02d", letter + i / 100, i % 100);
        if (!pathExists(stem + num)) break;
        out.push_back(stem + num);
    }
    return out;
}

// Other volumes of "name.7z.001" / "name.zip.001" style splits, in order.
std::vector<std::string> splitVolumes(const std::string& path) {
    std::vector<std::string> out{path};
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.substr(dot + 1) != "001") return out;
    std::string stem = path.substr(0, dot + 1);
    for (int i = 2; i < 1000; i++) {
        char num[8];
        snprintf(num, sizeof num, "%03d", i);
        if (!pathExists(stem + num)) break;
        out.push_back(stem + num);
    }
    return out;
}

bool isRarFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char m[7] = {};
    size_t n = fread(m, 1, sizeof m, f);
    fclose(f);
    return n == 7 && !memcmp(m, "Rar!\x1a\x07", 6);
}

// RAR multi-volume sets must be opened at the first part.
std::string firstRarVolume(const std::string& path) {
    std::string lower = path;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    size_t p = lower.rfind(".part");
    if (p != std::string::npos && lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".rar") == 0) {
        size_t digits = lower.size() - 4 - (p + 5);
        std::string candidate = path.substr(0, p + 5) + std::string(digits > 0 ? digits - 1 : 0, '0') + "1" +
                                path.substr(path.size() - 4);
        if (digits > 0 && pathExists(candidate)) return candidate;
    }
    if (lower.size() > 4 && lower[lower.size() - 4] == '.' && lower[lower.size() - 3] == 'r' &&
        isdigit((unsigned char)lower[lower.size() - 2]) && isdigit((unsigned char)lower.back())) {
        std::string candidate = path.substr(0, path.size() - 3) + "rar";
        if (pathExists(candidate)) return candidate;
    }
    return path;
}

// ---------------------------------------------------------------- libarchive

// Every file of the archive, in order. A RAR set is opened at its first part.
std::vector<std::string> archiveVolumes(const std::string& path) {
    std::string first = firstRarVolume(path);
    return isRarFile(first) ? rarVolumes(first) : splitVolumes(path);
}

archive* openReader(const std::string& path, const std::string& password, bool listOnly, bool& openOk) {
    archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    archive_read_support_format_raw(a);  // lone .gz / .xz / .bz2 files
    if (listOnly) {
        archive_read_set_format_option(a, "rar", "list_only", "1");
        archive_read_set_format_option(a, "rar5", "list_only", "1");
    }
    if (!password.empty()) archive_read_add_passphrase(a, password.c_str());
    auto volumes = archiveVolumes(path);
    std::vector<const char*> names;
    for (auto& v : volumes) names.push_back(v.c_str());
    names.push_back(nullptr);
    openOk = archive_read_open_filenames(a, names.data(), 256 * 1024) == ARCHIVE_OK;
    return a;
}

// Sorts libarchive's errors into what the caller can act on.
ArcResult classifyError(archive* a, const std::string& password, std::string& error) {
    const char* msg = archive_error_string(a);
    error = msg ? msg : "Unknown error";
    bool wantsPassword = error.find("assphrase") != std::string::npos || error.find("ncrypted") != std::string::npos;
    if (wantsPassword) {
        if (password.empty()) return ArcResult::NeedPassword;
        return ArcResult::WrongPassword;
    }
    // Data errors after decrypting usually mean the password was wrong.
    if (!password.empty() && (error.find("CRC") != std::string::npos || error.find("ecompress") != std::string::npos ||
                              error.find("Damaged") != std::string::npos || error.find("LZMA") != std::string::npos ||
                              error.find("Bad") != std::string::npos || error.find("Truncated") != std::string::npos))
        return ArcResult::WrongPassword;
    return ArcResult::Error;
}

// Each part as the drive can store it (names.hpp): "موسیقی/a?.mp3" -> "xn--.../xn--a-...mp3". A
// name that isn't UTF-8 (an old zip in a local code page) can't be encoded; its other bytes become _.
std::string diskRelative(const std::string& rel) {
    std::string out;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t e = rel.find('/', i);
        if (e == std::string::npos) e = rel.size();
        std::string part = toDiskName(rel.substr(i, e - i));
        for (auto& c : part)
            if ((unsigned char)c >= 0x80 || strchr(":*?\"<>|\\", c)) c = '_';
        if (!out.empty()) out += '/';
        out += part;
        i = e + 1;
    }
    return out;
}

std::string entryName(archive_entry* e) {
    const char* u = archive_entry_pathname_utf8(e);
    if (!u) u = archive_entry_pathname(e);
    return u ? u : "";
}

std::string rawOutputName(const std::string& archivePath) {
    std::string base = fromDiskName(baseName(archivePath));
    size_t dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
    return base.empty() ? "output" : base;
}

ArcResult extractWithLibarchive(const std::string& path, const std::string& dest, const std::string& password,
                                ConflictPolicy policy, ArcProgress& prog, std::string& error) {
    bool openOk = false;
    archive* a = openReader(path, password, false, openOk);
    if (!openOk) {
        ArcResult r = classifyError(a, password, error);
        archive_read_free(a);
        return r;
    }
    uint64_t archiveBytes = 0;
    for (auto& v : archiveVolumes(path)) archiveBytes += fileSize(v);
    prog.total = archiveBytes;
    prog.done = 0;
    ArcResult result = ArcResult::Ok;
    std::vector<char> buf(kBufSize);
    archive_entry* entry = nullptr;
    int files = 0;
    bool extractedAnything = false;

    for (;;) {
        if (prog.cancel) {
            result = ArcResult::Cancelled;
            break;
        }
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r == ARCHIVE_RETRY) continue;
        if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
            result = classifyError(a, password, error);
            break;
        }
        files++;
        bool raw = archive_format(a) == ARCHIVE_FORMAT_RAW;
        std::string rel = safeRelative(raw ? rawOutputName(path) : entryName(entry));
        mode_t type = archive_entry_filetype(entry);
        if (rel.empty()) continue;
        std::string target = dest + diskRelative(rel);
        if (type == AE_IFDIR) {
            makeDirs(target);
            continue;
        }
        if (type != AE_IFREG && type != 0) continue;  // links and devices don't exist on Switch media
        prog.setCurrent(rel);
        size_t slash = target.rfind('/');
        if (slash != std::string::npos) makeDirs(target.substr(0, slash));
        if (!resolveTarget(target, policy)) continue;

        FILE* out = fopen(target.c_str(), "wb");
        if (!out) {
            error = trf("Can't create {0}: {1}", {rel, strerror(errno)});
            result = ArcResult::Error;
            break;
        }
        la_ssize_t n = 0;
        bool writeFailed = false;
        while ((n = archive_read_data(a, buf.data(), buf.size())) > 0) {
            if (fwrite(buf.data(), 1, (size_t)n, out) != (size_t)n) {
                error = trf("Can't write {0}: {1}", {rel, strerror(errno)});
                writeFailed = true;
                break;
            }
            prog.done = (uint64_t)archive_filter_bytes(a, -1);
            if (prog.cancel) break;
        }
        bool closed = fclose(out) == 0;
        if (prog.cancel || writeFailed || n < 0 || !closed) {
            unlink(target.c_str());  // never leave a partial file behind
            if (prog.cancel) result = ArcResult::Cancelled;
            else if (writeFailed) result = ArcResult::Error;
            else if (!closed) {
                error = trf("Can't write {0}", {rel});
                result = ArcResult::Error;
            } else result = classifyError(a, password, error);
            break;
        }
        extractedAnything = true;
    }
    if (result == ArcResult::Ok && !extractedAnything && files == 0) {
        error = "No files found in this archive";
        result = ArcResult::Error;
    }
    if (archive_filter_count(a) >= 0 && result == ArcResult::Ok) prog.done = prog.total.load();
    archive_read_free(a);
    return result;
}

// ---------------------------------------------------------------- creating

void collectFiles(const std::string& path, const std::string& rel, std::vector<std::pair<std::string, std::string>>& out,
                  uint64_t& total) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        out.push_back({path, rel + "/"});
        DIR* d = opendir(path.c_str());
        if (!d) return;
        std::string base = path;
        if (base.empty() || base.back() != '/') base += '/';
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            collectFiles(base + e->d_name, rel + "/" + fromDiskName(e->d_name), out, total);
        }
        closedir(d);
    } else {
        out.push_back({path, rel});
        total += (uint64_t)st.st_size;
    }
}
}  // namespace

bool isArchiveName(const std::string& name) {
    std::string ext = fileExtension(name);
    static const char* exts[] = {"zip", "7z",   "rar",  "tar", "gz",  "tgz", "bz2", "tbz",  "tbz2", "xz",  "txz",
                                 "lz",  "lzma", "tlz",  "zst", "lz4", "lzo", "z",   "cab",  "iso",  "lha", "lzh",
                                 "ar",  "deb",  "rpm",  "cpio", "xar", "pkg", "jar", "apk",  "cbz",  "cbr", "cb7",
                                 "cbt", "wim",  "warc"};
    for (auto e : exts)
        if (ext == e) return true;
    // "name.7z.001", "name.zip.001", "name.r00"
    if (ext == "001") {
        std::string stem = fileExtension(stripExtension(name));
        return stem == "7z" || stem == "zip" || stem == "rar" || stem == "tar";
    }
    return ext.size() == 3 && ext[0] == 'r' && isdigit((unsigned char)ext[1]) && isdigit((unsigned char)ext[2]);
}

bool makeDirs(const std::string& dir) {
    if (isDirectory(dir)) return true;
    size_t start = dir.find(":/");
    start = start == std::string::npos ? 1 : start + 2;
    for (size_t i = start; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            std::string part = dir.substr(0, i);
            if (!part.empty() && !isDirectory(part)) mkdir(part.c_str(), 0777);
        }
    }
    return isDirectory(dir);
}

std::string uniquePath(const std::string& path) {
    if (!pathExists(path)) return path;
    std::string p = path;
    bool slash = !p.empty() && p.back() == '/';
    if (slash) p.pop_back();
    size_t sep = p.rfind('/');
    std::string dir = sep == std::string::npos ? "" : p.substr(0, sep + 1);
    std::string name = fromDiskName(sep == std::string::npos ? p : p.substr(sep + 1));  // number the real name
    size_t dot = name.rfind('.');
    bool hasExt = !slash && !isDirectory(p) && dot != std::string::npos && dot > 0;
    std::string stem = hasExt ? name.substr(0, dot) : name, ext = hasExt ? name.substr(dot) : "";
    for (int i = 1; i < 100000; i++) {
        std::string candidate = dir + toDiskName(stem + " (" + std::to_string(i) + ")" + ext);
        if (!pathExists(candidate)) return candidate + (slash ? "/" : "");
    }
    return path;
}

ArcResult extractArchive(const std::string& path, const std::string& destDir, const std::string& password,
                         ConflictPolicy policy, ArcProgress& prog, std::string& error) {
    std::string dest = destDir;
    if (dest.empty() || dest.back() != '/') dest += '/';
    if (!makeDirs(dest.substr(0, dest.size() - 1))) {
        error = "Can't create the destination folder";
        return ArcResult::Error;
    }
    return extractWithLibarchive(path, dest, password, policy, prog, error);
}

ArcResult listArchive(const std::string& path, const std::string& password, std::vector<ArcEntry>& out,
                      std::string& error, size_t limit) {
    out.clear();
    bool openOk = false;
    archive* a = openReader(path, password, true, openOk);
    if (!openOk) {
        ArcResult r = classifyError(a, password, error);
        archive_read_free(a);
        return r;
    }
    archive_entry* entry = nullptr;
    ArcResult result = ArcResult::Ok;
    while (out.size() < limit) {
        int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF) break;
        if (r == ARCHIVE_RETRY) continue;
        if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
            result = classifyError(a, password, error);
            break;
        }
        ArcEntry e;
        e.name = archive_format(a) == ARCHIVE_FORMAT_RAW ? rawOutputName(path) : entryName(entry);
        e.size = (uint64_t)archive_entry_size(entry);
        e.isDir = archive_entry_filetype(entry) == AE_IFDIR;
        out.push_back(e);
    }
    archive_read_free(a);
    return result;
}

ArcResult createArchive(const std::vector<std::string>& inputs, const std::string& outPath, ArcFormat format,
                        const std::string& password, ArcProgress& prog, std::string& error) {
    std::vector<std::pair<std::string, std::string>> files;
    uint64_t total = 0;
    for (auto& in : inputs) {
        std::string p = in;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        collectFiles(p, fromDiskName(baseName(p)), files, total);
    }
    prog.total = total;
    prog.done = 0;

    archive* a = archive_write_new();
    switch (format) {
        case ArcFormat::Zip:
            archive_write_set_format_zip(a);
            if (!password.empty()) {
                if (archive_write_set_options(a, "zip:encryption=aes256") != ARCHIVE_OK ||
                    archive_write_set_passphrase(a, password.c_str()) != ARCHIVE_OK) {
                    error = archive_error_string(a) ? archive_error_string(a) : "Encryption failed";
                    archive_write_free(a);
                    return ArcResult::Error;
                }
            }
            break;
        case ArcFormat::SevenZip:
            archive_write_set_format_7zip(a);
            break;
        case ArcFormat::TarGz:
            archive_write_set_format_pax_restricted(a);
            archive_write_add_filter_gzip(a);
            break;
    }
    ArcResult result = ArcResult::Ok;
    // Names are stored as UTF-8 in every format.
    archive_write_set_options(a, format == ArcFormat::Zip ? "zip:hdrcharset=UTF-8" : "hdrcharset=UTF-8");
    if (archive_write_open_filename(a, outPath.c_str()) != ARCHIVE_OK) {
        error = archive_error_string(a) ? archive_error_string(a) : "Can't create the archive";
        archive_write_free(a);
        return ArcResult::Error;
    }
    std::vector<char> buf(kBufSize);
    for (auto& f : files) {
        if (prog.cancel) {
            result = ArcResult::Cancelled;
            break;
        }
        struct stat st;
        if (stat(f.first.c_str(), &st) != 0) continue;
        bool dir = S_ISDIR(st.st_mode);
        archive_entry* e = archive_entry_new();
        // Plain bytes: the Switch has only the C locale, where the _utf8 setters yield no name.
        archive_entry_set_pathname(e, f.second.c_str());
        archive_entry_set_filetype(e, dir ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(e, dir ? 0755 : 0644);
        archive_entry_set_size(e, dir ? 0 : st.st_size);
        archive_entry_set_mtime(e, st.st_mtime, 0);
        if (archive_write_header(a, e) < ARCHIVE_WARN) {
            error = archive_error_string(a) ? archive_error_string(a) : "Can't write the archive";
            result = ArcResult::Error;
        }
        archive_entry_free(e);
        if (result != ArcResult::Ok) break;
        if (dir) continue;
        prog.setCurrent(fromDiskName(baseName(f.first)));
        FILE* in = fopen(f.first.c_str(), "rb");
        if (!in) {
            error = trf("Can't read {0}", {fromDiskName(baseName(f.first))});
            result = ArcResult::Error;
            break;
        }
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) {
            if (archive_write_data(a, buf.data(), n) < 0) {
                error = archive_error_string(a) ? archive_error_string(a) : "Can't write the archive";
                result = ArcResult::Error;
                break;
            }
            prog.done += n;
            if (prog.cancel) {
                result = ArcResult::Cancelled;
                break;
            }
        }
        fclose(in);
        if (result != ArcResult::Ok) break;
    }
    if (result == ArcResult::Ok && archive_write_close(a) != ARCHIVE_OK) {
        error = archive_error_string(a) ? archive_error_string(a) : "Can't finish the archive";
        result = ArcResult::Error;
    }
    archive_write_free(a);
    if (result != ArcResult::Ok) unlink(outPath.c_str());
    return result;
}
