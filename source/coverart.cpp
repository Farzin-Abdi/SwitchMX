#include "coverart.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "nanovg.h"
#include "platform.hpp"

namespace {
constexpr int kMaxSide = 288;
constexpr int kMaxTextures = 48;
constexpr uint64_t kMaxCacheBytes = 150ull << 20;
constexpr uint32_t kMagic = 0x31435853;  // "SXC1"
constexpr double kFileDeadline = 8.0;
constexpr double kFolderInfoLife = 5.0;
constexpr uint64_t kMaxImageBytes = 48u << 20;
constexpr int64_t kMaxPicturePixels = 64 * 1000 * 1000;  // decoding more could run out of memory

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool isImageName(const std::string& name) {
    static const char* exts[] = {"jpg", "jpeg", "png", "webp", "bmp", "tbn"};
    std::string ext = fileExtension(name);
    for (auto e : exts)
        if (ext == e) return true;
    return false;
}

// One folder's image files, and whether it holds a single video. Kept for the next file, because
// browsing asks about every file of the same folder.
struct FolderInfo {
    std::string dir;
    std::vector<std::pair<std::string, std::string>> images;  // lowercase stem, real name
    int videos = 0;
    std::string firstVideo;  // by name: a folder without a poster shows a frame of it
    double readAt = 0;
};

const FolderInfo& folderInfo(const std::string& dir) {
    static FolderInfo info;
    double now = platform::nowSeconds();
    if (info.dir == dir && now - info.readAt < kFolderInfoLife) return info;
    info = FolderInfo{};
    info.dir = dir;
    info.readAt = now;
    DIR* d = opendir(dir.c_str());
    if (!d) return info;
    int scanned = 0;
    while (dirent* e = readdir(d)) {
        if (++scanned > 5000) break;
        std::string name = e->d_name;
        if (isImageName(name)) info.images.push_back({lower(stripExtension(name)), name});
        else if (kindForExtension(fileExtension(name)) == MediaKind::Video && name[0] != '.') {
            info.videos++;
            if (info.firstVideo.empty() || name < info.firstVideo) info.firstVideo = name;
        }
    }
    closedir(d);
    return info;
}

std::string findImage(const FolderInfo& info, const std::vector<std::string>& stems) {
    for (auto& stem : stems)
        for (auto& img : info.images)
            if (img.first == stem) return info.dir + img.second;
    return "";
}

std::string folderImage(const FolderInfo& info) {
    return findImage(info, {"folder", "cover", "poster", "front", "album", "albumart", "albumartsmall", "thumb",
                            "fanart"});
}

// FFmpeg can't open "sdmc:/" paths itself (it would look for a protocol called sdmc), so it reads
// through stdio. The same callbacks enforce the time limit.
struct Reader {
    FILE* f = nullptr;
    double deadline = 0;
    const std::atomic<bool>* paused = nullptr;
    bool wasPaused = false, timedOut = false;
};

// Why a file gave no art, when that says nothing about the file itself.
struct Outcome {
    bool paused = false;     // ask again once playback is over
    bool temporary = false;  // too slow or too big right now: don't remember "no art"
    void add(const Reader& r) {
        paused = paused || r.wasPaused;
        temporary = temporary || r.timedOut;
    }
};

int readPacket(void* opaque, uint8_t* buf, int size) {
    Reader* r = (Reader*)opaque;
    size_t n = fread(buf, 1, (size_t)size, r->f);
    return n > 0 ? (int)n : AVERROR_EOF;
}

int64_t seekStream(void* opaque, int64_t offset, int whence) {
    Reader* r = (Reader*)opaque;
    if (whence == AVSEEK_SIZE) {
        off_t cur = ftello(r->f);
        fseeko(r->f, 0, SEEK_END);
        off_t end = ftello(r->f);
        fseeko(r->f, cur, SEEK_SET);
        return end;
    }
    if (fseeko(r->f, offset, whence & ~AVSEEK_FORCE) != 0) return -1;
    return ftello(r->f);
}

int interruptCheck(void* opaque) {
    Reader* r = (Reader*)opaque;
    if (r->paused->load()) {
        r->wasPaused = true;
        return 1;
    }
    if (platform::nowSeconds() > r->deadline) {
        r->timedOut = true;
        return 1;
    }
    return 0;
}

struct Rgb {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

bool scaleFrame(const AVFrame* f, Rgb& out) {
    if (f->width <= 0 || f->height <= 0) return false;
    float k = std::min(1.0f, (float)kMaxSide / std::max(f->width, f->height));
    int w = std::max(1, (int)(f->width * k)), h = std::max(1, (int)(f->height * k));
    SwsContext* sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
    if (!sws) return false;
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h * 3);
    uint8_t* dst[4] = {out.px.data(), nullptr, nullptr, nullptr};
    int stride[4] = {w * 3, 0, 0, 0};
    sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
    sws_freeContext(sws);
    return true;
}

float brightness(const Rgb& img) {
    if (img.px.empty()) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < img.px.size(); i += 3) sum += img.px[i + 1];
    return (float)sum / (img.px.size() / 3);
}

