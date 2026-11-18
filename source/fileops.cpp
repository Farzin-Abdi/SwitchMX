#include "fileops.hpp"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "library.hpp"
#include "platform.hpp"
#include "i18n.hpp"

namespace {
constexpr size_t kCopyBuffer = 4 << 20;

std::string trimSlash(std::string p) {
    while (p.size() > 1 && p.back() == '/' && p[p.size() - 2] != ':') p.pop_back();
    return p;
}

std::string withSlash(std::string p) {
    if (p.empty() || p.back() != '/') p += '/';
    return p;
}

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string deviceOf(const std::string& p) {
    size_t c = p.find(":/");
    if (c != std::string::npos) return p.substr(0, c);
    return "";  // the desktop has one namespace; rename() reports EXDEV across disks
}

struct Scan {
    uint64_t bytes = 0, files = 0, folders = 0;
    uint64_t largest = 0;
    std::string largestName;
};

void scanTree(const std::string& path, Scan& s, std::atomic<bool>& cancel) {
    if (cancel) return;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        s.bytes += (uint64_t)st.st_size;
        s.files++;
        if ((uint64_t)st.st_size > s.largest) {
            s.largest = (uint64_t)st.st_size;
            s.largestName = baseName(path);
        }
        return;
    }
    s.folders++;
    DIR* d = opendir(path.c_str());
    if (!d) return;
    std::string base = withSlash(path);
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        scanTree(base + e->d_name, s, cancel);
    }
    closedir(d);
}

bool copyFile(const std::string& from, const std::string& to, FileJob& job, std::vector<char>& buf, std::string& err) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) {
        err = trf("Can't read {0}: {1}", {baseName(from), strerror(errno)});
        return false;
    }
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) {
        err = trf("Can't create {0}: {1}", {baseName(to), strerror(errno)});
        fclose(in);
        return false;
    }
    setvbuf(in, nullptr, _IONBF, 0);
    setvbuf(out, nullptr, _IONBF, 0);
    bool good = true;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) {
        if (fwrite(buf.data(), 1, n, out) != n) {
            int e = errno;
            // FAT32 stops at 4 GB; say so instead of a puzzling error.
            if (e == EFBIG || (job.progress.done > 0 && ftello(out) >= 0xFFF00000LL))
                err = trf("{0} is larger than 4 GB, which this drive's FAT32 file system can't store. "
                          "Use an exFAT or NTFS drive.",
                          {baseName(to)});
            else if (e == ENOSPC)
                err = trf("The drive is full while writing {0}", {baseName(to)});
            else
                err = trf("Can't write {0}: {1}", {baseName(to), strerror(e)});
            good = false;
            break;
        }
        job.progress.done += n;
        if (job.progress.cancel) {
            good = false;
            break;
        }
    }
    if (ferror(in)) {
        err = trf("Can't read {0}", {baseName(from)});
        good = false;
    }
    fclose(in);
    if (fclose(out) != 0 && good) {
        err = trf("Can't finish writing {0}", {baseName(to)});
        good = false;
    }
    if (!good) unlink(to.c_str());
    return good;
}

bool removeTree(const std::string& path, FileJob& job, std::string& err) {
    if (job.progress.cancel) return false;
    std::string p = trimSlash(path);
    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
        err = trf("Can't find {0}", {baseName(p)});
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        job.progress.setCurrent(baseName(p));
        if (unlink(p.c_str()) != 0) {
            err = trf("Can't delete {0}: {1}", {baseName(p), strerror(errno)});
            return false;
        }
        job.progress.done += 1;
        return true;
    }
    // Read names first: deleting while iterating is unreliable on some file systems.
    std::vector<std::string> names;
    if (DIR* d = opendir(p.c_str())) {
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
        closedir(d);
    }
    std::string base = withSlash(p);
    for (auto& n : names)
        if (!removeTree(base + n, job, err)) return false;
    if (rmdir(p.c_str()) != 0) {
        err = trf("Can't delete folder {0}: {1}", {baseName(p), strerror(errno)});
        return false;
    }
    job.progress.done += 1;
    return true;
}

