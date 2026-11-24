// auditrun fixture. The same shader and render target are reused as the NVN vertex format changes.
#include "nnsdk/nvn/commands.h"
#include <array>
#include <bit>
#include <cstdio>
#include <fstream>

using namespace nvn;
extern "C" {
int nvnMemoryPoolInitialize(void*, const void*);
void nvnTextureBuilderSetDefaults(TextureDesc*);
int nvnTextureInitialize(void*, const TextureDesc*);
void nvnTextureReadTexels(const void*, const TextureViewDesc*, const CopyRegion*, void*);
void nvnTextureWriteTexels(const void*, const TextureViewDesc*, const CopyRegion*, const void*);
}
namespace {
using Words = std::array<uint32_t, 4>;
bool add_stage(Program& p, bool vertex, const char* file, bool sampled = false, bool depth_only = false) {
    std::ifstream f(file, std::ios::binary | std::ios::ate);
    const auto size = f.tellg();
    if (size <= 0 || size % 4) return false;
    auto& s = p.stages.emplace_back();
    s.shader.spirv.resize(size / 4);
    if (!f.seekg(0).read(reinterpret_cast<char*>(s.shader.spirv.data()), size)) return false;
    s.shader.stage = vertex ? nvnshader::Stage::Vertex : nvnshader::Stage::Fragment;
    s.shader.input_generics = vertex ? 16 : 1;
    s.shader.output_generics = vertex ? 1 : 0;
    s.shader.color_outputs = vertex || depth_only ? 0 : 1;
    if (sampled) {
        nvnshader::Resource r{};
        r.kind = nvnshader::Resource::Texture;
        r.texture_type = 2;
        r.cbuf_index = 2;
        r.cbuf_offset = 0x168;
        s.shader.resources.push_back(r);
    }
    s.spirv_id = spirv_identity(s.shader);
    s.tried = true;
    return create_shader_module(vk()->device, s.shader.spirv, &s.module) == VK_SUCCESS;
}
uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }
bool pan() {
    Program pre, paint;
    if (!add_stage(pre, true, "vertex_pan.vert.spv") || !add_stage(paint, true, "vertex_pan.vert.spv") ||
        !add_stage(pre, false, "vertex_pan_z.frag.spv", true, true) ||
        !add_stage(paint, false, "vertex_pan.frag.spv", true))
        return false;
    using Pixel = std::array<uint8_t, 4>;
    std::vector<uint8_t> storage(3 * 65536);
    int pool, handles[3];
    struct {
        void* device;
        void* data;
        size_t size;
        int flags;
    } mem{nullptr, storage.data(), storage.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &mem)) return false;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.pool = &pool;
    d.format = 0x25;
    for (int i = 0; i < 3; ++i) {
        d.width = d.height = i == 0 ? 64 : 16;
        d.levels = i == 0 ? 3 : 1;
        d.offset = i * 65536;
        if (i == 2) d.format = 0x35;
        if (!nvnTextureInitialize(&handles[i], &d)) return false;
    }
    const Pixel colors[] = {{128, 32, 4, 255}, {21, 153, 77, 255}, {211, 172, 49, 255}};
    for (int level = 0; level < 3; ++level) {
        int side = 64 >> level;
        std::vector<Pixel> data(side * side, colors[level]);
        for (int i = 0; i < side * side; ++i)
            data[i][3] = (i % side / 2) & 1 ? 255 : 0;
        TextureViewDesc v{};
        v.flags = kViewLevels;
        v.base_level = level;
        v.levels = 1;
        CopyRegion all{0, 0, 0, side, side, 1};
        nvnTextureWriteTexels(&handles[0], &v, &all, data.data());
    }
    DescriptorPool images, samplers;
    images.textures.resize(2);
    images.register_texture(1, {g_textures.get(&handles[0]), {}});
    Sampler s{};
    s.desc.min_filter = 2;
    s.desc.wrap[0] = s.desc.wrap[1] = 1;
    s.desc.lod_max = 2;
    s.desc.max_anisotropy = 1;
    samplers.samplers.push_back(&s);
    cmd::SetRenderTargets z{}, rt{};
    z.depth = rt.depth = g_textures.get(&handles[2]);
    rt.count = 1;
    rt.colors[0] = g_textures.get(&handles[1]);
    DepthStencilState write{}, equal{};
    write.depth_test = write.depth_write = 1;
    write.depth_func = 7;
    equal.depth_test = 1;
    equal.depth_func = 3;
    const cmd::Draw tri{4, -1, 3, 0, 0, 0, 0, 1};
    unsigned bad = 0;
    for (unsigned level = 0; level < 3; ++level)
        for (unsigned phase = 0; phase < 16; ++phase) {
            const uint32_t data[] = {phase | (level << 8), phase | (level << 8), phase | (level << 8)};
            auto no_spill = [](size_t) -> void* { std::abort(); };
            VertexAttribState attrs[5]{};
            attrs[4].format = 0x27;
            VertexStreamState stream{4, 0};
            BufferRange vb{uintptr_t(data), sizeof data};
            CommandList list;
            list.commands = {z,
                             cmd::Viewport{0, 0, 0, 16, 16},
                             cmd::Scissor{0, 0, 0, 16, 16},
                             cmd::DepthRange{0, 1},
                             cmd::ClearDepthStencil{0, true, 0, 255},
                             write,
                             cmd::BindProgram{&pre, 3},
                             cmd::SetPools{&images, &samplers},
                             cmd::BindTexture{1, 0, 1, true},
                             PolygonState{0, 1, 2, 0},
                             ColorState{0, 0, 8, 0},
                             ChannelMaskState{15},
                             cmd::BindVertexAttribs{{attrs, 5, no_spill}},
                             cmd::BindVertexStreams{{&stream, 1, no_spill}},
                             cmd::BindVertexBuffers{0, {&vb, 1, no_spill}},
                             tri,
                             rt,
                             cmd::ClearColor{0, {0, 0, 1, 1}, 15},
                             equal,
                             cmd::BindProgram{&paint, 3},
                             tri,
                             cmd::SetRenderTargets{}};
            execute({&list});
            wait_idle();
            std::array<Pixel, 256> pixels{};
            CopyRegion all{0, 0, 0, 16, 16, 1};
            nvnTextureReadTexels(&handles[1], nullptr, &all, pixels.data());
            for (unsigned i = 0; i < pixels.size(); ++i) {
                const Pixel want = ((i % 16 + phase) / 2) & 1 ? colors[level] : Pixel{0, 0, 255, 255};
                if (pixels[i] != want && ++bad <= 2)
                    printf("pan mismatch mip=%u phase=%u x=%u got %u %u %u %u\n", level, phase, i % 16, pixels[i][0],
                           pixels[i][1], pixels[i][2], pixels[i][3]);
            }
        }
    printf("panning cutout, depth prepass + EQUAL, 3 implicit mip levels: %u / %u mismatches\n", bad, 48 * 256);
    return bad == 0;
}
// VERTEX_OVERLAY: compile vertex_overlay.comp in addition to the vertex fetch shaders.
bool overlay_windows(Program& paint, const cmd::SetRenderTargets& target, void* texture) {
    Program writer;
    auto& cs = writer.stages.emplace_back();
    std::ifstream file("vertex_overlay.comp.spv", std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0 || size % 4) return false;
    cs.shader.spirv.resize(size / 4);
    if (!file.seekg(0).read(reinterpret_cast<char*>(cs.shader.spirv.data()), size)) return false;
    cs.shader.stage = nvnshader::Stage::Compute;
    nvnshader::Resource storage{nvnshader::Resource::StorageBuffer, 0, 0, 0x310, 0};
    storage.is_written = true;
    cs.shader.resources.push_back(storage);
    cs.tried = true;
    cs.spirv_id = spirv_identity(cs.shader);
    if (create_shader_module(vk()->device, cs.shader.spirv, &cs.module) != VK_SUCCESS) return false;

    constexpr unsigned stride = 256;
    alignas(256) std::array<uint8_t, 4096> ring{};
    std::array<Words, 16> original{};
    for (unsigned i = 0; i < original.size(); ++i) {
        original[i] = {i + 11, i + 23, i + 37, i + 51};
        memcpy(ring.data() + i * stride, original[i].data(), sizeof(Words));
    }
    const uint64_t base = reinterpret_cast<uintptr_t>(ring.data());
    const uint16_t indices[][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}};
    const size_t span = 2 * stride + sizeof(Words);
    auto no_spill = [](size_t) -> void* { std::abort(); };
    VertexAttribState attrs[5]{};
    attrs[4].format = 0x2f;
    CommandList list;
    list.commands = {target, cmd::Viewport{0, 0, 0, 16, 8}, cmd::Scissor{0, 0, 0, 16, 8},
                     cmd::ClearColor{0, {}, 15}, PolygonState{0, 1, 2, 0}, ColorState{0, 0, 8, 0},
                     ChannelMaskState{15}, cmd::BindVertexAttribs{{attrs, 5, no_spill}}};
    std::vector<Words> expected;
    auto draw = [&](unsigned row, unsigned v, bool tight, bool instanced, bool updated) {
        const BufferRange vb{tight ? base + row * stride : base, tight ? span : ring.size()};
        const VertexStreamState stream{stride, unsigned(instanced)};
        list.commands.emplace_back(cmd::BindProgram{&paint, 3});
        list.commands.emplace_back(cmd::BindVertexStreams{{&stream, 1, no_spill}});
        list.commands.emplace_back(cmd::BindVertexBuffers{0, {&vb, 1, no_spill}});
        list.commands.emplace_back(cmd::Scissor{0, int(expected.size()), 0, 1, 8});
        list.commands.emplace_back(cmd::Draw{4, 1, 3, 0, reinterpret_cast<uintptr_t>(indices[v]),
                                            tight || instanced ? 0 : int(row), instanced ? int(row + v) : 0, 1});
        Words value = original[row + v];
        if (updated) for (auto& word : value) word ^= 0x13579bdfu;
        expected.push_back(value);
    };
    draw(2, 0, false, false, false);  // prime the CPU vertex cache
    list.commands.emplace_back(cmd::BindProgram{&writer, 32});
    auto update = [&](unsigned row, unsigned bytes) {
        list.commands.emplace_back(cmd::BindStorageBuffer{5, 0, {base + row * stride, bytes}});
        list.commands.emplace_back(cmd::Dispatch{int(bytes / 4), 1, 1});
    };
    update(2, span);
    update(8, sizeof(Words));
    update(10, sizeof(Words));
    list.commands.emplace_back(cmd::Barrier{~0});
    for (unsigned i = 0; i < 3; ++i) {
        draw(2, i, true, false, true);
        draw(2, i, false, false, true);
        draw(8, i, false, false, i != 1);
        draw(8, i, false, true, i != 1);
    }
    list.commands.emplace_back(cmd::SetRenderTargets{});
    execute({&list});
    wait_idle();
    const CopyRegion all{0, 0, 0, 16, 8, 1};
    std::array<Words, 128> pixels{};
    nvnTextureReadTexels(texture, nullptr, &all, pixels.data());
    unsigned bad = 0;
    for (size_t x = 0; x < expected.size(); ++x)
        for (unsigned y = 0; y < 8; ++y) bad += pixels[y * 16 + x] != expected[x];
    printf("GPU vertex windows: %u/%zu pixels incorrect\n", bad, 8 * expected.size());

    // A compute buffer can be reused for a smaller set of particles while the CPU updates its tail.
    alignas(256) std::array<uint32_t, 1408> particles{};
    const uint64_t address = reinterpret_cast<uintptr_t>(particles.data());
    const cmd::BindStorageBuffer part{5, 0, {address, 5120}};
    CommandList initialize;
    initialize.commands = {cmd::BindProgram{&writer, 32},
                           cmd::BindStorageBuffer{5, 0, {address, sizeof(particles)}},
                           cmd::Dispatch{1408, 1, 1}, cmd::Barrier{~0},
                           part, cmd::Dispatch{1, 1, 1}, cmd::Barrier{~0}};
    execute({&initialize});
    wait_idle();
    particles[1280] = 1;
    const BufferRange vertices{address + 256, 3 * sizeof(Words)};
    const VertexStreamState stream{sizeof(Words), 0};
    CommandList sample;
    sample.commands = {cmd::BindProgram{&writer, 32}, part, cmd::Dispatch{1, 1, 1}, cmd::Barrier{~0},
                       target, cmd::Viewport{0, 0, 0, 16, 8}, cmd::Scissor{0, 0, 0, 16, 8},
                       cmd::BindProgram{&paint, 3}, cmd::BindVertexAttribs{{attrs, 5, no_spill}},
                       cmd::BindVertexStreams{{&stream, 1, no_spill}},
                       cmd::BindVertexBuffers{0, {&vertices, 1, no_spill}},
                       cmd::Draw{4, -1, 3, 0, 0, 0, 0, 1}, cmd::SetRenderTargets{}};
    execute({&sample});
    wait_idle();
    nvnTextureReadTexels(texture, nullptr, &all, pixels.data());
    Words gpu_value;
    gpu_value.fill(0x13579bdfu);
    const auto mismatches = pixels.size() - std::count(pixels.begin(), pixels.end(), gpu_value);
    printf("compute subrange after neighbouring CPU write: %zu/%zu pixels incorrect\n", mismatches, pixels.size());
    bad += mismatches;
    return bad == 0;
}
} // namespace
extern "C" int bench() {
    if (!vk()) return 1;
    if (getenv("VERTEX_PAN")) return pan() ? 0 : 6;
    Program p, reference;
    if (!add_stage(p, true, "vertex_fetch.vert.spv") || !add_stage(p, false, "vertex_fetch.frag.spv") ||
        !add_stage(reference, true, "vertex_fetch_reference.vert.spv") ||
        !add_stage(reference, false, "vertex_fetch.frag.spv"))
        return 2;
    std::vector<uint8_t> storage(65536);
    int pool, key;
    struct {
        void* device;
        void* data;
        size_t size;
        int flags;
    } mem{nullptr, storage.data(), storage.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &mem)) return 3;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.pool = &pool;
    d.width = 16;
    d.height = 8;
    d.format = 0x2f;
    if (!nvnTextureInitialize(&key, &d)) return 4;
    cmd::SetRenderTargets target{};
    target.count = 1;
    target.colors[0] = g_textures.get(&key);
    if (getenv("VERTEX_OVERLAY")) return overlay_windows(p, target, &key) ? 0 : 7;
    struct Case {
        uint8_t format;
        Words input, expected;
    };
    const Case cases[]{
        {0x27, {0xff800501}, {1, 5, 128, 255}},
        {0x28, {0xff800501}, {1, 5, 0xffffff80, 0xffffffff}},
        {0x55, {16777220}, {bits(16777220.f), 0, 0, bits(1)}},
        {0x56, {0xfffffff9}, {bits(-7), 0, 0, bits(1)}},
        {0x63, {0xff800501}, {bits(1), bits(5), bits(128), bits(255)}},
        {0x25, {0xff00ff00}, {0, bits(1), 0, bits(1)}},
        {0x29, {0x38003c00, 0xc0004000}, {bits(1), bits(.5f), bits(2), bits(-2)}},
        {0x03, {0x89}, {137, 0, 0, 1}},
        {0x2f, {0x7fc01234, 0x80000000, 0xffffffff, 0x01234567}, {0x7fc01234, 0x80000000, 0xffffffff, 0x01234567}},
        {0x27, {0x07050301}, {1, 3, 5, 7}},
    };
    unsigned errors = 0;
    auto no_spill = [](size_t) -> void* { std::abort(); };
    for (auto& c : cases) {
        std::array<Words, 3> vertices{c.input, c.input, c.input};
        VertexAttribState attrs[5]{};
        attrs[4].format = c.format;
        VertexStreamState stream{sizeof(Words), 0};
        BufferRange vb{uintptr_t(vertices.data()), sizeof(vertices)};
        CommandList list;
        list.commands = {target,
                         cmd::Viewport{0, 0, 0, 16, 8},
                         cmd::Scissor{0, 0, 0, 16, 8},
                         cmd::ClearColor{0, {}, 15},
                         cmd::BindProgram{&p, 3},
                         PolygonState{0, 1, 2, 0},
                         ColorState{0, 0, 8, 0},
                         ChannelMaskState{15},
                         cmd::BindVertexAttribs{{attrs, 5, no_spill}},
                         cmd::BindVertexStreams{{&stream, 1, no_spill}},
                         cmd::BindVertexBuffers{0, {&vb, 1, no_spill}},
                         cmd::Draw{4, -1, 3, 0, 0, 0, 0, 1},
                         cmd::SetRenderTargets{}};
        execute({&list});
        wait_idle();
        CopyRegion all{0, 0, 0, 16, 8, 1};
        std::array<Words, 128> pixels{};
        nvnTextureReadTexels(&key, nullptr, &all, pixels.data());
        const auto bad = pixels.size() - std::count(pixels.begin(), pixels.end(), c.expected);
        printf("vertex format %02x: %zu/%zu incorrect, got %08x %08x %08x %08x\n", c.format, bad, pixels.size(),
               pixels[0][0], pixels[0][1], pixels[0][2], pixels[0][3]);
        errors += bad;
        if (c.format == 0x55) {
            // 2^24+3 lies halfway between adjacent floats. Compare the specialized module
            // with an independent uint-input GLSL shader doing the specified conversion.
            for (auto& value : vertices)
                value[0] = 16777219u;
            execute({&list});
            wait_idle();
            nvnTextureReadTexels(&key, nullptr, &all, pixels.data());
            for (auto& command : list.commands)
                if (auto* bind = std::get_if<cmd::BindProgram>(&command)) bind->program = &reference;
            execute({&list});
            wait_idle();
            std::array<Words, 128> expected{};
            nvnTextureReadTexels(&key, nullptr, &all, expected.data());
            unsigned diff = 0;
            for (size_t i = 0; i < pixels.size(); ++i)
                diff += pixels[i] != expected[i] ||
                        (expected[i][0] != bits(16777218.f) && expected[i][0] != bits(16777220.f));
            printf("scaled tie: %u/%zu mismatches against typed GLSL, reference=%08x\n", diff, pixels.size(),
                   expected[0][0]);
            errors += diff;
        }
    }
    // Keep the shader and format fixed while changing only the declaration,
    // then only the bound storage. Instance 1 selects the second record.
    struct FetchCase { uint8_t stream; uint16_t offset; uint32_t stride, divisor; };
    const FetchCase layouts[] = {{0, 0, 16, 0}, {1, 4, 16, 0}, {1, 4, 32, 1}, {1, 4, 32, 1}, {0, 0, 16, 0}};
    std::array<std::array<uint8_t, 128>, 5> records{};
    for (unsigned i = 0; i < records.size(); ++i) {
        const auto layout = layouts[i];
        const uint32_t packed = 0xff030201u + i;
        records[i].fill(0xa5);
        for (unsigned v = layout.divisor ? 1 : 0; v < 3; ++v)
            memcpy(records[i].data() + v * layout.stride + layout.offset, &packed, sizeof packed);
        VertexAttribState attrs[5]{};
        attrs[4] = {0x27, layout.stream, layout.offset};
        VertexStreamState streams[2]{};
        streams[layout.stream] = {layout.stride, layout.divisor};
        BufferRange vb{uintptr_t(records[i].data()), records[i].size()};
        CommandList list;
        list.commands = {target, cmd::ClearColor{0, {}, 15}, cmd::BindProgram{&p, 3},
                         cmd::BindVertexAttribs{{attrs, 5, no_spill}},
                         cmd::BindVertexStreams{{streams, layout.stream + 1u, no_spill}},
                         cmd::BindVertexBuffers{layout.stream, {&vb, 1, no_spill}},
                         cmd::Draw{4, -1, 3, 0, 0, 0, 1, 1}, cmd::SetRenderTargets{}};
        execute({&list});
        wait_idle();
        CopyRegion all{0, 0, 0, 16, 8, 1};
        std::array<Words, 128> pixels{};
        nvnTextureReadTexels(&key, nullptr, &all, pixels.data());
        const Words expected{1 + i, 2, 3, 255};
        const auto bad = pixels.size() - std::count(pixels.begin(), pixels.end(), expected);
        printf("vertex declaration %u: %zu/%zu incorrect\n", i, bad, pixels.size());
        errors += bad;
    }
    return errors ? 5 : 0;
}