// Sends packets from `next` until the decoder gives a frame.
template <typename NextPacket>
bool decodeOne(const AVCodecParameters* par, bool keyframesOnly, NextPacket next, Rgb& out) {
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;
    bool ok = false;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    avcodec_parameters_to_context(ctx, par);
    ctx->thread_count = 1;
    if (keyframesOnly) {
        ctx->skip_frame = AVDISCARD_NONKEY;
        ctx->skip_loop_filter = AVDISCARD_ALL;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    }
    if (frame && pkt && avcodec_open2(ctx, codec, nullptr) == 0) {
        bool draining = false;
        for (int i = 0; i < 400 && !ok; i++) {
            int got = avcodec_receive_frame(ctx, frame);
            if (got == 0) {
                ok = scaleFrame(frame, out);
                break;
            }
            if (got != AVERROR(EAGAIN) || draining) break;
            if (!next(pkt)) {
                avcodec_send_packet(ctx, nullptr);
                draining = true;
                continue;
            }
            avcodec_send_packet(ctx, pkt);
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ok;
}

struct Media {
    Reader reader;
    AVFormatContext* fmt = nullptr;
    AVIOContext* io = nullptr;

    bool open(const std::string& path, const std::atomic<bool>* paused) {
        reader.f = fopen(path.c_str(), "rb");
        if (!reader.f) return false;
        reader.deadline = platform::nowSeconds() + kFileDeadline;
        reader.paused = paused;
        uint8_t* buf = (uint8_t*)av_malloc(64 * 1024);
        fmt = avformat_alloc_context();
        if (!buf || !fmt) {
            av_free(buf);
            return false;
        }
        io = avio_alloc_context(buf, 64 * 1024, 0, &reader, readPacket, nullptr, seekStream);
        if (!io) {
            av_free(buf);
            return false;
        }
        fmt->pb = io;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        fmt->interrupt_callback.callback = interruptCheck;
        fmt->interrupt_callback.opaque = &reader;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "probesize", "2000000", 0);
        int rc = avformat_open_input(&fmt, "", nullptr, &opts);
        av_dict_free(&opts);
        return rc == 0;  // on failure FFmpeg has freed fmt
    }

    ~Media() {
        if (fmt) avformat_close_input(&fmt);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        if (reader.f) fclose(reader.f);
    }
};

// A still picture, or a cover stored in a media file.
bool decodePicture(const std::string& path, const std::atomic<bool>* paused, Outcome& outcome, Rgb& out, bool wholeFile) {
    Media m;
    if (!m.open(path, paused)) {
        outcome.add(m.reader);
        return false;
    }
    int found = -1;
    for (unsigned i = 0; i < m.fmt->nb_streams && found < 0; i++) {
        AVStream* st = m.fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) && st->attached_pic.size > 0) found = (int)i;
        else if (wholeFile) found = (int)i;
    }
    if (found < 0) {
        outcome.add(m.reader);
        return false;
    }
    AVStream* st = m.fmt->streams[found];
    if ((int64_t)st->codecpar->width * st->codecpar->height > kMaxPicturePixels) return false;
    bool sent = false;
    bool ok = decodeOne(st->codecpar, false, [&](AVPacket* pkt) {
        if (st->attached_pic.size > 0) {
            if (sent) return false;
            sent = true;
            return av_packet_ref(pkt, &st->attached_pic) == 0;
        }
        for (;;) {
            if (av_read_frame(m.fmt, pkt) < 0) return false;
            if (pkt->stream_index == found) return true;
            av_packet_unref(pkt);
        }
    }, out);
    outcome.add(m.reader);
    return ok;
}

