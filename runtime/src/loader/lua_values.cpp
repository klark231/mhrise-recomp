#include "loader/lua_values.h"
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <lua.hpp>

namespace {
struct Value {
  float v[4];
};
constexpr const char *names[] = {"rise.Vector3f", "rise.Quaternion",
                                 "rise.Vector2f", "rise.Vector4f"};
constexpr const char *globals[] = {"Vector3f", "Quaternion", "Vector2f",
                                   "Vector4f"};
constexpr int dimensions[] = {3, 4, 2, 4};
Value *push(lua_State *L, int kind, Value v) {
  auto *out = static_cast<Value *>(lua_newuserdatauv(L, sizeof(Value), 0));
  *out = v;
  luaL_setmetatable(L, names[kind]);
  return out;
}
int argkind(lua_State *L) {
  return static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
}
Value &arg(lua_State *L, int index, int kind) {
  return *static_cast<Value *>(luaL_checkudata(L, index, names[kind]));
}
int construct(lua_State *L) {
  int k = argkind(L);
  if (lua_gettop(L) != dimensions[k])
    return luaL_error(L, "Wrong number of components");
  Value v{};
  for (int i = 0; i < dimensions[k]; ++i)
    v.v[i] = luaL_checknumber(L, k == 1 ? (i + 1) % 4 + 1 : i + 1);
  push(L, k, v);
  return 1;
}
int component(lua_State *L) {
  if (lua_isinteger(L, 2))
    return lua_tointeger(L, 2);
  const char *key = luaL_checkstring(L, 2);
  if (!key[0] || key[1])
    return -1;
  const char *axes = "xyzw";
  const char *at = strchr(axes, *key);
  return at ? int(at - axes) : -1;
}
int index(lua_State *L) {
  int k = argkind(L), i = component(L);
  if (i >= 0 && i < dimensions[k]) {
    lua_pushnumber(L, arg(L, 1, k).v[i]);
    return 1;
  }
  luaL_getmetatable(L, names[k]);
  lua_pushvalue(L, 2);
  lua_rawget(L, -2);
  return 1;
}
int assign(lua_State *L) {
  int k = argkind(L), i = component(L);
  if (i < 0 || i >= dimensions[k])
    return luaL_error(L, "Invalid component");
  arg(L, 1, k).v[i] = luaL_checknumber(L, 3);
  return 0;
}
int length(lua_State *L) {
  int k = argkind(L);
  auto a = arg(L, 1, k);
  float sum = 0;
  for (int i = 0; i < dimensions[k]; ++i)
    sum += a.v[i] * a.v[i];
  lua_pushnumber(L, std::sqrt(sum));
  return 1;
}
enum class Op { Dot, Add, Subtract, Scale, Cross, Lerp, Normalized, Normalize };
template <Op op> int calculate(lua_State *L) {
  int k = argkind(L);
  auto a = arg(L, 1, k);
  Value out{};
  if constexpr (op == Op::Normalized || op == Op::Normalize) {
    float sum = 0;
    for (int i = 0; i < dimensions[k]; ++i)
      sum += a.v[i] * a.v[i];
    float length = std::sqrt(sum);
    for (int i = 0; i < dimensions[k]; ++i)
      out.v[i] = a.v[i] / length;
    if constexpr (op == Op::Normalize) {
      arg(L, 1, k) = out;
      return 0;
    }
  } else if constexpr (op == Op::Scale) {
    float scalar = luaL_checknumber(L, 2);
    for (int i = 0; i < dimensions[k]; ++i)
      out.v[i] = a.v[i] * scalar;
  } else {
    auto b = arg(L, 2, k);
    if constexpr (op == Op::Dot) {
      float dot = 0;
      for (int i = 0; i < dimensions[k]; ++i)
        dot += a.v[i] * b.v[i];
      lua_pushnumber(L, dot);
      return 1;
    } else if constexpr (op == Op::Cross) {
      for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3, h = (i + 2) % 3;
        out.v[i] = a.v[j] * b.v[h] - a.v[h] * b.v[j];
      }
      k = 0;
    } else {
      float mix = 0;
      if constexpr (op == Op::Lerp)
        mix = luaL_checknumber(L, 3);
      for (int i = 0; i < dimensions[k]; ++i) {
        if constexpr (op == Op::Add)
          out.v[i] = a.v[i] + b.v[i];
        if constexpr (op == Op::Subtract)
          out.v[i] = a.v[i] - b.v[i];
        if constexpr (op == Op::Lerp)
          out.v[i] = a.v[i] + mix * (b.v[i] - a.v[i]);
      }
    }
  }
  push(L, k, out);
  return 1;
}
void set(lua_State *L, const char *name, lua_CFunction fn, int k) {
  lua_pushinteger(L, k);
  lua_pushcclosure(L, fn, 1);
  lua_setfield(L, -2, name);
}
} // namespace
void mod_lua::push_components(lua_State *L, bool quaternion, const float *data,
                              int dimension) {
  int k = quaternion ? 1 : dimension == 2 ? 2 : dimension == 4 ? 3 : 0;
  Value v{};
  memcpy(v.v, data, dimensions[k] * sizeof(float));
  push(L, k, v);
}
bool mod_lua::read_components(lua_State *L, int index, bool quaternion,
                              float *out, int dimension) {
  int k = quaternion ? 1 : dimension == 2 ? 2 : dimension == 4 ? 3 : 0;
  auto *v = static_cast<Value *>(luaL_testudata(L, index, names[k]));
  // PC mods also pass a four-component position to RE's aligned via.vec3.
  if (!v && !quaternion && dimension == 3)
    v = static_cast<Value *>(luaL_testudata(L, index, names[3]));
  if (!v)
    return false;
  memcpy(out, v->v, dimensions[k] * sizeof(float));
  return true;
}
void mod_lua::open_values(lua_State *L) {
  luaL_newmetatable(L, "rise.Matrix4x4f");
  lua_pushcfunction(L, [](lua_State *s) {
    auto *matrix = static_cast<const float *>(luaL_checkudata(s, 1, "rise.Matrix4x4f"));
    auto row = luaL_checkinteger(s, 2);
    luaL_argcheck(s, row >= 0 && row < 4, 2, "matrix index outside 0..3");
    push_components(s, false, matrix + 4 * row, 4);
    return 1;
  });
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  for (int k : {0, 1, 2, 3}) {
    luaL_newmetatable(L, names[k]);
    set(L, "__index", index, k);
    set(L, "__newindex", assign, k);
    set(L, "length", length, k);
    set(L, "dot", calculate<Op::Dot>, k);
    set(L, "normalized", calculate<Op::Normalized>, k);
    set(L, "normalize", calculate<Op::Normalize>, k);
    if (k != 1) {
      set(L, "__add", calculate<Op::Add>, k);
      set(L, "__sub", calculate<Op::Subtract>, k);
      set(L, "__mul", calculate<Op::Scale>, k);
      if (dimensions[k] >= 3) {
        set(L, "cross", calculate<Op::Cross>, k);
        set(L, "lerp", calculate<Op::Lerp>, k);
      }
    }
    set(
        L, "clone",
        [](lua_State *s) {
          int n = argkind(s);
          push(s, n, arg(s, 1, n));
          return 1;
        },
        k);
    lua_pop(L, 1);
    lua_newtable(L);
    set(L, "new", construct, k);
    if (k == 1) {
      lua_pushcfunction(L, [](lua_State *s) {
        Value v{};
        v.v[3] = 1;
        push(s, 1, v);
        return 1;
      });
      lua_setfield(L, -2, "identity");
    }
    lua_setglobal(L, globals[k]);
  }
}
