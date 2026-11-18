#include "readahead.hpp"

#include <mpv/stream_cb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {
constexpr const char* kScheme = "smx://";
constexpr size_t kChunk = 2 << 20;     // bytes per SD read
constexpr int64_t kKeepBehind = 4 << 20;  // kept behind the play position for small rewinds

size_t g_capacity = 32 << 20;
std::string g_lastNote;  // why the last file couldn't be opened (or what was given up), for the event log

std::mutex g_statsMutex;
void setNote(const std::string& n) {
    std::lock_guard<std::mutex> lk(g_statsMutex);
    g_lastNote = n;
}
ReadAheadStats g_stats;
double g_windowBytes = 0, g_windowTime = 0, g_windowSlowest = 0;
std::chrono::steady_clock::time_point g_windowStart = std::chrono::steady_clock::now();

void recordRead(size_t bytes, double secs, double buffered) {
    std::lock_guard<std::mutex> lk(g_statsMutex);
    g_windowBytes += bytes;
    g_windowTime += secs;
    g_windowSlowest = std::max(g_windowSlowest, secs);
    g_stats.bufferedMB = buffered / 1048576.0;
    g_stats.active = true;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - g_windowStart).count() > 2.0) {
        // Keep the last measurement while the buffer is full and nothing is being read.
        if (g_windowTime > 0) {
            g_stats.readMBps = g_windowBytes / 1048576.0 / g_windowTime;
            g_stats.slowestReadMs = g_windowSlowest * 1000;
        }
        g_windowBytes = g_windowTime = g_windowSlowest = 0;
        g_windowStart = now;
    }
}

class Reader {
public:
    bool open(const std::string& path) {
        f_ = fopen(path.c_str(), "rb");
        if (!f_) return false;
        setvbuf(f_, nullptr, _IONBF, 0);
        fseeko(f_, 0, SEEK_END);
        size_ = ftello(f_);
        fseeko(f_, 0, SEEK_SET);
        if (size_ < 0) return false;
        filePos_ = 0;
        // Songs don't need a buffer sized for movies, and clearing it delays the start. With
        // memory short or broken up (after a huge playlist, say) a smaller buffer still plays.
        size_t want = (size_t)std::min<int64_t>((int64_t)g_capacity, size_ + 2 * (int64_t)kChunk);
        for (;;) {
            try {
                ring_.resize(want);
                break;
            } catch (const std::bad_alloc&) {
                if (want <= (size_t)(2 << 20)) throw;
                want = std::max<size_t>(want / 2, 2 << 20);
                setNote("read-ahead buffer reduced to " + std::to_string(want >> 20) + " MB (memory is short)");
            }
        }
        thread_ = std::thread([this] { worker(); });
        return true;
    }

    ~Reader() {
        if (f_) fclose(f_);
        std::lock_guard<std::mutex> lk(g_statsMutex);
        g_stats.active = false;
        g_stats.bufferedMB = 0;
    }

    // Instead of `delete`. A read from a drive that was just pulled out can take a long time to
    // fail; the player must not hang on it, so after a moment the reader cleans up after itself.
    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (!thread_.joinable()) {
            delete this;
            return;
        }
        std::unique_lock<std::mutex> lk(m_);
        if (cv_.wait_for(lk, std::chrono::milliseconds(1500), [&] { return workerDone_; })) {
            lk.unlock();
            thread_.join();
            delete this;
            return;
        }
        orphaned_ = true;  // set under the same lock the worker checks, so exactly one side deletes
        lk.unlock();
        thread_.detach();
    }

    int64_t read(char* dst, uint64_t n) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return pos_ < end_ || eof_ || cancelled_ || error_; });
        if (cancelled_ || (error_ && pos_ >= end_)) return -1;
        if (pos_ >= end_) return 0;
        int64_t take = std::min<int64_t>((int64_t)n, end_ - pos_);
        copyOut(dst, pos_, take);
        pos_ += take;
        lk.unlock();
        cv_.notify_all();  // space may have opened up for the reader thread
        return take;
    }

    int64_t seek(int64_t target) {
        if (target < 0 || target > size_) return MPV_ERROR_GENERIC;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (target >= start_ && target <= end_) {
                pos_ = target;  // already buffered
            } else {
                generation_++;
                start_ = end_ = pos_ = target;
                eof_ = target >= size_;
                error_ = false;
            }
        }
        cv_.notify_all();
        return target;
    }

    int64_t size() const { return size_; }

    void cancel() {
        {
            std::lock_guard<std::mutex> lk(m_);
            cancelled_ = true;
        }
        cv_.notify_all();
    }

