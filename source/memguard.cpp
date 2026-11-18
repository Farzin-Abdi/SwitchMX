// The memory reserve behind platform::installMemoryReserve() (see platform.hpp).
#include <atomic>
#include <cstdlib>
#include <new>

#include "platform.hpp"

namespace platform {
namespace {
constexpr size_t kReserve = 32 << 20;
std::atomic<void*> g_reserve{nullptr};
std::atomic<bool> g_low{false};

// Called by operator new when nothing is left: hand back the reserve and try again. Without a
// reserve the allocation fails as usual (std::bad_alloc).
void onOutOfMemory() {
    if (void* r = g_reserve.exchange(nullptr)) {
        free(r);
        g_low = true;
        return;
    }
    std::set_new_handler(nullptr);
}
}  // namespace

void installMemoryReserve() {
    if (!g_reserve) {
        void* r = malloc(kReserve);
        if (r) {
            // Touch it so it is really taken, not just promised.
            for (size_t i = 0; i < kReserve; i += 4096) static_cast<volatile char*>(r)[i] = 0;
            g_reserve = r;
        }
    }
    std::set_new_handler(onOutOfMemory);
}

void releaseMemoryReserve() {
    if (void* r = g_reserve.exchange(nullptr)) free(r);
}

bool lowMemory() { return g_low; }
void clearLowMemory() { g_low = false; }
void rearmMemoryReserve() {
    if (!g_reserve) installMemoryReserve();
}
}  // namespace platform
