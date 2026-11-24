#!/usr/bin/env python3
"""List methods declared by a top-level type in Rise's on-disk NSO/TDB71.

Example: python3 tools/query_tdb.py /path/to/exefs/main snow.player.PlayerManager
"""
import argparse
from pathlib import Path
import struct

from nso import lz4_block


TDB71 = b"TDB\0" + struct.pack("<I", 71)


def read_tdb(path):
    with path.open("rb") as stream:
        header = stream.read(0x100)
        if len(header) != 0x100 or header[:4] != b"NSO0":
            raise ValueError("Expected a complete NSO header")
        file_offset, _, size = struct.unpack_from("<III", header, 0x30)
        packed_size, = struct.unpack_from("<I", header, 0x68)
        flags, = struct.unpack_from("<I", header, 0x0C)
        stored_size = packed_size if flags & 4 else size
        if file_offset + stored_size > stream.seek(0, 2):
            raise ValueError("Truncated NSO data segment")
        stream.seek(file_offset)
        segment = stream.read(stored_size)
        if flags & 4:
            try:
                segment = lz4_block(segment, size)
            except (IndexError, ValueError, ZeroDivisionError) as error:
                raise ValueError("Invalid LZ4 data segment") from error
    if segment.count(TDB71) != 1:
        raise ValueError("Expected exactly one TDB71 in the NSO data segment")
    return memoryview(segment)[segment.index(TDB71):]


def query(db, full_name):
    # Layouts: REFramework shared/sdk/RETypeDB.hpp and RETypeDefinition.hpp, v71.
    num_types, num_methods, num_fields, num_type_impl, _, num_method_impl = struct.unpack_from("<6I", db, 0x0C)
    types, type_impls, methods, method_impls = struct.unpack_from("<4Q", db, 0x60)
    for start, count, stride in ((types, num_types, 0x48), (type_impls, num_type_impl, 0x30),
                                 (methods, num_methods, 12), (method_impls, num_method_impl, 12)):
        if start + count * stride > len(db):
            raise ValueError("Truncated TDB metadata table")
    pool_offset, = struct.unpack_from("<Q", db, 0xD0)
    pool_size, = struct.unpack_from("<I", db, 0x50)
    pool = bytes(db[pool_offset:pool_offset + pool_size])
    if len(pool) != pool_size:
        raise ValueError("Truncated TDB string pool")

    def string(offset):
        return pool[offset:pool.index(b"\0", offset)].decode("utf-8")

    namespace, _, name = full_name.rpartition(".")
    print(f"TDB71: {num_types} types, {num_methods} methods, {num_fields} fields")
    matches = 0
    for idx in range(1, num_types):
        row = types + 0x48 * idx
        ids, links = struct.unpack_from("<QQ", db, row)
        impl_id = (links >> 38) & 0x3FFFF
        if impl_id >= num_type_impl:
            raise ValueError(f"Invalid implementation index for type {idx}")
        impl = type_impls + impl_id * 0x30
        name_offset, ns_offset = struct.unpack_from("<II", db, impl)
        if (ids >> 38) & 0x7FFFF or string(name_offset) != name or string(ns_offset) != namespace:
            continue
        if (ids & 0x7FFFF) != idx:
            raise ValueError("Type row ID mismatch")
        members, = struct.unpack_from("<Q", db, row + 0x20)
        first = (members >> 22) & 0x3FFFFF
        count, = struct.unpack_from("<H", db, impl + 0x18)
        if first + count > num_methods:
            raise ValueError("Invalid member method range")
        print(f"{full_name} [type {idx}]: {count} declared methods")
        for method in range(first, first + count):
            owner, impl_bits = struct.unpack_from("<II", db, methods + 12 * method)
            method_impl = impl_bits & 0x7FFFF
            if (owner & 0x7FFFF) != idx or method_impl >= num_method_impl:
                raise ValueError(f"Invalid owner or implementation for method {method}")
            offset, = struct.unpack_from("<I", db, method_impls + 12 * method_impl + 8)
            print(f"  {method}: {string(offset)}")
        matches += 1
    if not matches:
        raise ValueError(f"No top-level type named {full_name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("main", type=Path, help="Path to the extracted ExeFS main NSO")
    parser.add_argument("type_name", help="Namespace-qualified, top-level type name")
    args = parser.parse_args()
    try:
        query(read_tdb(args.main), args.type_name)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")
