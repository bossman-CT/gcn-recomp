// Input-to-screen latency, stage by stage.
//
// GCN_LATENCYLOG=1 prints, once a second of host time, how long each stage between the
// controller and the screen took over that second (average and worst), in host
// milliseconds:
//
//   pad age    how old the host's controller state was when the guest read it. The host
//              refreshes it once a pass of its loop (update_pad); the guest reads it when
//              the game polls the SI. Half the loop's period on average.
//   loop       how often the host loop refreshed the controllers.
//   frame      from the guest handing a frame to the GX front end (its copy to the display)
//              to the host's swap of it returning: the front end, the transform, the
//              renderer and the swap call.
//   swap       the swap call alone, part of the above.
//   vblank     from the guest's frame end to the first vertical blank of the compositor after
//              the swap returned: the soonest the frame can be on screen where a compositor
//              takes it (Windows' DWM; there only). The scanout follows.
//
// Two shares are not here. The game's own -- from its read of the controller to the frame
// that first shows the change -- is the game's code; a replay of a route with and without
// an injected press, its frame dumps compared, measures it exactly (docs/diagnostics.md,
// "Input latency"). What follows the swap -- the compositor, the display's scanout -- is
// outside the process.
//
// Only paced runs present every frame (an unpaced one swaps every eighth), so the frame
// figures mean something only at the normal pace.
#include "latency.h"
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace latency {
namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Stat {
    double sum = 0, worst = 0;
    uint32_t n = 0;
    void add(double ms) { sum += ms, n++, worst = std::max(worst, ms); }
    void print(const char* name) const {
        if (n) fprintf(stderr, "  %s %.1f ms (worst %.1f)", name, sum / n, worst);
    }
};

std::atomic<int64_t> g_sampled{0};
std::atomic<int64_t> g_frame_end[256];
std::mutex g_mutex;  // the stats: the age is the guest's, the rest the host's
Stat g_age, g_loop, g_frame, g_swap, g_vblank;
int64_t g_second_start;

}  // namespace

bool on() {
    static const bool v = getenv("GCN_LATENCYLOG") != nullptr;
    return v;
}

void pad_sampled() {
    if (!on()) return;
    const int64_t t = now_ns(), last = g_sampled.exchange(t, std::memory_order_relaxed);
    if (!last) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_loop.add((t - last) / 1e6);
}

void pad_read() {
    if (!on()) return;
    const int64_t s = g_sampled.load(std::memory_order_relaxed);
    if (!s) return;
    const double ms = (now_ns() - s) / 1e6;
    std::lock_guard<std::mutex> lk(g_mutex);
    g_age.add(ms);
}

void frame_end(uint32_t frame) {
    if (!on()) return;
    g_frame_end[frame & 255].store(now_ns(), std::memory_order_relaxed);
}

void presented(uint32_t frame, double swap_ms, int64_t swapped_ns) {
    if (!on()) return;
    const int64_t t = now_ns(), end = g_frame_end[frame & 255].load(std::memory_order_relaxed);
    int64_t vb = 0, period = 0;
    const bool timing = plat_compositor_timing(&vb, &period);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (end && swapped_ns > end) g_frame.add((swapped_ns - end) / 1e6);
    if (end && timing && period > 0) {
        if (vb <= swapped_ns) vb += ((swapped_ns - vb) / period + 1) * period;
        g_vblank.add((vb - end) / 1e6);
    }
    g_swap.add(swap_ms);
    if (!g_second_start) g_second_start = t;
    if (t - g_second_start < 1000000000) return;
    fprintf(stderr, "[latency] %u frames:", g_frame.n);
    g_age.print("pad age");
    g_loop.print("loop");
    g_frame.print("frame");
    g_swap.print("swap");
    g_vblank.print("vblank");
    fprintf(stderr, "\n");
    g_age = g_loop = g_frame = g_swap = g_vblank = Stat{};
    g_second_start = t;
}

}  // namespace latency
