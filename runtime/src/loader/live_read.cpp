#include "loader/live_read.h"
#include "loader/application_lua.h"
#include "loader/screen_projection.h"
#include "loader/lua_mod_hooks.h"
#include "loader/lua_values.h"
#include "loader/tls.h"
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <limits>
#include <locale>
#include <lua.hpp>
#include <mutex>
#include <optional>
#include <vector>

namespace rise {
namespace {
constexpr char object_mt[] = "rise.sample.object";
constexpr char released_mt[] = "rise.released.object";
constexpr char ref_mt[] = "rise.managed.reference";
char frame_key, object_cache_key;
struct Object {
  uintptr_t address;
  uint32_t type;
  uintptr_t release{};
};
std::mutex releases_mutex;
std::vector<std::pair<uintptr_t, uintptr_t>> releases;
ReadFrame &frame(lua_State *L) {
  return *static_cast<ReadFrame *>(lua_touserdata(L, lua_upvalueindex(1)));
}
bool isa(Tdb &t, uint32_t id, uint32_t expected) {
  if (!expected)
    return true;
  for (; id; id = t.parent(id))
    if (id == expected)
      return true;
  return false;
}
uint32_t actual_type(Tdb &t, uintptr_t obj) {
  auto info = *reinterpret_cast<const uintptr_t *>(obj);
  if (!info)
    throw std::runtime_error("Object has no class information");
  auto type = *reinterpret_cast<const uintptr_t *>(info);
  auto first = reinterpret_cast<uintptr_t>(t.types.data());
  if (type < first || type - first >= t.types.size() || (type - first) % 72)
    throw std::runtime_error("Object class is not in the active TDB");
  return (type - first) / 72;
}
void push_obj(lua_State *L, ReadFrame &f, uintptr_t obj, uint32_t expected) {
  if (!obj) {
    lua_pushnil(L);
    return;
  }
  if (f.persistent) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &object_cache_key);
    lua_rawgetp(L, -1, reinterpret_cast<void *>(obj));
    if (auto *cached = static_cast<Object *>(luaL_testudata(L, -1, object_mt));
        cached && isa(f.db, cached->type, expected)) {
      lua_remove(L, -2);
      return;
    }
    lua_pop(L, 1);
  }
  auto id = actual_type(f.db, obj);
  if (!isa(f.db, id, expected))
    throw std::runtime_error("Unexpected runtime object type");
  auto *handle = static_cast<Object *>(lua_newuserdatauv(L, sizeof(Object), 0));
  *handle = {obj, id};
  if (f.persistent) {
    reinterpret_cast<void (*)(uintptr_t)>(f.main.addr(0x6b43e80))(obj);
    handle->release = f.main.addr(0x6b44408);
  }
  luaL_setmetatable(L, object_mt);
  if (f.persistent) {
    lua_pushvalue(L, -1);
    lua_rawsetp(L, -3, reinterpret_cast<void *>(obj));
    lua_remove(L, -2);
  }
}
Object object(lua_State *L, int index) {
  auto *p = static_cast<Object *>(luaL_checkudata(L, index, object_mt));
  if (lua_rawlen(L, index) != sizeof(Object))
    luaL_error(L, "Invalid sample handle");
  return *p;
}
void queue_release(Object &handle) {
  if (!handle.release) return;
  std::lock_guard lock(releases_mutex);
  releases.emplace_back(handle.release, handle.address);
  handle.release = 0;
}
uintptr_t function(ReadFrame &f, uint32_t method) {
  auto offset = read<int32_t>(f.db.methods, size_t(method) * 12 + 8);
  uintptr_t address = f.encoded_base + offset;
  if (!offset || address < f.main.addr(f.main.seg_off[0]) ||
      address >= f.main.addr(f.main.seg_off[0] + f.main.seg_size[0]) ||
      address % 4)
    throw std::runtime_error("Game getter is not initialized");
  return address;
}
uint32_t owner(Tdb &t, uint32_t method) {
  return read<uint32_t>(t.methods, size_t(method) * 12) & id_mask;
}
// NS RuntimeMethodInfo.Invoke delegates to the VM with a TDB method row and
// boxed arguments. The VM handles virtual dispatch and the ARM64 ABI, including
// aggregate returns.
enum class ValueKind {
  Void,
  Reference,
  String,
  Bool,
  I8,
  U8,
  I16,
  U16,
  I32,
  U32,
  I64,
  U64,
  F32,
  F64,
  Vec2,
  Vec3,
  Vec4,
  Quat,
  Mat4,
  Struct
};
ValueKind value_kind(Tdb &db, uint32_t id) {
  auto name = db.full_name(id);
  if (name == "System.String")
    return ValueKind::String;
  if (name == "System.Void")
    return ValueKind::Void;
  if (name == "System.IntPtr" || name == "System.UIntPtr")
    throw std::runtime_error("Unmanaged pointer signature");
  if (db.parent(id) == db.find("System.Enum")) {
    auto [first, count] = db.field_range(id);
    if (!count || db.field(first, id).name != "value__")
      throw std::runtime_error("Invalid enum representation");
    return value_kind(db, db.field(first, id).type);
  }
  if (!isa(db, id, db.find("System.ValueType")))
    return ValueKind::Reference;
  static constexpr std::pair<std::string_view, ValueKind> values[] = {
      {"System.Boolean", ValueKind::Bool}, {"System.SByte", ValueKind::I8},
      {"System.Byte", ValueKind::U8},      {"System.Int16", ValueKind::I16},
      {"System.UInt16", ValueKind::U16},   {"System.Char", ValueKind::U16},
      {"System.Int32", ValueKind::I32},    {"System.UInt32", ValueKind::U32},
      {"System.Int64", ValueKind::I64},    {"System.UInt64", ValueKind::U64},
      {"System.Single", ValueKind::F32},   {"System.Double", ValueKind::F64},
      {"via.vec2", ValueKind::Vec2},       {"via.vec3", ValueKind::Vec3},
      {"via.vec4", ValueKind::Vec4},       {"via.Quaternion", ValueKind::Quat},
      {"via.mat4", ValueKind::Mat4}};
  for (auto [type, kind] : values)
    if (name == type)
      return kind;
  return ValueKind::Struct;
}
std::span<uint8_t> boxed_payload(ReadFrame &f, uintptr_t obj, uint32_t id) {
  auto class_info = *reinterpret_cast<const uintptr_t *>(obj);
  int32_t offset = *reinterpret_cast<const int32_t *>(class_info - 8);
  uint32_t size = read<uint32_t>(f.db.types, 72ull * id + 20);
  if (offset < 16 || uint32_t(offset) > size)
    throw std::runtime_error("Invalid boxed value layout");
  return {reinterpret_cast<uint8_t *>(obj + offset), size - uint32_t(offset)};
}
template <class T> void put_value(std::span<uint8_t> data, T value) {
  if (data.size() < sizeof(value))
    throw std::runtime_error("Value exceeds game type size");
  memcpy(data.data(), &value, sizeof(value));
}
template <class T> T integer_arg(lua_State *L, int arg) {
  int valid;
  auto n = lua_tointegerx(L, arg, &valid);
  if (!valid)
    throw std::runtime_error("Integer argument required");
  if constexpr (sizeof(T) < sizeof(n)) {
    if (n < std::numeric_limits<T>::min() ||
        n > int64_t(std::numeric_limits<T>::max()))
      throw std::runtime_error("Integer outside game type range");
  }
  return static_cast<T>(n);
}
void write_scalar(lua_State *L, int arg, ValueKind kind,
                  std::span<uint8_t> bytes) {
  switch (kind) {
  case ValueKind::Mat4: {
    auto *matrix = luaL_testudata(L, arg, "rise.Matrix4x4f");
    if (!matrix || lua_rawlen(L, arg) != 16 * sizeof(float))
      throw std::runtime_error("Expected a Matrix4x4f value");
    slice(bytes, 0, 16 * sizeof(float));
    memcpy(bytes.data(), matrix, 16 * sizeof(float));
    return;
  }
  case ValueKind::Bool:
    if (!lua_isboolean(L, arg))
      throw std::runtime_error("Boolean argument required");
    return put_value(bytes, uint8_t(lua_toboolean(L, arg)));
  case ValueKind::I8:
    return put_value(bytes, integer_arg<int8_t>(L, arg));
  case ValueKind::U8:
    return put_value(bytes, integer_arg<uint8_t>(L, arg));
  case ValueKind::I16:
    return put_value(bytes, integer_arg<int16_t>(L, arg));
  case ValueKind::U16:
    return put_value(bytes, integer_arg<uint16_t>(L, arg));
  case ValueKind::I32:
    return put_value(bytes, integer_arg<int32_t>(L, arg));
  case ValueKind::U32:
    return put_value(bytes, integer_arg<uint32_t>(L, arg));
  case ValueKind::I64:
  case ValueKind::U64:
    return put_value(bytes, integer_arg<int64_t>(L, arg));
  case ValueKind::F32:
  case ValueKind::F64: {
    if (lua_type(L, arg) != LUA_TNUMBER)
      throw std::runtime_error("Numeric argument required");
    auto v = lua_tonumber(L, arg);
    if (kind == ValueKind::F32)
      return put_value(bytes, float(v));
    return put_value(bytes, v);
  }
  case ValueKind::Vec2:
  case ValueKind::Vec3:
  case ValueKind::Vec4:
  case ValueKind::Quat: {
    int dim = kind == ValueKind::Vec2 ? 2 : kind == ValueKind::Vec3 ? 3 : 4;
    bool quat = kind == ValueKind::Quat;
    float data[4]{};
    if (!mod_lua::read_components(L, arg, quat, data, dim))
      throw std::runtime_error("Incorrect vector or quaternion argument");
    auto n = dim * sizeof(float);
    slice(bytes, 0, n);
    memcpy(bytes.data(), data, n);
    return;
  }
  default:
    throw std::runtime_error("Expected a compatible managed value");
  }
}
void read_scalar(lua_State *L, ValueKind kind, Bytes bytes) {
  switch (kind) {
  case ValueKind::Mat4: {
    auto data = slice(bytes, 0, 16 * sizeof(float));
    auto *matrix = lua_newuserdatauv(L, data.size(), 0);
    memcpy(matrix, data.data(), data.size());
    luaL_setmetatable(L, "rise.Matrix4x4f");
    return;
  }
  case ValueKind::Bool:
    lua_pushboolean(L, read<uint8_t>(bytes, 0));
    return;
  case ValueKind::I8:
    lua_pushinteger(L, read<int8_t>(bytes, 0));
    return;
  case ValueKind::U8:
    lua_pushinteger(L, read<uint8_t>(bytes, 0));
    return;
  case ValueKind::I16:
    lua_pushinteger(L, read<int16_t>(bytes, 0));
    return;
  case ValueKind::U16:
    lua_pushinteger(L, read<uint16_t>(bytes, 0));
    return;
  case ValueKind::I32:
    lua_pushinteger(L, read<int32_t>(bytes, 0));
    return;
  case ValueKind::U32:
    lua_pushinteger(L, read<uint32_t>(bytes, 0));
    return;
  case ValueKind::I64:
  case ValueKind::U64:
    lua_pushinteger(L, read<int64_t>(bytes, 0));
    return;
  case ValueKind::F32:
    lua_pushnumber(L, read<float>(bytes, 0));
    return;
  case ValueKind::F64:
    lua_pushnumber(L, read<double>(bytes, 0));
    return;
  case ValueKind::Vec2:
  case ValueKind::Vec3:
  case ValueKind::Vec4:
  case ValueKind::Quat: {
    int dim = kind == ValueKind::Vec2 ? 2 : kind == ValueKind::Vec3 ? 3 : 4;
    bool quat = kind == ValueKind::Quat;
    auto data = slice(bytes, 0, dim * sizeof(float));
    mod_lua::push_components(L, quat,
                             reinterpret_cast<const float *>(data.data()), dim);
    return;
  }
  default:
    throw std::runtime_error("Expected a scalar or vector return type");
  }
}
void read_box(lua_State *L, ReadFrame &f, uintptr_t value, uint32_t expected) {
  if (!value) {
    lua_pushnil(L);
    return;
  }
  auto actual = actual_type(f.db, value);
  if (!isa(f.db, actual, expected))
    throw std::runtime_error("Incorrect managed return type");
  auto kind = value_kind(f.db, actual);
  if (kind == ValueKind::String) {
    auto n = *reinterpret_cast<const int32_t *>(value + 0x10);
    if (n < 0)
      throw std::runtime_error("Invalid string length");
    auto *chars = reinterpret_cast<const char16_t *>(value + 0x14);
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> codec;
    auto str = codec.to_bytes(chars, chars + n);
    lua_pushlstring(L, str.data(), str.size());
    return;
  }
  if (kind == ValueKind::Reference || kind == ValueKind::Struct) {
    push_obj(L, f, value, actual);
    return;
  }
  read_scalar(L, kind, boxed_payload(f, value, actual));
}
// Of the overloads named so with as many parameters as there are arguments, the one the arguments fit best: a
// string for System.String, a number for the numeric types, a boolean for System.Boolean, nil for anything that can
// be null; the first declared on a tie
uint32_t pick_overload(lua_State *L, ReadFrame &f, uint32_t type, std::string_view name, int arg) {
  const int count = lua_gettop(L) - arg + 1;
  uint32_t best = 0;
  int best_score = -1;
  for (uint32_t k : f.db.overloads(type, name, count)) {
    auto sig = f.db.signature_offset(k);
    int score = 0;
    for (int i = 0; i < count; ++i) {
      const std::string p = f.db.full_name(f.db.parameter_type(sig, i + 1));
      const bool numeric = p == "System.Int32" || p == "System.UInt32" || p == "System.Int64" || p == "System.UInt64" ||
                           p == "System.Int16" || p == "System.UInt16" || p == "System.Byte" || p == "System.SByte" ||
                           p == "System.Single" || p == "System.Double";
      switch (lua_type(L, arg + i)) {
        case LUA_TSTRING: score += p == "System.String" ? 2 : 0; break;
        case LUA_TNUMBER: score += numeric ? 2 : p == "System.String" ? -4 : 0; break;
        case LUA_TBOOLEAN: score += p == "System.Boolean" ? 2 : 0; break;
        case LUA_TNIL: score += numeric || p == "System.Boolean" ? -1 : 1; break;
        default: break;
      }
    }
    if (score > best_score) best = k, best_score = score;
  }
  if (!best) throw std::runtime_error("Unknown method: " + f.db.full_name(type) + "." + std::string(name));
  return best;
}

