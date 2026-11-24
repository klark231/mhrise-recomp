// Checks shaders/astc_encode.comp on the CPU: its encoder part (between ASTC_CODE_BEGIN and ASTC_CODE_END) built as
// C++, blocks decoded by ARM's astcenc (runtime/external/astcenc), compared with the texels they came from and with
// what astcenc's own fastest preset gives. tools/astc_gpu_test.sh builds and runs it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "astcenc.h"
#include "nnsdk/nvn/bcn.h"

namespace shader {
using uint = uint32_t;
template <typename T, size_t N, typename... V> std::array<T, N> make_array(V... v) { return {{static_cast<T>(v)...}}; }
#define CONST_ARRAY(type, name, n) const std::array<type, n> name = make_array<type, n>
#define SHARED
inline uint min(uint a, uint b) { return a < b ? a : b; }
inline uint max(uint a, uint b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
inline float abs(float x) { return std::fabs(x); }
inline float sqrt(float x) { return std::sqrt(x); }
#include "astc_encode_code.inc"
}  // namespace shader

static astcenc_context* g_dec;
static astcenc_context* g_enc;

static void decode(const uint8_t block[16], uint8_t out[64]) {
    astcenc_image img{4, 4, 1, ASTCENC_TYPE_U8, nullptr};
    void* slices[1] = {out};
    img.data = slices;
    const astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    astcenc_decompress_image(g_dec, block, 16, &img, &sw, 0);
    astcenc_decompress_reset(g_dec);
}

static double sq_err(const uint8_t a[64], const uint8_t b[64]) {
    double e = 0;
    for (int i = 0; i < 64; i++) e += double(a[i] - b[i]) * (a[i] - b[i]);
    return e;
}

static void gpu_encode(const uint8_t texels[64], uint8_t out[16]) {
    for (int i = 0; i < 16; i++) {
        uint32_t v;
        memcpy(&v, texels + 4 * i, 4);
        shader::px[i] = v;
        for (int c = 0; c < 4; c++) shader::tx[i][c] = float(texels[4 * i + c]);
    }
    shader::encode_block();
    memcpy(out, shader::ob, 16);
}

static void ref_encode(const uint8_t texels[64], uint8_t out[16]) {
    astcenc_image img{4, 4, 1, ASTCENC_TYPE_U8, nullptr};
    void* slices[1] = {const_cast<uint8_t*>(texels)};
    img.data = slices;
    const astcenc_swizzle sw{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    astcenc_compress_image(g_enc, &img, &sw, out, 16, 0);
    astcenc_compress_reset(g_enc);
}

int main() {
    astcenc_config cfg;
    astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg);
    if (astcenc_context_alloc(&cfg, 1, &g_dec, nullptr)) return 1;
    astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST, 0, &cfg);
    if (astcenc_context_alloc(&cfg, 1, &g_enc, nullptr)) return 1;
    shader::build_tables(0, 1);  // (the shader's workgroup does this between its lanes)
    std::mt19937 rng(7);
    const char* names[] = {"smooth opaque", "smooth with alpha", "BC3 decoded", "BC7 decoded", "noise"};
    int failures = 0;
    for (int set = 0; set < 5; set++) {
        double ours = 0, ref = 0, mismatch = 0;
        const int n = 20000;
        for (int k = 0; k < n; k++) {
            uint8_t texels[64];
            if (set <= 1) {  // gradients with a little noise, the kind of texel a BC block holds
                float base[4], dx[4], dy[4];
                for (int c = 0; c < 4; c++) base[c] = rng() % 256, dx[c] = int(rng() % 41) - 20, dy[c] = int(rng() % 41) - 20;
                for (int i = 0; i < 16; i++)
                    for (int c = 0; c < 4; c++) {
                        float v = base[c] + dx[c] * (i & 3) + dy[c] * (i >> 2) + int(rng() % 7) - 3;
                        if (c == 3 && set == 0) v = 255;
                        texels[4 * i + c] = uint8_t(std::clamp(v, 0.f, 255.f));
                    }
            } else if (set <= 3) {
                uint8_t blk[16];
                for (auto& b : blk) b = rng();
                nvn::bcn::decode(set == 2 ? nvn::bcn::kBC3 : nvn::bcn::kBC7, blk, 1, 1, texels, 16, 4, 4);
            } else {
                for (auto& t : texels) t = rng();
            }
            uint8_t a[16], b[16], da[64], db[64];
            gpu_encode(texels, a);
            ref_encode(texels, b);
            decode(a, da);
            decode(b, db);
            // a block astcenc can't read decodes to magenta (255, 0, 255, 255) for every texel: count it
            bool bad = true;
            for (int i = 0; i < 16; i++) bad = bad && da[4 * i] == 255 && da[4 * i + 1] == 0 && da[4 * i + 2] == 255;
            bool really_magenta = true;
            for (int i = 0; i < 16; i++) really_magenta = really_magenta && texels[4 * i] == 255 && texels[4 * i + 1] == 0 && texels[4 * i + 2] == 255;
            if (bad && !really_magenta) mismatch++;
            ours += sq_err(texels, da);
            ref += sq_err(texels, db);
        }
        auto psnr = [&](double e) { return e == 0 ? 99.0 : 10 * std::log10(255.0 * 255 / (e / (n * 64.0))); };
        printf("%-18s GPU encoder %5.1f dB, astcenc fastest %5.1f dB, invalid blocks %.0f\n", names[set], psnr(ours), psnr(ref), mismatch);
        failures += mismatch > 0;
    }
    return failures;
}
