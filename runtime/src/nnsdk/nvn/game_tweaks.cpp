// Graphics settings that live in the game itself (via.render.RenderConfig and friends), for MH Rise 16.0.1 only.
// The PC version shows these in its options menu; the Switch build has the same code and just never changes them.
// Everything is found through the engine's reflection table (the get_/set_ names RenderConfig registers) and the
// setters it points to:
//   Shadow quality   set_ShadowQuality (main+0x78a0c58) picks the shadow map size and calls the shadow system's
//                    setter (main+0x7564f1c, size at +348) on *(main+0x12950580). 384/512/768/1024/2048 on PC.
//   Model detail     set_MeshQuality (main+0x78a0d68) stores a float on *(main+0x12950568): +60 or +64 depending on
//                    the LOD mode (main+0x7467ac8 / 0x7467ad0 are those two stores).
//   Texture detail   set_TextureLoadLevelBias (main+0x78a10d8): Renderer+6728, mip levels the streamer skips (0..12).
// Renderer switches (via.render.Renderer's own get_/set_ list; its setters store straight into the Renderer):
//   Foliage sway     SwingEnable +0x1a59 and SwingOnShadowEnable +0x1a56 (the PC's "Foliage Sway")
//   Dynamic shadows  set_DynamicShadowEnable (main+0x7930a18), +0x1a55, tells the shadow system too (the PC's option)
// Not offered, because the Switch game already has them the cheap way: FXAAEnable +0x1a5b (off), ShadowCacheEnable
// +0x1a53 (on), SmallObjectCullingEnable +0x1a1e (on), EffectSSREnable +0x1a64 (off). The PC menu's RenderConfig
// (motion blur, depth of field, bloom…) is never read by the Switch game: its getters were never called.
//   Effects buffer   ReducedTransparentBuffer +0x1a68: how much smaller the buffer for particles and other transparent
//                    effects is (the Switch game uses 2; the PC's quality table goes 1, 1, 2, 3, 4)
//   Ambient occlusion GeometryAOQuality +0x1a8c (the Switch game uses 1)
//   Render distance  SmallObjectCullingRatio +0x1a30 (float). The culling pass (main+0x7461cf8 and on) projects each
//                    object's box to the screen and keeps it only if its size there is over this ratio times a
//                    factor of the camera's (main+0x74977b4 multiplies them). So a higher ratio drops small or far
//                    things sooner: twice the ratio, half the distance an object stays drawn at. The game sets 12 in
//                    the village and 11.2 in hunts.
// And logged only, until we know what its number means: DefaultShadowLodBias +0x1a44.
// Each one is read once from the game as its default, changed while its switch is on and put back when it's off.
// Called from the thread that presents, once per frame: the game calls these setters from its own threads too, and
// the shadow setter only stores the new size and lets the engine rebuild the maps.
// Before touching anything the instructions at those addresses are checked, so another build of main is left alone.
// What's in effect goes to <datos>/game_settings.txt (one line per setting) whenever it changes, for the settings menu.
// Switches (live): shadow_half, shadow_quarter / model_detail_low, model_detail_lowest, model_detail_high /
// texture_bias_1, texture_bias_2 / no_foliage_sway, no_dynamic_shadows / effects_lower, effects_lowest / no_ao /
// draw_distance_75, draw_distance_50, draw_distance_33, draw_distance_25.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nnsdk/nvn/commands.h"
#include "nnsdk/nvn/flags.h"
#include "nnsdk/nvn/nvn.h"
#include "nnsdk/nvn/vk.h"
#include "nnsdk/runtime.h"

