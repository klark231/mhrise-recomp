// auditrun fixture. Compile texture_views.vert, attachment_access.frag and its -DSAMPLE_COPY variant.
#include "nnsdk/nvn/commands.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>

using namespace nvn;
extern "C" {
int nvnMemoryPoolInitialize(void*, const void*);
void nvnMemoryPoolFlushMappedRange(const void*, int64_t, size_t);
void nvnTextureBuilderSetDefaults(TextureDesc*);
int nvnTextureInitialize(void*, const TextureDesc*);
void nvnTextureReadTexels(const void*, const TextureViewDesc*, const CopyRegion*, void*);
void nvnTextureWriteTexels(const void*, const TextureViewDesc*, const CopyRegion*, const void*);
int nvnCommandBufferInitialize(void*, void*);
void nvnCommandBufferBeginRecording(void*);
uint64_t nvnCommandBufferEndRecording(void*);
void nvnCommandBufferCopyTextureToTexture(void*, const void*, const void*, const CopyRegion*, const void*, const void*,
                                          const CopyRegion*, int);
}

namespace {
using Pixel = std::array<uint8_t, 4>;
constexpr int W = 96, H = 64;
enum Target { Scene, Auxiliary, Z, ColorArray, ZArray, Result, SampledDS, Count };
constexpr Pixel base{64, 96, 128, 255}, guard{37, 37, 37, 37}, border{17, 17, 17, 255};
const CopyRegion full{0, 0, 0, W, H, 1};
const cmd::Scissor screen{0, 0, 0, W, H};

bool stage(Program& program, bool vertex, const char* file, unsigned id, unsigned outputs, int inputs) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    const auto size = f.tellg();
    if (size <= 0 || size % 4) return false;
    auto& s = program.stages.emplace_back();
    s.shader.stage = vertex ? nvnshader::Stage::Vertex : nvnshader::Stage::Fragment;
    s.print = s.spirv_id = id;
    s.shader.spirv.resize(size / 4);
    if (!f.seekg(0).read(reinterpret_cast<char*>(s.shader.spirv.data()), size)) return false;
    s.shader.color_outputs = outputs;
    for (int i = 0; i < inputs; ++i) {
        nvnshader::Resource r{};
        r.kind = nvnshader::Resource::Texture;
        r.binding = i;
        r.texture_type = 2;
        r.cbuf_index = 2;
        r.cbuf_offset = 0x168 + 8 * i;
        s.shader.resources.push_back(r);
    }
    VolkDeviceTable v{};
    volkLoadDeviceTable(&v, vk()->device);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = s.shader.spirv.data();
    info.codeSize = size;
    s.tried = true;
    return v.vkCreateShaderModule(vk()->device, &info, nullptr, &s.module) == VK_SUCCESS;
}
TextureViewDesc slice(unsigned layer, int count = 1) {
    TextureViewDesc v{};
    v.flags = kViewLayers;
    v.base_layer = layer;
    v.layers = count;
    return v;
}
cmd::ClearColor clear(Pixel p) { return {0, {p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f}, 15}; }
void run(CommandList& list) {
    execute({&list});
    wait_idle();
}