private:
    void copyOut(char* dst, int64_t off, int64_t n) {
        size_t cap = ring_.size();
        size_t at = (size_t)(off % (int64_t)cap);
        size_t first = std::min<size_t>((size_t)n, cap - at);
        memcpy(dst, &ring_[at], first);
        if ((size_t)n > first) memcpy(dst + first, &ring_[0], (size_t)n - first);
    }

    void copyIn(const char* src, int64_t off, size_t n) {
        size_t cap = ring_.size();
        size_t at = (size_t)(off % (int64_t)cap);
        size_t first = std::min(n, cap - at);
        memcpy(&ring_[at], src, first);
        if (n > first) memcpy(&ring_[0], src + first, n - first);
    }

    void worker() {
#ifdef __SWITCH__
        // Above the decoder threads (0x3B) so reading never waits behind decoding.
        svcSetThreadPriority(threadGetCurHandle(), 0x2C);
#endif
        std::vector<char> tmp(kChunk);
        std::unique_lock<std::mutex> lk(m_);
        while (!quit_) {
            int64_t cap = (int64_t)ring_.size();
            // Drop data well behind the play position to make room.
            int64_t free = cap - (end_ - start_);
            if (free < (int64_t)kChunk) {
                int64_t newStart = std::max(start_, std::min(end_, pos_ - kKeepBehind));
                start_ = newStart;
                free = cap - (end_ - start_);
            }
            if (eof_ || error_ || free < (int64_t)kChunk / 4) {
                cv_.wait(lk);
                continue;
            }
            int64_t off = end_;
            size_t want = (size_t)std::min<int64_t>({(int64_t)kChunk, free, size_ - off});
            if (want == 0) {
                eof_ = true;
                cv_.notify_all();
                continue;
            }
            uint64_t gen = generation_;
            lk.unlock();

            auto t0 = std::chrono::steady_clock::now();
            if (filePos_ != off) fseeko(f_, off, SEEK_SET);
            size_t got = fread(tmp.data(), 1, want, f_);
            filePos_ = off + (int64_t)got;
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            lk.lock();
            if (gen != generation_) continue;  // a seek happened meanwhile; discard
            if (got == 0) {
                bool failed = ferror(f_);
                clearerr(f_);
                filePos_ = -1;  // seek again before the next try
                // A busy or briefly stalled card (another app writing to it) can fail a read; trying
                // again keeps the film playing instead of skipping ahead.
                if (failed && ++retries_ <= 8) {
                    lk.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(25 * retries_));
                    lk.lock();
                    continue;
                }
                if (failed) error_ = true;
                else eof_ = true;
            } else {
                retries_ = 0;
                copyIn(tmp.data(), off, got);
                end_ += (int64_t)got;
                if (end_ >= size_) eof_ = true;
            }
            recordRead(got, secs, (double)(end_ - pos_));
            cv_.notify_all();
        }
        workerDone_ = true;
        bool orphan = orphaned_;
        lk.unlock();
        cv_.notify_all();
        if (orphan) delete this;
    }

    FILE* f_ = nullptr;
    int64_t size_ = 0, filePos_ = 0;
    int retries_ = 0;
    std::vector<char> ring_;
    std::mutex m_;
    std::condition_variable cv_;
    int64_t start_ = 0, end_ = 0, pos_ = 0;  // absolute file offsets
    uint64_t generation_ = 0;
    bool eof_ = false, error_ = false, quit_ = false, cancelled_ = false;
    bool workerDone_ = false, orphaned_ = false;
    std::thread thread_;
};

