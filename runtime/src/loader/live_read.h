#pragma once
#include "loader/tdb71.h"
struct lua_State;
namespace rise {
struct ReadFrame {
  const Module &main;
  Tdb &db;
  uintptr_t context;
  uintptr_t manager;
  uintptr_t encoded_base;
  bool persistent = false;
};
void open_live_read(lua_State *L, ReadFrame &frame);
void validate_reflection_hook(lua_State *L, uint32_t method);
uintptr_t native_method_address(lua_State *, uint32_t method);
int descriptor_function(lua_State *);
uintptr_t live_object_address(lua_State *, int index, uint32_t expected = 0);
bool push_cached_live_object(lua_State *, uintptr_t, uint32_t expected = 0);
void push_live_object(lua_State *, uintptr_t, uint32_t);
bool is_live_object(lua_State *, int);
int access_value_field(lua_State *, Tdb &, uint32_t, std::span<uint8_t>,
                       std::string_view, bool write);
int invoke_descriptor(lua_State *);
int read_descriptor_field(lua_State *);
int write_descriptor_field(lua_State *);
int descriptor_runtime_type(lua_State *);
int descriptor_create_instance(lua_State *);
uintptr_t current_vm_context(const Module &);
void drain_managed_releases();
} // namespace rise