unsigned transfer_variants() {
    // Shift two overlapping layers/slices of the same image. The source values must all
    // come from before the copy, with untouched slices preserved.
    std::vector<uint8_t> storage(8 * 65536);
    int pool, keys[5], cb;
    struct {
        void* device;
        void* data;
        size_t size;
        int flags;
    } mem{nullptr, storage.data(), storage.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &mem) || !nvnCommandBufferInitialize(&cb, nullptr)) return 1;
    auto copy = [&](int src, int dst, const CopyRegion& from, const CopyRegion& to) {
        nvnCommandBufferBeginRecording(&cb);
        nvnCommandBufferCopyTextureToTexture(&cb, &keys[src], nullptr, &from, &keys[dst], nullptr, &to, 0);
        auto* list = reinterpret_cast<CommandList*>(nvnCommandBufferEndRecording(&cb));
        execute({list});
        wait_idle();
    };
    unsigned wrong = 0;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.pool = &pool;
    d.width = 16;
    d.depth = 4;
    d.format = 0x25;
    const int types[]{kTarget1DArray, kTarget2DArray, kTarget3D};
    for (int i = 0; i < 3; ++i) {
        d.offset = i * 65536;
        d.target = types[i];
        d.height = i == 0 ? 1 : 8;
        if (!nvnTextureInitialize(&keys[i], &d)) return 1;
        const CopyRegion all{0, 0, 0, 16, i == 0 ? 4 : 8, i == 0 ? 1 : 4};
        std::vector<Pixel> texels(16 * d.height * 4);
        for (unsigned j = 0; j < texels.size(); ++j)
            texels[j] = {uint8_t(j), uint8_t(j / (16 * d.height)), 91, 255};
        nvnTextureWriteTexels(&keys[i], nullptr, &all, texels.data());
        CopyRegion from{0, 0, 0, 16, i == 0 ? 2 : 8, i == 0 ? 1 : 2}, to = from;
        if (i == 0)
            to.y = 1;
        else
            to.z = 1;
        copy(i, i, from, to);
        std::vector<Pixel> read(texels.size());
        nvnTextureReadTexels(&keys[i], nullptr, &all, read.data());
        const int plane = 16 * d.height;
        for (unsigned j = 0; j < read.size(); ++j) {
            const int layer = j / plane;
            wrong += read[j] != texels[layer == 1 || layer == 2 ? j - plane : j];
        }
    }
    d.target = kTarget2D;
    d.depth = 1;
    d.height = 4;
    d.format = 0x08; // R16_UINT: nearest-only blit
    d.offset = 3 * 65536;
    d.width = 8;
    if (!nvnTextureInitialize(&keys[3], &d)) return 1;
    d.offset = 4 * 65536;
    d.width = 16;
    if (!nvnTextureInitialize(&keys[4], &d)) return 1;
    const CopyRegion small{0, 0, 0, 8, 4, 1}, large{0, 0, 0, 16, 4, 1};
    std::array<uint16_t, 32> src;
    for (unsigned i = 0; i < src.size(); ++i)
        src[i] = i * 37 + 1;
    nvnTextureWriteTexels(&keys[3], nullptr, &small, src.data());
    copy(3, 4, small, large);
    std::array<uint16_t, 64> dst{};
    nvnTextureReadTexels(&keys[4], nullptr, &large, dst.data());
    for (unsigned i = 0; i < dst.size(); ++i)
        wrong += dst[i] != src[i / 2];
    printf("overlapping 1D/2D array layers, volume slices and scaled R16_UINT: %u mismatches\n", wrong);
    return wrong;
}
} // namespace

