#include "visualizer.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "platform.hpp"

namespace vis {
namespace {
constexpr float PI = 3.14159265f;
constexpr int kFft = 2048;            // samples per analysis window
constexpr double kFps = 60;           // analysis frames per second of audio
constexpr double kAhead = 2.5;        // seconds decoded ahead of playback
constexpr double kBehind = 0.6;       // seconds kept behind it
constexpr float kMinDb = -72, kMaxDb = -12;

// ------------------------------------------------------------------ FFT

struct Fft {
    float cosT[kFft / 2], sinT[kFft / 2], window[kFft];
    int rev[kFft];
    Fft() {
        int bits = 0;
        while ((1 << bits) < kFft) bits++;
        for (int i = 0; i < kFft; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[i] = r;
            window[i] = 0.5f - 0.5f * std::cos(2 * PI * i / (kFft - 1));
        }
        for (int i = 0; i < kFft / 2; i++) {
            cosT[i] = std::cos(2 * PI * i / kFft);
            sinT[i] = -std::sin(2 * PI * i / kFft);
        }
    }
    // In place, re/im of length kFft.
    void run(float* re, float* im) const {
        for (int i = 0; i < kFft; i++)
            if (rev[i] > i) {
                std::swap(re[i], re[rev[i]]);
                std::swap(im[i], im[rev[i]]);
            }
        for (int len = 2; len <= kFft; len <<= 1) {
            int half = len / 2, step = kFft / len;
            for (int i = 0; i < kFft; i += len)
                for (int j = 0; j < half; j++) {
                    float wr = cosT[j * step], wi = sinT[j * step];
                    float xr = re[i + j + half] * wr - im[i + j + half] * wi;
                    float xi = re[i + j + half] * wi + im[i + j + half] * wr;
                    re[i + j + half] = re[i + j] - xr;
                    im[i + j + half] = im[i + j] - xi;
                    re[i + j] += xr;
                    im[i + j] += xi;
                }
        }
    }
};

const Fft& fft() {
    static Fft f;
    return f;
}

void analyse(const float* ring, size_t ringPos, int rate, Frame& out) {
    const Fft& F = fft();
    static thread_local float re[kFft], im[kFft];
    float sum = 0;
    for (int i = 0; i < kFft; i++) {
        float v = ring[(ringPos + i) % kFft];
        sum += v * v;
        re[i] = v * F.window[i];
        im[i] = 0;
    }
    out.level = std::min(1.0f, std::sqrt(sum / kFft) * 2.2f);
    // The waveform shows the newest part of the window.
    for (int i = 0; i < kWave; i++) {
        size_t a = ringPos + kFft - 2 * kWave + 2 * i;
        out.wave[i] = 0.5f * (ring[a % kFft] + ring[(a + 1) % kFft]);
    }
    F.run(re, im);
    float lo = 40, hi = std::min(16000.0f, rate * 0.45f);
    float binHz = (float)rate / kFft;
    for (int b = 0; b < kBands; b++) {
        float f0 = lo * std::pow(hi / lo, (float)b / kBands), f1 = lo * std::pow(hi / lo, (float)(b + 1) / kBands);
        int i0 = std::max(1, (int)(f0 / binHz)), i1 = std::max(i0, (int)(f1 / binHz));
        float mag = 0;
        for (int i = i0; i <= i1 && i < kFft / 2; i++) mag = std::max(mag, std::sqrt(re[i] * re[i] + im[i] * im[i]));
        float db = 20 * std::log10(mag / (kFft / 4.0f) + 1e-9f);
        // Music has less energy up high; tilt so treble is visible too.
        db += 4.5f * std::log2(std::max(f0, 60.0f) / 800.0f);
        out.bands[b] = std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
    }
}

// ------------------------------------------------------------------ file reading

struct Io {
    FILE* f = nullptr;
    const std::atomic<bool>* quit = nullptr;
};
int ioRead(void* o, uint8_t* buf, int size) {
    size_t n = fread(buf, 1, (size_t)size, ((Io*)o)->f);
    return n > 0 ? (int)n : AVERROR_EOF;
}
int64_t ioSeek(void* o, int64_t offset, int whence) {
    FILE* f = ((Io*)o)->f;
    if (whence == AVSEEK_SIZE) {
        off_t cur = ftello(f);
        fseeko(f, 0, SEEK_END);
        off_t end = ftello(f);
        fseeko(f, cur, SEEK_SET);
        return end;
    }
    if (fseeko(f, offset, whence & ~AVSEEK_FORCE) != 0) return -1;
    return ftello(f);
}
int ioInterrupt(void* o) { return ((Io*)o)->quit->load() ? 1 : 0; }

// Mono float samples from any decoded layout.
void appendMono(const AVFrame* fr, std::vector<float>& out) {
    int ch = std::max(1, fr->ch_layout.nb_channels), n = fr->nb_samples;
    auto fmt = (AVSampleFormat)fr->format;
    bool planar = av_sample_fmt_is_planar(fmt);
    AVSampleFormat packed = av_get_packed_sample_fmt(fmt);
    out.resize((size_t)n);
    for (int i = 0; i < n; i++) {
        float acc = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t* base = planar ? fr->extended_data[c] : fr->extended_data[0];
            int idx = planar ? i : i * ch + c;
            switch (packed) {
                case AV_SAMPLE_FMT_U8: acc += (((const uint8_t*)base)[idx] - 128) / 128.0f; break;
                case AV_SAMPLE_FMT_S16: acc += ((const int16_t*)base)[idx] / 32768.0f; break;
                case AV_SAMPLE_FMT_S32: acc += ((const int32_t*)base)[idx] / 2147483648.0f; break;
                case AV_SAMPLE_FMT_FLT: acc += ((const float*)base)[idx]; break;
                case AV_SAMPLE_FMT_DBL: acc += (float)((const double*)base)[idx]; break;
                default: break;
            }
        }
        out[i] = acc / ch;
    }
}
}  // namespace

// ------------------------------------------------------------------ Analyzer