// The largest real video stream. Some releases add a one-frame logo as a second video track.
int mainVideoStream(AVFormatContext* fmt) {
    int best = -1;
    int64_t bestArea = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* par = st->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO || (st->disposition & AV_DISPOSITION_ATTACHED_PIC)) continue;
        if (par->codec_id == AV_CODEC_ID_MJPEG || par->codec_id == AV_CODEC_ID_PNG || par->codec_id == AV_CODEC_ID_BMP)
            continue;
        int64_t area = (int64_t)par->width * par->height;
        if (area > bestArea) {
            bestArea = area;
            best = (int)i;
        }
    }
    return best;
}

// A frame from a little way into the video: the opening is usually black or a logo.
bool grabFrame(const std::string& path, const std::atomic<bool>* paused, Outcome& outcome, Rgb& out) {
    Media m;
    if (!m.open(path, paused)) {
        outcome.add(m.reader);
        return false;
    }
    int video = mainVideoStream(m.fmt);
    if (video < 0 || m.fmt->streams[video]->codecpar->width <= 0) {
        avformat_find_stream_info(m.fmt, nullptr);
        video = mainVideoStream(m.fmt);
    }
    if (video < 0) {
        outcome.add(m.reader);
        return false;
    }
    AVStream* st = m.fmt->streams[video];
    // 4K decoding takes more memory than applet mode can spare; it may work from a game later.
    if (platform::isAppletMode() && (int64_t)st->codecpar->width * st->codecpar->height > 1920 * 1088) {
        outcome.temporary = true;
        return false;
    }
    for (unsigned i = 0; i < m.fmt->nb_streams; i++)
        if ((int)i != video) m.fmt->streams[i]->discard = AVDISCARD_ALL;

    double duration = m.fmt->duration > 0 ? m.fmt->duration / (double)AV_TIME_BASE : 0;
    bool ok = false;
    for (double frac : {0.12, 0.3, 0.55}) {
        double at = duration > 0 ? std::min(duration * frac, 900.0) : 0;
        if (at > 0) av_seek_frame(m.fmt, -1, (int64_t)(at * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
        Rgb img;
        int budget = 300;
        bool got = decodeOne(st->codecpar, true, [&](AVPacket* pkt) {
            while (budget-- > 0) {
                if (av_read_frame(m.fmt, pkt) < 0) return false;
                if (pkt->stream_index == video) return true;
                av_packet_unref(pkt);
            }
            return false;
        }, img);
        if (got) {
            ok = true;
            bool dark = brightness(img) < 18;
            if (out.px.empty() || !dark) out = std::move(img);
            if (!dark) break;
        }
        if (m.reader.wasPaused || m.reader.timedOut || duration <= 0) break;
    }
    outcome.add(m.reader);
    return ok;
}

uint64_t cacheKey(const std::string& path, const struct stat& st) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&](const void* data, size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    };
    mix(path.data(), path.size());
    uint64_t size = (uint64_t)st.st_size, mtime = (uint64_t)st.st_mtime;
    if (!S_ISDIR(st.st_mode)) mix(&size, sizeof size);
    else mix("v2", 2);  // folders: asked again since they can show a video's frame
    mix(&mtime, sizeof mtime);
    return h;
}

std::string cacheFileName(const std::string& dir, uint64_t key) {
    char name[32];
    snprintf(name, sizeof name, "%016llx.art", (unsigned long long)key);
    return dir + name;
}

