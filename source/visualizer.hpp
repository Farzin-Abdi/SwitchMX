// Music visualizers. An Analyzer decodes the playing file a second time, ahead of playback, and
// turns it into spectrum and waveform frames stamped with their playback time; the styles draw
// those frames. mpv keeps doing the playback itself.
#pragma once
#include <pthread.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ui.hpp"

namespace vis {

constexpr int kBands = 64;
constexpr int kWave = 256;

struct Frame {
    double t = 0;           // playback time this frame describes, in seconds
    float bands[kBands] = {};  // 0..1, low to high frequencies (log spaced)
    float wave[kWave] = {};    // -1..1
    float level = 0;           // RMS, 0..1
};

// What the file is, for the quality chips (FLAC · 24-bit · 96 kHz).
struct FileInfo {
    std::string codec;
    int sampleRate = 0, bits = 0, channels = 0;
    int64_t bitrate = 0;
};

class Analyzer {
public:
    ~Analyzer() { stop(); }
    void start(const std::string& path);
    // Returns at once even if the decoder is stuck on a drive that was pulled out; the decoder
    // thread then finishes on its own, with state it shares with nobody any more.
    void stop();
    // The frame nearest to `t`. Asking also tells the decoder where playback is.
    bool sample(double t, Frame& out);
    bool info(FileInfo& out);
    const std::string& path() const;
    // Decoder threads still running (stopped ones finish on their own); the app waits for these
    // before it exits.
    static int running() { return running_; }

private:
    struct Shared {
        std::string path;
        std::atomic<bool> quit{false};
        std::atomic<double> want{0};
        std::mutex m;
        std::condition_variable cv;
        std::deque<Frame> frames;  // guarded by m
        FileInfo info;             // guarded by m
        bool haveInfo = false, done = false;
    };
    static void* threadMain(void* arg);
    static std::atomic<int> running_;
    static void work(Shared* s);
    std::shared_ptr<Shared> s_;
};

// Smoothed values the styles draw from, plus their particles, stars and rings.
struct State {
    float bands[kBands] = {}, peaks[kBands] = {}, wave[kWave] = {};
    float bass = 0, mid = 0, treble = 0, energy = 0, bassAvg = 0;
    float beat = 0;  // 1 on a beat, fading
    float sinceBeat = 10, time = 0;
    bool live = false;  // false while there is nothing to analyse (streams): an idle motion is shown
    struct Particle { float x, y, vx, vy, life, size, hue; };
    struct Star { float x, y, z, pz; };
    struct Ring { float r, a; };
    std::vector<Particle> particles;
    std::vector<Star> stars;
    std::vector<Ring> rings;
    // Recent spectra, newest first, for styles that show the music's history.
    static constexpr int kHistBands = 48, kHistLen = 26;
    std::vector<std::array<float, kHistBands>> history;
    float histTimer = 0;
};
void update(State& s, const Frame* f, float dt, bool playing);

enum class Place { Stage, Around, Backdrop, None };
int styleCount();
const char* styleName(int style);
Place placement(int style);
int paletteCount();
const char* paletteName(int palette);

struct Look {
    int palette = 0;
    NVGcolor cover, accent, accent2;
};
// A bright colour from an average image colour, so dark or grey covers still give a usable palette.
NVGcolor vivid(float r, float g, float b);

// `area` is used by stage and backdrop styles; (cx, cy, radius) by the styles around the cover.
void draw(NVGcontext* vg, int style, const Look& look, const State& s, Rect area, float cx, float cy, float radius);

}  // namespace vis