void Analyzer::start(const std::string& path) {
    stop();
    auto shared = std::make_shared<Shared>();
    shared->path = path;
    auto* arg = new std::shared_ptr<Shared>(shared);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (pthread_create(&thread, &attr, threadMain, arg) == 0) s_ = shared;
    else delete arg;
    pthread_attr_destroy(&attr);
}

void Analyzer::stop() {
    if (!s_) return;
    // No waiting: the decoder thread sees `quit`, finishes on its own and frees what it shares.
    s_->quit = true;
    s_->cv.notify_all();
    s_.reset();
}

const std::string& Analyzer::path() const {
    static const std::string none;
    return s_ ? s_->path : none;
}

std::atomic<int> Analyzer::running_{0};

void* Analyzer::threadMain(void* arg) {
    struct Count {
        Count() { running_++; }
        ~Count() { running_--; }
    } count;
    auto* holder = (std::shared_ptr<Shared>*)arg;
    std::shared_ptr<Shared> s = *holder;
    delete holder;
    platform::lowerThreadPriority();
    work(s.get());
    {
        std::lock_guard<std::mutex> lk(s->m);
        s->done = true;
    }
    s->cv.notify_all();
    return nullptr;
}

bool Analyzer::sample(double t, Frame& out) {
    if (!s_) return false;
    s_->want = t;
    s_->cv.notify_all();
    std::lock_guard<std::mutex> lk(s_->m);
    auto& frames = s_->frames;
    if (frames.empty() || t < frames.front().t - 0.25 || t > frames.back().t + 0.25) return false;
    auto it = std::lower_bound(frames.begin(), frames.end(), t, [](const Frame& f, double v) { return f.t < v; });
    if (it == frames.end()) --it;
    out = *it;
    return true;
}

bool Analyzer::info(FileInfo& out) {
    if (!s_) return false;
    std::lock_guard<std::mutex> lk(s_->m);
    if (!s_->haveInfo) return false;
    out = s_->info;
    return true;
}

void Analyzer::work(Shared* s) {
    Io io;
    io.f = fopen(s->path.c_str(), "rb");
    if (!io.f) return;
    io.quit = &s->quit;
    AVFormatContext* fmt = avformat_alloc_context();
    uint8_t* buf = (uint8_t*)av_malloc(64 * 1024);
    AVIOContext* avio = buf ? avio_alloc_context(buf, 64 * 1024, 0, &io, ioRead, nullptr, ioSeek) : nullptr;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int stream = -1;
    if (!fmt || !avio || !pkt || !frame) goto done;
    fmt->pb = avio;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    fmt->interrupt_callback.callback = ioInterrupt;
    fmt->interrupt_callback.opaque = &io;
    if (avformat_open_input(&fmt, "", nullptr, nullptr) != 0) {
        fmt = nullptr;  // freed by FFmpeg
        goto done;
    }
    avformat_find_stream_info(fmt, nullptr);
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (stream < 0) goto done;
    {
        AVCodecParameters* par = fmt->streams[stream]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec || !(dec = avcodec_alloc_context3(codec))) goto done;
        avcodec_parameters_to_context(dec, par);
        dec->thread_count = 1;
        if (avcodec_open2(dec, codec, nullptr) != 0) goto done;
        for (unsigned i = 0; i < fmt->nb_streams; i++)
            if ((int)i != stream) fmt->streams[i]->discard = AVDISCARD_ALL;
        std::lock_guard<std::mutex> lk(s->m);
        s->info.codec = codec->name ? codec->name : "";
        s->info.sampleRate = par->sample_rate;
        s->info.channels = par->ch_layout.nb_channels;
        s->info.bits = par->bits_per_raw_sample ? par->bits_per_raw_sample : par->bits_per_coded_sample;
        s->info.bitrate = par->bit_rate ? par->bit_rate : fmt->bit_rate;
        s->haveInfo = true;
    }
    {
        AVStream* st = fmt->streams[stream];
        double tb = av_q2d(st->time_base);
        int rate = std::max(8000, dec->sample_rate);
        int hop = std::max(1, (int)(rate / kFps));
        std::vector<float> ring(kFft, 0.0f), mono;
        size_t ringPos = 0;  // where the oldest sample is
        int sinceHop = 0;
        double clock = 0;    // time of the next decoded sample
        bool eof = false;
        Frame f;
        while (!s->quit) {
            double want = s->want;
            bool needSeek = false, ahead = false;
            {
                std::unique_lock<std::mutex> lk(s->m);
                while (!s->frames.empty() && s->frames.front().t < want - kBehind) s->frames.pop_front();
                if (!s->frames.empty()) {
                    needSeek = want < s->frames.front().t - 0.3 || want > s->frames.back().t + 1.0;
                    ahead = s->frames.back().t > want + kAhead;
                } else {
                    needSeek = std::fabs(clock - want) > 1.0;
                }
                if (!needSeek && (ahead || eof)) {
                    s->cv.wait_for(lk, std::chrono::milliseconds(40));
                    continue;
                }
                if (needSeek) s->frames.clear();
            }
            if (needSeek) {
                int64_t ts = (int64_t)(std::max(0.0, want - 0.3) * AV_TIME_BASE);
                av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(dec);
                std::fill(ring.begin(), ring.end(), 0.0f);
                sinceHop = 0;
                clock = std::max(0.0, want - 0.3);
                eof = false;
                continue;
            }
            int r = av_read_frame(fmt, pkt);
            if (r < 0) {
                eof = true;
                continue;
            }
            if (pkt->stream_index != stream) {
                av_packet_unref(pkt);
                continue;
            }
            avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            while (avcodec_receive_frame(dec, frame) == 0) {
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE) clock = frame->best_effort_timestamp * tb;
                appendMono(frame, mono);
                std::vector<Frame> made;
                for (size_t i = 0; i < mono.size(); i++) {
                    ring[ringPos] = mono[i];
                    ringPos = (ringPos + 1) % kFft;
                    if (++sinceHop >= hop) {
                        sinceHop = 0;
                        analyse(ring.data(), ringPos, rate, f);
                        // The window is centred half a window before its newest sample.
                        f.t = clock + (double)(i + 1) / rate - kFft / 2.0 / rate;
                        made.push_back(f);
                    }
                }
                clock += (double)mono.size() / rate;
                av_frame_unref(frame);
                if (!made.empty()) {
                    std::lock_guard<std::mutex> lk(s->m);
                    for (auto& m : made)
                        if (s->frames.empty() || m.t > s->frames.back().t) s->frames.push_back(m);
                    while (s->frames.size() > 600) s->frames.pop_front();
                }
            }
        }
    }
done:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    if (avio) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    } else {
        av_free(buf);
    }
    fclose(io.f);
}

// ------------------------------------------------------------------ smoothing

void update(State& s, const Frame* f, float dt, bool playing) {
    s.time += dt;
    s.live = f != nullptr;
    float target[kBands], wave[kWave];
    if (f && playing) {
        std::copy(f->bands, f->bands + kBands, target);
        std::copy(f->wave, f->wave + kWave, wave);
    } else if (!f && playing) {
        // Nothing to analyse (a stream): a calm motion so the screen still breathes.
        for (int i = 0; i < kBands; i++)
            target[i] = 0.18f + 0.12f * std::sin(s.time * 1.7f + i * 0.35f) * std::sin(s.time * 0.6f + i * 0.11f);
        for (int i = 0; i < kWave; i++) wave[i] = 0.12f * std::sin(s.time * 3 + i * 0.1f);
    } else {
        std::fill(target, target + kBands, 0.0f);
        std::fill(wave, wave + kWave, 0.0f);
    }
    float up = std::min(1.0f, dt * 32), down = std::min(1.0f, dt * 7);
    for (int i = 0; i < kBands; i++) {
        float& v = s.bands[i];
        v += (target[i] - v) * (target[i] > v ? up : down);
        if (v > s.peaks[i]) s.peaks[i] = v;
        else s.peaks[i] = std::max(v, s.peaks[i] - dt * 0.35f);
    }
    float w = std::min(1.0f, dt * 28);
    for (int i = 0; i < kWave; i++) s.wave[i] += (wave[i] - s.wave[i]) * w;

    auto avg = [&](int a, int b) {
        float t = 0;
        for (int i = a; i < b; i++) t += s.bands[i];
        return t / (b - a);
    };
    s.bass = avg(0, 8);
    s.mid = avg(8, 36);
    s.treble = avg(36, kBands);
    s.energy = avg(0, kBands);
    s.bassAvg += (s.bass - s.bassAvg) * std::min(1.0f, dt * 1.5f);
    s.sinceBeat += dt;
    s.beat = std::max(0.0f, s.beat - dt * 3.2f);
    if (playing && s.bass > 0.28f && s.bass > s.bassAvg * 1.18f && s.sinceBeat > 0.2f) {
        s.beat = 1;
        s.sinceBeat = 0;
        if (s.rings.size() < 8) s.rings.push_back({0, 1});
    }

    // Particles rise from the bottom, faster and more of them with the music.
    if (s.particles.size() < 150 && playing) {
        int spawn = (int)(dt * (20 + 260 * s.energy) + (s.beat > 0.95f ? 24 : 0));
        for (int i = 0; i < spawn && s.particles.size() < 150; i++) {
            float rx = (float)((i * 7919 + (int)(s.time * 1000)) % 1000) / 1000.0f;
            float ry = (float)((i * 104729 + (int)(s.time * 777)) % 1000) / 1000.0f;
            s.particles.push_back({rx, 1.05f, (ry - 0.5f) * 0.12f, -(0.15f + 0.6f * s.bass + 0.3f * ry), 1.0f,
                                   2 + 5 * ry, rx});
        }
    }
    for (auto& p : s.particles) {
        p.x += p.vx * dt;
        p.y += p.vy * dt * (1 + 2 * s.beat);
        p.life -= dt * 0.45f;
    }
    s.particles.erase(std::remove_if(s.particles.begin(), s.particles.end(),
                                     [](const State::Particle& p) { return p.life <= 0 || p.y < -0.1f; }),
                      s.particles.end());

    if (s.stars.empty()) {
        for (int i = 0; i < 240; i++) {
            float a = (float)((i * 2654435761u) % 10000) / 10000.0f, b = (float)((i * 40503u) % 10000) / 10000.0f;
            s.stars.push_back({a * 2 - 1, b * 2 - 1, 0.05f + (float)(i % 97) / 97.0f, 1});
        }
    }
    float speed = playing ? 0.12f + 1.6f * s.energy + 1.2f * s.beat : 0.04f;
    for (auto& st : s.stars) {
        st.pz = st.z;
        st.z -= dt * speed;
        if (st.z <= 0.03f) {
            st.z = 1;
            st.pz = 1;
        }
    }
    for (auto& r : s.rings) {
        r.r += dt * (0.9f + s.energy);
        r.a -= dt * 0.7f;
    }
    s.rings.erase(std::remove_if(s.rings.begin(), s.rings.end(), [](const State::Ring& r) { return r.a <= 0; }),
                  s.rings.end());

    // A snapshot of the spectrum 24 times a second; paused music keeps its history still.
    s.histTimer -= dt;
    if (s.histTimer <= 0 && playing) {
        s.histTimer = 1.0f / 24;
        std::array<float, State::kHistBands> row;
        for (int i = 0; i < State::kHistBands; i++) {
            float f = (float)i / (State::kHistBands - 1) * (kBands - 1);
            int k = (int)f;
            float fr = f - k;
            row[i] = k + 1 < kBands ? s.bands[k] * (1 - fr) + s.bands[k + 1] * fr : s.bands[kBands - 1];
        }
        s.history.insert(s.history.begin(), row);
        if ((int)s.history.size() > State::kHistLen) s.history.pop_back();
    }
}

// ------------------------------------------------------------------ styles