// mpv's cookie is an id, not the Reader itself: mpv can call cancel_fn from another thread after
// close_fn has already run (between closing the stream and freeing it), and a late cancel must
// find nothing rather than a deleted Reader.
std::mutex g_readersMutex;
std::map<uintptr_t, Reader*> g_readers;
uintptr_t g_nextId = 1;

Reader* lookup(void* cookie) {
    std::lock_guard<std::mutex> lk(g_readersMutex);
    auto it = g_readers.find((uintptr_t)cookie);
    return it == g_readers.end() ? nullptr : it->second;
}

int64_t cbRead(void* cookie, char* buf, uint64_t n) {
    Reader* r = lookup(cookie);
    return r ? r->read(buf, n) : -1;
}
int64_t cbSeek(void* cookie, int64_t off) {
    Reader* r = lookup(cookie);
    return r ? r->seek(off) : MPV_ERROR_GENERIC;
}
int64_t cbSize(void* cookie) {
    Reader* r = lookup(cookie);
    return r ? r->size() : MPV_ERROR_UNSUPPORTED;
}
void cbClose(void* cookie) {
    Reader* r = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_readersMutex);
        auto it = g_readers.find((uintptr_t)cookie);
        if (it == g_readers.end()) return;
        r = it->second;
        g_readers.erase(it);
    }
    r->close();
}
void cbCancel(void* cookie) {
    // Held while cancelling so cbClose can't take the Reader away in the middle.
    std::lock_guard<std::mutex> lk(g_readersMutex);
    auto it = g_readers.find((uintptr_t)cookie);
    if (it != g_readers.end()) it->second->cancel();
}

int cbOpen(void*, char* uri, mpv_stream_cb_info* info) {
    std::string path = stripReadAheadUrl(uri);
    Reader* r = new (std::nothrow) Reader();
    if (!r) return MPV_ERROR_NOMEM;
    bool opened = false;
    try {
        opened = r->open(path);
        if (!opened) setNote("couldn't open " + path + ": " + strerror(errno));
    } catch (const std::bad_alloc&) {
        // Reported to mpv like any other file that can't be opened.
        setNote("not enough memory to open " + path);
    } catch (...) {  // e.g. no thread could be started: never let it out into mpv
        setNote("couldn't start reading " + path);
    }
    if (!opened) {
        delete r;
        return MPV_ERROR_LOADING_FAILED;
    }
    {
        std::lock_guard<std::mutex> lk(g_readersMutex);
        uintptr_t id = g_nextId++;
        g_readers[id] = r;
        info->cookie = (void*)id;
    }
    info->read_fn = cbRead;
    info->seek_fn = cbSeek;
    info->size_fn = cbSize;
    info->close_fn = cbClose;
    info->cancel_fn = cbCancel;
    return 0;
}
}  // namespace

void registerReadAhead(mpv_handle* mpv, size_t capacityBytes) {
    g_capacity = std::max<size_t>(capacityBytes, 8 << 20);
    mpv_stream_cb_add_ro(mpv, "smx", nullptr, cbOpen);
}

std::string readAheadUrl(const std::string& path) { return kScheme + path; }

std::string stripReadAheadUrl(const std::string& s) {
    size_t n = strlen(kScheme);
    return s.compare(0, n, kScheme) == 0 ? s.substr(n) : s;
}

std::string takeReadAheadNote() {
    std::lock_guard<std::mutex> lk(g_statsMutex);
    std::string n;
    n.swap(g_lastNote);
    return n;
}

ReadAheadStats readAheadStats() {
    std::lock_guard<std::mutex> lk(g_statsMutex);
    return g_stats;
}
