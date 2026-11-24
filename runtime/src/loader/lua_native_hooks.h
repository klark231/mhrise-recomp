#pragma once
#include "loader/native_hook.h"
#include "loader/tdb71.h"
struct lua_State;
namespace mod_lua {
void initialize_native_hooks(lua_State *, rise::Tdb &, const Module &);
uint64_t register_native_hook(lua_State *, uint32_t method, int pre, int post,
                              bool ignore_jump);
void remove_native_hook(uint64_t);
bool native_hooks_in_flight();
int hook_to_managed(lua_State *);
int is_managed_argument(lua_State *);
int hook_to_ptr(lua_State *);
int hook_native_field(lua_State *);
int hook_to_integer(lua_State *);
int hook_to_number(lua_State *);
int hook_number_to_ptr(lua_State *);
} // namespace mod_lua
