#pragma once
// NVNformat -> VkFormat y bytes por bloque. El orden del enum es el de NVN (estilo OpenGL);
// lo confirman los dos formatos que NieR registra para borrado rápido: 0x25 (RGBA8, el de
// la ventana de 1280x720) y 0x3f (R11G11B10F, el del buffer HDR).

#include <cstdint>

#include "nnsdk/nvn/vk.h"

namespace nvn {

struct FormatInfo {
    VkFormat vk;
    uint8_t bytes;   // por píxel, o por bloque si block > 1
    uint8_t block;   // ancho del bloque comprimido (1 = sin comprimir)
    const char* name;
    uint8_t block_h = 0;  // alto del bloque si no es cuadrado (ASTC 5x4, 8x6…); 0 = igual al ancho
    uint32_t block_height() const { return block_h ? block_h : block; }
};

// nullptr si no lo conocemos todavía (se registra en el log).
const FormatInfo* format_info(int nvn_format);

// A vertex attribute as Vulkan reads it: its format, and how the vertex shader takes it (the four bits of
// nvnshader::Options::input_types: 0 float, 1 int, 2 uint, 3 int to float, 4 uint to float; plus kFetchWIsOne when a
// three-component format is read as four and w has to read as 1, as it does on the console). Formats the driver can't
// read from a vertex buffer are swapped for one it can (see formats.cpp); replaced says so.
constexpr uint8_t kFetchWIsOne = 8;
struct VertexInput {
    VkFormat format;
    uint8_t fetch;
    bool replaced;
};
VertexInput vertex_input(uint8_t nvn_format);

}  // namespace nvn