extern "C" int bench() {
    if (!vk()) return 1;
    if (getenv("TRANSFER_VARIANTS")) return transfer_variants() ? 6 : 0;
    Program paint, sample, second;
    if (!stage(paint, true, "texture_views.vert.spv", 501, 0, 0) ||
        !stage(paint, false, "attachment.frag.spv", 502, 1, 0) ||
        !stage(sample, true, "texture_views.vert.spv", 501, 0, 0) ||
        !stage(sample, false, "attachment_sample.frag.spv", 503, 3, 3) ||
        !stage(second, false, "attachment_second.frag.spv", 504, 2, 0))
        return 2;

    int pool, keys[Count], cb;
    std::vector<uint8_t> bytes(Count * 131072, 37);
    struct {
        void* device;
        void* bytes;
        size_t size;
        int flags;
    } mem{nullptr, bytes.data(), bytes.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &mem) || !nvnCommandBufferInitialize(&cb, nullptr)) return 3;
    Texture* t[Count];
    for (int i = 0; i < Count; ++i) {
        TextureDesc d;
        nvnTextureBuilderSetDefaults(&d);
        d.pool = &pool;
        d.offset = i * 131072;
        d.width = W;
        d.height = H;
        d.format = i == Z || i == ZArray ? 0x35 : 0x25;
        if (i == ColorArray || i == ZArray) {
            d.target = kTarget2DArray;
            d.depth = 3;
        }
        if (!nvnTextureInitialize(&keys[i], &d)) return 4;
        t[i] = g_textures.get(&keys[i]);
    }
    CommandList seed;
    cmd::SetRenderTargets rt{};
    rt.count = 1;
    rt.colors[0] = t[Scene];
    rt.depth = t[Z];
    seed.commands = {rt,
                     screen,
                     clear(base),
                     cmd::ClearDepthStencil{.75f, true, 0x5a, 255},
                     cmd::Scissor{0, 0, 0, W / 2, H},
                     cmd::ClearDepthStencil{.25f, true, 0, 0},
                     cmd::Scissor{0, 0, 0, W, H / 4},
                     cmd::ClearDepthStencil{0, false, 0x33, 255},
                     screen};
    rt.colors[0] = t[ColorArray];
    rt.depth = t[ZArray];
    for (int i = 0; i < 3; ++i) {
        rt.color_views[0] = rt.depth_view = slice(i);
        seed.commands.emplace_back(rt);
        seed.commands.emplace_back(clear(border));
        seed.commands.emplace_back(cmd::ClearDepthStencil{1, true, 0xa6, 255});
    }
    seed.commands.emplace_back(cmd::SetRenderTargets{});
    run(seed);
    if (getenv("ATTACHMENT_TRACE")) {
        setenv("NIER_TRAZA_LIGERA", "1", 1);
        request_trace(false);
        debug_dump_frame(nullptr);
        next_frame();
    }

    const auto ds_version = t[Z]->content_version;
    rt = {};
    rt.count = 2;
    rt.colors[0] = t[Scene];
    rt.colors[1] = t[Auxiliary];
    rt.depth = t[Z];
    DepthStencilState ds{};
    ds.depth_test = 1;
    ds.depth_write = 0;
    ds.depth_func = 7; // reversed Z, GEQUAL
    ds.stencil_test = 1;
    ds.front_func = ds.back_func = 3;
    ds.front_fail = ds.back_fail = ds.front_zfail = ds.back_zfail = 1;
    ds.front_zpass = ds.back_zpass = 3; // REPLACE, but the stencil write mask below is zero
    const cmd::Draw triangle{4, -1, 3, 0, 0, 0, 0, 1};
    CommandList effect;
    effect.commands = {rt,
                       screen,
                       cmd::Viewport{0, 0, 0, W, H},
                       cmd::DepthRange{.5f, .5f},
                       PolygonState{0, 1, 2, 0},
                       cmd::BindProgram{&paint, 3},
                       ds,
                       cmd::Stencil{cmd::Stencil::Mask, 3, 0},
                       cmd::Stencil{cmd::Stencil::Ref, 3, 0x5a},
                       ChannelMaskState{0xf5},
                       ColorState{1, 0, 8, 0},
                       BlendState{0, 5, 6, 2, 1, 1, 1, 0},
                       cmd::Scissor{0, 4, 3, W - 8, H - 6},
                       triangle,
                       cmd::SetRenderTargets{}};
    run(effect);
    const bool aux_wrote = t[Auxiliary]->owns_level(0), ds_wrote = t[Z]->content_version != ds_version;

    const bool region_layers = getenv("REGION_LAYERS") != nullptr;
    auto dest = region_layers ? slice(1, 2) : slice(2);
    CopyRegion dst = full;
    dst.z = region_layers ? 1 : 0;
    nvnCommandBufferBeginRecording(&cb);
    nvnCommandBufferCopyTextureToTexture(&cb, &keys[Scene], nullptr, &full, &keys[ColorArray], &dest, &dst, 0);
    nvnCommandBufferCopyTextureToTexture(&cb, &keys[Z], nullptr, &full, &keys[ZArray], &dest, &dst, 0);
    auto* list = reinterpret_cast<CommandList*>(nvnCommandBufferEndRecording(&cb));
    execute({list});
    wait_idle();

    DescriptorPool images, samplers;
    images.textures.resize(4);
    Sampler nearest{};
    nearest.desc.min_filter = 2;
    nearest.desc.max_anisotropy = 1;
    samplers.samplers.push_back(&nearest);
    unsigned bad_color = 0, bad_ds = 0, bad_aux = 0;
    std::array<Pixel, W * H> color{}, depth{}, aux{};
    nvnTextureReadTexels(&keys[Auxiliary], nullptr, &full, aux.data());
    for (Pixel p : aux)
        bad_aux += p != guard;
    for (int layer = 0; layer < 3; ++layer) {
        const auto view = slice(layer);
        auto stencil = view;
        stencil.flags |= kViewDepthStencil;
        stencil.depth_stencil_mode = 1;
        images.register_texture(1, {t[ColorArray], view});
        images.register_texture(2, {t[ZArray], view});
        images.register_texture(3, {t[ZArray], stencil});
        rt = {};
        rt.count = 2;
        rt.colors[0] = t[Result];
        rt.colors[1] = t[SampledDS];
        CommandList post;
        post.commands = {rt,
                         screen,
                         cmd::Viewport{0, 0, 0, W, H},
                         cmd::SetPools{&images, &samplers},
                         cmd::BindTexture{1, 0, 1, true},
                         cmd::BindTexture{1, 1, 2, true},
                         cmd::BindTexture{1, 2, 3, true},
                         cmd::BindProgram{&sample, 3},
                         ColorState{0, 0, 8, 0},
                         DepthStencilState{},
                         ChannelMaskState{0xff},
                         triangle,
                         cmd::SetRenderTargets{}};
        run(post);
        nvnTextureReadTexels(&keys[Result], nullptr, &full, color.data());
        nvnTextureReadTexels(&keys[SampledDS], nullptr, &full, depth.data());
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool hit = x >= 4 && x < W / 2 && y >= H / 4 && y < H - 3;
                const auto c = color[y * W + x];
                // UNORM blending permits rounding at either side of the half-texel value.
                const bool blended = (c[0] == 159 || c[0] == 160) && c[1] == 96 && c[2] == 64 && c[3] == 255;
                bad_color += layer != 2 ? c != border : hit ? !blended : c != base;
                const Pixel expected{static_cast<uint8_t>(layer != 2  ? 255
                                                          : x < W / 2 ? 64
                                                                      : 191),
                                     static_cast<uint8_t>(layer != 2  ? 0xa6
                                                          : y < H / 4 ? 0x33
                                                                      : 0x5a),
                                     0, 255};
                bad_ds += depth[y * W + x] != expected;
            }
    }
    const size_t offset = Auxiliary * 131072, size = t[Auxiliary]->layout.total_size();
    std::fill_n(bytes.data() + offset, size, 201);
    nvnMemoryPoolFlushMappedRange(&pool, offset, size);
    nvnCommandBufferBeginRecording(&cb);
    nvnCommandBufferCopyTextureToTexture(&cb, &keys[Auxiliary], nullptr, &full, &keys[Result], nullptr, &full, 0);
    list = reinterpret_cast<CommandList*>(nvnCommandBufferEndRecording(&cb));
    execute({list});
    wait_idle();
    nvnTextureReadTexels(&keys[Result], nullptr, &full, color.data());
    const auto stale = color.size() - std::count(color.begin(), color.end(), Pixel{201, 201, 201, 201});
    // Same framebuffer, changed fragment exports and stencil mask half way through a pass.
    // Its store decisions must cover the second draw as well as the first.
    cmd::SetRenderTargets transition{};
    transition.count = 2;
    transition.colors[0] = t[Scene];
    transition.colors[1] = t[Auxiliary];
    transition.depth = t[Z];
    auto replace = ds;
    replace.depth_test = 0;
    replace.front_func = replace.back_func = 8;
    CommandList change;
    change.commands = {transition,
                       screen,
                       cmd::BindProgram{&paint, 3},
                       ds,
                       cmd::Stencil{cmd::Stencil::Mask, 3, 0},
                       ChannelMaskState{0xff},
                       triangle,
                       cmd::BindProgram{&second, 2},
                       replace,
                       cmd::Stencil{cmd::Stencil::Mask, 3, 255},
                       cmd::Stencil{cmd::Stencil::Ref, 3, 0x81},
                       cmd::Scissor{0, W / 2, 0, W / 2, H},
                       triangle,
                       cmd::SetRenderTargets{}};
    run(change);
    nvnTextureReadTexels(&keys[Auxiliary], nullptr, &full, aux.data());
    auto live_stencil = TextureViewDesc{};
    live_stencil.flags = kViewDepthStencil;
    live_stencil.depth_stencil_mode = 1;
    images.register_texture(2, {t[Z], {}});
    images.register_texture(3, {t[Z], live_stencil});
    CommandList read;
    read.commands = {rt, screen, DepthStencilState{}, cmd::BindProgram{&sample, 3}, triangle, cmd::SetRenderTargets{}};
    run(read);
    nvnTextureReadTexels(&keys[SampledDS], nullptr, &full, depth.data());
    unsigned late_errors = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool right = x >= W / 2;
            late_errors += aux[y * W + x] != (right ? Pixel{0, 255, 0, 255} : Pixel{201, 201, 201, 201});
            late_errors += depth[y * W + x][1] != (right ? 0x81 : y < H / 4 ? 0x33 : 0x5a);
        }
    printf("late fragment export and stencil write: %u mismatches / %d channels checked\n", late_errors, 2 * W * H);
    printf("attachment ownership: unexported MRT written=%d read-only depth/stencil changed=%d stale after mapped "
           "update=%zu/%zu\n",
           aux_wrote, ds_wrote, stale, color.size());
    printf("blend -> copy -> sample: color=%u ds=%u sibling=%u wrong of %d/%d/%d (region layers=%d)\n", bad_color,
           bad_ds, bad_aux, 3 * W * H, 3 * W * H, W * H, region_layers);
    return aux_wrote || ds_wrote || bad_color || bad_ds || bad_aux || stale || late_errors ? 5 : 0;
}
