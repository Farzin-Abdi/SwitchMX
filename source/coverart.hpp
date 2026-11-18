// Cover art and thumbnails for the file browser. A background thread finds and decodes the art
// (next to the file, inside it, in the folder, or a frame of the video) and keeps it on disk.
#pragma once
#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "library.hpp"

struct NVGcontext;

class CoverArt {
public:
    struct Art {
        int image = 0;  // NanoVG image handle
        int w = 0, h = 0;
        float age = 0;  // seconds since it appeared, for the fade-in
        float r = 0.5f, g = 0.5f, b = 0.5f;  // its most colourful average, for backgrounds
        int blurred = 0;  // a tiny blurred copy: drawn large it makes a soft backdrop
    };

    void start(const std::string& dataDir);
    void stop();

    // Once per UI frame. `busy` pauses the work, so it can't disturb video playback.
    void frame(NVGcontext* vg, float dt, bool enabled, bool videoFrames, bool busy);
    // The art for a file, or nullptr while there is none. Asking queues the work.
    const Art* get(const std::string& path, MediaKind kind);

private:
    enum class State { Queued, Ready, None };
    struct Entry {
        State state = State::Queued;
        MediaKind kind = MediaKind::Other;
        Art art;
        uint64_t lastSeen = 0;
    };
    struct Decoded {
        std::string path;
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;  // empty when the file has no art
        bool retry = false;        // interrupted: ask again later
    };

    static void* threadMain(void* self);
    void work();
    Decoded load(const std::string& path, MediaKind kind);
    static int makeBlurred(NVGcontext* vg, const Decoded& d);

    std::string dir_;
    pthread_t thread_{};
    bool started_ = false;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false;
    std::vector<std::string> queue_;
    std::unordered_map<std::string, Entry> entries_;  // guarded by m_
    std::vector<Decoded> done_;                       // guarded by m_
    std::unordered_set<uint64_t> noArt_;              // worker thread only
    uint64_t frame_ = 0;
    bool enabled_ = true;
    int textures_ = 0;
    std::atomic<bool> paused_{false};
    std::atomic<bool> videoFrames_{true};
};