bool readCache(const std::string& file, Rgb& out) {
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return false;
    uint32_t head[3] = {};
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == kMagic && head[1] <= kMaxSide && head[2] <= kMaxSide;
    if (ok) {
        out.w = (int)head[1];
        out.h = (int)head[2];
        out.px.resize((size_t)out.w * out.h * 3);
        ok = out.px.empty() || fread(out.px.data(), 1, out.px.size(), f) == out.px.size();
    }
    fclose(f);
    return ok;
}

void writeCache(const std::string& file, const Rgb& img) {
    std::string tmp = file + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    uint32_t head[3] = {kMagic, (uint32_t)img.w, (uint32_t)img.h};
    bool ok = fwrite(head, sizeof head, 1, f) == 1;
    if (ok && !img.px.empty()) ok = fwrite(img.px.data(), 1, img.px.size(), f) == img.px.size();
    ok = fclose(f) == 0 && ok;
    if (ok) {
        unlink(file.c_str());
        ok = rename(tmp.c_str(), file.c_str()) == 0;
    }
    if (!ok) unlink(tmp.c_str());
}

// The art is cheap to regenerate, so a cache that has grown too large just starts over.
void trimCache(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> files;
    uint64_t total = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        files.push_back(e->d_name);
        struct stat st;
        if (stat((dir + e->d_name).c_str(), &st) == 0) total += (uint64_t)st.st_size;
    }
    closedir(d);
    if (total <= kMaxCacheBytes) return;
    for (auto& f : files) unlink((dir + f).c_str());
}
}  // namespace

void CoverArt::start(const std::string& dataDir) {
    dir_ = dataDir + "covers/";
    mkdir(dir_.c_str(), 0777);
    av_log_set_level(AV_LOG_QUIET);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Decoders keep large objects on the stack.
    pthread_attr_setstacksize(&attr, 1 << 20);
    started_ = pthread_create(&thread_, &attr, threadMain, this) == 0;
    pthread_attr_destroy(&attr);
}

void CoverArt::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    paused_ = true;  // stops a decode in progress
    cv_.notify_all();
    pthread_join(thread_, nullptr);
    started_ = false;
}

void* CoverArt::threadMain(void* self) {
    platform::lowerThreadPriority();
    ((CoverArt*)self)->work();
    return nullptr;
}

void CoverArt::work() {
    trimCache(dir_);
    // Files without art share one small list, instead of a file each on the SD card.
    if (FILE* f = fopen((dir_ + "noart.bin").c_str(), "rb")) {
        uint64_t key;
        while (fread(&key, sizeof key, 1, f) == 1) noArt_.insert(key);
        fclose(f);
    }
    for (;;) {
        std::string path;
        MediaKind kind = MediaKind::Other;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return quit_ || (!queue_.empty() && !paused_); });
            if (quit_) return;
            // Newest first, skipping whatever has been scrolled away.
            while (!queue_.empty() && path.empty()) {
                std::string p = std::move(queue_.back());
                queue_.pop_back();
                auto it = entries_.find(p);
                if (it == entries_.end()) continue;
                if (frame_ - it->second.lastSeen > 2) {
                    entries_.erase(it);
                    continue;
                }
                path = std::move(p);
                kind = it->second.kind;
            }
        }
        if (path.empty()) continue;
        Decoded result = load(path, kind);
        std::lock_guard<std::mutex> lk(m_);
        done_.push_back(std::move(result));
    }
}

