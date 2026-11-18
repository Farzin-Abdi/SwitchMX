// Finding subtitles online (OpenSubtitles.com). Each search or download runs on its own thread;
// the player checks back each frame. Requests go over FFmpeg's TLS connection, which on the Switch
// uses the console's own SSL service.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SubResult {
    std::string release, language, uploader;
    long long fileId = 0;
    int downloads = 0;
    bool hashMatch = false;  // made for exactly this file
};

class SubSearch {
public:
    enum State { Idle, Busy, Done, Failed };
    ~SubSearch();
    // A query from a file name: "@Bot_The_Blue_Planet_S01E01_1080p_BluRay" -> "The Blue Planet S01E01".
    static std::string queryFor(const std::string& path);
    void search(const std::string& apiKey, const std::string& query, const std::string& languages,
                const std::string& videoPath);
    // Saves the subtitle to `outPath`.
    void download(const std::string& apiKey, long long fileId, const std::string& outPath);
    State state() const;
    std::string message() const;  // the error, or a note such as downloads left today
    std::vector<SubResult> results() const;
    std::string savedPath() const;
    std::string takeDownloadError();  // why the last download failed, once
    bool downloading() const { return downloading_; }
    // Searches and downloads still running (the app waits for them before closing).
    static int running();

private:
    struct Shared;
    static void runDetached(std::function<void()> fn, std::shared_ptr<Shared> s);
    std::shared_ptr<Shared> s_;
    bool downloading_ = false;
};