namespace {
struct StyleDef {
    const char* name;
    Place place;
};
const StyleDef kStyles[] = {
    {"Bars", Place::Stage},           {"Radial", Place::Around},       {"Neon bars", Place::Stage},
    {"Mountains", Place::Stage},      {"Ring wave", Place::Around},    {"Oscilloscope", Place::Stage},
    {"Particles", Place::Backdrop},   {"Mirror", Place::Stage},        {"Pulse", Place::Around},
    {"Spectrum line", Place::Stage},  {"Starfield", Place::Backdrop},  {"LED matrix", Place::Stage},
    {"Aurora", Place::Backdrop},      {"Halo", Place::Around},         {"Orbit", Place::Around},
    {"Ridges", Place::Stage},         {"Rays", Place::Backdrop},       {"Kaleidoscope", Place::Backdrop},
    {"Off", Place::None},
};
const char* kPalettes[] = {"Album colours", "Theme", "Rainbow", "Sunset", "Ocean", "Neon", "Fire", "White"};

NVGcolor lerp3(NVGcolor a, NVGcolor b, NVGcolor c, float t) {
    return t < 0.5f ? nvgLerpRGBA(a, b, t * 2) : nvgLerpRGBA(b, c, (t - 0.5f) * 2);
}

NVGcolor pal(const Look& L, float t, float alpha) {
    t = std::clamp(t, 0.0f, 1.0f);
    NVGcolor c;
    switch (L.palette) {
        case 0: c = lerp3(L.cover, nvgLerpRGBA(L.cover, L.accent2, 0.5f), nvgLerpRGBA(L.cover, nvgRGB(255, 255, 255), 0.6f), t); break;
        case 1: c = lerp3(L.accent, L.accent2, nvgLerpRGBA(L.accent2, nvgRGB(255, 255, 255), 0.4f), t); break;
        case 2: c = nvgHSL(t * 0.83f, 0.85f, 0.6f); break;
        case 3: c = lerp3(nvgRGB(255, 61, 127), nvgRGB(255, 138, 61), nvgRGB(255, 210, 61), t); break;
        case 4: c = lerp3(nvgRGB(40, 70, 255), nvgRGB(0, 200, 255), nvgRGB(61, 255, 210), t); break;
        case 5: c = lerp3(nvgRGB(255, 43, 214), nvgRGB(122, 92, 255), nvgRGB(0, 240, 255), t); break;
        case 6: c = lerp3(nvgRGB(255, 42, 0), nvgRGB(255, 128, 0), nvgRGB(255, 225, 77), t); break;
        default: c = nvgRGB(240, 242, 248); break;
    }
    c.a = alpha;
    return c;
}

// Smooth curve through the points (Catmull-Rom as Béziers). The path must already be begun.
void curveThrough(NVGcontext* vg, const std::vector<float>& xs, const std::vector<float>& ys, bool moveFirst) {
    size_t n = xs.size();
    if (n < 2) return;
    if (moveFirst) nvgMoveTo(vg, xs[0], ys[0]);
    else nvgLineTo(vg, xs[0], ys[0]);
    for (size_t i = 0; i + 1 < n; i++) {
        size_t p0 = i ? i - 1 : 0, p3 = std::min(n - 1, i + 2);
        float c1x = xs[i] + (xs[i + 1] - xs[p0]) / 6, c1y = ys[i] + (ys[i + 1] - ys[p0]) / 6;
        float c2x = xs[i + 1] - (xs[p3] - xs[i]) / 6, c2y = ys[i + 1] - (ys[p3] - ys[i]) / 6;
        nvgBezierTo(vg, c1x, c1y, c2x, c2y, xs[i + 1], ys[i + 1]);
    }
}

// Bands resampled to n values, so every style can choose its own resolution.
float bandAt(const State& s, float x) {
    float f = std::clamp(x, 0.0f, 1.0f) * (kBands - 1);
    int i = (int)f;
    float fr = f - i;
    return i + 1 < kBands ? s.bands[i] * (1 - fr) + s.bands[i + 1] * fr : s.bands[kBands - 1];
}
float peakAt(const State& s, float x) {
    int i = std::clamp((int)(x * (kBands - 1) + 0.5f), 0, kBands - 1);
    return s.peaks[i];
}

void glowLine(NVGcontext* vg, NVGpaint paint, float width) {
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    nvgStrokePaint(vg, paint);
    nvgGlobalAlpha(vg, 0.22f);
    nvgStrokeWidth(vg, width * 4);
    nvgStroke(vg);
    nvgGlobalAlpha(vg, 0.4f);
    nvgStrokeWidth(vg, width * 2);
    nvgStroke(vg);
    nvgRestore(vg);
    nvgStrokePaint(vg, paint);
    nvgStrokeWidth(vg, width);
    nvgStroke(vg);
}

void drawBars(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    int n = std::max(24, (int)(a.w / 20));
    float slot = a.w / n, bw = slot * 0.66f;
    float base = a.y + a.h * 0.78f, maxH = a.h * 0.74f;
    for (int i = 0; i < n; i++) {
        float x = a.x + i * slot + (slot - bw) / 2, t = (float)i / (n - 1);
        float h = std::max(3.0f, bandAt(s, t) * maxH);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, base - h, bw, h, std::min(bw / 2, 4.0f));
        nvgFillPaint(vg, nvgLinearGradient(vg, 0, base - maxH, 0, base, pal(L, t, 1), pal(L, t * 0.6f, 0.85f)));
        nvgFill(vg);
        // A cap that falls slowly after each peak.
        float ph = peakAt(s, t) * maxH;
        if (ph > h + 4) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, base - ph - 3, bw, 3, 1.5f);
            nvgFillColor(vg, pal(L, t, 0.9f));
            nvgFill(vg);
        }
        // Reflection
        float rh = std::min(h * 0.45f, a.bottom() - base - 2);
        if (rh > 2) {
            nvgBeginPath(vg);
            nvgRect(vg, x, base + 3, bw, rh);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, base, 0, base + rh, pal(L, t, 0.28f), pal(L, t, 0)));
            nvgFill(vg);
        }
    }
}

void drawNeonBars(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    int n = std::max(32, (int)(a.w / 13));
    float slot = a.w / n, bw = std::max(2.0f, slot * 0.38f);
    float base = a.bottom() - 6, maxH = a.h * 0.9f;
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (n - 1), x = a.x + i * slot + slot / 2;
        float h = std::max(2.0f, bandAt(s, t) * maxH);
        NVGcolor c = pal(L, t, 1);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x - bw * 1.8f, base - h - 4, bw * 3.6f, h + 8, bw * 1.8f);
        nvgFillColor(vg, pal(L, t, 0.14f));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x - bw / 2, base - h, bw, h, bw / 2);
        nvgFillColor(vg, c);
        nvgFill(vg);
        float ph = peakAt(s, t) * maxH;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x - bw, base - ph - 8, bw * 2, 3, 1.5f);
        nvgFillColor(vg, nvgLerpRGBA(c, nvgRGB(255, 255, 255), 0.6f));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

