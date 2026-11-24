#pragma once
#include "loader/nso.h"
#include <cstddef>
#include <cstdint>

namespace mod_lua {
// Offsets shared with native_hook.S.
struct alignas(16) NativeFrame {
  uint64_t x[9];
  uintptr_t original;
  alignas(16) uint8_t v[8][16];
  uint64_t result_x[2];
  alignas(16) uint8_t result_v[4][16];
  uintptr_t caller_sp;
  const uint8_t *stack_copy;
  size_t stack_size;
  void *invocation;
  void *site;
  uintptr_t saved_x19, saved_fp, saved_lr;
  uint64_t flags, fpsr, fpcr;
};
static_assert(sizeof(NativeFrame) == 384);
static_assert(offsetof(NativeFrame, v) == 80 &&
              offsetof(NativeFrame, result_x) == 208);
static_assert(offsetof(NativeFrame, caller_sp) == 288 &&
              offsetof(NativeFrame, site) == 320);
static_assert(offsetof(NativeFrame, saved_fp) == 336 &&
              offsetof(NativeFrame, fpcr) == 368);

struct NativePatch {
  uintptr_t target{}, original{};
};
void install_native_entry(const Module &, uintptr_t target, NativePatch *);
uintptr_t follow_native_entry(const Module &, uintptr_t target, bool follow);
} // namespace mod_lua
extern "C" void rise_native_call(mod_lua::NativeFrame *);
extern "C" void rise_native_dispatch(mod_lua::NativeFrame *);
