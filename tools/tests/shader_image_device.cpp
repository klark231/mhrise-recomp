// Android auditrun fixture. Compile image_access.comp with glslc and place
// image_access.comp.spv in the working directory alongside the other probe inputs.
#include "nnsdk/nvn/commands.h"
#include <array>
#include <cstdio>
#include <fstream>

using namespace nvn;
extern "C" {
int nvnMemoryPoolInitialize(void*, const void*);
void nvnTextureBuilderSetDefaults(TextureDesc*);
int nvnTextureInitialize(void*, const TextureDesc*);
int nvnCommandBufferInitialize(void*, void*);
void nvnCommandBufferBeginRecording(void*);
uint64_t nvnCommandBufferEndRecording(void*);
void nvnCommandBufferSetRenderTargets(void*, int, void* const*, const void* const*, void*, const void*);
void nvnTextureReadTexels(const void*, const TextureViewDesc*, const CopyRegion*, void*);
}

extern "C" int bench() {
    auto* ctx = vk();
    if (!ctx) return 1;
    std::ifstream file("image_access.comp.spv", std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0 || size % 4) return 2;
    Program compute;
    auto& stage = compute.stages.emplace_back();
    stage.shader.stage = nvnshader::Stage::Compute;
    stage.shader.spirv.resize(size / 4);
    if (!file.seekg(0).read(reinterpret_cast<char*>(stage.shader.spirv.data()), size)) return 3;
    for (uint32_t i = 0; i < 2; ++i) {
        nvnshader::Resource r{nvnshader::Resource::Image, i, 0, 0x120 + 8*i, 2};
        r.is_written = i == 1;
        stage.shader.resources.push_back(r);
    }
    stage.tried = true;
    stage.spirv_id = stage.print = 0x494d474143434553ULL;
    VolkDeviceTable v;
    volkLoadDeviceTable(&v, ctx->device);
    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = size;
    mi.pCode = stage.shader.spirv.data();
    if (v.vkCreateShaderModule(ctx->device, &mi, nullptr, &stage.module) != VK_SUCCESS) return 4;

    int pool, cb, keys[3];
    std::vector<uint8_t> bytes(3*65536, 17);
    struct { void* device; void* storage; size_t size; int flags; } builder{nullptr, bytes.data(), bytes.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &builder) || !nvnCommandBufferInitialize(&cb, nullptr)) return 5;
    TextureDesc desc;
    nvnTextureBuilderSetDefaults(&desc);
    desc.width = desc.height = 16;
    desc.pool = &pool;
    desc.format = 0x25;
    desc.flags = kTextureFlagImage;
    void* handles[3];
    for (int i = 0; i < 3; ++i) {
        desc.offset = i*65536;
        handles[i] = &keys[i];
        if (!nvnTextureInitialize(handles[i], &desc)) return 6;
    }
    nvnCommandBufferBeginRecording(&cb);
    nvnCommandBufferSetRenderTargets(&cb, 3, handles, nullptr, nullptr, nullptr);
    auto* bindings = reinterpret_cast<CommandList*>(nvnCommandBufferEndRecording(&cb));
    auto rt = std::get<cmd::SetRenderTargets>(bindings->commands.front());
    DescriptorPool images;
    images.textures.resize(3);
    images.textures[1].texture = rt.colors[0];
    images.textures[2].texture = rt.colors[1];
    CopyRegion all{0, 0, 0, 16, 16, 1};
    rt.count = 1;
    CommandList list;
    list.commands = {rt, cmd::ClearColor{0,{1,0,0,1},15}, cmd::SetRenderTargets{},
                     cmd::SetPools{&images,nullptr}, cmd::BindProgram{&compute,32},
                     cmd::BindImage{5,0,1}, cmd::BindImage{5,1,2}, cmd::Dispatch{1,16,1},
                     cmd::CopyTexture{rt.colors[1],{},all,rt.colors[2],{},all}};
    execute({&list});
    wait_idle();
    using Pixel = std::array<uint8_t, 4>;
    std::array<Pixel, 16*16> result{};
    nvnTextureReadTexels(handles[2], nullptr, &all, result.data());
    unsigned errors = 0;
    for (size_t i = 0; i < result.size(); ++i)
        errors += result[i] != (i % 16 < 8 ? Pixel{0,255,0,255} : Pixel{17,17,17,17});
    printf("attachment -> compute image load/store -> copy: %u mismatches / %zu pixels\n", errors, result.size());
    return errors != 0;
}