CoverArt::Decoded CoverArt::load(const std::string& path, MediaKind kind) {
    Decoded result;
    result.path = path;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return result;
    uint64_t key = cacheKey(path, st);
    if (noArt_.count(key)) return result;
    std::string cacheFile = cacheFileName(dir_, key);
    Rgb img;
    if (readCache(cacheFile, img)) {
        result.w = img.w;
        result.h = img.h;
        result.rgb = std::move(img.px);
        return result;
    }

    Outcome outcome;
    bool usedFrames = false;
    if (kind == MediaKind::Folder) {
        std::string dir = path;
        if (dir.empty() || dir.back() != '/') dir += '/';
        const FolderInfo& info = folderInfo(dir);
        std::string art = folderImage(info);
        bool got = !art.empty() && decodePicture(art, &paused_, outcome, img, true);
        // No poster: a frame of its first video (a season folder shows its first episode).
        if (!got && !outcome.paused && videoFrames_ && !info.firstVideo.empty()) {
            usedFrames = true;
            std::string first = info.dir + info.firstVideo;
            grabFrame(first, &paused_, outcome, img);
        }
    } else if (kind == MediaKind::Image) {
        if ((uint64_t)st.st_size <= kMaxImageBytes) decodePicture(path, &paused_, outcome, img, true);
    } else {
        const FolderInfo& info = folderInfo(parentDir(path));
        std::string stem = lower(stripExtension(baseName(path)));
        std::string own = findImage(info, {stem, stem + "-poster", stem + "-cover", stem + "-thumb", stem + ".poster",
                                           stem + "-fanart"});
        bool got = !own.empty() && decodePicture(own, &paused_, outcome, img, true);
        if (!got && !outcome.paused) got = decodePicture(path, &paused_, outcome, img, false);
        std::string shared = got || outcome.paused ? "" : folderImage(info);
        // A folder of episodes shares its poster, so frames tell them apart better.
        if (!got && !outcome.paused && !shared.empty() && (kind == MediaKind::Audio || info.videos <= 1)) {
            got = decodePicture(shared, &paused_, outcome, img, true);
            shared.clear();
        }
        if (!got && !outcome.paused && kind == MediaKind::Video && videoFrames_) {
            usedFrames = true;
            got = grabFrame(path, &paused_, outcome, img);
        }
        if (!got && !outcome.paused && !shared.empty()) decodePicture(shared, &paused_, outcome, img, true);
    }
    // Only an interrupted attempt is repeated: asking again for a file that is too slow to
    // finish would keep the CPU busy on it for as long as it stays on screen.
    if (outcome.paused || paused_) {
        result.retry = true;
        return result;
    }
    if (!img.px.empty()) {
        writeCache(cacheFile, img);
    } else if (!outcome.temporary && (kind != MediaKind::Video || usedFrames)) {
        // A video stays undecided while thumbnails are off: they may be turned on later.
        noArt_.insert(key);
        if (FILE* f = fopen((dir_ + "noart.bin").c_str(), "ab")) {
            fwrite(&key, sizeof key, 1, f);
            fclose(f);
        }
    }
    result.w = img.w;
    result.h = img.h;
    result.rgb = std::move(img.px);
    return result;
}

// 16×16 block averages, box-blurred twice. Stretched to the whole screen, linear filtering turns
// this into a smooth wash of the cover's colours, which no amount of scaling the sharp image gives.
int CoverArt::makeBlurred(NVGcontext* vg, const Decoded& d) {
    constexpr int N = 16;
    float px[N * N][3] = {};
    int cnt[N * N] = {};
    for (int y = 0; y < d.h; y += 2)
        for (int x = 0; x < d.w; x += 2) {
            int cell = (y * N / d.h) * N + x * N / d.w;
            const uint8_t* s = &d.rgb[((size_t)y * d.w + x) * 3];
            for (int c = 0; c < 3; c++) px[cell][c] += s[c];
            cnt[cell]++;
        }
    for (int i = 0; i < N * N; i++)
        for (int c = 0; c < 3; c++) px[i][c] = cnt[i] ? px[i][c] / cnt[i] : 0;
    for (int pass = 0; pass < 2; pass++) {
        float out[N * N][3];
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++)
                for (int c = 0; c < 3; c++) {
                    float t = 0;
                    int n = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int yy = std::clamp(y + dy, 0, N - 1), xx = std::clamp(x + dx, 0, N - 1);
                            t += px[yy * N + xx][c];
                            n++;
                        }
                    out[y * N + x][c] = t / n;
                }
        memcpy(px, out, sizeof px);
    }
    uint8_t rgba[N * N * 4];
    for (int i = 0; i < N * N; i++) {
        for (int c = 0; c < 3; c++) rgba[i * 4 + c] = (uint8_t)std::clamp(px[i][c], 0.0f, 255.0f);
        rgba[i * 4 + 3] = 255;
    }
    return nvgCreateImageRGBA(vg, N, N, 0, rgba);
}

