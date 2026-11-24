#pragma once
// BC textures stored as ASTC 4x4 on GPUs that read ASTC but not BC (most Mali and PowerVR drivers). Decoded to RGBA8 (bcn.h), a BC1 texture is
// 8 times bigger than the game's and a BC3 or BC7 one 4 times: every frame reads that much more memory, which is what a
// small Mali runs short of. ASTC 4x4 is 16 bytes per 4x4 block, the size of BC3/BC7 and twice BC1, and the GPU reads it
// natively. BC1 converts block for block (from_bc1). The others are encoded (ARM's astcenc, runtime/external/astcenc, its
// fastest preset) unless every block's alpha is one value (from_bc2_bc3), which costs CPU time, so every such level is kept in the ASTC cache keyed by its BC blocks: the second
// time a texture loads, it's a file read.
// <data>/bc_astc (Graphics → Textures as ASTC): on. Decided once, when the first BC texture is created.

#include <cstddef>
#include <cstdint>

namespace nvn::astc {

// The switch is on and the GPU samples ASTC 4x4 (UNORM and sRGB).
bool wanted();

// RGBA8 texels (width x height, rows of width * 4 bytes) to ASTC 4x4 blocks, row after row of blocks:
// ((width + 3) / 4) * ((height + 3) / 4) * 16 bytes in out. false: the encoder failed (out is left as it was).
bool encode(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb, uint8_t* out);

// BC1 blocks straight to ASTC 4x4 blocks, `count` of each (8 bytes in, 16 out), with no encoder: a BC1 block is a line
// between two colours with 2-bit positions on it, which an ASTC block holds as it is. The texels come out as bcn.cpp's
// BC1 decode gives them, to a step or two of rounding; much faster than encode(), and nothing to cache.
void from_bc1(const uint8_t* bc1, uint8_t* out, size_t count);

// BC2 or BC3 blocks the same way, when every block's alpha is a single value (opaque textures stored with an alpha
// channel, mostly): the colour half is a BC1 block that always takes the four-colour line, and the alpha rides along
// as two equal endpoints. false: some block's alpha varies (out may be partly written), so the level needs encode().
// Mapping such blocks directly instead of encoding them is an idea from the Mesa wrapper (MIT; README, Credits).
bool from_bc2_bc3(bool bc3, const uint8_t* blocks, uint8_t* out, size_t count);

// The disk cache: an encoded level by its key. load: true and `size` bytes in out if the file is there and has that size.
// store: written by a background thread (a copy is taken).
bool load(uint64_t key, size_t size, uint8_t* out);
void store(uint64_t key, const uint8_t* data, size_t size);

}  // namespace nvn::astc
