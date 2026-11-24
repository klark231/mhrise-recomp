#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
struct lua_State;
struct Module;
namespace rise {
struct Tdb;
}
namespace mod_lua {
enum class PreHookResult : int { CallOriginal = 0, SkipOriginal = 1 };
PreHookResult pre_action(lua_State *);
int *swap_hook_storage(int *);

bool has_reflection_hooks(uint32_t method);
// Scope and registry references live for one invocation, including nested
// calls.
class ReflectionHookCall {
public:
  ReflectionHookCall(lua_State *L, uint32_t method);
  ReflectionHookCall(const ReflectionHookCall &) = delete;
  ~ReflectionHookCall();
  PreHookResult before(int arguments, int slots,
                       const std::function<void(int)> &validate);
  void after(int result, const std::function<void()> &normalize);

private:
  struct Callback {
    size_t index;
    int storage;
  };
  lua_State *L;
  std::vector<Callback> callbacks;
};
void install_api(lua_State *, rise::Tdb &, const Module &);
void tag_method(lua_State *, uint32_t id);
void push_type_definition(lua_State *, rise::Tdb &, uint32_t id);
uint32_t type_definition_id(lua_State *, int index);
void log_message(int priority, const char *message);
size_t checkpoint();
void rollback(lua_State *, size_t checkpoint);
} // namespace mod_lua
