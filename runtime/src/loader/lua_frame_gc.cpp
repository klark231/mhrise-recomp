#include "loader/lua_frame_gc.h"
#include <algorithm>

namespace mod_lua {
namespace {
using namespace std::chrono;
constexpr int quiet_us = 50, reference_us = 300, maximum_us = 900;
// Lua expresses the incremental work quantum as log2(bytes). Recheck time after
// roughly 1 KiB of GC work; atomic phases and individual objects can take longer.
constexpr int step_size_log2 = 10;
constexpr auto idle_interval = seconds(5);
uint64_t heap_size(lua_State *L) {
  return uint64_t(lua_gc(L, LUA_GCCOUNT)) * 1024 + lua_gc(L, LUA_GCCOUNTB);
}
} // namespace

void *FrameGc::allocate(void *context, void *ptr, size_t old_size, size_t size) {
  auto &gc = *static_cast<FrameGc *>(context);
  // Lua uses old_size as a type tag for fresh allocations.
  size_t before = ptr ? old_size : 0;
  void *result = gc.upstream_(gc.upstream_data_, ptr, old_size, size);
  if (result && size > before)
    gc.allocated_ += size - before;
  return result;
}

void FrameGc::start(lua_State *L) {
  *this = {};
  upstream_ = lua_getallocf(L, &upstream_data_);
  lua_setallocf(L, allocate, this);
  lua_gc(L, LUA_GCINC, 0, 0, step_size_log2);
  lua_gc(L, LUA_GCSTOP);
  stats_.heap_bytes = live_bytes_ = heap_size(L);
  last_cycle_ = Clock::now();
  collecting_ = true;
}

void FrameGc::end_frame(lua_State *L) {
  lua_Debug activation;
  if (in_slice_ || lua_getstack(L, 0, &activation)) return;
  lua_gc(L, LUA_GCSTOP);
  auto now = Clock::now();
  uint64_t heap = stats_.heap_bytes = heap_size(L);
  double frame_bytes = static_cast<double>(allocated_ - last_frame_bytes_);
  last_frame_bytes_ = allocated_;
  // Respond immediately to bursts, then let pressure decay over several frames.
  recent_bytes_ = std::max(frame_bytes, 0.875 * recent_bytes_ + 0.125 * frame_bytes);
  const uint64_t allowance = std::max<uint64_t>(128 * 1024, live_bytes_ / 4);
  if (!collecting_ && allocated_ - cycle_bytes_ < allowance &&
      now - last_cycle_ < idle_interval)
    return;
  collecting_ = true;
  in_slice_ = true;
  // At a rate that consumes the allowance in eight frames, use the 300 us
  // reference budget. Also catch up if the heap grows during a long cycle.
  double excess = std::max(0.0, double(heap) - double(live_bytes_) - double(allowance));
  double pressure = std::max(recent_bytes_ * 8, excess) / allowance;
  stats_.budget_us = static_cast<int>(std::min<double>(
      maximum_us, quiet_us + (reference_us - quiet_us) * pressure));
  const auto deadline = now + std::chrono::microseconds(stats_.budget_us);
  auto previous = now;
  // A mod may have explicitly changed the collector. The scheduled work
  // always uses small incremental steps, independent of Lua's accumulated debt.
  lua_gc(L, LUA_GCINC, 0, 0, step_size_log2);
  do {
    bool done = lua_gc(L, LUA_GCSTEP, 0) == 1;
    auto after = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(after - previous).count();
    stats_.peak_step_us = std::max<int64_t>(stats_.peak_step_us, elapsed);
    previous = after;
    if (done) {
      live_bytes_ = heap_size(L);
      cycle_bytes_ = allocated_;
      last_cycle_ = after;
      collecting_ = false;
      ++stats_.cycles;
      break;
    }
  } while (previous < deadline);
  stats_.heap_bytes = heap_size(L);
  stats_.slice_us = std::chrono::duration_cast<std::chrono::microseconds>(previous - now).count();
  in_slice_ = false;
}
} // namespace mod_lua
