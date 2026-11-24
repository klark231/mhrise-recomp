#pragma once
#include <chrono>
#include <cstdint>
#include <lua.hpp>

namespace mod_lua {
// One controller per resident VM. The owner keeps it alive through lua_close.
class FrameGc {
public:
  struct Stats {
    uint64_t heap_bytes{}, cycles{};
    int budget_us{};
    int64_t slice_us{}, peak_step_us{};
  };
  void start(lua_State *L);
  void end_frame(lua_State *L);
  const Stats &stats() const { return stats_; }

private:
  using Clock = std::chrono::steady_clock;
  static void *allocate(void *, void *, size_t, size_t);
  lua_Alloc upstream_{};
  void *upstream_data_{};
  uint64_t allocated_{}, last_frame_bytes_{}, cycle_bytes_{}, live_bytes_{};
  double recent_bytes_{};
  Clock::time_point last_cycle_{};
  bool collecting_{}, in_slice_{};
  Stats stats_{};
};
} // namespace mod_lua