uintptr_t engine_context(ReadFrame &f) {
  auto ctx = f.persistent ? current_vm_context(f.main) : f.context;
  if (!ctx)
    throw std::runtime_error("Managed reflection requires an engine thread");
  return ctx;
}
uintptr_t managed_string(lua_State *L, ReadFrame &f, int index) {
  size_t len = 0;
  auto raw = lua_tolstring(L, index, &len);
  std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> codec;
  auto utf16 = codec.from_bytes(raw, raw + len);
  if (utf16.size() > INT32_MAX)
    throw std::runtime_error("Managed string too long");
  using NewString = uintptr_t (*)(uintptr_t, size_t, const char16_t *, int);
  auto str = reinterpret_cast<NewString>(f.main.addr(0x6b638b8))(
      engine_context(f), utf16.size(), nullptr, 0);
  if (!str || actual_type(f.db, str) != f.db.find("System.String") ||
      *reinterpret_cast<const int32_t *>(str + 0x10) != int32_t(utf16.size()))
    throw std::runtime_error("Invalid game string allocation");
  memcpy(reinterpret_cast<void *>(str + 0x14), utf16.data(), utf16.size() * 2);
  return str;
}
uintptr_t allocate_managed(ReadFrame &f, uint32_t expected, bool persistent) {
  using Allocate = uintptr_t (*)(uintptr_t, const uint8_t *, int);
  auto instance = reinterpret_cast<Allocate>(f.main.addr(0x6b64068))(
      engine_context(f), f.db.types.data() + 72ull * expected, persistent ? 1 : 0);
  if (!instance || actual_type(f.db, instance) != expected)
    throw std::runtime_error("Game VM could not allocate " + f.db.full_name(expected));
  return instance;
}
uintptr_t box_scalar(lua_State *L, ReadFrame &f, int arg, uint32_t expected) {
  auto kind = value_kind(f.db, expected);
  alignas(16) uint8_t encoded[16 * sizeof(float)]{};
  write_scalar(L, arg, kind, encoded);
  auto box = allocate_managed(f, expected, false);
  auto bytes = boxed_payload(f, box, expected);
  auto n = read<uint32_t>(f.db.impl(72ull * expected), 8);
  if (!n || n > sizeof(encoded) || n > bytes.size())
    throw std::runtime_error("Invalid boxed argument size");
  memcpy(bytes.data(), encoded, n);
  return box;
}