namespace nvn {

namespace {

constexpr size_t kRendererPointer = 0x12950538;  // via.render.Renderer*
constexpr size_t kShadowPointer = 0x12950580;    // the shadow system (what set_ShadowQuality talks to)
constexpr size_t kLodPointer = 0x12950568;       // mesh LOD state (set_MeshQuality, set_ShaderLodGlobalValue)

constexpr size_t kShadowSetter = 0x7564f1c;  // void (shadows*, uint32_t size)
constexpr size_t kShadowSize = 348;
constexpr size_t kLodRate0 = 60, kLodRate1 = 64;  // float, LOD mode 0 and 1
constexpr size_t kTextureBias = 6728;             // uint32_t, Renderer

struct Check {
    size_t at;
    uint32_t code;
};
constexpr Check kChecks[] = {
    {0x7564f1c, 0xF81E0FF3},  // str x19, [sp, #-32]!     (shadow size setter)
    {0x7564f28, 0xB9415C08},  // ldr w8, [x0, #348]
    {0x7564f40, 0xB9015C01},  // str w1, [x0, #348]
    {0x78a0c68, 0xF942C113},  // ldr x19, [x8, #1408]       (set_ShadowQuality loads the shadow system)
    {0x7467ac8, 0xBD003C00},  // str s0, [x0, #60]         (LOD rate, mode 0)
    {0x7467ad0, 0xBD004000},  // str s0, [x0, #64]         (LOD rate, mode 1)
    {0x78a0d90, 0xF942B508},  // ldr x8, [x8, #1384]        (set_MeshQuality loads the LOD state)
    {0x78a10dc, 0xF9429D08},  // ldr x8, [x8, #1336]        (set_TextureLoadLevelBias loads the Renderer)
    {0x78a10ec, 0xB91A4909},  // str w9, [x8, #6728]
    {0x5a750, 0x52834B2A},    // mov w10, #0x1a59         (set_SwingEnable)
    {0x5a778, 0x52834ACA},    // mov w10, #0x1a56         (set_SwingOnShadowEnable)
    {0x7930a18, 0x52834AA8},  // mov w8, #0x1a55          (set_DynamicShadowEnable)
    {0x5a5d0, 0xB91A6928},    // str w8, [x9, #0x1a68]    (set_ReducedTransparentBuffer)
    {0x5a2b8, 0xB91A8D28},    // str w8, [x9, #0x1a8c]    (set_GeometryAOQuality)
    {0x5a6e8, 0xBD1A3100},    // str s0, [x8, #0x1a30]    (set_SmallObjectCullingRatio)
    {0x59f90, 0xBD1A4500},    // str s0, [x8, #0x1a44]    (set_DefaultShadowLodBias)
    {0x72878, 0xF9468508},    // ldr x8, [x8, #0xd08]     (get_UseGpuOcclusionCulling: its GOT entry)
    {0x72880, 0x39407508},    // ldrb w8, [x8, #0x1d]
};
constexpr size_t kDynamicShadowSetter = 0x7930a18;  // void (Renderer*, bool)

// GPU occlusion culling: UseGpuOcclusionCulling, byte +0x1d of the object behind GOT main+0x12674d08 (the loader fills
// the entry: entry -> global variable -> object; get_ thunk main+0x72874)
constexpr size_t kOcclusionGot = 0x12674d08;

const uint8_t* known_main() {
    static const uint8_t* base = [] () -> const uint8_t* {
        const uint8_t* b = nnsdk::host().main_base;
        if (!b || nnsdk::host().main_size <= kShadowPointer + 8) return nullptr;
        for (const Check& c : kChecks) {
            uint32_t w;
            memcpy(&w, b + c.at, 4);
            if (w != c.code) {
                NVN_LOG("game settings: main+%#zx doesn't match 16.0.1, leaving the game's settings alone\n", c.at);
                return nullptr;
            }
        }
        NVN_LOG("game settings: MH Rise 16.0.1 (shadows, model detail, texture detail and renderer switches available)\n");
        return b;
    }();
    return base;
}

template <typename T>
T* object_at(const uint8_t* base, size_t pointer) {
    const uint64_t p = *reinterpret_cast<const volatile uint64_t*>(base + pointer);
    return p ? reinterpret_cast<T*>(p) : nullptr;
}

// Divides the game's own shadow map size (the Switch default isn't the PC's 2048), never below 256
uint32_t wanted_shadow_divisor() {
    if (NVN_FLAG("shadow_quarter")) return 4;
    if (NVN_FLAG("shadow_half")) return 2;
    return 1;
}

float wanted_lod_scale() {
    if (NVN_FLAG("model_detail_min")) return 0.25f;
    if (NVN_FLAG("model_detail_lowest")) return 0.5f;
    if (NVN_FLAG("model_detail_low")) return 0.75f;
    if (NVN_FLAG("model_detail_high")) return 1.5f;
    return 1.0f;
}

uint32_t wanted_texture_bias() {
    if (NVN_FLAG("texture_bias_4")) return 4;
    if (NVN_FLAG("texture_bias_3")) return 3;
    if (NVN_FLAG("texture_bias_2")) return 2;
    if (NVN_FLAG("texture_bias_1")) return 1;
    return 0;
}

// What the game has right now, for game_settings.txt
struct Status {
    uint32_t shadow = 0, shadow_game = 0;
    float lod = 0, lod_game = 0, lod_scale = 1;
    uint32_t bias = 0, bias_game = 0;
    bool shadow_seen = false, lod_seen = false, bias_seen = false;
};
Status g_status;

void write_status() {
    const Status& st = g_status;
    char text[512];
    int n = 0;
    if (st.shadow_seen)
        n += snprintf(text + n, sizeof text - n, "shadow %u %u\n", st.shadow, st.shadow_game);
    if (st.lod_seen)
        n += snprintf(text + n, sizeof text - n, "model %.3f %g %g\n", st.lod_scale, st.lod, st.lod_game);
    if (st.bias_seen) n += snprintf(text + n, sizeof text - n, "texture %u %u\n", st.bias, st.bias_game);
    static std::string last;
    if (last == text) return;
    last = text;
    const char* data = getenv("NIER_DATA");
    const std::string path = std::string(data ? data : ".") + "/game_settings.txt";
    if (FILE* f = fopen(path.c_str(), "w")) {
        fputs(text, f);
        fclose(f);
    }
}

void shadows(const uint8_t* base) {
    uint8_t* sys = object_at<uint8_t>(base, kShadowPointer);
    if (!sys) return;
    auto* size = reinterpret_cast<volatile uint32_t*>(sys + kShadowSize);
    static uint32_t game_size = 0;  // what the game had before we touched it
    static uint32_t ours = 0;       // the last size we set (0: none)
    if (!*size) return;  // not set up yet
    if (!ours || *size != ours) game_size = *size;  // the game changed it (a load, a cutscene): that's the new default
    const uint32_t divisor = wanted_shadow_divisor();
    const uint32_t target = divisor > 1 ? std::max<uint32_t>(game_size / divisor, 256) : game_size;
    g_status.shadow_seen = true;
    g_status.shadow = *size, g_status.shadow_game = game_size;
    if (*size == target) return;
    using Setter = void (*)(void*, uint32_t);
    reinterpret_cast<Setter>(const_cast<uint8_t*>(base) + kShadowSetter)(sys, target);
    NVN_LOG("game settings: shadow maps %u -> %u (game default %u)\n", ours ? ours : game_size, target, game_size);
    ours = divisor > 1 ? target : 0;
    g_status.shadow = *size;
}

void model_detail(const uint8_t* base) {
    uint8_t* lod = object_at<uint8_t>(base, kLodPointer);
    if (!lod) return;
    auto* rate = reinterpret_cast<volatile float*>(lod + kLodRate0);
    auto* rate1 = reinterpret_cast<volatile float*>(lod + kLodRate1);
    static float game[2] = {0, 0}, ours[2] = {0, 0};
    for (int k = 0; k < 2; k++) {
        volatile float* r = k ? rate1 : rate;
        if (ours[k] == 0.0f || *r != ours[k]) game[k] = *r;
    }
    const float scale = wanted_lod_scale();
    static float logged = 1.0f;
    for (int k = 0; k < 2; k++) {
        volatile float* r = k ? rate1 : rate;
        const float target = game[k] * scale;
        if (*r != target) *r = target;
        ours[k] = scale != 1.0f ? target : 0.0f;
    }
    g_status.lod_seen = true;
    g_status.lod = *rate, g_status.lod_game = game[0], g_status.lod_scale = scale;
    if (scale != logged) {
        NVN_LOG("game settings: model detail x%.2f (LOD rates %g / %g -> %g / %g)\n", scale, game[0], game[1],
                game[0] * scale, game[1] * scale);
        logged = scale;
    }
}

void texture_detail(const uint8_t* base) {
    uint8_t* renderer = object_at<uint8_t>(base, kRendererPointer);
    if (!renderer) return;
    auto* bias = reinterpret_cast<volatile uint32_t*>(renderer + kTextureBias);
    static uint32_t game = 0, ours = ~0u;
    if (ours == ~0u || *bias != ours) game = *bias;
    const uint32_t want = wanted_texture_bias();
    const uint32_t target = want ? std::min<uint32_t>(game + want, 12) : game;
    if (*bias != target) {
        *bias = target;
        NVN_LOG("game settings: texture streaming skips %u mip levels (game default %u)\n", target, game);
    }
    ours = want ? target : ~0u;
    g_status.bias_seen = true;
    g_status.bias = *bias, g_status.bias_game = game;
}

// One byte switch of the Renderer: the game's own value while ours is off, ours (want) while it's on.
struct ByteSwitch {
    const char* name;  // for the log
    size_t offset;
    uint8_t want;
    uint8_t game = 0;
    bool seen = false, ours = false;
};

void byte_switch(uint8_t* renderer, ByteSwitch& b, bool on) {
    volatile uint8_t* v = renderer + b.offset;
    if (!b.seen || !b.ours || *v != b.want) b.game = *v;  // the game set it (a load): that's the new default
    if (!b.seen) NVN_LOG("game settings: %s is %u in the game\n", b.name, b.game);
    b.seen = true;
    const uint8_t target = on ? b.want : b.game;
    if (*v != target) {
        *v = target;
        NVN_LOG("game settings: %s %u (game's %u)\n", b.name, target, b.game);
    }
    b.ours = on;
}

// The same for a 32-bit value; want(game) gives ours from the game's.
struct IntSwitch {
    const char* name;
    size_t offset;
    uint32_t game = 0, last = 0;
    bool seen = false, ours = false;
};

template <typename Want>
void int_switch(uint8_t* renderer, IntSwitch& s, bool on, Want want) {
    volatile uint32_t* v = reinterpret_cast<volatile uint32_t*>(renderer + s.offset);
    if (!s.seen || !s.ours || *v != s.last) s.game = *v;  // the game set it: that's the new default
    if (!s.seen) NVN_LOG("game settings: %s is %u in the game\n", s.name, s.game);
    s.seen = true;
    const uint32_t target = on ? want(s.game) : s.game;
    if (*v != target) {
        *v = target;
        NVN_LOG("game settings: %s %u (game's %u)\n", s.name, target, s.game);
    }
    s.last = target;
    s.ours = on;
}

// Render distance: the share of the game's distance that small objects stay drawn at (the culling ratio divided by it)
float wanted_draw_distance() {
    if (NVN_FLAG("draw_distance_25")) return 0.25f;
    if (NVN_FLAG("draw_distance_33")) return 1.0f / 3;
    if (NVN_FLAG("draw_distance_50")) return 0.5f;
    if (NVN_FLAG("draw_distance_75")) return 0.75f;
    return 1.0f;
}

void draw_distance(uint8_t* renderer) {
    volatile float* ratio = reinterpret_cast<volatile float*>(renderer + 0x1a30);
    static float game = 0, last = 0;
    static bool seen = false, ours = false;
    if (!seen || !ours || *ratio != last) {  // the game set it (a new area): that's the new default
        if (!seen || *ratio != game) NVN_LOG("game settings: SmallObjectCullingRatio = %g\n", *ratio);
        game = *ratio;
    }
    seen = true;
    const float share = wanted_draw_distance();
    const float target = share < 1.0f ? game / share : game;
    if (*ratio != target) {
        *ratio = target;
        NVN_LOG("game settings: render distance %.0f%% (culling ratio %g, game's %g)\n", share * 100, target, game);
    }
    last = target;
    ours = share < 1.0f;
}

IntSwitch g_effects{"effects buffer", 0x1a68};
IntSwitch g_ao{"ambient occlusion", 0x1a8c};

ByteSwitch g_sway{"foliage sway", 0x1a59, 0};
ByteSwitch g_sway_shadow{"foliage sway in shadows", 0x1a56, 0};

void renderer_switches(const uint8_t* base) {
    uint8_t* renderer = object_at<uint8_t>(base, kRendererPointer);
    if (!renderer) return;
    const bool no_sway = NVN_FLAG("no_foliage_sway");
    byte_switch(renderer, g_sway, no_sway);
    byte_switch(renderer, g_sway_shadow, no_sway);

    // Dynamic shadows go through the game's setter, which also tells the shadow system.
    volatile uint8_t* dynamic = renderer + 0x1a55;
    static uint8_t game_dynamic = 0;
    static bool seen = false, ours = false;
    if (!seen || !ours || *dynamic != 0) game_dynamic = *dynamic;
    if (!seen) NVN_LOG("game settings: dynamic shadows is %u in the game\n", game_dynamic);
    seen = true;
    const bool off = NVN_FLAG("no_dynamic_shadows");
    const uint8_t target = off ? 0 : game_dynamic;
    if (*dynamic != target) {
        using Setter = void (*)(void*, bool);
        reinterpret_cast<Setter>(const_cast<uint8_t*>(base) + kDynamicShadowSetter)(renderer, target != 0);
        NVN_LOG("game settings: dynamic shadows %u (game's %u)\n", target, game_dynamic);
    }
    ours = off;

    // Effects buffer: one or two steps smaller than the game's. Ambient occlusion: 0.
    {
        const uint32_t steps = NVN_FLAG("effects_lowest") ? 2 : NVN_FLAG("effects_lower") ? 1 : 0;
        int_switch(renderer, g_effects, steps != 0, [&](uint32_t game) { return std::min<uint32_t>(game + steps, 4); });
        int_switch(renderer, g_ao, NVN_FLAG("no_ao"), [](uint32_t) { return 0u; });
    }
    draw_distance(renderer);

    // The numbers we don't change yet: logged when the game sets them, to learn what it uses.
    struct Logged {
        const char* name;
        size_t offset;
        bool is_float;
        uint32_t last;
        bool seen;
    };
    static Logged logged[] = {{"DefaultShadowLodBias", 0x1a44, true, 0, false}};
    for (Logged& l : logged) {
        uint32_t v;
        memcpy(&v, renderer + l.offset, 4);
        if (l.seen && v == l.last) continue;
        l.seen = true, l.last = v;
        float f;
        memcpy(&f, &v, 4);
        if (l.is_float) NVN_LOG("game settings: %s = %g\n", l.name, f);
        else NVN_LOG("game settings: %s = %u\n", l.name, v);
    }
}

// The object behind a GOT entry (null until the game has made it).
uint8_t* got_object(const uint8_t* base, size_t got) {
    const uint64_t var = *reinterpret_cast<const volatile uint64_t*>(base + got);
    if (!var) return nullptr;
    const uint64_t obj = *reinterpret_cast<const volatile uint64_t*>(var);
    return obj ? reinterpret_cast<uint8_t*>(obj) : nullptr;
}

ByteSwitch g_gpu_occlusion{"GPU occlusion culling", 0x1d, 0};

// The game's GPU occlusion test (boxes on a 128x64 depth target, then compute shaders that sort what's visible) is off
// on Mali by default: those compute shaders use the Switch's 32-lane warps, Mali's subgroups are 16 lanes (Eden's
// translator only handles larger ones), and grass and trees popped in and out every frame, with ARM's driver and PanVK
// alike. Off, the game culls by the camera alone. <data>/gpu_occlusion keeps it on; <data>/no_gpu_occlusion turns it
// off anywhere.
void culling_switches(const uint8_t* base) {
    static const bool mali = nvn::vk() && nvn::vk()->props.vendorID == 0x13B5;
    const bool off = NVN_FLAG("no_gpu_occlusion") || (mali && !NVN_FLAG("gpu_occlusion"));
    if (uint8_t* occ = got_object(base, kOcclusionGot)) byte_switch(occ, g_gpu_occlusion, off);
}

}  // namespace

void game_tweaks() {
    const uint8_t* base = known_main();
    if (!base) return;
    shadows(base);
    model_detail(base);
    texture_detail(base);
    renderer_switches(base);
    culling_switches(base);
    write_status();
}

}  // namespace nvn