bool copyTree(const std::string& from, const std::string& to, ConflictPolicy policy, bool move, FileJob& job,
              std::vector<char>& buf, std::string& err, uint64_t& skipped) {
    if (job.progress.cancel) return false;
    std::string src = trimSlash(from), dst = trimSlash(to);
    struct stat st;
    if (stat(src.c_str(), &st) != 0) {
        err = trf("Can't find {0}", {baseName(src)});
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        // Folders with the same name merge; conflicts are decided file by file.
        if (exists(dst) && !isDir(dst)) {
            if (policy == ConflictPolicy::Skip) {
                skipped++;
                return true;
            }
            if (policy == ConflictPolicy::KeepBoth) dst = uniquePath(dst);
            else unlink(dst.c_str());
        }
        if (!makeDirs(dst)) {
            err = trf("Can't create folder {0}", {baseName(dst)});
            return false;
        }
        std::vector<std::string> names;
        if (DIR* d = opendir(src.c_str())) {
            while (dirent* e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names.push_back(e->d_name);
            closedir(d);
        }
        std::string s = withSlash(src), t = withSlash(dst);
        bool allMoved = true;
        for (auto& n : names) {
            uint64_t before = skipped;
            if (!copyTree(s + n, t + n, policy, move, job, buf, err, skipped)) return false;
            if (skipped != before) allMoved = false;
        }
        if (move && allMoved) rmdir(src.c_str());
        return true;
    }
    std::string target = dst;
    if (exists(target)) {
        if (policy == ConflictPolicy::Skip) {
            skipped++;
            job.progress.done += (uint64_t)st.st_size;
            return true;
        }
        if (policy == ConflictPolicy::KeepBoth) target = uniquePath(target);
    }
    job.progress.setCurrent(baseName(src));
    if (!copyFile(src, target, job, buf, err)) return false;
    if (move && unlink(src.c_str()) != 0) {
        err = trf("Copied, but can't remove the original of {0}", {baseName(src)});
        return false;
    }
    return true;
}

void searchTree(const std::string& dir, const std::string& lowerTerm, FileJob& job, int depth) {
    if (job.progress.cancel || depth > 32 || job.foundPaths.size() >= 2000) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::string base = withSlash(dir);
    job.progress.setCurrent(base);
    std::vector<std::string> folders;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string name = e->d_name, lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        std::string full = base + name;
        // Only a stat when the directory entry doesn't say; this walks whole trees.
        bool folder = e->d_type == DT_DIR || (e->d_type != DT_REG && isDir(full));
        if (lower.find(lowerTerm) != std::string::npos && job.foundPaths.size() < 2000)
            job.foundPaths.push_back(folder ? full + "/" : full);
        if (folder) folders.push_back(full);
    }
    closedir(d);
    for (auto& f : folders) searchTree(f, lowerTerm, job, depth + 1);
}
}  // namespace

bool isProtectedPath(const std::string& path) {
    static const char* roots[] = {"sdmc:/atmosphere", "sdmc:/bootloader", "sdmc:/emuMMC",  "sdmc:/Nintendo",
                                  "sdmc:/hbmenu.nro", "sdmc:/boot.dat",   "sdmc:/boot.ini", "sdmc:/exosphere.ini",
                                  "sdmc:/payload.bin"};
    // SwitchMX's own folder: the app, its settings and the lists it keeps are protected; what's
    // made there (screenshots, caches, anything put there by hand) can be removed.
    static const char* own = "sdmc:/switch/SwitchMX";
    size_t on = strlen(own);
    if (strncasecmp(path.c_str(), own, on) == 0 && (path.size() == on || path[on] == '/')) {
        std::string rest = path.substr(std::min(path.size(), on + 1));
        while (!rest.empty() && rest.back() == '/') rest.pop_back();
        std::string first = rest.substr(0, rest.find('/'));
        static const char* keep[] = {"", "SwitchMX.nro", "config.ini", "history.tsv", "favorites.txt", "streams.tsv",
                                     "tracks.tsv", "playlists", "forwarder_build", "forwarder_asked", "mpv"};
        for (auto k : keep)
            if (strcasecmp(first.c_str(), k) == 0) return true;
        return false;
    }
    for (auto r : roots) {
        size_t n = strlen(r);
        if (strncasecmp(path.c_str(), r, n) == 0 && (path.size() == n || path[n] == '/')) return true;
    }
    return false;
}

