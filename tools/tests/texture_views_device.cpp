// Offscreen auditrun fixture. Compile texture_views.vert and .frag with glslc.
// Compile texture_views_uint.frag as uint_write.spv, then with -DREAD_BITS as uint_read.spv.
#include "nnsdk/nvn/commands.h"
#include "nnsdk/types.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace nvn;
namespace nn::os { Result SetMemoryHeapSize(size_t); Result AllocateMemoryBlock(uintptr_t*, size_t); }
struct Mapping { const void* pool; int64_t physical, virt; uint64_t size; int32_t kind, pad; };
extern "C" {
Boolean nvnMemoryPoolMapVirtual(void*, int, const Mapping*);
Boolean nvnMemoryPoolInitialize(void*, const void*);
void nvnMemoryPoolFinalize(void*);
void nvnTextureBuilderSetDefaults(TextureDesc*);
size_t nvnTextureBuilderGetStorageSize(const TextureDesc*);
Boolean nvnTextureInitialize(void*, const TextureDesc*);
void nvnTextureFinalize(void*);
void nvnTextureReadTexels(const void*, const TextureViewDesc*, const CopyRegion*, void*);
void nvnTextureWriteTexels(const void*, const TextureViewDesc*, const CopyRegion*, const void*);
Boolean nvnTexturePoolInitialize(void*, void*, int64_t, int);
void nvnTexturePoolRegisterTexture(void*, int, const void*, const TextureViewDesc*);
}

