#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

enum class ArcResult { Ok, NeedPassword, WrongPassword, Cancelled, Error };
enum class ConflictPolicy { Replace, KeepBoth, Skip };
enum class ArcFormat { Zip, SevenZip, TarGz };

struct ArcProgress {
    std::atomic<uint64_t> done{0}, total{0};  // bytes; total 0 = unknown
    std::atomic<bool> cancel{false};
    void setCurrent(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        current = s;
    }
    std::string getCurrent() {
        std::lock_guard<std::mutex> lk(m);
        return current;
    }

private:
    std::mutex m;
    std::string current;
};

struct ArcEntry {
    std::string name;
    uint64_t size = 0;
    bool isDir = false;
};

// True for anything worth offering "Extract" on (by name; the content decides the rest).
bool isArchiveName(const std::string& name);

// All entry points run on a worker thread and are safe to cancel through `progress`.
ArcResult extractArchive(const std::string& path, const std::string& destDir, const std::string& password,
                         ConflictPolicy policy, ArcProgress& progress, std::string& error);
ArcResult listArchive(const std::string& path, const std::string& password, std::vector<ArcEntry>& out,
                      std::string& error, size_t limit = 2000);
// `inputs` are files or folders; names inside the archive are relative to each input's parent.
// A password is only supported for ZIP (AES-256).
ArcResult createArchive(const std::vector<std::string>& inputs, const std::string& outPath, ArcFormat format,
                        const std::string& password, ArcProgress& progress, std::string& error);

// Helpers shared with file operations.
bool makeDirs(const std::string& dir);
std::string uniquePath(const std::string& path);  // "name (1).ext" if the path exists