// An inline value with managed references needs the VM's assignment barriers.
// Only copy complete values whose instance fields recursively contain plain data.
void check_plain_value(Tdb &db, uint32_t id) {
  auto [first, count] = db.field_range(id);
  for (size_t i = first; i < first + count; ++i) {
    auto field = db.field(i, id);
    if (field.flags & (0x10 | 0x40))
      continue;
    auto kind = value_kind(db, field.type);
    if (kind == ValueKind::Reference || kind == ValueKind::String)
      throw std::runtime_error("Cannot copy managed references in " + db.full_name(id));
    if (kind == ValueKind::Struct)
      check_plain_value(db, field.type);
  }
}
void struct_field(lua_State *L, ReadFrame &f, uint32_t id,
                  std::span<uint8_t> storage, int write_arg) {
  check_plain_value(f.db, id);
  auto size = read<uint32_t>(f.db.impl(72ull * id), 8);
  if (!size || size > storage.size())
    throw std::runtime_error("Invalid inline value size");
  uintptr_t box;
  if (write_arg) {
    auto *source = static_cast<Object *>(luaL_testudata(L, write_arg, object_mt));
    if (!source || source->type != id)
      throw std::runtime_error("Expected boxed " + f.db.full_name(id));
    box = source->address;
  } else {
    box = allocate_managed(f, id, true);
  }
  auto bytes = boxed_payload(f, box, id);
  if (size > bytes.size())
    throw std::runtime_error("Value exceeds boxed payload");
  if (write_arg)
    memcpy(storage.data(), bytes.data(), size);
  else {
    memcpy(bytes.data(), storage.data(), size);
    push_obj(L, f, box, id);
  }
}
size_t callable_signature(Tdb &d, uint32_t method) {
  auto type = owner(d, method);
  auto declared = d.full_name(type);
  bool game = declared.starts_with("snow.") || declared.starts_with("via.");
  bool collection = declared == "System.Array" ||
                    declared.starts_with("System.Collections.Generic.") ||
                    declared.starts_with("System.Collections.ObjectModel.ReadOnlyCollection`1<");
  bool activator = declared == "System.Activator" &&
                   d.prototype(method, type) == "CreateInstance(System.Type)";
  if (!game && !collection && !activator && declared != "System.Math" &&
      declared != "System.String" && declared != "System.Enum")
    throw std::runtime_error("Not a game reflection interface: " + declared);
  auto sig = d.signature_offset(method);
  if (!sig || !read<uint16_t>(d.byte_pool, sig + 2))
    throw std::runtime_error("Missing managed invoker");
  return sig;
}
void normalize_hook_result(lua_State *L, ReadFrame &f, uint32_t ret,
                           ValueKind kind) {
  if (lua_islightuserdata(L, -1)) {
    uint64_t value = reinterpret_cast<uintptr_t>(lua_touserdata(L, -1));
    if (kind == ValueKind::Reference || kind == ValueKind::String) {
      if (!value) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
      }
      if (!push_cached_live_object(L, value, ret))
        throw std::runtime_error("Unknown managed hook result");
      lua_remove(L, -2);
    } else {
      lua_pop(L, 1);
      read_scalar(L, kind,
                  {reinterpret_cast<const uint8_t *>(&value), sizeof(value)});
      return;
    }
  }
  if ((kind == ValueKind::Reference || kind == ValueKind::String) &&
      lua_isinteger(L, -1) && lua_tointeger(L, -1) == 0) {
    lua_pop(L, 1);
    lua_pushnil(L);
    return;
  }
  if (auto *object = static_cast<Object *>(luaL_testudata(L, -1, object_mt))) {
    read_box(L, f, object->address, ret);
    lua_remove(L, -2);
    return;
  }
  if (kind == ValueKind::String && lua_type(L, -1) == LUA_TSTRING)
    return;
  if (kind == ValueKind::Reference || kind == ValueKind::String ||
      kind == ValueKind::Void)
    throw std::runtime_error("Incorrect hook return type for " +
                             f.db.full_name(ret));
  if (kind == ValueKind::Bool && lua_isinteger(L, -1)) {
    auto value = lua_tointeger(L, -1);
    if (value != 0 && value != 1)
      throw std::runtime_error("Boolean hook result outside 0..1");
    lua_pop(L, 1);
    lua_pushboolean(L, value);
  }
  alignas(16) uint8_t bytes[16 * sizeof(float)]{};
  write_scalar(L, -1, kind, bytes);
  lua_pop(L, 1);
  read_scalar(L, kind, bytes);
}
uintptr_t method_argument(lua_State *L, ReadFrame &f, int index,
                          uint32_t expected, bool materialize) {
  auto kind = value_kind(f.db, expected);
  if (lua_islightuserdata(L, index)) {
    uint64_t raw = reinterpret_cast<uintptr_t>(lua_touserdata(L, index));
    if (kind == ValueKind::Reference || kind == ValueKind::String) {
      if (!raw)
        return 0;
      if (!push_cached_live_object(L, raw, expected))
        throw std::runtime_error("Unknown managed hook argument");
    } else
      read_scalar(L, kind,
                  {reinterpret_cast<const uint8_t *>(&raw), sizeof(raw)});
    auto result = method_argument(L, f, lua_gettop(L), expected, materialize);
    lua_pop(L, 1);
    return result;
  }
  if (lua_isnil(L, index))
    return 0;
  if (auto *o = static_cast<Object *>(luaL_testudata(L, index, object_mt))) {
    if (!isa(f.db, o->type, expected))
      throw std::runtime_error("Incompatible managed argument");
    return o->address;
  }
  if (kind == ValueKind::String && lua_type(L, index) == LUA_TSTRING)
    return materialize ? managed_string(L, f, index) : 0;
  if (kind == ValueKind::Reference || kind == ValueKind::String)
    throw std::runtime_error("Expected a managed object or nil");
  if (materialize)
    return box_scalar(L, f, index, expected);
  alignas(16) uint8_t scratch[16 * sizeof(float)]{};
  write_scalar(L, index, kind, scratch);
  return 0;
}
// Keep a real managed Object[1] behind each typed ref. The VM then owns both
// the old and the replacement reference across calls, including aliased slots.
Object ref_array(lua_State *L, ReadFrame &f, int index) {
  lua_getiuservalue(L, index, 1);
  auto array = object(L, -1);
  lua_pop(L, 1);
  if (array.type != f.db.find("System.Object[]") ||
      *reinterpret_cast<const int32_t *>(array.address + 0x1c) != 1)
    throw std::runtime_error("Invalid managed reference slot");
  return array;
}
uintptr_t ref_argument(lua_State *L, ReadFrame &f, int index, uint32_t type) {
  auto *declared = static_cast<uint32_t *>(luaL_testudata(L, index, ref_mt));
  if (!declared || *declared != type)
    throw std::runtime_error("Expected a reference slot for " + f.db.full_name(type));
  auto array = ref_array(L, f, index);
  // NS TDB71 reference-array Address(Int32) tags heap interiors for the
  // VM's reference assignment barrier, and leaves temporary interiors untagged.
  auto address = array.address + 0x20;
  if (*reinterpret_cast<const int32_t *>(array.address + 8) >= 0)
    address |= uintptr_t{1} << 63;
  return address;
}
uintptr_t invoke_reference_method(lua_State *L, ReadFrame &f, uint32_t method,
                                  uintptr_t receiver, int arg, size_t sig,
                                  uintptr_t ctx, std::span<uintptr_t> args) {
  auto &db = f.db;
  auto flags = read<uint16_t>(db.method_impl(method), 4);
  if ((flags & (0x10 | 0x40)) ||
      value_kind(db, owner(db, method)) != ValueKind::Reference ||
      value_kind(db, db.parameter_type(sig, 0)) != ValueKind::Void)
    throw std::runtime_error("ByRef invocation requires a nonvirtual void instance method on a class");
  for (size_t i = 0; i < args.size(); ++i) {
    auto type = db.parameter_type(sig, i + 1);
    auto modifier = db.parameter_modifier(sig, i + 1);
    auto kind = value_kind(db, type);
    if ((kind != ValueKind::Reference && kind != ValueKind::String) ||
        (modifier != 0 && modifier != 2))
      throw std::runtime_error("Unsupported ByRef method signature");
    args[i] = modifier == 2 ? ref_argument(L, f, arg + i, type)
                            : method_argument(L, f, arg + i, type, true);
  }

  // NS's boxed Invoke would type-check the slot as an object and unbox it.
  // Use its generated signature wrapper with the already marshalled inputs.
  struct InvokeFrame {
    uintptr_t reserved[2]{};
    const uint8_t *method{};
    uintptr_t exception{}, unused[2]{};
    uintptr_t *arguments{};
    uintptr_t result{}, receiver{};
  } call;
  static_assert(offsetof(InvokeFrame, arguments) == 0x30 &&
                offsetof(InvokeFrame, receiver) == 0x40);
  call.method = db.methods.data() + 12ull * method;
  call.arguments = args.data();
  call.receiver = receiver;
  (void)function(f, method);
  auto table = *reinterpret_cast<const uintptr_t *>(f.main.addr(0x12a7a798));
  auto entry = table + sizeof(uintptr_t) * read<uint16_t>(db.byte_pool, sig + 2);
  if (!f.main.contains(entry) || !f.main.contains(entry + sizeof(uintptr_t) - 1))
    throw std::runtime_error("Managed invocation table unavailable");
  auto wrapper = *reinterpret_cast<const uintptr_t *>(entry);
  auto text = f.main.addr(f.main.seg_off[0]);
  if (wrapper % 4 || wrapper - text >= f.main.seg_size[0])
    throw std::runtime_error("Invalid managed signature wrapper");
  reinterpret_cast<void (*)(InvokeFrame *, uintptr_t)>(wrapper)(&call, ctx);
  return call.exception;
}
int managed_call(lua_State *L, ReadFrame &f, uint32_t method,
                 uintptr_t receiver, int arg, bool keep_box = false) {
  auto &d = f.db;
  auto type = owner(d, method);
  auto sig = callable_signature(d, method);
  auto count = read<uint16_t>(d.byte_pool, sig);
  if (lua_gettop(L) - arg + 1 != count)
    throw std::runtime_error("Incorrect method argument count");
  bool is_static = read<uint16_t>(d.method_impl(method), 4) & 0x10;
  if (!is_static && (!receiver || !isa(d, actual_type(d, receiver), type)))
    throw std::runtime_error("Incorrect method receiver");
  if (is_static)
    receiver = 0;
  auto ctx = engine_context(f);
  auto *exception_frame = *reinterpret_cast<uint8_t **>(ctx + 0x50);
  if (!exception_frame ||
      *reinterpret_cast<uintptr_t *>(exception_frame + 0x18))
    throw std::runtime_error("Game VM has a pending exception");
  if (!d.plain_parameter(sig, 0))
    throw std::runtime_error("Unmanaged or by-reference return type");
  auto ret = d.parameter_type(sig, 0);
  auto result_kind = value_kind(d, ret);
  std::optional<mod_lua::ReflectionHookCall> hooks;
  bool skip = false;
  if (f.persistent && mod_lua::has_reflection_hooks(method)) {
    hooks.emplace(L, method);
    lua_createtable(L, count + 2, 0);
    int table = lua_gettop(L);
    lua_pushlightuserdata(L, reinterpret_cast<void *>(ctx));
    lua_rawseti(L, table, 1);
    int first = is_static ? 2 : 3;
    if (!is_static) {
      push_obj(L, f, receiver, type);
      lua_rawseti(L, table, 2);
    }
    for (unsigned i = 0; i < count; ++i) {
      lua_pushvalue(L, arg + i);
      lua_rawseti(L, table, first + i);
    }
    auto validate = [&](int candidate) {
      lua_rawgeti(L, candidate, 1);
      if (!lua_islightuserdata(L, -1) ||
          lua_touserdata(L, -1) != reinterpret_cast<void *>(ctx))
        throw std::runtime_error("Hook changed the VM context");
      lua_pop(L, 1);
      if (!is_static) {
        lua_rawgeti(L, candidate, 2);
        auto *obj = static_cast<Object *>(luaL_testudata(L, -1, object_mt));
        if (!obj || obj->address != receiver)
          throw std::runtime_error("Hook changed the method receiver");
        lua_pop(L, 1);
      }
      for (unsigned i = 0; i < count; ++i) {
        lua_rawgeti(L, candidate, first + i);
        method_argument(L, f, lua_gettop(L), d.parameter_type(sig, i + 1),
                        false);
        lua_pop(L, 1);
      }
    };
    validate(table);
    skip = hooks->before(table, first + count - 1, validate) ==
           mod_lua::PreHookResult::SkipOriginal;
    for (unsigned i = 0; i < count; ++i) {
      lua_rawgeti(L, table, first + i);
      lua_replace(L, arg + i);
    }
    lua_pop(L, 1);
  }
  std::vector<uintptr_t> args(count);
  bool byref = false;
  for (unsigned i = 0; i < count; ++i) {
    auto modifier = d.parameter_modifier(sig, i + 1);
    if (modifier != 0 && modifier != 2)
      throw std::runtime_error("Unmanaged pointer parameter");
    byref |= modifier == 2;
  }
  uintptr_t result = 0, wrapper_exception = 0;
  if (!skip && byref)
    wrapper_exception = invoke_reference_method(L, f, method, receiver, arg, sig, ctx, args);
  else if (!skip) {
    for (unsigned i = 0; i < count; ++i)
      args[i] = method_argument(L, f, arg + i, d.parameter_type(sig, i + 1), true);
    using Invoke = uintptr_t (*)(uintptr_t, const uint8_t *, uintptr_t,
                                 const uintptr_t *, uint32_t);
    result = reinterpret_cast<Invoke>(f.main.addr(0x6b61f2c))(
        ctx, d.methods.data() + 12ull * method, receiver, args.data(), count);
  }
  auto &exception = *reinterpret_cast<uintptr_t *>(exception_frame + 0x18);
  if (exception || wrapper_exception) {
    exception = 0;
    throw std::runtime_error("Managed exception in " + d.full_name(type) + "." +
                             std::string(d.method(method, type)));
  }
  if (result_kind == ValueKind::Void && !hooks)
    return 0;
  if (!skip && !result && result_kind != ValueKind::Reference &&
      result_kind != ValueKind::String && result_kind != ValueKind::Void)
    throw std::runtime_error("Missing boxed return value");
  if (result_kind == ValueKind::Void || skip)
    lua_pushnil(L);
  else if (keep_box)
    push_obj(L, f, result, ret);
  else
    read_box(L, f, result, ret);
  if (hooks) {
    hooks->after(lua_gettop(L),
                 [&] { normalize_hook_result(L, f, ret, result_kind); });
    if (skip && lua_isnil(L, -1) && result_kind != ValueKind::Void &&
        result_kind != ValueKind::Reference && result_kind != ValueKind::String)
      throw std::runtime_error(
          "Skipped value-returning method needs a post-hook result");
    if (result_kind == ValueKind::Void) {
      lua_pop(L, 1);
      return 0;
    }
  }
  return 1;
}
int get_field(lua_State *L, ReadFrame &f, Object obj, std::string_view name,
              int write_arg = 0, int pos = -1) {
  auto &t = f.db;
  if (pos < 0) pos = t.lookup_field(obj.type, name);
  if (pos >= 0) {
    auto id = read<uint32_t>(t.fields, 8ull * pos) & id_mask;
    auto field = t.field(pos, id);
    if (field.flags & (0x10 | 0x40))
      throw std::runtime_error("Expected an instance field");
    int64_t offset = t.field_offset_from_base(field);
    auto size = read<uint32_t>(t.types, size_t(obj.type) * 72 + 20);
    if (offset < 16 || uint64_t(offset) >= size)
      throw std::runtime_error("Invalid instance field offset");
    auto bytes = std::span<uint8_t>(
        reinterpret_cast<uint8_t *>(obj.address + offset), size - offset);
    auto kind = value_kind(t, field.type);
    if (write_arg) {
      if (!f.persistent)
        throw std::runtime_error("Field update requires the live runtime");
      engine_context(f);
      if (kind == ValueKind::Struct)
        struct_field(L, f, field.type, bytes, write_arg);
      else
        write_scalar(L, write_arg, kind, bytes);
      return 0;
    }
    if (kind == ValueKind::Reference || kind == ValueKind::String)
      read_box(L, f, read<uintptr_t>(bytes, 0), field.type);
    else if (kind == ValueKind::Struct)
      struct_field(L, f, field.type, bytes, 0);
    else
      read_scalar(L, kind, bytes);
    return 1;
  }
  if (write_arg)
    throw std::runtime_error("Unknown player field: " + std::string(name));
  lua_pushnil(L);
  return 1;
}
int array_access(lua_State *L, ReadFrame &f, Object o, std::string_view name,
                 int arg) {
  auto size = *reinterpret_cast<const int32_t *>(o.address + 0x1c);
  if (size < 0)
    throw std::runtime_error("Invalid array size");
  if (name == "get_size" || name == "get_Count" || name == "get_Length") {
    lua_pushinteger(L, size);
    return 1;
  }
  auto base = f.db.find("System.Array");
  auto get = f.db.lookup_method(base, "GetValue(System.Int32)");
  if (name == "get_elements") {
    lua_createtable(L, size, 0);
    int table = lua_gettop(L);
    for (int i = 0; i < size; ++i) {
      lua_pushinteger(L, i);
      managed_call(L, f, get, o.address, table + 1);
      lua_rawseti(L, table, i + 1);
      lua_pop(L, 1);
    }
    return 1;
  }
  auto index = luaL_checkinteger(L, arg);
  if (index < 0 || index >= size)
    throw std::runtime_error("Array index out of range");
  if (name == "get_element" || name == "get_Item") {
    lua_pushinteger(L, index);
    return managed_call(L, f, get, o.address, lua_gettop(L));
  }
  auto elem = (read<uint64_t>(f.db.types, 72ull * o.type + 8) >> 19) & id_mask;
  if (!elem)
    throw std::runtime_error("Missing array element type");
  auto kind = value_kind(f.db, elem);
  if (is_live_object(L, arg + 1) || lua_isnil(L, arg + 1))
    lua_pushvalue(L, arg + 1);
  else if (kind == ValueKind::String)
    push_obj(L, f, managed_string(L, f, arg + 1), elem);
  else
    push_obj(L, f, box_scalar(L, f, arg + 1, elem), elem);
  int value = lua_gettop(L);
  lua_pushinteger(L, index);
  return managed_call(
      L, f, f.db.lookup_method(base, "SetValue(System.Object,System.Int32)"),
      o.address, value);
}
bool array_method(std::string_view name) {
  return name == "get_size" || name == "get_Count" || name == "get_Length" ||
         name == "get_elements" || name == "get_element" ||
         name == "get_Item" || name == "set_element" || name == "set_Item";
}
// Rise 16.0.1 CoreHandle exposes the embedded core (+0x18) to its layers.
// The core's tree pointer is at +8. Resolve node IDs anew on
// each status read so a retained Lua handle never dereferences a retired node.
uintptr_t layer_tree(lua_State *L, int layer) {
  auto o = object(L, layer);
  return *reinterpret_cast<const uintptr_t *>(o.address + 0x20);
}
uintptr_t motion_node(ReadFrame &f, uintptr_t tree, uint64_t id) {
  if (!tree)
    return 0;
  using Lookup = uintptr_t (*)(uintptr_t, uint64_t);
  return reinterpret_cast<Lookup>(f.main.addr(0x7d2513c))(tree, id);
}
int node_status(lua_State *L) {
  auto tree = layer_tree(L, lua_upvalueindex(2));
  auto node =
      motion_node(frame(L), tree, lua_tointeger(L, lua_upvalueindex(3)));
  if (!node)
    return luaL_error(L, "Motion node no longer exists");
  lua_pushinteger(L, *reinterpret_cast<const int32_t *>(
                         node + lua_tointeger(L, lua_upvalueindex(4))));
  return 1;
}
int node_by_id(lua_State *L) {
  auto id = luaL_checkinteger(L, 2);
  if (!motion_node(frame(L), layer_tree(L, lua_upvalueindex(2)), id)) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  for (int status = 0; status < 2; ++status) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushinteger(L, id);
    lua_pushinteger(L, 0x64 + 4 * status);
    lua_pushcclosure(L, node_status, 4);
    lua_setfield(L, -2, status ? "get_status2" : "get_status1");
  }
  return 1;
}
int invoke(lua_State *L) {
  auto &f = frame(L);
  auto obj = object(L, 1);
  std::string_view name = lua_tostring(L, lua_upvalueindex(2));
  int arg = 2;
  if (name == "call") {
    name = luaL_checkstring(L, 2);
    arg = 3;
  }
  try {
    if (name == "get_tree_object") {
      const auto core = f.db.find("via.behaviortree.BehaviorTree.CoreHandle");
      if (core && isa(f.db, obj.type, core)) {
        if (!layer_tree(L, 1)) {
          lua_pushnil(L);
          return 1;
        }
        lua_newtable(L);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushvalue(L, 1);
        lua_pushcclosure(L, node_by_id, 2);
        lua_setfield(L, -2, "get_node_by_id");
        return 1;
      }
    }
    if (name == "get_address") {
      lua_pushinteger(L, obj.address);
      return 1;
    }
    if (name == "get_reference_count") {
      // Rise NS add_ref and release atomically update the 32-bit word after the class pointer.
      auto *count = reinterpret_cast<const uint32_t *>(obj.address + sizeof(uintptr_t));
      lua_pushinteger(L, __atomic_load_n(count, __ATOMIC_RELAXED));
      return 1;
    }
    if (name == "add_ref" || name == "release" || name == "force_release") {
      if (!f.persistent || !obj.release)
        throw std::runtime_error("Expected a retained live managed object");
      lua_settop(L, 1);
      // Resident userdata already owns one reference. REFramework's add_ref is idempotent.
      if (name == "add_ref") return 1;
      auto &handle = *static_cast<Object *>(lua_touserdata(L, 1));
      lua_rawgetp(L, LUA_REGISTRYINDEX, &object_cache_key);
      lua_pushnil(L);
      lua_rawsetp(L, -2, reinterpret_cast<void *>(handle.address));
      lua_pop(L, 1);
      luaL_setmetatable(L, released_mt);
      queue_release(handle);
      return 0;
    }
    if (array_method(name) && isa(f.db, obj.type, f.db.find("System.Array")))
      return array_access(L, f, obj, name, arg);
    if (name == "get_type_definition") {
      mod_lua::push_type_definition(L, f.db, obj.type);
      return 1;
    }
    if (name == "get_field")
      return get_field(L, f, obj, luaL_checkstring(L, arg));
    if (name == "set_field")
      return get_field(L, f, obj, luaL_checkstring(L, arg), arg + 1);
    uint32_t method = 0;
    try {
      method = f.db.lookup_method(obj.type, std::string(name), lua_gettop(L) - arg + 1);
    } catch (const std::runtime_error &) {
      // Several overloads with this many parameters (BehaviorTree.setCurrentNode takes a node's hash or its name):
      // the one whose parameters fit the arguments, as a PC mod means it; on a tie the first declared, which is
      // what REFramework's call gets by name
      method = pick_overload(L, f, obj.type, name, arg);
    }
    if (method)
      return managed_call(L, f, method, obj.address, arg);
    // PC scripts also access an existing instance field as
    // get_<field>/set_<field>.
    if (name.starts_with("get_") && lua_gettop(L) < arg) {
      if (f.db.lookup_field(obj.type, name.substr(4)) >= 0)
        return get_field(L, f, obj, name.substr(4));
    }
    if (name.starts_with("set_") && lua_gettop(L) == arg)
      return get_field(L, f, obj, name.substr(4), arg);
    throw std::runtime_error("Unknown method: " + f.db.full_name(obj.type) +
                             "." + std::string(name));
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int singleton(lua_State *L) {
  auto &f = frame(L);
  std::string_view name = luaL_checkstring(L, 1);
  try {
    auto type = f.db.find(name);
    auto method = type ? f.db.lookup_method(type, "get_Instance", 0) : 0;
    if (!method) {
      lua_pushnil(L);
      return 1;
    }
    return managed_call(L, f, method, 0, 2);
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
// Rise 16.0.1 registers native DTI descriptors in a vector (items, size,
// capacity). On Switch the two destructor entries precede the instance getter.
// SceneManager, Application and LightRenderer use this same registration ABI.
struct NativeType {
  const uintptr_t *vtable;
  uint32_t hash;
  uint16_t flags;
  uint8_t padding[18];
  const char *name;
};
static_assert(offsetof(NativeType, name) == 0x20);
struct NativeTypes {
  NativeType *const *items;
  uint32_t size, capacity;
};
int native_singleton(lua_State *L) {
  auto &f = frame(L);
  const char *name = luaL_checkstring(L, 1);
  const auto &registry = *reinterpret_cast<const NativeTypes *>(f.main.addr(0x12a8c030));
  if (registry.size > registry.capacity || (registry.size && !registry.items))
    return luaL_error(L, "Invalid native type registry");
  // Cache the index, not the instance. Revalidate it if the registry changes.
  lua_getfield(L, lua_upvalueindex(2), name);
  auto index = static_cast<uint32_t>(luaL_optinteger(L, -1, UINT32_MAX));
  lua_pop(L, 1);
  auto matches = [&](uint32_t i) {
    auto *t = registry.items[i];
    return t && (t->flags & 0x80) && t->name && !strcmp(t->name, name);
  };
  if (index >= registry.size || !matches(index)) {
    index = 0;
    while (index < registry.size && !matches(index))
      ++index;
    if (index == registry.size) {
      lua_pushnil(L);
      return 1;
    }
    lua_pushinteger(L, index);
    lua_setfield(L, lua_upvalueindex(2), name);
  }
  auto *t = registry.items[index];
  if (!t->vtable || !f.main.contains(reinterpret_cast<uintptr_t>(t->vtable + 2)))
    return luaL_error(L, "Invalid native singleton descriptor");
  auto getter = t->vtable[2];
  if (getter % 4 || getter - f.main.addr(f.main.seg_off[0]) >= f.main.seg_size[0])
    return luaL_error(L, "Invalid native singleton getter");
  auto *instance = reinterpret_cast<void *(*)(NativeType *)>(getter)(t);
  if (instance)
    lua_pushlightuserdata(L, instance);
  else
    lua_pushnil(L);
  return 1;
}

uintptr_t method_receiver(lua_State *L, ReadFrame &f, uint32_t method, int index) {
  if ((read<uint16_t>(f.db.method_impl(method), 4) & 0x10) || lua_isnil(L, index))
    return 0;
  return object(L, index).address;
}
int call_native_func(lua_State *L) {
  auto &f = frame(L);
  auto type = mod_lua::type_definition_id(L, 2);
  const char *name = luaL_checkstring(L, 3);
  try {
    auto method = f.db.lookup_method(type, name);
    if (!method) {
      lua_pushnil(L);
      return 1;
    }
    return managed_call(L, f, method, method_receiver(L, f, method, 1), 4);
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int push_runtime_type(lua_State *L, ReadFrame &f, uint32_t id) {
  if (!id) {
    lua_pushnil(L);
    return 1;
  }
  // System.Object.GetType indexes the managed System.Type table by TDB ID.
  auto slot = *reinterpret_cast<uintptr_t *>(f.main.addr(0x12674830));
  auto table = slot ? *reinterpret_cast<const uintptr_t *const *>(slot) : nullptr;
  if (!table)
    throw std::runtime_error("Runtime type table unavailable");
  if (!f.persistent) {
    auto get = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(
        function(f, 704591));
    if (get(f.context, f.manager) != table[actual_type(f.db, f.manager)])
      throw std::runtime_error("Runtime type table mismatch");
  }
  push_obj(L, f, table[id], f.db.find("System.Type"));
  return 1;
}
int runtime_type(lua_State *L) {
  auto &f = frame(L);
  const char *name = luaL_checkstring(L, 1);
  try {
    return push_runtime_type(L, f, f.db.find(name));
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int instantiate(lua_State *L, ReadFrame &f, uint32_t id) {
  if (!id) {
    lua_pushnil(L);
    return 1;
  }
  const bool simplify = lua_isboolean(L, 2) && lua_toboolean(L, 2);
  push_runtime_type(L, f, id);
  if (lua_isnil(L, -1)) return 1;
  if (simplify) {
    lua_pop(L, 1);
    // NS 16.0.1 Activator.createInstance's ValType arm calls this allocator
    // directly. Select that arm without changing the shared type descriptor.
    const size_t row = 72ull * id;
    const auto kind = (read<uint32_t>(f.db.types, row + 44) >> 26) & 7;
    const auto generic = (read<uint64_t>(f.db.types, row + 48) >> 26) & 0x3ffffff;
    const bool open = generic && (read<uint32_t>(f.db.byte_pool, generic) & id_mask) == id;
    if ((kind != 1 && kind != 5) || (read<uint32_t>(f.db.types, row + 16) & 0xa0) ||
        open || id == f.db.find("System.Void") ||
        read<uint32_t>(f.db.types, row + 20) < 16 || !read<uintptr_t>(f.db.types, row + 64))
      throw std::runtime_error("Simplified allocation requires a concrete fixed-size managed type: " +
                               f.db.full_name(id));
    // The result escapes the call into Lua; request heap storage before retaining it.
    push_obj(L, f, allocate_managed(f, id, true), id);
    return 1;
  }
  auto factory = f.db.lookup_method(f.db.find("System.Activator"), "CreateInstance(System.Type)");
  if (!factory) throw std::runtime_error("System.Activator.CreateInstance is unavailable");
  return managed_call(L, f, factory, 0, lua_gettop(L), true);
}
int create_instance(lua_State *L) {
  auto &f = frame(L);
  try {
    return instantiate(L, f, f.db.find(luaL_checkstring(L, 1)));
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}

int create_number(lua_State *L) {
  auto &f = frame(L);
  try {
    auto type = f.db.find(lua_tostring(L, lua_upvalueindex(2)));
    if (!type)
      throw std::runtime_error("Missing primitive type in TDB");
    push_obj(L, f, box_scalar(L, f, 1, type), type);
    return 1;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int create_array(lua_State *L) {
  auto length = luaL_checkinteger(L, 2);
  if (length < 0 || length > 0x1000000)
    return luaL_error(L, "Invalid managed array length");
  try {
    auto &f = frame(L);
    if (lua_type(L, 1) == LUA_TSTRING) {
      lua_pushvalue(L, lua_upvalueindex(1));
      lua_pushcclosure(L, runtime_type, 1);
      lua_pushvalue(L, 1);
      lua_call(L, 1, 1);
    } else if (lua_istable(L, 1)) {
      lua_getfield(L, 1, "get_runtime_type");
      luaL_checktype(L, -1, LUA_TFUNCTION);
      lua_pushvalue(L, 1);
      lua_call(L, 1, 1);
    } else
      lua_pushvalue(L, 1);
    if (!is_live_object(L, -1))
      throw std::runtime_error("Unknown managed array element type");
    lua_pushinteger(L, length);
    auto method = f.db.lookup_method(
        f.db.find("System.Array"), "CreateInstance(System.Type,System.Int32)");
    return managed_call(L, f, method, 0, lua_gettop(L) - 1);
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}

int create_value(lua_State *L) {
  auto id = mod_lua::type_definition_id(L, 1);
  try {
    auto &f = frame(L);
    if (!isa(f.db, id, f.db.find("System.ValueType")) ||
        id == f.db.find("System.Void"))
      throw std::runtime_error("ValueType.new requires a concrete value type");
    auto size = read<uint32_t>(f.db.impl(72ull * id), 8);
    if (!size)
      throw std::runtime_error("Value type has no concrete storage");
    auto box = allocate_managed(f, id, true);
    auto bytes = boxed_payload(f, box, id);
    if (size > bytes.size())
      throw std::runtime_error("Value exceeds boxed payload");
    memset(bytes.data(), 0, size);
    push_obj(L, f, box, id);
    return 1;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int create_ref(lua_State *L) {
  auto id = mod_lua::type_definition_id(L, 1);
  lua_settop(L, 2);
  try {
    auto &f = frame(L);
    auto kind = value_kind(f.db, id);
    if (kind != ValueKind::Reference && kind != ValueKind::String)
      throw std::runtime_error("Expected a managed reference type");
    if (!lua_isnil(L, 2))
      live_object_address(L, 2, id);
    auto *declared = static_cast<uint32_t *>(lua_newuserdatauv(L, sizeof(id), 1));
    *declared = id;
    luaL_setmetatable(L, ref_mt);
    int slot = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushcclosure(L, create_array, 1);
    lua_pushliteral(L, "System.Object");
    lua_pushinteger(L, 1);
    lua_call(L, 2, 1);
    if (!lua_isnil(L, 2)) {
      lua_pushvalue(L, 2);
      lua_seti(L, -2, 0);
    }
    lua_setiuservalue(L, slot, 1);
    return 1;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int read_ref(lua_State *L) {
  auto *declared = static_cast<uint32_t *>(luaL_checkudata(L, 1, ref_mt));
  try {
    auto &f = frame(L);
    auto array = ref_array(L, f, 1);
    auto value = *reinterpret_cast<const uintptr_t *>(array.address + 0x20);
    push_obj(L, f, value, *declared);
    return 1;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
} // namespace
ReadFrame &live_frame(lua_State *L) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &frame_key);
  auto *f = static_cast<ReadFrame *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  if (!f)
    throw std::runtime_error("Method requires the live game runtime");
  return *f;
}
bool is_live_object(lua_State *L, int index) {
  return luaL_testudata(L, index, object_mt) != nullptr;
}
uintptr_t native_method_address(lua_State *L, uint32_t method) {
  return function(live_frame(L), method);
}
int descriptor_function(lua_State *L) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &frame_key);
  auto *frame = static_cast<ReadFrame *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  auto method = static_cast<uint32_t>(lua_tointeger(L, lua_upvalueindex(2)));
  try {
    if (frame && read<int32_t>(frame->db.methods, 12ull * method + 8))
      lua_pushlightuserdata(L, reinterpret_cast<void *>(function(*frame, method)));
    else
      lua_pushnil(L);
    return 1;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int access_value_field(lua_State *L, Tdb &t, uint32_t id, std::span<uint8_t> bytes,
                       std::string_view name, bool write) {
  auto pos = t.lookup_field(id, name);
  if (pos < 0)
    throw std::runtime_error("Unknown value field: " + std::string(name));
  auto owner = read<uint32_t>(t.fields, 8ull * pos) & id_mask;
  auto field = t.field(pos, owner);
  if (field.flags & (0x10 | 0x40) || field.offset >= bytes.size())
    throw std::runtime_error("Expected a field inside the value buffer");
  auto data = bytes.subspan(field.offset);
  auto kind = value_kind(t, field.type);
  if (write) {
    write_scalar(L, 4, kind, data);
    return 0;
  }
  read_scalar(L, kind, data);
  return 1;
}
uintptr_t live_object_address(lua_State *L, int index, uint32_t expected) {
  auto *obj = static_cast<Object *>(luaL_testudata(L, index, object_mt));
  if (!obj)
    throw std::runtime_error("Expected a live managed object");
  if (expected && !isa(live_frame(L).db, obj->type, expected))
    throw std::runtime_error("Incompatible managed hook argument");
  return obj->address;
}
bool push_cached_live_object(lua_State *L, uintptr_t address,
                             uint32_t expected) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &object_cache_key);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return false;
  }
  lua_rawgetp(L, -1, reinterpret_cast<void *>(address));
  auto *obj = static_cast<Object *>(luaL_testudata(L, -1, object_mt));
  if (obj && isa(live_frame(L).db, obj->type, expected)) {
    lua_remove(L, -2);
    return true;
  }
  lua_pop(L, 2);
  return false;
}
void validate_reflection_hook(lua_State *L, uint32_t method) {
  auto &f = live_frame(L);
  auto sig = callable_signature(f.db, method);
  auto n = read<uint16_t>(f.db.byte_pool, sig);
  for (unsigned i = 0; i <= n; ++i) {
    if (!f.db.plain_parameter(sig, i))
      throw std::runtime_error(
          "Hook signature contains a pointer or by-reference value");
    (void)value_kind(f.db, f.db.parameter_type(sig, i));
  }
}
void push_live_object(lua_State *L, uintptr_t address, uint32_t expected) {
  push_obj(L, live_frame(L), address, expected);
}
int invoke_descriptor(lua_State *L) {
  try {
    auto &f = live_frame(L);
    auto id = uint32_t(lua_tointeger(L, lua_upvalueindex(2)));
    auto receiver = method_receiver(L, f, id, 2);
    return managed_call(L, f, id, receiver, 3);
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
static int descriptor_field(lua_State *L, bool write) {
  try {
    auto &f = live_frame(L);
    uint32_t i = lua_tointeger(L, lua_upvalueindex(2));
    auto declaring = read<uint32_t>(f.db.fields, 8ull * i) & id_mask;
    auto field = f.db.field(i, declaring);
    if (write && (field.flags & (0x10 | 0x20 | 0x40)))
      throw std::runtime_error("Expected a writable instance field");
    if ((field.flags & 0x50) == 0x50) {
      auto off = f.db.literal_offset(i);
      if (off < 0) {
        auto str = f.db.name(-int64_t(off));
        lua_pushlstring(L, str.data(), str.size());
      } else
        read_scalar(L, value_kind(f.db, field.type),
                    slice(f.db.byte_pool, off, f.db.byte_pool.size() - off));
      return 1;
    }
    auto o = object(L, 2);
    if (!isa(f.db, o.type, declaring))
      throw std::runtime_error("Incorrect field receiver");
    return get_field(L, f, o, field.name, write ? 3 : 0, i);
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int read_descriptor_field(lua_State *L) { return descriptor_field(L, false); }
int write_descriptor_field(lua_State *L) { return descriptor_field(L, true); }
int descriptor_runtime_type(lua_State *L) {
  try {
    auto &f = live_frame(L);
    auto id = uint32_t(lua_tointeger(L, lua_upvalueindex(2)));
    return push_runtime_type(L, f, id);
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int descriptor_create_instance(lua_State *L) {
  try {
    return instantiate(L, live_frame(L), lua_tointeger(L, lua_upvalueindex(2)));
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
static uintptr_t camera_address(ReadFrame &f) {
  auto ctx = engine_context(f);
  auto slot = *reinterpret_cast<const uintptr_t *>(f.main.addr(0x12674898));
  auto scene = slot ? *reinterpret_cast<const uintptr_t *>(slot) : 0;
  using Get = uintptr_t (*)(uintptr_t, uintptr_t);
  auto view = scene ? reinterpret_cast<Get>(function(f, 734924))(ctx, scene) : 0;
  return view ? reinterpret_cast<Get>(function(f, 734346))(ctx, view) : 0;
}
std::optional<CameraMatrices> camera_matrices(lua_State *L) {
  auto &f = live_frame(L);
  auto camera = camera_address(f);
  if (!camera) return std::nullopt;
  auto type = f.db.find("via.Camera");
  auto read_matrix = [&](const char *name, std::array<float, 16> &out) {
    auto method = f.db.lookup_method(type, name, 0);
    if (!method) throw std::runtime_error(std::string("Missing camera getter: ") + name);
    managed_call(L, f, method, camera, lua_gettop(L) + 1);
    auto *matrix = luaL_testudata(L, -1, "rise.Matrix4x4f");
    if (!matrix || lua_rawlen(L, -1) != sizeof(out))
      throw std::runtime_error(std::string("Expected camera matrix from ") + name);
    memcpy(out.data(), matrix, sizeof(out));
    lua_pop(L, 1);
  };
  CameraMatrices matrices;
  read_matrix("get_ViewMatrix", matrices.view);
  read_matrix("get_ProjectionMatrix", matrices.projection);
  return matrices;
}
int primary_camera(lua_State *L) {
  try {
    auto &f = frame(L);
    auto cam = camera_address(f);
    push_obj(L, f, cam, f.db.parameter_type(f.db.signature_offset(734346), 0));
    return 1;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
uintptr_t current_vm_context(const Module &m) {
  size_t words = 0;
  auto *layout = tls::observed_layout(words);
  if (!layout || words <= 4)
    return 0;
  int32_t index;
  memcpy(&index, reinterpret_cast<const char *>(layout) + 0x20, sizeof(index));
  auto vm = *reinterpret_cast<const uintptr_t *>(m.addr(0x12950358));
  if (!vm || index < 0 ||
      uint32_t(index) >= *reinterpret_cast<uint32_t *>(vm + 0x410))
    return 0;
  auto table = *reinterpret_cast<const uintptr_t *const *>(vm + 0x408);
  return table ? table[index] : 0;
}
void drain_managed_releases() {
  std::unique_lock lua_lock(mod_lua::lua_mutex(), std::try_to_lock);
  if (!lua_lock.owns_lock()) return;
  std::vector<std::pair<uintptr_t, uintptr_t>> batch;
  {
    std::lock_guard lock(releases_mutex);
    batch.swap(releases);
  }
  for (auto [release, obj] : batch)
    reinterpret_cast<void (*)(uintptr_t)>(release)(obj);
}
static int indexed_item(lua_State *L, ReadFrame &f, Object o, bool write) {
  if (isa(f.db, o.type, f.db.find("System.Array")))
    return array_access(L, f, o, write ? "set_element" : "get_element", 2);
  // Closed IList implementations expose get_Item(Int32) and optionally set_Item(Int32,T).
  auto getter = f.db.lookup_method(o.type, "get_Item(System.Int32)", 1);
  if (!getter) throw std::runtime_error("Managed object has no integer indexer");
  auto method = getter;
  if (write) {
    auto element = f.db.parameter_type(f.db.signature_offset(getter), 0);
    method = f.db.lookup_method(o.type, "set_Item(System.Int32," + f.db.full_name(element) + ")", 2);
    if (!method) throw std::runtime_error("Managed integer indexer is read-only");
  }
  return managed_call(L, f, method, o.address, 2);
}
int next_managed_item(lua_State *L) {
  try {
    auto index = luaL_checkinteger(L, 2) + 1;
    if (index >= lua_tointeger(L, lua_upvalueindex(2)))
      return 0;
    lua_settop(L, 1);
    lua_pushinteger(L, index);
    indexed_item(L, frame(L), object(L, 1), false);
    lua_pushinteger(L, index);
    lua_insert(L, -2);
    return 2;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
int managed_pairs(lua_State *L) {
  try {
    auto &f = frame(L);
    auto o = object(L, 1);
    auto array = f.db.find("System.Array");
    if (!isa(f.db, o.type, array) &&
        !f.db.lookup_method(o.type, "get_Item(System.Int32)", 1)) {
      auto convert = f.db.lookup_method(o.type, "ToArray()", 0);
      if (!convert)
        throw std::runtime_error(
            "Collection has no indexed items or ToArray()");
      managed_call(L, f, convert, o.address, 2);
      lua_replace(L, 1);
      o = object(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    if (isa(f.db, o.type, array))
      array_access(L, f, o, "get_size", 2);
    else {
      auto count = f.db.lookup_method(o.type, "get_Count()", 0);
      if (!count)
        throw std::runtime_error("Collection has no count");
      managed_call(L, f, count, o.address, lua_gettop(L) + 1);
    }
    lua_pushcclosure(L, next_managed_item, 2);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, -1);
    return 3;
  } catch (const std::exception &e) {
    lua_pushstring(L, e.what());
  }
  return lua_error(L);
}
void open_live_read(lua_State *L, ReadFrame &f) {
  lua_pushlightuserdata(L, &f);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &frame_key);
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &object_cache_key);
  luaL_newmetatable(L, released_mt);
  lua_pushcfunction(L, [](lua_State *s) { return luaL_error(s, "Managed handle has been released"); });
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  luaL_newmetatable(L, object_mt);
  lua_pushlightuserdata(L,&f);
  lua_pushcclosure(L,managed_pairs,1);
  lua_setfield(L,-2,"__pairs");
  lua_pushlightuserdata(L, &f);
  lua_pushcclosure(
      L,
      [](lua_State *s) {
        auto &f = frame(s);
        auto o = object(s, 1);
        try {
          if (lua_isinteger(s, 2))
            return indexed_item(s, f, o, false);
          if (lua_type(s, 2) == LUA_TSTRING &&
              f.db.lookup_field(o.type, lua_tostring(s, 2)) >= 0)
            return get_field(s, f, o, lua_tostring(s, 2));
        } catch (const std::exception &e) {
          lua_pushstring(s, e.what());
          return lua_error(s);
        }
        lua_pushvalue(s, lua_upvalueindex(1));
        lua_pushvalue(s, 2);
        lua_pushcclosure(s, invoke, 2);
        return 1;
      },
      1);
  lua_setfield(L, -2, "__index");
  lua_pushlightuserdata(L, &f);
  lua_pushcclosure(
      L,
      [](lua_State *s) {
        try {
          auto &f = frame(s);
          auto o = object(s, 1);
          if (lua_isinteger(s, 2))
            return indexed_item(s, f, o, true);
          return get_field(s, f, o, luaL_checkstring(s, 2), 3);
        } catch (const std::exception &e) {
          lua_pushstring(s, e.what());
        }
        return lua_error(s);
      },
      1);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, [](lua_State *s) {
    auto *left = static_cast<Object *>(luaL_testudata(s, 1, object_mt));
    auto *right = static_cast<Object *>(luaL_testudata(s, 2, object_mt));
    lua_pushboolean(s, left && right && left->address == right->address);
    return 1;
  });
  lua_setfield(L, -2, "__eq");
  lua_pushcfunction(L, [](lua_State *s) {
    auto *p = static_cast<Object *>(luaL_checkudata(s, 1, object_mt));
    queue_release(*p);
    return 0;
  });
  lua_setfield(L, -2, "__gc");
  lua_pop(L, 1);
  luaL_newmetatable(L, ref_mt);
  lua_newtable(L);
  lua_pushlightuserdata(L, &f);
  lua_pushcclosure(L, read_ref, 1);
  lua_setfield(L, -2, "get");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  lua_newtable(L);
  lua_pushlightuserdata(L, &f);
  lua_pushcclosure(L, create_value, 1);
  lua_setfield(L, -2, "new");
  lua_setglobal(L, "ValueType");
  lua_getglobal(L, "sdk");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }
  if (!f.persistent) {
    lua_pushcfunction(L, [](lua_State *s) {
      lua_pushboolean(s, is_live_object(s, 1));
      return 1;
    });
    lua_setfield(L, -2, "is_managed_object");
  }
  lua_pushlightuserdata(L, &f);
  lua_newtable(L);
  lua_pushcclosure(L, native_singleton, 2);
  lua_setfield(L, -2, "get_native_singleton");
  lua_pushlightuserdata(L, &f);
  lua_pushcclosure(
      L,
      [](lua_State *s) {
        auto &f = frame(s);
        auto ctx = f.persistent ? current_vm_context(f.main) : f.context;
        if (ctx)
          lua_pushlightuserdata(s, reinterpret_cast<void *>(ctx));
        else
          lua_pushnil(s);
        return 1;
      },
      1);
  lua_setfield(L, -2, "get_thread_context");
  lua_pushlightuserdata(L, &f);
  lua_pushcclosure(
      L,
      [](lua_State *s) {
        luaL_checktype(s, 1, LUA_TSTRING);
        try {
          auto &f = frame(s);
          push_obj(s, f, managed_string(s, f, 1), f.db.find("System.String"));
          return 1;
        } catch (const std::exception &e) {
          lua_pushstring(s, e.what());
        }
        return lua_error(s);
      },
      1);
  lua_setfield(L, -2, "create_managed_string");
  for (auto [key, func] :
       {std::pair<const char *, lua_CFunction>{"typeof", runtime_type},
        {"get_managed_singleton", singleton},
        {"call_native_func", call_native_func},
        {"create_instance", create_instance}, {"create_ref", create_ref},
        {"get_primary_camera", primary_camera},
        {"create_managed_array", create_array}}) {
    lua_pushlightuserdata(L, &f);
    lua_pushcclosure(L, func, 1);
    lua_setfield(L, -2, key);
  }
  lua_pushlightuserdata(L, &f);
  lua_pushcclosure(
      L,
      [](lua_State *s) {
        auto &f = frame(s);
        const char *name = luaL_checkstring(s, 1);
        try {
          mod_lua::push_type_definition(s, f.db, f.db.find(name));
          return 1;
        } catch (const std::exception &e) {
          lua_pushstring(s, e.what());
        }
        return lua_error(s);
      },
      1);
  lua_setfield(L, -2, "find_type_definition");
  for (auto [name, type] : {
           std::pair{"create_sbyte", "System.SByte"}, {"create_byte", "System.Byte"},
           {"create_int16", "System.Int16"}, {"create_uint16", "System.UInt16"},
           {"create_int32", "System.Int32"}, {"create_uint32", "System.UInt32"},
           {"create_int64", "System.Int64"}, {"create_uint64", "System.UInt64"},
           {"create_single", "System.Single"}, {"create_double", "System.Double"}}) {
    lua_pushlightuserdata(L, &f);
    lua_pushstring(L, type);
    lua_pushcclosure(L, create_number, 2);
    lua_setfield(L, -2, name);
  }
  lua_pushlightuserdata(L, &f);
  lua_pushliteral(L, "call");
  lua_pushcclosure(L, invoke, 2);
  lua_setfield(L, -2, "call_object_func");
  lua_setglobal(L, "sdk");
}
} // namespace rise