void drawMountains(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    const int n = 40;
    for (int layer = 0; layer < 3; layer++) {
        std::vector<float> xs(n), ys(n);
        float scale = 0.55f + 0.22f * layer, shift = layer * 0.07f;
        for (int i = 0; i < n; i++) {
            float t = (float)i / (n - 1);
            xs[i] = a.x + t * a.w;
            float v = bandAt(s, std::fmod(t + shift, 1.0f));
            ys[i] = a.bottom() - 4 - v * a.h * scale - 6;
        }
        nvgBeginPath(vg);
        nvgMoveTo(vg, a.x, a.bottom());
        curveThrough(vg, xs, ys, false);
        nvgLineTo(vg, a.right(), a.bottom());
        nvgClosePath(vg);
        float alpha = 0.3f + 0.25f * (2 - layer);
        nvgFillPaint(vg, nvgLinearGradient(vg, a.x, 0, a.right(), 0, pal(L, 0.1f + layer * 0.3f, alpha),
                                          pal(L, 0.5f + layer * 0.25f, alpha)));
        nvgFill(vg);
    }
}

void drawOscilloscope(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    float cy = a.cy(), amp = a.h * 0.42f;
    nvgBeginPath(vg);
    for (int i = 0; i < kWave; i++) {
        float x = a.x + a.w * i / (kWave - 1), y = cy + std::clamp(s.wave[i] * 1.6f, -1.0f, 1.0f) * amp;
        if (i == 0) nvgMoveTo(vg, x, y);
        else nvgLineTo(vg, x, y);
    }
    nvgLineJoin(vg, NVG_ROUND);
    glowLine(vg, nvgLinearGradient(vg, a.x, 0, a.right(), 0, pal(L, 0, 1), pal(L, 1, 1)), 2.5f);
}

void drawMirror(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    const int n = 48;
    float cy = a.cy(), half = a.h * 0.46f;
    std::vector<float> xs(n), top(n), bottom(n);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (n - 1);
        float v = bandAt(s, std::fabs(t - 0.5f) * 2);  // low notes in the middle
        xs[i] = a.x + t * a.w;
        top[i] = cy - 2 - v * half;
        bottom[i] = cy + 2 + v * half;
    }
    nvgBeginPath(vg);
    curveThrough(vg, xs, top, true);
    std::reverse(xs.begin(), xs.end());
    std::reverse(bottom.begin(), bottom.end());
    curveThrough(vg, xs, bottom, false);
    nvgClosePath(vg);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, cy - half, 0, cy + half, pal(L, 0.9f, 0.9f), pal(L, 0.2f, 0.9f)));
    nvgFill(vg);
    // A lit edge along the top half.
    std::reverse(xs.begin(), xs.end());
    nvgBeginPath(vg);
    curveThrough(vg, xs, top, true);
    nvgLineJoin(vg, NVG_ROUND);
    glowLine(vg, nvgLinearGradient(vg, 0, cy - half, 0, cy, pal(L, 1, 0.9f), pal(L, 0.6f, 0.6f)), 1.6f);
    nvgBeginPath(vg);
    nvgRect(vg, a.x, cy - 1, a.w, 2);
    nvgFillColor(vg, pal(L, 1, 0.6f));
    nvgFill(vg);
}

void drawSpectrumLine(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    const int n = 56;
    std::vector<float> xs(n), ys(n);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (n - 1);
        xs[i] = a.x + t * a.w;
        ys[i] = a.bottom() - 8 - bandAt(s, t) * a.h * 0.85f;
    }
    nvgBeginPath(vg);
    nvgMoveTo(vg, a.x, a.bottom());
    curveThrough(vg, xs, ys, false);
    nvgLineTo(vg, a.right(), a.bottom());
    nvgClosePath(vg);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, a.y, 0, a.bottom(), pal(L, 0.7f, 0.45f), pal(L, 0.2f, 0.02f)));
    nvgFill(vg);
    nvgBeginPath(vg);
    curveThrough(vg, xs, ys, true);
    glowLine(vg, nvgLinearGradient(vg, a.x, 0, a.right(), 0, pal(L, 0, 1), pal(L, 1, 1)), 2.5f);
}

void drawLed(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    int cols = std::max(16, (int)(a.w / 22)), rows = std::max(8, (int)(a.h / 16));
    float cw = a.w / cols, rh = a.h / rows, r = std::min(cw, rh) * 0.32f;
    std::vector<int> lit(cols), peak(cols);
    for (int c = 0; c < cols; c++) {
        float t = (float)c / (cols - 1);
        lit[c] = (int)std::round(bandAt(s, t) * rows);
        peak[c] = (int)std::round(peakAt(s, t) * rows);
    }
    // One path per row and one for all unlit dots: a fill per dot would be hundreds of draw calls.
    nvgBeginPath(vg);
    for (int y = 0; y < rows; y++)
        for (int c = 0; c < cols; c++)
            if (!(y < lit[c] || y == peak[c] - 1)) nvgCircle(vg, a.x + c * cw + cw / 2, a.bottom() - y * rh - rh / 2, r);
    nvgFillColor(vg, nvgRGBAf(1, 1, 1, 0.06f));
    nvgFill(vg);
    for (int y = 0; y < rows; y++) {
        nvgBeginPath(vg);
        bool any = false;
        for (int c = 0; c < cols; c++)
            if (y < lit[c] || y == peak[c] - 1) {
                nvgCircle(vg, a.x + c * cw + cw / 2, a.bottom() - y * rh - rh / 2, r);
                any = true;
            }
        if (!any) continue;
        nvgFillColor(vg, pal(L, (float)y / rows, 1));
        nvgFill(vg);
    }
}

