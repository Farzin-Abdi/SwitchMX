#pragma once
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "archiver.hpp"

// One background file operation. The UI polls it and keeps running while it works.
class FileJob {
public:
    ~FileJob();

    static FileJob* copy(const std::vector<std::string>& paths, const std::string& destDir, bool move,
                         ConflictPolicy policy);
    static FileJob* remove(const std::vector<std::string>& paths);
    static FileJob* extract(const std::string& archive, const std::string& destDir, ConflictPolicy policy);
    static FileJob* compress(const std::vector<std::string>& paths, const std::string& output, ArcFormat format,
                             const std::string& password);
    static FileJob* measure(const std::vector<std::string>& paths);
    static FileJob* search(const std::string& rootDir, const std::string& term);
    static FileJob* listContents(const std::string& archive);

    std::string title;
    ArcProgress progress;

    std::atomic<bool> finished{false};
    std::string message;              // error or summary, set before finished
    bool ok = false;
    bool cancelled = false;

    // Password handshake: the worker waits here while the UI asks the user.
    std::atomic<bool> needsPassword{false};
    bool wrongPassword = false;
    void submitPassword(const std::string& password);  // empty = give up

    // Results
    uint64_t totalBytes = 0;
    uint64_t fileCount = 0, folderCount = 0;
    std::vector<std::string> foundPaths;
    std::vector<ArcEntry> entries;

private:
    FileJob() = default;
    void start(std::function<void(FileJob&)> work);
    bool askPassword(bool wrong, std::string& out);
    ArcResult withPassword(const std::function<ArcResult(const std::string&, std::string&)>& attempt);
    static void* threadMain(void* self);

    std::function<void(FileJob&)> work_;
    pthread_t thread_{};
    bool started_ = false;
    std::mutex m_;
    std::condition_variable cv_;
    bool answered_ = false;
    std::string answer_;
};

// Top-level SD card items that can stop the console or homebrew from booting if changed.
bool isProtectedPath(const std::string& path);