namespace {
using Pixel = std::array<uint8_t,4>;
constexpr int width = 32, height = 16;
constexpr Pixel guard{17,17,17,17};
TextureViewDesc slice(int mip, int layer, bool srgb = false) {
    TextureViewDesc v{};
    v.flags = kViewLevels | kViewLayers | kViewFormat;
    v.base_level = mip;
    v.base_layer = layer;
    v.levels = v.layers = 1;
    v.format = srgb ? 0x38 : 0x25;
    return v;
}
enum class TestShader { Vertex, Sample, WriteBits, ReadBits, Filter };
bool program_stage(Program& p, TestShader shader, VolkDeviceTable& v, VkDevice device) {
    const char* names[]{"texture_views.vert.spv", "texture_views.frag.spv", "uint_write.spv", "uint_read.spv", "texture_views_filter.frag.spv"};
    std::ifstream f(names[static_cast<int>(shader)], std::ios::binary | std::ios::ate);
    const auto size = f.tellg();
    if (size <= 0 || size % 4) return false;
    auto& s = p.stages.emplace_back();
    s.shader.stage = shader == TestShader::Vertex ? nvnshader::Stage::Vertex : nvnshader::Stage::Fragment;
    s.shader.spirv.resize(size / 4);
    if (!f.seekg(0).read(reinterpret_cast<char*>(s.shader.spirv.data()), size)) return false;
    if (shader != TestShader::Vertex) s.shader.color_outputs = 1;
    if (shader == TestShader::Sample || shader == TestShader::ReadBits || shader == TestShader::Filter)
        s.shader.resources.push_back({nvnshader::Resource::Texture, 0, 2, 0x168, 2});
    s.tried = true;
    s.print = s.spirv_id = 0x5649455700ull + static_cast<unsigned>(shader);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = s.shader.spirv.data();
    return v.vkCreateShaderModule(device, &ci, nullptr, &s.module) == VK_SUCCESS;
}

bool check_sparse_layers(VolkDeviceTable& vk, VkDevice device, int* pool_key, int slot, DescriptorPool& samplers,
                         int* output_key, const cmd::SetRenderTargets& output) {
    Program p;
    if (!program_stage(p, TestShader::Vertex, vk, device) || !program_stage(p, TestShader::Filter, vk, device)) return false;
    constexpr size_t tile = 65536, span = 2 << 20;
    uintptr_t heap = 0;
    if (nn::os::SetMemoryHeapSize(8 << 20).value || nn::os::AllocateMemoryBlock(&heap, span).value) return false;
    int physical, virt, key;
    struct Pool { void* device; void* storage; size_t size; int flags; };
    Pool a{nullptr, reinterpret_cast<void*>(heap), span, kPoolFlagPhysical}, b{nullptr, nullptr, span, kPoolFlagVirtual};
    if (!nvnMemoryPoolInitialize(&physical, &a) || !nvnMemoryPoolInitialize(&virt, &b)) return false;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.target = kTarget2DArray;
    d.width = d.height = 256;
    d.depth = 2;
    d.levels = 3;
    d.format = 0x25;
    d.flags = 0x20;
    d.pool = &virt;
    if (!nvnTextureInitialize(&key, &d)) return false;
    const auto& layout = g_textures.get(&key)->layout;
    const Pixel zero{}, red{227,13,37,255}, green{21,193,39,255}, gold{200,174,47,255};
    if (layout.total_size() > span) return false;
    for (unsigned layer = 0; layer < 2; ++layer) {
        for (unsigned level = 0; level < 3; ++level) {
            const auto& lv = layout.levels[level];
            auto* pixels = reinterpret_cast<Pixel*>(heap + layer * layout.layer_size + lv.offset);
            std::fill_n(pixels, lv.size / sizeof(Pixel), level == 0 ? (layer ? red : zero) : level == 1 ? green : gold);
        }
    }
    auto map = [&](const void* pool, size_t offset, size_t size) {
        Mapping req{pool, int64_t(offset), int64_t(offset), size, 0xfe, 0};
        return nvnMemoryPoolMapVirtual(&virt, 1, &req) != 0;
    };
    if (!map(&physical, 0, span)) return false;
    auto sample = [&](const char* phase, int layer, int base, int count, const Pixel& wanted) {
        auto v = slice(base, layer);
        v.levels = count;
        nvnTexturePoolRegisterTexture(pool_key, slot, &key, &v);
        CommandList list;
        list.commands = {output, cmd::Viewport{0,0,0,width,height}, cmd::Scissor{0,0,0,width,height},
                         cmd::ClearColor{0,{1,0,1,1},15}, cmd::BindProgram{&p,3}, PolygonState{0,1,2,0},
                         ChannelMaskState{15}, ColorState{0,0,8,0}, cmd::SetPools{g_texture_pools.get(pool_key),&samplers},
                         cmd::BindTexture{1,0,uint64_t(slot),true}, cmd::Draw{4,-1,3,0,0,0,0,1}, cmd::SetRenderTargets{}};
        execute({&list}); wait_idle();
        std::array<Pixel,width*height> pixels{};
        CopyRegion area{0,0,0,width,height,1};
        nvnTextureReadTexels(output_key, &output.color_views[0], &area, pixels.data());
        auto bad = pixels.size() - std::count(pixels.begin(), pixels.end(), wanted);
        printf("%s layer %d mip %d+%d: %zu/%zu incorrect, rgba %u %u %u %u\n", phase,layer,base,count,bad,pixels.size(),
               pixels[0][0],pixels[0][1],pixels[0][2],pixels[0][3]);
        return bad;
    };
    size_t bad = sample("mapped zero tile",0,0,3,zero) + sample("mapped sibling",1,0,3,red);
    const size_t page = (layout.layer_size + tile - 1) / tile * tile;
    if (page + tile > layout.layer_size + layout.levels[0].size || !map(nullptr,page,tile)) return false;
    bad += sample("unmapped sibling",0,0,3,zero);
    bad += sample("coarser resident",1,0,3,green);
    // The central sample is outside the first tile. A bounded view still sees its own mapped texels.
    bad += sample("bounded partial level",1,0,1,red);
    bad += sample("explicit tail",1,2,1,gold);
    if (!map(nullptr,layout.layer_size,layout.levels[0].size)) return false;
    bad += sample("bounded empty level",1,0,1,zero);
    if (!map(&physical,layout.layer_size,layout.levels[0].size)) return false;
    bad += sample("remapped",1,0,3,red);
    nvnTexturePoolRegisterTexture(pool_key,slot,nullptr,nullptr);
    nvnTextureFinalize(&key);
    nvnMemoryPoolFinalize(&virt);
    nvnMemoryPoolFinalize(&physical);
    return bad == 0;
}

bool check_r16_views(VolkDeviceTable& v, VkDevice device, Program& sample, int* pool_key, int slot,
                     DescriptorPool& samplers, int* output_key, const cmd::SetRenderTargets& output) {
    Program write, read;
    if (!program_stage(write, TestShader::Vertex, v, device) || !program_stage(write, TestShader::WriteBits, v, device) ||
        !program_stage(read, TestShader::Vertex, v, device) || !program_stage(read, TestShader::ReadBits, v, device)) return false;

    int pool, key;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.width = 1280;
    d.height = 720;
    d.format = 0x05; // R16F allocation
    d.pool = &pool;
    std::vector<uint8_t> bytes(nvnTextureBuilderGetStorageSize(&d), 0);
    struct {void* device; void* bytes; size_t size; int flags;} b{nullptr, bytes.data(), bytes.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &b) || !nvnTextureInitialize(&key, &d)) return false;
    cmd::SetRenderTargets target{};
    target.count = 1;
    target.colors[0] = g_textures.get(&key);
    target.color_views[0].flags = kViewFormat;
    target.color_views[0].format = 0x08; // R16_UINT view
    CommandList produce;
    produce.commands = {target, cmd::BindProgram{&write,3}, PolygonState{0,1,2,0}, ChannelMaskState{15},
                        cmd::Viewport{0,0,0,float(d.width),float(d.height)}, cmd::Scissor{0,0,0,d.width,d.height},
                        cmd::Draw{4,-1,3,0,0,0,0,1}, cmd::SetRenderTargets{}};
    execute({&produce});
    wait_idle();
    std::vector<uint16_t> raw(d.width*d.height);
    CopyRegion all{0,0,0,d.width,d.height,1};
    nvnTextureReadTexels(&key,nullptr,&all,raw.data());
    const auto bad_bits = raw.size()-std::count(raw.begin(),raw.end(),uint16_t{0x3800});
    printf("R16_UINT attachment on R16F: %zu incorrect words / %zu\n", bad_bits, raw.size());
    unsigned bad_samples = 0;
    for (bool as_uint : {false, true}) {
        nvnTexturePoolRegisterTexture(pool_key, slot, &key, as_uint ? &target.color_views[0] : nullptr);
        CommandList consume;
        consume.commands = {output, cmd::SetPools{g_texture_pools.get(pool_key),&samplers}, cmd::BindTexture{1,0,uint64_t(slot),true},
                            cmd::BindProgram{as_uint ? &read : &sample,3}, cmd::Viewport{0,0,0,width,height},
                            cmd::Scissor{0,0,0,width,height},cmd::Draw{4,-1,3,0,0,0,0,1},cmd::SetRenderTargets{}};
        execute({&consume});
        wait_idle();
        std::array<Pixel,width*height> rgba{};
        CopyRegion rect{0,0,0,width,height,1};
        nvnTextureReadTexels(output_key,&output.color_views[0],&rect,rgba.data());
        const Pixel expected = as_uint ? Pixel{0,255,0,255} : Pixel{128,0,0,255};
        bad_samples += rgba.size()-std::count(rgba.begin(),rgba.end(),expected);
    }
    printf("R16F and R16_UINT sampling: %u mismatches / %d pixels\n", bad_samples, width*height*2);
    nvnTexturePoolRegisterTexture(pool_key, slot, nullptr, nullptr);
    nvnTextureFinalize(&key);
    nvnMemoryPoolFinalize(&pool);
    return bad_bits == 0 && bad_samples == 0;
}
}