// Bars all the way round the cover, mirrored left to right so both halves match. Drawn as one
// glow pass and one sharp pass, each a single path coloured from root to tip.
void drawRadial(NVGcontext* vg, const Look& L, const State& s, float cx, float cy, float R) {
    const int n = 96;
    float r0 = R + 9 + s.beat * 3, len = R * 0.36f;
    float bw = std::max(2.5f, 2 * PI * r0 / n * 0.52f);
    auto spokes = [&](float extra) {
        nvgBeginPath(vg);
        for (int i = 0; i < n; i++) {
            float t = (float)i / n;
            float v = bandAt(s, std::fabs(t - 0.5f) * 2);
            float ang = -PI / 2 + t * 2 * PI;
            float r1 = r0 + 3 + v * len + extra;
            nvgMoveTo(vg, cx + std::cos(ang) * r0, cy + std::sin(ang) * r0);
            nvgLineTo(vg, cx + std::cos(ang) * r1, cy + std::sin(ang) * r1);
        }
    };
    NVGpaint paint = nvgRadialGradient(vg, cx, cy, r0, r0 + len, pal(L, 0.15f, 1), pal(L, 0.95f, 1));
    nvgSave(vg);
    nvgLineCap(vg, NVG_ROUND);
    // Soft light behind, swelling with the bass.
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r0 + len * 1.2f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, R * 0.95f, r0 + len * 1.2f,
                                       pal(L, 0.4f, 0.16f + 0.22f * s.bass + 0.12f * s.beat), pal(L, 0.4f, 0)));
    nvgFill(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    spokes(2);
    nvgStrokePaint(vg, paint);
    nvgGlobalAlpha(vg, 0.28f);
    nvgStrokeWidth(vg, bw * 2.4f);
    nvgStroke(vg);
    nvgRestore(vg);
    nvgSave(vg);
    nvgLineCap(vg, NVG_ROUND);
    spokes(0);
    nvgStrokePaint(vg, paint);
    nvgStrokeWidth(vg, bw);
    nvgStroke(vg);
    // Falling peak dots and a fine inner ring.
    nvgBeginPath(vg);
    for (int i = 0; i < n; i += 2) {
        float t = (float)i / n;
        float pv = peakAt(s, std::fabs(t - 0.5f) * 2);
        if (pv < 0.04f) continue;
        float ang = -PI / 2 + t * 2 * PI, r = r0 + 8 + pv * len;
        nvgCircle(vg, cx + std::cos(ang) * r, cy + std::sin(ang) * r, bw * 0.45f);
    }
    nvgFillColor(vg, pal(L, 1, 0.85f));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r0 - 4);
    nvgStrokeColor(vg, pal(L, 0.6f, 0.35f + 0.4f * s.beat));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
    nvgRestore(vg);
}

// A smooth closed curve through points around a centre (Catmull-Rom, wrapping round).
void closedCurve(NVGcontext* vg, const std::vector<float>& xs, const std::vector<float>& ys) {
    size_t n = xs.size();
    nvgMoveTo(vg, xs[0], ys[0]);
    for (size_t i = 0; i < n; i++) {
        size_t p0 = (i + n - 1) % n, p1 = i, p2 = (i + 1) % n, p3 = (i + 2) % n;
        nvgBezierTo(vg, xs[p1] + (xs[p2] - xs[p0]) / 6, ys[p1] + (ys[p2] - ys[p0]) / 6,
                    xs[p2] - (xs[p3] - xs[p1]) / 6, ys[p2] - (ys[p3] - ys[p1]) / 6, xs[p2], ys[p2]);
    }
    nvgClosePath(vg);
}

// Three soft, slowly turning shapes round the cover: bass, mids and treble.
void drawHalo(NVGcontext* vg, const Look& L, const State& s, float cx, float cy, float R) {
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    const int n = 36;
    for (int layer = 2; layer >= 0; layer--) {
        std::vector<float> xs(n), ys(n);
        float rot = s.time * (0.08f + 0.05f * layer) * (layer == 1 ? -1 : 1);
        float lo = layer * 0.3f;
        for (int i = 0; i < n; i++) {
            float t = (float)i / n;
            float v = bandAt(s, lo + std::fabs(t - 0.5f) * 2 * 0.4f);
            float ang = -PI / 2 + t * 2 * PI + rot;
            float r = R + 8 + layer * 6 + v * R * (0.42f - 0.08f * layer) + s.beat * 4;
            xs[i] = cx + std::cos(ang) * r;
            ys[i] = cy + std::sin(ang) * r;
        }
        nvgBeginPath(vg);
        closedCurve(vg, xs, ys);
        NVGcolor c = pal(L, 0.2f + layer * 0.35f, 1);
        nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, R, R * 1.45f, withAlpha(c, 0.30f), withAlpha(c, 0.02f)));
        nvgFill(vg);
        nvgStrokeColor(vg, withAlpha(c, 0.75f));
        nvgStrokeWidth(vg, 2);
        nvgStroke(vg);
    }
    nvgRestore(vg);
}

