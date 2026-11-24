#include "loader/lua_frame_gc.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using mod_lua::FrameGc;

static void script(lua_State *L, const char *source) {
  if (luaL_dostring(L, source) != LUA_OK) {
    std::fprintf(stderr, "%s\n", lua_tostring(L, -1));
    std::abort();
  }
}

static void finish_cycle(FrameGc &gc, lua_State *L) {
  auto before = gc.stats().cycles;
  for (int i = 0; i < 10000 && gc.stats().cycles == before; ++i)
    gc.end_frame(L);
  assert(gc.stats().cycles == before + 1);
}

int main() {
  using namespace std::chrono_literals;
  FrameGc gc;
  lua_State *L = luaL_newstate();
  assert(L);
  luaL_openlibs(L);
  script(L, "function churn(n) for i=1,n do local v={i,i+1,i+2,i+3} end end");
  gc.start(L);
  finish_cycle(gc, L);
  assert(!lua_gc(L, LUA_GCISRUNNING));
  assert(gc.stats().budget_us == 50);
  auto idle_cycles = gc.stats().cycles;
  for (int i = 0; i < 120; ++i) gc.end_frame(L);
  assert(gc.stats().cycles == idle_cycles);

  script(L, "keep={}; for i=1,8192 do keep[i]={i} end; churn(65536)");
  auto high_water = lua_gc(L, LUA_GCCOUNT);
  gc.end_frame(L);
  assert(gc.stats().budget_us == 900);
  for (int i = 0; i < 120; ++i) gc.end_frame(L);
  script(L, "assert(#keep==8192 and keep[8192][1]==8192)");
  auto retained = gc.stats().heap_bytes;
  assert(retained / 1024 < static_cast<uint64_t>(high_water) / 2);

  // Replace objects at a steady rate without growing the live set.
  bool used_intermediate_budget = false;
  for (int i = 0; i < 1000; ++i) {
    lua_getglobal(L, "churn");
    lua_pushinteger(L, 256);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) std::abort();
    gc.end_frame(L);
    used_intermediate_budget |= gc.stats().budget_us > 50 && gc.stats().budget_us < 900;
  }
  assert(used_intermediate_budget);
  assert(gc.stats().cycles > idle_cycles + 2);
  assert(gc.stats().heap_bytes < retained * 3);
  std::printf("steady: %llu cycles, %llu KiB, latest budget %d us, peak step %lld us\n",
              (unsigned long long)gc.stats().cycles,
              (unsigned long long)(gc.stats().heap_bytes / 1024), gc.stats().budget_us,
              (long long)gc.stats().peak_step_us);

  // Dropping the last reference can create garbage with no new allocation.
  for (int i = 0; i < 300; ++i) gc.end_frame(L);
  lua_pushnil(L);
  lua_setglobal(L, "keep");
  std::this_thread::sleep_for(5100ms);
  finish_cycle(gc, L);
  assert(gc.stats().heap_bytes < retained / 4);
  assert(gc.stats().budget_us < 100);
  std::printf("idle after dropping retained table: %llu KiB, budget %d us (burst budget 900 us)\n",
              (unsigned long long)(gc.stats().heap_bytes / 1024), gc.stats().budget_us);
  lua_close(L);

  // The resident controller survives a script reload but its statistics do not.
  L = luaL_newstate();
  assert(L);
  gc.start(L);
  assert(gc.stats().cycles == 0 && gc.stats().peak_step_us == 0);
  finish_cycle(gc, L);
  lua_close(L);
}
