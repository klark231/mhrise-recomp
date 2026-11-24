#pragma once
struct lua_State;
namespace mod_lua {
void open_values(lua_State *);
void push_components(lua_State *, bool quaternion, const float *,
                     int dimension = 3);
bool read_components(lua_State *, int, bool quaternion, float *,
                     int dimension = 3);
} // namespace mod_lua