// Points of light circling the cover, thrown outwards by their part of the spectrum.
void drawOrbit(NVGcontext* vg, const Look& L, const State& s, float cx, float cy, float R) {
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    const int n = 110;
    for (int pass = 0; pass < 2; pass++) {
        nvgBeginPath(vg);
        for (int i = 0; i < n; i++) {
            float seed = (float)((i * 7919) % 1000) / 1000.0f;
            float band = (float)((i * 37) % n) / n;
            float v = bandAt(s, band);
            float speed = (0.12f + seed * 0.25f) * (i % 2 ? 1 : -1);
            float ang = seed * 2 * PI + s.time * speed;
            float r = R + 12 + seed * R * 0.12f + v * R * 0.4f + s.beat * 6;
            float size = 1.2f + v * 3.2f;
            float x = cx + std::cos(ang) * r, y = cy + std::sin(ang) * r;
            nvgCircle(vg, x, y, pass ? size : size * 3.2f);
        }
        nvgFillColor(vg, pal(L, pass ? 0.95f : 0.4f, pass ? 0.95f : 0.12f));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

void drawRingWave(NVGcontext* vg, const Look& L, const State& s, float cx, float cy, float R) {
    for (int layer = 0; layer < 2; layer++) {
        const int n = 128;
        nvgBeginPath(vg);
        for (int i = 0; i <= n; i++) {
            float t = (float)(i % n) / n, ang = -PI / 2 + t * 2 * PI;
            float w = layer == 0 ? s.wave[(int)(t * (kWave - 1))] * 1.8f : bandAt(s, std::fabs(t - 0.5f) * 2) * 0.9f;
            float r = R + 16 + layer * 8 + std::clamp(w, -1.0f, 1.0f) * R * 0.3f;
            float x = cx + std::cos(ang) * r, y = cy + std::sin(ang) * r;
            if (i == 0) nvgMoveTo(vg, x, y);
            else nvgLineTo(vg, x, y);
        }
        nvgClosePath(vg);
        nvgLineJoin(vg, NVG_ROUND);
        glowLine(vg, nvgLinearGradient(vg, cx - R, cy - R, cx + R, cy + R, pal(L, layer * 0.5f, 1),
                                       pal(L, 0.5f + layer * 0.5f, 1)),
                 2.2f);
    }
}

void drawPulse(NVGcontext* vg, const Look& L, const State& s, float cx, float cy, float R) {
    float glow = R * (1.25f + 0.5f * s.bass + 0.25f * s.beat);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, glow * 1.4f);
    nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, R * 0.9f, glow * 1.4f, pal(L, 0.4f, 0.35f + 0.3f * s.beat),
                                       pal(L, 0.4f, 0)));
    nvgFill(vg);
    for (auto& r : s.rings) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, R + 8 + r.r * R * 0.9f);
        nvgStrokeColor(vg, pal(L, 1 - r.a, r.a * 0.8f));
        nvgStrokeWidth(vg, 2 + 4 * r.a);
        nvgStroke(vg);
    }
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, R + 8 + s.energy * 14);
    nvgStrokeColor(vg, pal(L, 0.8f, 0.9f));
    nvgStrokeWidth(vg, 3);
    nvgStroke(vg);
}

