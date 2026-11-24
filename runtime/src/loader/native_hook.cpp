#include "loader/native_hook.h"
#include <android/log.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_set>

extern "C" void rise_native_entry();
namespace mod_lua {
namespace {
constexpr size_t reserved_probe_bytes = 4096, slot_bytes = 128;
size_t used = 0;
int64_t signed_bits(uint32_t n, unsigned bits) {
  return (int64_t(n) ^ (1ll << (bits - 1))) - (1ll << (bits - 1));
}
bool in_text(const Module &m, uintptr_t p) {
  return p % 4 == 0 && p >= m.addr(m.seg_off[0]) &&
         p < m.addr(m.seg_off[0] + m.seg_size[0]);
}
uint32_t branch(uintptr_t from, uintptr_t to) {
  int64_t delta = int64_t(to) - int64_t(from);
  if (delta % 4 || delta < -(1ll << 27) || delta >= (1ll << 27))
    throw std::runtime_error("Method outside NSO branch reach");
  return 0x14000000u | (uint32_t(delta / 4) & 0x03ffffff);
}
void permission(uintptr_t address, size_t bytes, int flags) {
  auto page = size_t(sysconf(_SC_PAGESIZE));
  auto first = address & ~(page - 1);
  auto last = (address + bytes + page - 1) & ~(page - 1);
  if (mprotect(reinterpret_cast<void *>(first), last - first, flags))
    throw std::runtime_error("NSO hook page permission: " +
                             std::string(strerror(errno)));
}
void flush(void *first, size_t size) {
  __builtin___clear_cache(static_cast<char *>(first),
                          static_cast<char *>(first) + size);
}
// Relocate one entry instruction to the loader's reserved area. The original
// entry is replaced atomically after both the wrapper and trampoline exist.
void relocate(uint32_t instruction, uintptr_t source, uintptr_t here,
              uint32_t *code) {
  auto next = source + 4;
  if ((instruction & 0x9f000000) == 0x10000000 ||
      (instruction & 0x9f000000) == 0x90000000) {
    auto imm =
        ((instruction >> 29) & 3) | (((instruction >> 5) & 0x7ffff) << 2);
    bool page = instruction & 0x80000000;
    uintptr_t value =
        page ? (source & ~uintptr_t(4095)) + signed_bits(imm, 21) * 4096
             : source + signed_bits(imm, 21);
    code[0] = 0x58000040 | (instruction & 31);
    code[1] = branch(here + 4, next);
    memcpy(code + 2, &value, 8);
    return;
  }
  if ((instruction & 0xfc000000) == 0x14000000) {
    code[0] =
        branch(here, source + signed_bits(instruction & 0x3ffffff, 26) * 4);
    return;
  }
  if ((instruction & 0xfc000000) == 0x94000000)
    throw std::runtime_error("Unsupported BL at method entry");
  if ((instruction & 0xff000010) == 0x54000000 ||
      (instruction & 0x7e000000) == 0x34000000 ||
      (instruction & 0x7e000000) == 0x36000000) {
    bool test = (instruction & 0x7e000000) == 0x36000000;
    unsigned bits = test ? 14 : 19;
    uint32_t mask = ((1u << bits) - 1) << 5;
    auto target = source + signed_bits((instruction & mask) >> 5, bits) * 4;
    uint32_t inverse =
        (instruction & 0xff000010) == 0x54000000 ? 1 : 0x01000000;
    code[0] = ((instruction ^ inverse) & ~mask) | (2u << 5);
    code[1] = branch(here + 4, target);
    code[2] = branch(here + 8, next);
    return;
  }
  if ((instruction & 0x3b000000) == 0x18000000) {
    auto address = source + signed_bits((instruction >> 5) & 0x7ffff, 19) * 4;
    unsigned opc = instruction >> 30;
    bool simd = instruction & 0x04000000;
    if (opc == 3) {
      if (simd)
        throw std::runtime_error("Reserved SIMD literal instruction");
      code[0] = branch(here, next);
      return;
    }
    static constexpr uint32_t integer_load[] = {0xb9400200, 0xf9400200,
                                                0xb9800200};
    static constexpr uint32_t simd_load[] = {0xbd400200, 0xfd400200,
                                             0x3dc00200};
    code[0] = 0x58000090;
    code[1] = (simd ? simd_load[opc] : integer_load[opc]) | (instruction & 31);
    code[2] = branch(here + 8, next);
    code[3] = 0xd503201f;
    memcpy(code + 4, &address, 8);
    return;
  }
  if ((instruction & 0xffffffe0) == 0xd53bd040)
    throw std::runtime_error("Unprocessed guest TLS at method entry");
  code[0] = instruction;
  code[1] = branch(here + 4, next);
}
} // namespace
uintptr_t follow_native_entry(const Module &m, uintptr_t target, bool follow) {
  std::unordered_set<uintptr_t> visited;
  while (true) {
    if (!in_text(m, target))
      throw std::runtime_error("Method target outside active NSO text");
    if (!follow)
      return target;
    if (!visited.insert(target).second)
      throw std::runtime_error("Cyclic method entry jump");
    uint32_t instruction;
    memcpy(&instruction, reinterpret_cast<void *>(target), 4);
    if ((instruction & 0xfc000000) != 0x14000000)
      return target;
    auto next = target + signed_bits(instruction & 0x3ffffff, 26) * 4;
    if (!in_text(m, next))
      return target;
    target = next;
  }
}
void install_native_entry(const Module &m, uintptr_t target,
                          NativePatch *site) {
  if (!in_text(m, target))
    throw std::runtime_error("Invalid NSO hook target");
  if ((used + 1) * slot_bytes > Module::kProbeAreaSize - reserved_probe_bytes)
    throw std::runtime_error("NSO hook patch area is full");
  auto start = m.patch_area + Module::kPatchAreaSize - Module::kProbeAreaSize +
               reserved_probe_bytes + used * slot_bytes;
  uint32_t instruction;
  memcpy(&instruction, reinterpret_cast<void *>(target), 4);
  std::array<uint32_t, slot_bytes / 4> code{};
  code[0] = 0x58000090; // ldr x16, site at +16
  code[1] = 0x580000b1; // ldr x17, wrapper at +24
  code[2] = 0xd61f0220;
  code[3] = 0xd503201f;
  auto handler = reinterpret_cast<uintptr_t>(&rise_native_entry);
  memcpy(code.data() + 4, &site, 8);
  memcpy(code.data() + 6, &handler, 8);
  relocate(instruction, target, reinterpret_cast<uintptr_t>(start + 32),
           code.data() + 8);
  auto jump = branch(target, reinterpret_cast<uintptr_t>(start));
  permission(reinterpret_cast<uintptr_t>(start), slot_bytes,
             PROT_READ | PROT_WRITE | PROT_EXEC);
  memcpy(start, code.data(), slot_bytes);
  flush(start, slot_bytes);
  permission(reinterpret_cast<uintptr_t>(start), slot_bytes,
             PROT_READ | PROT_EXEC);
  site->target = target;
  site->original = reinterpret_cast<uintptr_t>(start + 32);
  permission(target, 4, PROT_READ | PROT_WRITE | PROT_EXEC);
  __atomic_store_n(reinterpret_cast<uint32_t *>(target), jump,
                   __ATOMIC_RELEASE);
  flush(reinterpret_cast<void *>(target), 4);
  ++used;
  // Keep the site alive even if restoring permissions fails after publication.
  if (mprotect(reinterpret_cast<void *>(target &
                                        ~uintptr_t(sysconf(_SC_PAGESIZE) - 1)),
               sysconf(_SC_PAGESIZE), PROT_READ | PROT_EXEC))
    __android_log_print(ANDROID_LOG_ERROR, "RiseLua",
                        "Hook text permission restore failed: %s",
                        strerror(errno));
}
} // namespace mod_lua