FileJob::~FileJob() {
    if (started_) pthread_join(thread_, nullptr);
}

void* FileJob::threadMain(void* self) {
    FileJob* job = (FileJob*)self;
    job->work_(*job);
    job->finished = true;
    return nullptr;
}

void FileJob::start(std::function<void(FileJob&)> work) {
    work_ = std::move(work);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // libarchive keeps large objects on the stack.
    pthread_attr_setstacksize(&attr, 1 << 20);
    started_ = pthread_create(&thread_, &attr, threadMain, this) == 0;
    pthread_attr_destroy(&attr);
    if (!started_) {
        message = "Couldn't start the operation";
        finished = true;
    }
}

bool FileJob::askPassword(bool wrong, std::string& out) {
    std::unique_lock<std::mutex> lk(m_);
    wrongPassword = wrong;
    answered_ = false;
    needsPassword = true;
    cv_.wait(lk, [&] { return answered_ || progress.cancel.load(); });
    needsPassword = false;
    if (!answered_) return false;
    out = answer_;
    return !out.empty();
}

void FileJob::submitPassword(const std::string& password) {
    {
        std::lock_guard<std::mutex> lk(m_);
        answer_ = password;
        answered_ = true;
    }
    cv_.notify_all();
}

ArcResult FileJob::withPassword(const std::function<ArcResult(const std::string&, std::string&)>& attempt) {
    std::string password, error;
    for (;;) {
        ArcResult r = attempt(password, error);
        if (r != ArcResult::NeedPassword && r != ArcResult::WrongPassword) {
            if (r == ArcResult::Error) message = error;
            return r;
        }
        if (!askPassword(r == ArcResult::WrongPassword, password)) return ArcResult::Cancelled;
        error.clear();
    }
}

FileJob* FileJob::copy(const std::vector<std::string>& paths, const std::string& destDir, bool move,
                       ConflictPolicy policy) {
    FileJob* j = new FileJob();
    j->title = move ? "Moving" : "Copying";
    j->start([paths, destDir, move, policy](FileJob& job) {
        std::string dest = withSlash(destDir);
        // A folder can't go inside itself.
        for (auto& p : paths) {
            std::string src = withSlash(trimSlash(p));
            if (isDir(p) && dest.rfind(src, 0) == 0) {
                job.message = trf("Can't put \"{0}\" inside itself", {baseName(trimSlash(p))});
                return;
            }
        }
        // Moving within a drive is an instant rename; everything else is a copy.
        std::vector<std::string> toCopy;
        for (auto& p : paths) {
            std::string src = trimSlash(p), target = dest + baseName(src);
            if (move && deviceOf(src) == deviceOf(dest)) {
                if (src == trimSlash(target)) continue;
                if (!exists(target)) {
                    job.progress.setCurrent(baseName(src));
                    if (rename(src.c_str(), target.c_str()) == 0) continue;
                }
            }
            toCopy.push_back(p);
        }
        Scan total;
        for (auto& p : toCopy) scanTree(trimSlash(p), total, job.progress.cancel);
        job.progress.total = total.bytes;

        // FAT32 drives can't hold files of 4 GB or more: say so before copying anything.
        unsigned long long limit = platform::fileSizeLimit(dest);
        if (limit && total.largest > limit) {
            job.message = trf("\"{0}\" is {1}, but this drive is FAT32, which can't store files over 4 GB. Use an "
                              "exFAT or NTFS drive, or reformat this one as exFAT.",
                              {total.largestName, formatSize(total.largest)});
            return;
        }
        // Fail early instead of filling the drive and stopping half way.
        struct statvfs vfs;
        if (total.bytes && statvfs(trimSlash(dest).c_str(), &vfs) == 0 && vfs.f_blocks) {
            uint64_t freeBytes = (uint64_t)vfs.f_bavail * (vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize);
            if (freeBytes && total.bytes > freeBytes) {
                job.message = trf("Not enough free space: needs {0}, {1} free", {formatSize(total.bytes), formatSize(freeBytes)});
                return;
            }
        }
        std::vector<char> buf(kCopyBuffer);
        std::string err;
        uint64_t skipped = 0;
        for (auto& p : toCopy) {
            std::string src = trimSlash(p), target = dest + baseName(src);
            // Copying a file into its own folder always keeps both.
            ConflictPolicy use = trimSlash(target) == src ? ConflictPolicy::KeepBoth : policy;
            if (trimSlash(target) == src && isDir(src)) target = uniquePath(target);
            if (!copyTree(src, target, use, move, job, buf, err, skipped)) {
                job.cancelled = job.progress.cancel;
                job.message = err;
                return;
            }
        }
        job.ok = true;
        if (skipped) job.message = std::to_string(skipped) + " skipped";
    });
    return j;
}