void drawParticles(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    for (auto& p : s.particles) {
        float x = a.x + p.x * a.w, y = a.y + p.y * a.h, r = p.size * (1 + s.beat * 0.6f);
        nvgBeginPath(vg);
        nvgCircle(vg, x, y, r * 3);
        nvgFillPaint(vg, nvgRadialGradient(vg, x, y, 0, r * 3, pal(L, p.hue, 0.55f * p.life), pal(L, p.hue, 0)));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

void drawStarfield(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    float cx = a.cx(), cy = a.cy(), k = std::max(a.w, a.h) * 0.5f;
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    nvgLineCap(vg, NVG_ROUND);
    for (auto& st : s.stars) {
        float x0 = cx + st.x / st.pz * k * 0.25f, y0 = cy + st.y / st.pz * k * 0.25f;
        float x1 = cx + st.x / st.z * k * 0.25f, y1 = cy + st.y / st.z * k * 0.25f;
        if (!a.contains(x1, y1)) continue;
        float bright = std::clamp(1.2f - st.z, 0.1f, 1.0f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, y0);
        nvgLineTo(vg, x1, y1);
        nvgStrokeColor(vg, pal(L, (st.x + 1) / 2, bright));
        nvgStrokeWidth(vg, 0.6f + 2.4f * (1 - st.z));
        nvgStroke(vg);
    }
    nvgRestore(vg);
}

void drawAurora(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    float amps[3] = {s.bass, s.mid, s.treble};
    for (int k = 0; k < 3; k++) {
        const int n = 32;
        std::vector<float> xs(n), top(n), bottom(n);
        float base = a.y + a.h * (0.45f + 0.14f * k), amp = a.h * (0.05f + 0.2f * amps[k]);
        for (int i = 0; i < n; i++) {
            float t = (float)i / (n - 1);
            xs[i] = a.x + t * a.w;
            float wv = std::sin(t * 6.3f + s.time * (0.5f + 0.3f * k) + k * 2) * 0.6f +
                       std::sin(t * 13 - s.time * 0.7f + k) * 0.4f;
            top[i] = base + wv * amp - amp * 0.6f;
            bottom[i] = base + wv * amp + amp * 0.9f + 30;
        }
        nvgBeginPath(vg);
        curveThrough(vg, xs, top, true);
        std::reverse(xs.begin(), xs.end());
        std::reverse(bottom.begin(), bottom.end());
        curveThrough(vg, xs, bottom, false);
        nvgClosePath(vg);
        nvgFillPaint(vg, nvgLinearGradient(vg, 0, base - amp, 0, base + amp + 30, pal(L, k / 2.0f, 0.32f),
                                           pal(L, k / 2.0f + 0.2f, 0)));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

// Recent spectra stacked into the distance, like a mountain range seen from the air.
void drawRidges(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    int rows = (int)s.history.size();
    if (rows == 0) return;
    const int n = State::kHistBands;
    float depth = a.h * 0.55f;
    for (int r = rows - 1; r >= 0; r--) {
        float k = (float)r / State::kHistLen;      // 0 = now (front), 1 = oldest (back)
        float y0 = a.bottom() - 6 - k * depth;
        float inset = a.w * 0.16f * k;
        float amp = a.h * (0.42f - 0.2f * k);
        std::vector<float> xs(n), ys(n);
        for (int i = 0; i < n; i++) {
            float t = (float)i / (n - 1);
            // Taper the edges so each ridge rises from flat ground.
            float edge = std::sin(t * PI);
            xs[i] = a.x + inset + t * (a.w - 2 * inset);
            ys[i] = y0 - s.history[r][i] * amp * (0.35f + 0.65f * edge);
        }
        nvgBeginPath(vg);
        nvgMoveTo(vg, xs[0], y0 + 2);
        curveThrough(vg, xs, ys, false);
        nvgLineTo(vg, xs[n - 1], y0 + 2);
        nvgClosePath(vg);
        // Each ridge hides the ones behind it.
        nvgFillColor(vg, nvgRGBAf(0.03f, 0.03f, 0.05f, 0.92f));
        nvgFill(vg);
        nvgBeginPath(vg);
        curveThrough(vg, xs, ys, true);
        nvgStrokeColor(vg, pal(L, 1 - k, (1 - k) * 0.9f + 0.1f));
        nvgStrokeWidth(vg, r == 0 ? 2.4f : 1.4f);
        nvgStroke(vg);
    }
}

// Beams of light from the middle of the screen, turning slowly, long where the music is loud.
void drawRays(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    float cx = a.cx(), cy = a.cy(), far = std::hypot(a.w, a.h) * 0.55f;
    const int n = 64;
    float rot = s.time * 0.06f;
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    for (int pass = 0; pass < 2; pass++) {
        nvgBeginPath(vg);
        for (int i = pass; i < n; i += 2) {
            float t = (float)i / n;
            float v = bandAt(s, std::fabs(t - 0.5f) * 2);
            float ang = rot + t * 2 * PI, w = PI / n * (0.5f + 0.5f * v);
            float len = far * (0.25f + 0.75f * v) * (1 + 0.08f * s.beat);
            nvgMoveTo(vg, cx, cy);
            nvgLineTo(vg, cx + std::cos(ang - w) * len, cy + std::sin(ang - w) * len);
            nvgLineTo(vg, cx + std::cos(ang + w) * len, cy + std::sin(ang + w) * len);
            nvgClosePath(vg);
        }
        nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, 20, far, pal(L, pass ? 0.8f : 0.3f, 0.22f + 0.1f * s.beat),
                                           pal(L, pass ? 0.8f : 0.3f, 0)));
        nvgFill(vg);
    }
    nvgRestore(vg);
}

// The spectrum folded six ways into a slowly turning flower of soft, rounded petals.
void drawKaleidoscope(NVGcontext* vg, const Look& L, const State& s, Rect a) {
    float cx = a.cx(), cy = a.cy(), base = std::min(a.w, a.h) * 0.13f;
    const int folds = 6, per = 8, n = folds * per;
    nvgSave(vg);
    nvgGlobalCompositeOperation(vg, NVG_LIGHTER);
    for (int layer = 0; layer < 3; layer++) {
        float rot = s.time * (0.04f + 0.03f * layer) * (layer % 2 ? -1 : 1) + layer * PI / folds;
        float scale = 1 + layer * 0.7f;
        std::vector<float> xs(n), ys(n);
        for (int i = 0; i < n; i++) {
            float seg = (float)(i % per) / per;                 // position within a petal
            float mirror = std::fabs(seg - 0.5f) * 2;            // 1 at the edges, 0 in the middle
            float band = (1 - mirror) * 0.55f + layer * 0.15f;  // petal tips follow the louder lows
            float v = (bandAt(s, band) + bandAt(s, band + 0.04f)) * 0.5f;
            float petal = 0.55f + 0.45f * std::cos(mirror * PI * 0.5f);
            float ang = rot + (float)i / n * 2 * PI;
            float r = base * scale * (0.75f + petal * (0.35f + v * 0.9f)) + s.beat * 5;
            xs[i] = cx + std::cos(ang) * r;
            ys[i] = cy + std::sin(ang) * r;
        }
        nvgBeginPath(vg);
        closedCurve(vg, xs, ys);
        NVGcolor c = pal(L, layer / 2.0f, 1);
        nvgFillPaint(vg, nvgRadialGradient(vg, cx, cy, base * scale * 0.4f, base * scale * 1.8f, withAlpha(c, 0.10f),
                                           withAlpha(c, 0.0f)));
        nvgFill(vg);
        nvgStrokeColor(vg, withAlpha(c, 0.12f));
        nvgStrokeWidth(vg, 8);
        nvgStroke(vg);
        nvgStrokeColor(vg, withAlpha(c, 0.55f));
        nvgStrokeWidth(vg, 1.6f);
        nvgStroke(vg);
    }
    nvgRestore(vg);
}
}  // namespace

int styleCount() { return (int)(sizeof(kStyles) / sizeof(kStyles[0])); }
const char* styleName(int style) { return kStyles[std::clamp(style, 0, styleCount() - 1)].name; }
Place placement(int style) { return kStyles[std::clamp(style, 0, styleCount() - 1)].place; }
int paletteCount() { return (int)(sizeof(kPalettes) / sizeof(kPalettes[0])); }
const char* paletteName(int p) { return kPalettes[std::clamp(p, 0, paletteCount() - 1)]; }

NVGcolor vivid(float r, float g, float b) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    float h = 0, d = mx - mn;
    if (d > 1e-4f) {
        if (mx == r) h = std::fmod((g - b) / d, 6.0f);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
        if (h < 0) h += 1;
    }
    float sat = mx > 0 ? d / mx : 0;
    // Grey covers keep the theme's feel instead of turning a random hue.
    return nvgHSL(h, sat < 0.08f ? 0.0f : std::max(0.6f, sat), sat < 0.08f ? 0.72f : 0.6f);
}

void draw(NVGcontext* vg, int style, const Look& look, const State& s, Rect area, float cx, float cy, float radius) {
    switch (style) {
        case 0: drawBars(vg, look, s, area); break;
        case 1: drawRadial(vg, look, s, cx, cy, radius); break;
        case 2: drawNeonBars(vg, look, s, area); break;
        case 3: drawMountains(vg, look, s, area); break;
        case 4: drawRingWave(vg, look, s, cx, cy, radius); break;
        case 5: drawOscilloscope(vg, look, s, area); break;
        case 6: drawParticles(vg, look, s, area); break;
        case 7: drawMirror(vg, look, s, area); break;
        case 8: drawPulse(vg, look, s, cx, cy, radius); break;
        case 9: drawSpectrumLine(vg, look, s, area); break;
        case 10: drawStarfield(vg, look, s, area); break;
        case 11: drawLed(vg, look, s, area); break;
        case 12: drawAurora(vg, look, s, area); break;
        case 13: drawHalo(vg, look, s, cx, cy, radius); break;
        case 14: drawOrbit(vg, look, s, cx, cy, radius); break;
        case 15: drawRidges(vg, look, s, area); break;
        case 16: drawRays(vg, look, s, area); break;
        case 17: drawKaleidoscope(vg, look, s, area); break;
        default: break;
    }
}

}  // namespace vis
