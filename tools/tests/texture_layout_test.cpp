#include "nnsdk/nvn/layout.h"
#include <cassert>
#include <utility>

int main() {
    using nvn::compute_layout;
    // Both 8- and 16-GOB layouts occupy 256 KiB for Rise's 512x77 RGBA8 animation maps.
    // Check the actual texel addresses on either side of a GOB column and a tile row.
    const auto vat = compute_layout(512, 77, 1, 1, 2, 4, 1, 1, false, 0, false);
    const auto& level = vat.levels[0];
    assert(level.size == 262144);
    std::vector<uint8_t> tiled(level.size), pixels(512 * 77 * 4);
    tiled[4096] = 37;
    tiled[8192] = 89;
    tiled[131072] = 163;
    nvn::deswizzle_level(level, 4, tiled.data(), pixels.data());
    assert(pixels[16 * 4] == 37 && pixels[32 * 4] == 89 && pixels[64 * 512 * 4] == 163);
    assert(vat.levels[1].offset == level.size && vat.levels[1].block_height_gobs == 8);

    const std::pair<uint32_t, uint32_t> boundaries[] = {
        {1, 1}, {10, 1}, {11, 2}, {21, 2}, {22, 4}, {42, 4}, {43, 8}, {85, 8}, {86, 16}, {512, 16},
    };
    for (const auto& [rows, gobs] : boundaries) {
        for (uint32_t block : {1u, 4u, 6u}) {
            const auto h = (rows - 1) * block + 1;  // include the final partial compressed block
            const auto layout = compute_layout(512, h, 1, 2, 1, block == 1 ? 4 : 16, block, block, false, 0, false);
            assert(layout.levels[0].block_height_gobs == gobs);
        }
    }

    const auto packaged = compute_layout(512, 77, 1, 1, 1, 4, 1, 1, false, 0, false, 4, 0);
    assert(packaged.levels[0].block_height_gobs == 16);
    const auto volume = compute_layout(32, 77, 1, 1, 1, 4, 1, 1, false, 0, true);
    assert(volume.levels[0].block_height_gobs == 16);
}