extern "C" int bench() {
    auto* ctx = vk();
    if (!ctx) return 1;
    VolkDeviceTable v;
    volkLoadDeviceTable(&v, ctx->device);
    Program shader;
    if (!program_stage(shader, TestShader::Vertex, v, ctx->device) ||
        !program_stage(shader, TestShader::Sample, v, ctx->device)) return 2;
    int pool, pool_key, keys[2];
    constexpr int slot = 257;
    std::vector<uint8_t> storage(3*65536,17);
    struct { void* device; void* bytes; size_t size; int flags; } builder{nullptr,storage.data(),storage.size(),0};
    if (!nvnMemoryPoolInitialize(&pool, &builder)) return 3;
    TextureDesc desc;
    nvnTextureBuilderSetDefaults(&desc);
    desc.target = kTarget2DArray;
    desc.width = width;
    desc.height = height;
    desc.depth = desc.levels = 2;
    desc.format = 0x25;
    desc.pool = &pool;
    for (int i=0; i<2; ++i) {
        desc.offset = i*65536;
        if (!nvnTextureInitialize(&keys[i], &desc)) return 4;
    }
    using Surfaces = std::array<std::array<std::vector<Pixel>,2>,2>;
    Surfaces pixels[2];
    for (int level=0; level<2; ++level) for (int layer=0; layer<2; ++layer) {
        auto sv = slice(level, layer);
        CopyRegion region{0,0,0,width >> level,height >> level,1};
        auto& src = pixels[0][level][layer];
        src.assign(region.width*region.height, Pixel{uint8_t(32+level*64+layer*16),64,128,255});
        pixels[1][level][layer].assign(src.size(), guard);
        nvnTextureWriteTexels(&keys[0], &sv, &region, src.data());
    }
    if (!nvnTexturePoolInitialize(&pool_key, &pool, 2*65536, slot+1)) return 6;
    auto* images = g_texture_pools.get(&pool_key);
    DescriptorPool samplers;
    Sampler nearest{};
    nearest.desc.min_filter = 2;
    nearest.desc.max_anisotropy = 1;
    samplers.samplers.push_back(&nearest);
    if (getenv("VIEW_R16_ONLY") || getenv("VIEW_RESIDENCY")) {
        cmd::SetRenderTargets output{};
        output.count = 1;
        output.colors[0] = g_textures.get(&keys[1]);
        output.color_views[0] = slice(0,0);
        if (getenv("VIEW_RESIDENCY"))
            return check_sparse_layers(v, ctx->device, &pool_key, slot, samplers, &keys[1], output) ? 0 : 8;
        return check_r16_views(v, ctx->device, shader, &pool_key, slot, samplers, &keys[1], output) ? 0 : 7;
    }
    struct Step {int source, src_mip, src_layer, dst_mip, dst_layer; bool srgb;};
    const Step steps[] = {{0,0,0,0,0,false}, {0,0,1,0,0,true}, {0,1,1,1,1,true}, {0,0,1,0,0,true},
                          {1,0,0,1,0,false}, {1,1,0,1,0,true}, {0,1,0,0,1,false}};
    unsigned total = 0;
    const bool extent_only = getenv("VIEW_EXTENT_ONLY") != nullptr;
    for (const auto& step : steps) {
        if (extent_only) break;
        auto read_view = slice(step.src_mip,step.src_layer);
        if (step.source == 0 && step.src_mip == 0) {
            read_view.flags |= kViewTarget;
            read_view.target = kTarget2D;
        }
        nvnTexturePoolRegisterTexture(&pool_key, slot, &keys[step.source], &read_view);
        cmd::SetRenderTargets rt{};
        rt.count = 1;
        rt.colors[0] = g_textures.get(&keys[1]);
        rt.color_views[0] = slice(step.dst_mip,step.dst_layer,step.srgb);
        const int w = width >> step.dst_mip, h = height >> step.dst_mip;
        CommandList list;
        list.commands = {rt,cmd::Viewport{0,0,0,float(w),float(h)},cmd::Scissor{0,1,1,w-2,h-2},
                         cmd::SetPools{images,&samplers},cmd::BindProgram{&shader,3},cmd::BindTexture{1,0,slot,true},
                         PolygonState{0,1,2,0},ChannelMaskState{0xffffffff},cmd::Draw{4,-1,3,0,0,0,0,1},cmd::SetRenderTargets{}};
        const auto previous_images = g_texture_gpu_images.load();
        execute({&list});
        wait_idle();
        if (step.source == 1) {
            const bool overlap = step.src_mip == step.dst_mip && step.src_layer == step.dst_layer;
            const auto added = g_texture_gpu_images.load() - previous_images;
            printf("feedback overlap=%d new images=%llu\n", overlap, (unsigned long long)added);
            total += added != unsigned(overlap);
        }
        const int src_width = width >> step.src_mip;
        Pixel p = pixels[step.source][step.src_mip][step.src_layer][2*src_width+2];
        if (step.srgb) for (int c=0; c<3; ++c) {
            const double linear = p[c]/255.0;
            p[c] = std::lround(255*(linear <= 0.0031308 ? 12.92*linear : 1.055*std::pow(linear,1.0/2.4)-0.055));
        }
        auto& dst = pixels[1][step.dst_mip][step.dst_layer];
        for (int y=1; y<h-1; ++y) std::fill_n(dst.begin()+y*w+1,w-2,p);
        unsigned errors = 0;
        for (int level=0; level<2; ++level) for (int layer=0; layer<2; ++layer) {
            const auto sv = slice(level,layer);
            CopyRegion region{0,0,0,width >> level,height >> level,1};
            const auto& expected = pixels[1][level][layer];
            std::vector<Pixel> got(expected.size());
            nvnTextureReadTexels(&keys[1], &sv, &region, got.data());
            for (size_t i=0; i<got.size(); ++i) {
                bool bad = false;
                for (int c=0; c<4; ++c) bad |= std::abs(int(got[i][c])-expected[i][c]) > (c==3 || expected[i]==guard ? 0 : 1);
                errors += bad;
            }
        }
        printf("source %d mip %d layer %d -> mip %d layer %d srgb %d: %u mismatches\n",
               step.source,step.src_mip,step.src_layer,step.dst_mip,step.dst_layer,step.srgb,errors);
        total += errors;
    }
    // Two pending clears on one allocation. Reading mip 0 must see the first.
    // The second remains visible outside the scissor, including through a new sRGB attachment.
    for (bool srgb : {false, true}) {
        if (extent_only) break;
        auto* tex = g_textures.get(&keys[1]);
        cmd::SetRenderTargets a{}, b{};
        a.count = b.count = 1;
        a.colors[0] = b.colors[0] = tex;
        a.color_views[0] = slice(0, 0);
        b.color_views[0] = slice(1, 1);
        auto draw_target = b;
        draw_target.color_views[0] = slice(1, 1, srgb);
        nvnTexturePoolRegisterTexture(&pool_key, slot, &keys[1], &a.color_views[0]);
        CommandList list;
        list.commands = {a, cmd::Scissor{0,0,0,width,height}, cmd::ClearColor{0,{.25f,.25f,.25f,1},15},
                         b, cmd::ClearColor{0,{.5f,.5f,.5f,1},15}, draw_target,
                         cmd::Viewport{0,0,0,width/2,height/2}, cmd::Scissor{0,1,1,width/2-2,height/2-2},
                         cmd::Draw{4,-1,3,0,0,0,0,1}, cmd::SetRenderTargets{}};
        execute({&list});
        wait_idle();
        const CopyRegion area{0,0,0,width/2,height/2,1};
        std::vector<Pixel> got(area.width*area.height);
        nvnTextureReadTexels(&keys[1], &b.color_views[0], &area, got.data());
        unsigned errors = 0;
        for (int y=0; y<area.height; ++y) for (int x=0; x<area.width; ++x) {
            const bool border = !x || !y || x==area.width-1 || y==area.height-1;
            const int want = border ? 128 : srgb ? 137 : 64;
            const Pixel p = got[y*area.width+x];
            errors += std::abs(int(p[0])-want) > 1 || p[1]!=p[0] || p[2]!=p[0] || p[3]!=255;
        }
        printf("pending mip/layer clears srgb=%d: %u mismatches\n", srgb, errors);
        total += errors;
    }
    // With a fixed viewport, dropping a smaller MRT attachment expands the framebuffer.
    // A reused pass cannot address the strip beyond its old extent.
    cmd::SetRenderTargets large{};
    large.count = 1;
    large.colors[0] = g_textures.get(&keys[1]);
    large.color_views[0] = slice(0,1);
    auto mixed = large;
    mixed.count = 2;
    mixed.colors[1] = g_textures.get(&keys[0]);
    mixed.color_views[1] = slice(1,1);
    auto source = slice(0,0);
    source.flags |= kViewTarget;
    source.target = kTarget2D;
    nvnTexturePoolRegisterTexture(&pool_key, slot, &keys[0], &source);
    CommandList expand;
    expand.commands = {cmd::SetPools{images,&samplers}, cmd::BindProgram{&shader,3}, cmd::BindTexture{1,0,slot,true},
                       PolygonState{0,1,2,0}, large, cmd::Scissor{0,0,0,width,height}, cmd::ClearColor{0,{0,0,0,1},15},
                       mixed, ChannelMaskState{15}, cmd::Viewport{0,0,0,width,height},
                       cmd::Scissor{0,0,0,width/2,height/2}, cmd::Draw{4,-1,3,0,0,0,0,1},
                       large, cmd::Scissor{0,width-4,0,4,height},
                       cmd::Draw{4,-1,3,0,0,0,0,1}, cmd::SetRenderTargets{}};
    execute({&expand});
    wait_idle();
    std::array<Pixel, width*height> result{};
    const CopyRegion full{0,0,0,width,height,1};
    nvnTextureReadTexels(&keys[1], &large.color_views[0], &full, result.data());
    unsigned extent_errors = 0;
    for (int y=0; y<height; ++y) for (int x=0; x<width; ++x) {
        const bool painted = (x<width/2 && y<height/2) || x>=width-4;
        extent_errors += result[y*width+x] != (painted ? Pixel{32,64,128,255} : Pixel{0,0,0,255});
    }
    printf("framebuffer expansion: %u mismatches / %zu pixels\n", extent_errors, result.size());
    total += extent_errors;
    if (!extent_only && !check_r16_views(v, ctx->device, shader, &pool_key, slot, samplers, &keys[1], large)) ++total;
    if (getenv("VIEW_PUBLICATION")) {
        const TextureViewDesc descriptors[]{slice(0,0), slice(1,1,true)};
        Texture* textures[]{g_textures.get(&keys[0]), g_textures.get(&keys[1])};
        std::atomic<bool> start{}, done{};
        nvnTexturePoolRegisterTexture(&pool_key, slot, &keys[0], &descriptors[0]);
        constexpr unsigned iterations = 100000;
        std::thread writer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned i = 0; i < iterations; ++i)
                nvnTexturePoolRegisterTexture(&pool_key, slot, &keys[i % 2], &descriptors[i % 2]);
            done.store(true, std::memory_order_release);
        });
        start.store(true, std::memory_order_release);
        unsigned reads = 0, errors = 0;
        do {
            const auto entry = images->texture_at(slot);
            const int index = entry.texture == textures[0] ? 0 : 1;
            const auto& want = descriptors[index];
            errors += entry.texture != textures[index] || entry.view.base_level != want.base_level ||
                      entry.view.base_layer != want.base_layer || entry.view.format != want.format;
            ++reads;
        } while (reads < iterations || !done.load(std::memory_order_acquire));
        writer.join();
        printf("descriptor publication: %u writes, %u reads, %u mixed records\n", iterations, reads, errors);
        total += errors;
    }
    return total ? 5 : 0;
}
