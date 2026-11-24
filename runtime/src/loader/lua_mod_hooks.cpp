#include "loader/lua_mod_hooks.h"
#include "loader/application_lua.h"
#include "loader/live_read.h"
#include "loader/lua_native_hooks.h"
#include <android/log.h>
#include <deque>
#include <lua.hpp>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace {
using namespace rise;
struct Hook {
  uint32_t method;
  int pre, post;
  uint64_t native_id{};
};
std::deque<Hook> hooks;
std::unordered_map<uint32_t, std::vector<size_t>> reflection;
Tdb *db = nullptr;
bool profile_matches = false;
char method_key;
thread_local int *active_storage = nullptr;
int get_storage(lua_State *L) {
  if (!active_storage) {
    lua_pushnil(L);
    return 1;
  }
  if (*active_storage == LUA_NOREF) {
    lua_newtable(L);
    *active_storage = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, *active_storage);
  return 1;
}
int register_hook(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_rawgetp(L, 1, &method_key);
  auto method = luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  if (!profile_matches)
    return luaL_error(L, "Hook profile does not match this NS build");
  bool local = false, ignore_jump = false;
  if (lua_istable(L, 4)) {
    lua_getfield(L, 4, "scope");
    local = lua_type(L, -1) == LUA_TSTRING &&
            std::string_view(lua_tostring(L, -1)) == "reflection";
    lua_pop(L, 1);
    if (!local)
      return luaL_error(L, "Unknown hook scope");
  } else if (!lua_isnoneornil(L, 4)) {
    luaL_checktype(L, 4, LUA_TBOOLEAN);
    ignore_jump = lua_toboolean(L, 4);
  }
  bool pre = !lua_isnoneornil(L, 2), post = !lua_isnoneornil(L, 3);
  if (pre)
    luaL_checktype(L, 2, LUA_TFUNCTION);
  if (post)
    luaL_checktype(L, 3, LUA_TFUNCTION);
  if (!pre && !post)
    return luaL_error(L, "Expected a hook callback");
  int before = LUA_NOREF, after = LUA_NOREF;
  try {
    if (local)
      validate_reflection_hook(L, method);
    if (pre) {
      lua_pushvalue(L, 2);
      before = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    if (post) {
      lua_pushvalue(L, 3);
      after = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    auto native = local ? 0
                        : mod_lua::register_native_hook(L, method, before,
                                                        after, ignore_jump);
    if (local)
      reflection[method].push_back(hooks.size());
    hooks.push_back({uint32_t(method), before, after, native});
    return 0;
  } catch (const std::exception &e) {
    luaL_unref(L, LUA_REGISTRYINDEX, before);
    luaL_unref(L, LUA_REGISTRYINDEX, after);
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
void call_hook(lua_State *L, int ref, int arg, int &storage) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  lua_pushvalue(L, arg);
  auto previous = mod_lua::swap_hook_storage(&storage);
  int status = lua_pcall(L, 1, 1, 0);
  mod_lua::swap_hook_storage(previous);
  if (status != LUA_OK)
    throw std::runtime_error(lua_isstring(L, -1) ? lua_tostring(L, -1)
                                                 : "Non-string Lua hook error");
}
void disable_hook(lua_State *L, Hook &h, const char *error) {
  auto declaring = read<uint32_t>(db->methods, 12ull * h.method) & id_mask;
  mod_lua::log_message(ANDROID_LOG_ERROR,
                       (db->full_name(declaring) + "." +
                        std::string(db->method(h.method, declaring)) +
                        " hook disabled: " + error)
                           .c_str());
  luaL_unref(L, LUA_REGISTRYINDEX, h.pre);
  luaL_unref(L, LUA_REGISTRYINDEX, h.post);
  h.pre = h.post = LUA_NOREF;
}
} // namespace
mod_lua::PreHookResult mod_lua::pre_action(lua_State *L) {
  // REFramework copies back the argument table passed to the callback.
  // A returned table (including that same table) carries no control result.
  return lua_type(L, -1) == LUA_TNUMBER &&
                 lua_tonumber(L, -1) == static_cast<int>(PreHookResult::SkipOriginal)
             ? PreHookResult::SkipOriginal
             : PreHookResult::CallOriginal;
}
int *mod_lua::swap_hook_storage(int *ref) {
  auto old = active_storage;
  active_storage = ref;
  return old;
}
bool mod_lua::has_reflection_hooks(uint32_t method) {
  auto it = reflection.find(method);
  return it != reflection.end() && !it->second.empty();
}
mod_lua::ReflectionHookCall::ReflectionHookCall(lua_State *state,
                                                uint32_t method)
    : L(state) {
  for (auto i : reflection.at(method))
    callbacks.push_back({i, LUA_NOREF});
}
mod_lua::ReflectionHookCall::~ReflectionHookCall() {
  for (auto &cb : callbacks)
    luaL_unref(L, LUA_REGISTRYINDEX, cb.storage);
}
mod_lua::PreHookResult
mod_lua::ReflectionHookCall::before(int arguments, int slots,
                                    const std::function<void(int)> &validate) {
  auto action = PreHookResult::CallOriginal;
  for (auto &cb : callbacks) {
    auto &h = hooks[cb.index];
    if (h.pre == LUA_NOREF)
      continue;
    int top = lua_gettop(L);
    try {
      lua_createtable(L, slots, 0);
      int draft = lua_gettop(L);
      for (int i = 1; i <= slots; ++i) {
        lua_rawgeti(L, arguments, i);
        lua_rawseti(L, draft, i);
      }
      call_hook(L, h.pre, draft, cb.storage);
      auto request = pre_action(L);
      validate(draft);
      for (int i = 1; i <= slots; ++i) {
        lua_rawgeti(L, draft, i);
        lua_rawseti(L, arguments, i);
      }
      if (request == PreHookResult::SkipOriginal)
        action = request;
    } catch (const std::exception &e) {
      disable_hook(L, h, e.what());
    }
    lua_settop(L, top);
  }
  return action;
}
void mod_lua::ReflectionHookCall::after(
    int result, const std::function<void()> &normalize) {
  for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it) {
    auto &h = hooks[it->index];
    if (h.post == LUA_NOREF)
      continue;
    int top = lua_gettop(L);
    try {
      call_hook(L, h.post, result, it->storage);
      if (!lua_isnil(L, -1) && !lua_rawequal(L, -1, result)) {
        normalize();
        lua_replace(L, result);
      }
    } catch (const std::exception &e) {
      disable_hook(L, h, e.what());
    }
    lua_settop(L, top);
  }
}
void mod_lua::tag_method(lua_State *L, uint32_t id) {
  lua_pushinteger(L, id);
  lua_rawsetp(L, -2, &method_key);
}
void mod_lua::install_api(lua_State *L, Tdb &t, const Module &main) {
  db = &t;
  constexpr uint8_t supported[32] = {0x55, 0xd5, 0x0c, 0xa1, 0x80, 0x5e, 0x9c,
                                     0x5b, 0xba, 0x50, 0x8d, 0x60, 0x1a, 0xdd,
                                     0x85, 0x2f, 0x25, 0x5a, 0x32, 0xba};
  profile_matches = !memcmp(main.build_id, supported, sizeof supported);
  initialize_native_hooks(L, t, main);
  for (auto [name, fn] :
       {std::pair<const char *, lua_CFunction>{"hook", register_hook},
        {"to_managed_object", hook_to_managed},
        {"is_managed_object", is_managed_argument},
        {"to_ptr", hook_to_ptr},
        {"to_int64", hook_to_integer}}) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
  }
  for (bool write : {false, true}) {
    lua_pushboolean(L, write);
    lua_pushcclosure(L, hook_native_field, 1);
    lua_setfield(L, -2, write ? "set_native_field" : "get_native_field");
  }
  for (bool wide : {false, true}) {
    lua_pushboolean(L, wide);
    lua_pushcclosure(L, hook_to_number, 1);
    lua_setfield(L, -2, wide ? "to_double" : "to_float");
    lua_pushboolean(L, wide);
    lua_pushcclosure(L, hook_number_to_ptr, 1);
    lua_setfield(L, -2, wide ? "double_to_ptr" : "float_to_ptr");
  }
  lua_newtable(L);
  lua_pushinteger(L, int(PreHookResult::CallOriginal));
  lua_setfield(L, -2, "CALL_ORIGINAL");
  lua_pushinteger(L, int(PreHookResult::SkipOriginal));
  lua_setfield(L, -2, "SKIP_ORIGINAL");
  lua_setfield(L, -2, "PreHookResult");
  lua_newtable(L);
  lua_pushcfunction(L, [](lua_State *s) {
    lua_pushinteger(s, gettid());
    return 1;
  });
  lua_setfield(L, -2, "get_id");
  lua_pushcfunction(L, [](lua_State *s) {
    lua_pushinteger(s,
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return 1;
  });
  lua_setfield(L, -2, "get_hash");
  lua_pushcfunction(L, get_storage);
  lua_setfield(L, -2, "get_hook_storage");
  lua_setglobal(L, "thread");
}
size_t mod_lua::checkpoint() { return hooks.size(); }
void mod_lua::rollback(lua_State *L, size_t count) {
  while (hooks.size() > count) {
    auto &h = hooks.back();
    if (h.native_id)
      remove_native_hook(h.native_id);
    else {
      auto it = reflection.find(h.method);
      it->second.pop_back();
      if (it->second.empty())
        reflection.erase(it);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, h.pre);
    luaL_unref(L, LUA_REGISTRYINDEX, h.post);
    hooks.pop_back();
  }
}
