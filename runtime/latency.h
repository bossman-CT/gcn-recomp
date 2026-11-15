// Input-to-screen latency, stage by stage (GCN_LATENCYLOG=1). See latency.cpp.
#pragma once
#include <cstdint>

namespace latency {

bool on();
// Host loop: the controllers' state was just refreshed (update_pad).
void pad_sampled();
// Guest: it read a controller's state from the SI.
void pad_read();
// Guest: it handed frame `frame` (gx::g_frames_submitted) to the GX front end.
void frame_end(uint32_t frame);
// Host: frame `frame` (gx::present_count) was rendered and swapped; the swap call took
// swap_ms and returned at swapped_ns (steady_clock).
void presented(uint32_t frame, double swap_ms, int64_t swapped_ns);

}  // namespace latency
