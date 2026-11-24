#pragma once
#include <vector>
#include "loader/nso.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>

namespace rise {
using Bytes = std::span<const uint8_t>;
constexpr uint32_t id_mask = (1u << 19) - 1;
inline Bytes slice(Bytes data, uint64_t offset, uint64_t length) {
  if (offset > data.size() || length > data.size() - offset)
    throw std::runtime_error("TDB range exceeds NSO data segment");
  return data.subspan(offset, length);
}

template <class T> T read(Bytes data, size_t offset) {
  T value;
  memcpy(&value, slice(data, offset, sizeof(T)).data(), sizeof(T));
  return value;
}

// Capture table spans while the header still has offsets. Row IDs and name
// offsets remain valid after initialization; native method offsets become
// available then.
struct Tdb {
  mutable std::unordered_map<std::string, uint32_t> resolved_names;
  mutable std::map<std::tuple<uint32_t, std::string, int>, uint32_t>
      resolved_methods;
  mutable std::map<std::pair<uint32_t, std::string>, int64_t> resolved_fields;
  Bytes types, type_impls, methods, method_impls, strings, fields, field_impls,
      parameters, byte_pool, initializers;
  explicit Tdb(const Module &main) {
    auto data =
        slice({main.base, main.size}, main.seg_off[2], main.seg_size[2]);
    const uint8_t signature[] = {'T', 'D', 'B', 0, 71, 0, 0, 0};
    auto start = std::search(data.begin(), data.end(), std::begin(signature),
                             std::end(signature));
    if (start == data.end())
      throw std::runtime_error("TDB71 not found");
    Bytes db = data.subspan(start - data.begin());
    if (read<uint32_t>(db, 8) != 0)
      throw std::runtime_error("Expected pre-initialization TDB");
    auto table = [&](size_t offset, size_t count, uint64_t stride) {
      return slice(db, read<uint64_t>(db, offset),
                   read<uint32_t>(db, count) * stride);
    };
    types = table(0x60, 0x0c, 72);
    type_impls = table(0x68, 0x18, 48);
    methods = table(0x70, 0x10, 12);
    method_impls = table(0x78, 0x20, 12);
    strings = table(0xd0, 0x50, 1);
    fields = table(0x80, 0x14, 8);
    field_impls = table(0x88, 0x1c, 12);
    parameters = table(0xa8, 0x30, 12);
    initializers = table(0xb8, 0x38, 4);
    byte_pool = table(0xd8, 0x54, 1);
  }
  std::string_view name(uint32_t offset) const {
    if (offset >= strings.size())
      throw std::runtime_error("Invalid name offset");
    const char *s = reinterpret_cast<const char *>(strings.data() + offset);
    const char *end =
        static_cast<const char *>(memchr(s, 0, strings.size() - offset));
    if (!end)
      throw std::runtime_error("Unterminated TDB name");
    return {s, static_cast<size_t>(end - s)};
  }
  Bytes impl(size_t row) const {
    auto id = (read<uint64_t>(types, row + 8) >> 38) & 0x3ffff;
    return slice(type_impls, id * 48, 48);
  }
  struct Field {
    uint32_t owner, type, offset, flags;
    std::string_view name;
  };
  std::pair<size_t, size_t> field_range(uint32_t type) const {
    size_t row = size_t(type) * 72;
    return {(read<uint64_t>(types, row + 32) >> 44) & id_mask,
            (read<uint64_t>(impl(row), 16) >> 33) & 0xffffff};
  }
  Field field(size_t index, uint32_t owner) const {
    auto bits = read<uint64_t>(fields, index * 8);
    if ((bits & id_mask) != owner)
      throw std::runtime_error("Field owner mismatch");
    auto info = slice(field_impls, ((bits >> 19) & id_mask) * 12, 12);
    return {owner, uint32_t((bits >> 38) & id_mask),
            read<uint32_t>(info, 4) & 0x3ffffff,
            uint32_t(read<uint16_t>(info, 2) >> 1),
            name(read<uint32_t>(info, 8) & 0xfffffff)};
  }
  int64_t field_offset_from_base(const Field &field) const {
    auto vt = read<uintptr_t>(types, 72ull * field.owner + 64);
    return int64_t(field.offset) +
           (vt ? *reinterpret_cast<const int32_t *>(vt - 8) : 0);
  }
  int64_t lookup_field(uint32_t type, std::string_view name) const {
    auto key = std::pair{type, std::string(name)};
    if (auto i = resolved_fields.find(key); i != resolved_fields.end())
      return i->second;
    int64_t found = -1;
    for (auto id = type; id && found < 0; id = parent(id)) {
      auto [first, count] = field_range(id);
      for (size_t i = first; i < first + count; ++i)
        if (field(i, id).name == name) {
          found = i;
          break;
        }
    }
    resolved_fields.emplace(std::move(key), found);
    return found;
  }
  uint32_t find(std::string_view full) const {
    std::string key(full);
    if (auto it = resolved_names.find(key); it != resolved_names.end())
      return it->second;
    if (full.ends_with("[]")) {
      auto element = find(full.substr(0, full.size() - 2));
      auto id =
          element
              ? uint32_t(read<uint64_t>(types, 72ull * element + 8) & id_mask)
              : 0;
      resolved_names.emplace(std::move(key), id);
      return id;
    }
    size_t start = 0, end = full.size(), depth = 0;
    for (size_t i = 0; i < full.size(); ++i) {
      if (full[i] == '<' && depth++ == 0)
        end = i;
      else if (full[i] == '>' && depth)
        --depth;
      else if (full[i] == '.' && !depth) {
        start = i + 1;
        end = full.size();
      }
    }
    auto short_name = full.substr(start, end - start);
    for (size_t row = 72; row < types.size(); row += 72) {
      const auto ids = read<uint64_t>(types, row);
      auto info = impl(row);
      if (name(read<uint32_t>(info, 0)) == short_name &&
          full_name(row / 72) == full) {
        if ((ids & id_mask) != row / 72)
          throw std::runtime_error("Invalid declaring type ID");
        resolved_names.emplace(std::move(key), row / 72);
        return row / 72;
      }
    }
    resolved_names.emplace(std::move(key), 0);
    return 0;
  }
  std::string_view method(size_t id, uint32_t owner) const {
    auto row = slice(methods, id * 12, 12);
    if ((read<uint32_t>(row, 0) & id_mask) != owner)
      throw std::runtime_error("Invalid method owner");
    auto impl_id = read<uint32_t>(row, 4) & id_mask;
    return name(read<uint32_t>(method_impls, impl_id * 12 + 8));
  }
  Bytes method_impl(uint32_t id) const {
    return slice(method_impls,
                 (read<uint32_t>(methods, id * 12ull + 4) & id_mask) * 12ull,
                 12);
  }
  std::pair<uint32_t, uint32_t> method_range(uint32_t id) const {
    auto row = id * 72ull;
    return {uint32_t((read<uint64_t>(types, row + 32) >> 22) & 0x3fffff),
            read<uint16_t>(impl(row), 24)};
  }
  uint32_t lookup_method(uint32_t type, std::string name,
                         int arity = -1) const {
    name.erase(std::remove_if(name.begin(), name.end(),
                              [](char c) { return c == ' ' || c == '\t'; }),
               name.end());
    auto key = std::tuple{type, name, arity};
    if (auto i = resolved_methods.find(key); i != resolved_methods.end())
      return i->second;
    auto open = name.find('(');
    auto short_name = name.substr(0, open);
    uint32_t found = 0;
    for (uint32_t id = type; id && !found; id = parent(id)) {
      auto [first, count] = method_range(id);
      for (uint32_t k = first; k < first + count; ++k) {
        if (method(k, id) != short_name ||
            (arity >= 0 &&
             read<uint16_t>(byte_pool, signature_offset(k)) != arity))
          continue;
        if (open != name.npos) {
          auto p = prototype(k, id);
          p.erase(std::remove(p.begin(), p.end(), ' '), p.end());
          if (p != name)
            continue;
        }
        if (arity < 0) {
          // RETypeDefinition::get_method picks the first concrete overload in
          // declaration order. Object:call still resolves by name and arity.
          auto sig = signature_offset(k);
          auto count = read<uint16_t>(byte_pool, sig);
          bool generic_definition = false;
          for (unsigned slot = 0; slot <= count; ++slot) {
            auto arg = parameter_type(sig, slot);
            if (this->name(read<uint32_t>(impl(72ull * arg), 0)).find('!') !=
                std::string_view::npos) {
              generic_definition = true;
              break;
            }
          }
          if (generic_definition)
            continue;
          found = k;
          break;
        }
        if (found)
          throw std::runtime_error("Ambiguous overload: " + name);
        found = k;
      }
    }
    resolved_methods.emplace(std::move(key), found);
    return found;
  }
  // Every overload of a name with that many parameters, nearest type first and in declaration order (for picking
  // one by the arguments when lookup_method finds several)
  std::vector<uint32_t> overloads(uint32_t type, std::string_view short_name, int arity) const {
    std::vector<uint32_t> out;
    for (uint32_t id = type; id && out.empty(); id = parent(id)) {
      auto [first, count] = method_range(id);
      for (uint32_t k = first; k < first + count; ++k)
        if (method(k, id) == short_name && read<uint16_t>(byte_pool, signature_offset(k)) == arity)
          out.push_back(k);
    }
    return out;
  }
  uint32_t parameter_row(size_t signature, unsigned slot) const {
    return read<uint32_t>(byte_pool, signature + 4 + 4 * slot);
  }
  uint32_t parameter_modifier(size_t signature, unsigned slot) const {
    return read<uint32_t>(parameters,
                          parameter_row(signature, slot) * 12ull + 4) >> 30;
  }
  bool plain_parameter(size_t signature, unsigned slot) const {
    return parameter_modifier(signature, slot) == 0;
  }
  uint32_t parent(uint32_t type) const {
    return (read<uint64_t>(types, size_t(type) * 72) >> 19) & id_mask;
  }
  std::string full_name(uint32_t type) const {
    auto info = impl(size_t(type) * 72);
    if (name(read<uint32_t>(info, 0)).empty()) {
      auto element = (read<uint64_t>(types, 72ull * type + 8) >> 19) & id_mask;
      if (element && parent(type) &&
          name(read<uint32_t>(impl(72ull * parent(type)), 0)) == "Array")
        return full_name(element) + "[]";
    }
    auto owner = (read<uint64_t>(types, size_t(type) * 72) >> 38) & id_mask;
    auto ns =
        owner ? full_name(owner) : std::string(name(read<uint32_t>(info, 4)));
    if (!ns.empty())
      ns += '.';
    ns += name(read<uint32_t>(info, 0));
    auto generics = (read<uint64_t>(types, size_t(type) * 72 + 48) >> 26) & 0x3ffffff;
    if (generics) {
      auto header = read<uint32_t>(byte_pool, generics);
      auto count = header >> 19;
      if (count && (header & id_mask) != type) {
        ns += '<';
        for (uint32_t i = 0; i < count; ++i) {
          if (i)
            ns += ',';
          auto arg = read<uint32_t>(byte_pool, generics + 4 + 4 * i);
          if (arg && arg < types.size() / 72)
            ns += full_name(arg);
        }
        ns += '>';
      }
    }
    return ns;
  }
  size_t signature_offset(size_t id) const {
    auto row = slice(methods, id * 12, 12);
    return (read<uint32_t>(row, 0) >> 19) |
           ((read<uint32_t>(row, 4) >> 19) << 13);
  }
  int32_t literal_offset(uint32_t field_id) const {
    auto bits = read<uint64_t>(fields, field_id * 8ull);
    auto index = ((bits >> 19) & id_mask) * 12ull;
    auto lo = read<uint32_t>(field_impls, index + 4) >> 26,
         mid = read<uint32_t>(field_impls, index + 8) >> 28;
    auto initializer = lo | (mid << 6) | (((bits >> 57) & 63) << 10);
    return read<int32_t>(initializers, initializer * 4ull);
  }
  uint32_t parameter_type(size_t signature, unsigned slot) const {
    auto param = read<uint32_t>(
        byte_pool, signature + 4 + 4 * slot); // slot 0 is the return type
    return read<uint32_t>(parameters, size_t(param) * 12 + 8) & id_mask;
  }
  std::string prototype(size_t id, uint32_t owner) const {
    size_t offset = signature_offset(id);
    auto count = read<uint16_t>(byte_pool, offset);
    std::string result(method(id, owner));
    result += '(';
    for (unsigned i = 0; i < count; ++i) {
      if (i)
        result += ", ";
      result += full_name(parameter_type(offset, i + 1));
    }
    return result + ")";
  }
};

} // namespace rise