FileJob* FileJob::remove(const std::vector<std::string>& paths) {
    FileJob* j = new FileJob();
    j->title = "Deleting";
    j->start([paths](FileJob& job) {
        Scan total;
        for (auto& p : paths) scanTree(trimSlash(p), total, job.progress.cancel);
        job.progress.total = total.files + total.folders;
        std::string err;
        for (auto& p : paths) {
            if (!removeTree(p, job, err)) {
                job.cancelled = job.progress.cancel;
                job.message = err;
                return;
            }
        }
        job.ok = true;
    });
    return j;
}

FileJob* FileJob::extract(const std::string& archive, const std::string& destDir, ConflictPolicy policy) {
    FileJob* j = new FileJob();
    j->title = "Extracting";
    j->start([archive, destDir, policy](FileJob& job) {
        ArcResult r = job.withPassword([&](const std::string& pw, std::string& err) {
            job.progress.done = 0;
            return extractArchive(archive, destDir, pw, policy, job.progress, err);
        });
        job.cancelled = r == ArcResult::Cancelled;
        job.ok = r == ArcResult::Ok;
    });
    return j;
}

FileJob* FileJob::compress(const std::vector<std::string>& paths, const std::string& output, ArcFormat format,
                           const std::string& password) {
    FileJob* j = new FileJob();
    j->title = "Compressing";
    j->start([paths, output, format, password](FileJob& job) {
        std::string err;
        ArcResult r = createArchive(paths, output, format, password, job.progress, err);
        job.cancelled = r == ArcResult::Cancelled;
        job.ok = r == ArcResult::Ok;
        if (r == ArcResult::Error) job.message = err;
    });
    return j;
}

FileJob* FileJob::measure(const std::vector<std::string>& paths) {
    FileJob* j = new FileJob();
    j->title = "Calculating size";
    j->start([paths](FileJob& job) {
        Scan s;
        for (auto& p : paths) scanTree(trimSlash(p), s, job.progress.cancel);
        job.totalBytes = s.bytes;
        job.fileCount = s.files;
        job.folderCount = s.folders;
        job.cancelled = job.progress.cancel;
        job.ok = !job.cancelled;
    });
    return j;
}

FileJob* FileJob::search(const std::string& rootDir, const std::string& term) {
    FileJob* j = new FileJob();
    j->title = "Searching";
    j->start([rootDir, term](FileJob& job) {
        std::string lower = term;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        searchTree(trimSlash(rootDir), lower, job, 0);
        std::sort(job.foundPaths.begin(), job.foundPaths.end());
        job.cancelled = job.progress.cancel;
        job.ok = !job.cancelled;
    });
    return j;
}

FileJob* FileJob::listContents(const std::string& archive) {
    FileJob* j = new FileJob();
    j->title = "Reading archive";
    j->start([archive](FileJob& job) {
        ArcResult r = job.withPassword(
            [&](const std::string& pw, std::string& err) { return listArchive(archive, pw, job.entries, err); });
        job.cancelled = r == ArcResult::Cancelled;
        job.ok = r == ArcResult::Ok;
    });
    return j;
}
