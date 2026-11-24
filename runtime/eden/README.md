# Eden's shader recompiler

`src/shader_recompiler` and the headers it uses from `src/common` are from [Eden](https://git.eden-emu.dev/eden-emu/eden)
(GPL-3.0-or-later, [LICENSE.txt](LICENSE.txt); each file keeps its copyright header), taken at commit
`060026dc2d1daa749cf0b39378f132f506cbfd93`. They used to be fetched by `tools/setup.sh` and patched at setup; they're
kept here so this project can change them directly. Only the SPIR-V backend is built (`runtime/recompiler/CMakeLists.txt`).

This project's changes, on top of that commit:

- Bindless arrays: textures indexed from a constant buffer with a dynamic index, counted up to the 64 KB a constant
  buffer can hold, keeping the `& mask | sampler` the game applies to a handle (without it the fixed sampler is lost:
  the shadow sampler, for one).
- Draw parameters: base vertex, base instance and draw ID from Vulkan's draw parameters.
- Geometry shaders that only use stream 0.
- Invariant position: the same position math in every shader, so a depth pre-pass and an EQUAL colour pass agree.
- Which attributes count as generics (`IsGeneric`).
- Mali (`has_broken_pack_half`): on ARM's older drivers, packHalf2x16 and f32-to-f16 conversions in vertex stages as
  integer operations.
  With it, the point size also goes through a float operation (`max(size, 1)`): written straight from an integer
  register (the terrain's tessellation), it aborted the driver's compiler.
- Quad butterflies: in pixel shaders, butterfly shuffles by 1..3 become quad swaps, which include the helper pixels on
  triangle edges.
- Qualcomm: an `OpReturn` in place of `OpUnreachable` where its driver mis-optimises.
- Shared memory in the entry point's interface only where SPIR-V 1.4+ asks for it.
- `RuntimeInfo::generic_input_w_one`: generics read from a four-component format that stands in for a
  three-component one the driver can't fetch (`nvn::vertex_input`); their w reads as 1.

A change here that alters the SPIR-V produced needs `kKeyVersion` in `runtime/src/nnsdk/nvn/program.cpp` bumped, or
cached translations keep being used.