void CoverArt::frame(NVGcontext* vg, float dt, bool enabled, bool videoFrames, bool busy) {
    std::lock_guard<std::mutex> lk(m_);
    frame_++;
    enabled_ = enabled;
    videoFrames_ = videoFrames;
    bool wasPaused = paused_.exchange(busy || !enabled);
    if (wasPaused && !paused_) cv_.notify_all();

    std::vector<Decoded> ready;
    // A few uploads per frame keep scrolling smooth.
    size_t take = std::min<size_t>(done_.size(), 2);
    ready.assign(std::make_move_iterator(done_.begin()), std::make_move_iterator(done_.begin() + take));
    done_.erase(done_.begin(), done_.begin() + take);
    for (auto& d : ready) {
        auto it = entries_.find(d.path);
        if (it == entries_.end()) continue;
        if (d.retry) {
            entries_.erase(it);
            continue;
        }
        if (d.rgb.empty()) {
            it->second.state = State::None;
            continue;
        }
        std::vector<uint8_t> rgba((size_t)d.w * d.h * 4);
        // Colourful pixels count more, so a cover's grey background doesn't wash its colour out.
        double sr = 0, sg = 0, sb = 0, sw = 0;
        for (size_t i = 0, n = (size_t)d.w * d.h; i < n; i++) {
            uint8_t r = d.rgb[i * 3], g = d.rgb[i * 3 + 1], b = d.rgb[i * 3 + 2];
            rgba[i * 4] = r;
            rgba[i * 4 + 1] = g;
            rgba[i * 4 + 2] = b;
            rgba[i * 4 + 3] = 255;
            if (i % 7) continue;
            double w = 0.05 + (std::max({r, g, b}) - std::min({r, g, b})) / 255.0;
            sr += r * w;
            sg += g * w;
            sb += b * w;
            sw += w;
        }
        if (sw > 0) {
            it->second.art.r = (float)(sr / sw / 255);
            it->second.art.g = (float)(sg / sw / 255);
            it->second.art.b = (float)(sb / sw / 255);
        }
        // Mipmaps keep small list thumbnails from shimmering.
        int image = nvgCreateImageRGBA(vg, d.w, d.h, NVG_IMAGE_GENERATE_MIPMAPS, rgba.data());
        it->second.art.blurred = makeBlurred(vg, d);
        if (!image) {
            it->second.state = State::None;
            continue;
        }
        it->second.state = State::Ready;
        it->second.art.image = image;
        it->second.art.w = d.w;
        it->second.art.h = d.h;
        textures_++;
    }
    for (auto& kv : entries_)
        if (kv.second.state == State::Ready && kv.second.lastSeen + 1 >= frame_) kv.second.art.age += dt;

    // Drop the textures that have been out of view the longest.
    while (textures_ > kMaxTextures) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.state == State::Ready && (oldest == entries_.end() || it->second.lastSeen < oldest->second.lastSeen))
                oldest = it;
        if (oldest == entries_.end() || oldest->second.lastSeen + 1 >= frame_) break;
        nvgDeleteImage(vg, oldest->second.art.image);
        if (oldest->second.art.blurred) nvgDeleteImage(vg, oldest->second.art.blurred);
        entries_.erase(oldest);
        textures_--;
    }
}

const CoverArt::Art* CoverArt::get(const std::string& path, MediaKind kind) {
    if (!enabled_ || !started_ || path.empty() || isUrl(path)) return nullptr;
    if (kind != MediaKind::Video && kind != MediaKind::Audio && kind != MediaKind::Image && kind != MediaKind::Folder)
        return nullptr;
    std::lock_guard<std::mutex> lk(m_);
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        Entry e;
        e.kind = kind;
        e.lastSeen = frame_;
        entries_.emplace(path, std::move(e));
        queue_.push_back(path);
        cv_.notify_one();
        return nullptr;
    }
    it->second.lastSeen = frame_;
    return it->second.state == State::Ready ? &it->second.art : nullptr;
}
