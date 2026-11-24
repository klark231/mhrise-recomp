// Android offscreen fixture. Replay old NVN recordings after recycling guest handles.
#include "nnsdk/nvn/commands.h"
#include "nnsdk/nvn/driver.h"
#include <algorithm>
#include <array>

using namespace nvn;
namespace {
struct PoolBuilder {
    void* device;
    void* storage;
    size_t bytes;
    int flags;
};
extern "C" {
int nvnMemoryPoolInitialize(void*, const PoolBuilder*);
void nvnMemoryPoolFinalize(void*);
void nvnTextureBuilderSetDefaults(TextureDesc*);
int nvnTextureInitialize(void*, const TextureDesc*);
void nvnTextureFinalize(void*);
void nvnTextureReadTexels(const void*, const TextureViewDesc*, const CopyRegion*, void*);
int nvnTexturePoolInitialize(void*, void*, int64_t, int);
void nvnTexturePoolFinalize(void*);
void nvnTexturePoolRegisterTexture(void*, int, const void*, const TextureViewDesc*);
void nvnSamplerBuilderSetDefaults(SamplerDesc*);
int nvnSamplerInitialize(void*, const SamplerDesc*);
void nvnSamplerFinalize(void*);
int nvnSamplerPoolInitialize(void*, void*, int64_t, int);
void nvnSamplerPoolFinalize(void*);
void nvnSamplerPoolRegisterSampler(void*, int, const void*);
void nvnSamplerPoolRegisterSamplerBuilder(void*, int, const SamplerDesc*);
int nvnQueueInitialize(void*, const void*);
void nvnQueueSubmitCommands(void*, int, const uint64_t*);
void nvnQueueFinish(void*);
void nvnQueueFinalize(void*);
int nvnCommandBufferInitialize(void*, void*);
void nvnCommandBufferBeginRecording(void*);
void nvnCommandBufferSetRenderTargets(void*, int, void* const*, const void* const*, void*, const void*);
void nvnCommandBufferClearColor(void*, int, const float*, int);
void nvnCommandBufferClearTexture(void*, const void*, const void*, const CopyRegion*, const float*, int);
void nvnCommandBufferSetTexturePool(void*, const void*);
void nvnCommandBufferSetSamplerPool(void*, const void*);
uint64_t nvnCommandBufferEndRecording(void*);
void nvnCommandBufferFinalize(void*);
}
} // namespace

extern "C" int bench() {
    if (!vk() || !driver::custom()) return 1;
    int pool, image, alias, textures, sampler, samplers, queue, cmd;
    std::array<uint32_t, 64 * 64> backing{}, pixels{};
    backing.fill(0xff00ff00);
    PoolBuilder builder{nullptr, backing.data(), sizeof(backing), 0x82};
    if (!nvnMemoryPoolInitialize(&pool, &builder) || !nvnQueueInitialize(&queue, nullptr) ||
        !nvnCommandBufferInitialize(&cmd, nullptr) || !nvnTexturePoolInitialize(&textures, &pool, 0, 2) ||
        !nvnSamplerPoolInitialize(&samplers, &pool, 0, 2))
        return 2;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.pool = &pool;
    d.width = d.height = 64;
    d.format = 0x25;
    if (!nvnTextureInitialize(&image, &d) || !nvnTextureInitialize(&alias, &d)) return 3;
    auto ref = g_textures.reference(&image);
    std::weak_ptr<Texture> original = g_textures.acquire(&image);
    if (!ref || ref.get() != g_textures.get(&alias)) return 4;
    nvnTexturePoolRegisterTexture(&textures, 1, &image, nullptr);
    const auto texture_pool = g_texture_pools.reference(&textures);
    const auto sampler_pool = g_sampler_pools.reference(&samplers);
    SamplerDesc s;
    nvnSamplerBuilderSetDefaults(&s);
    if (!nvnSamplerInitialize(&sampler, &s)) return 5;
    nvnSamplerPoolRegisterSampler(&samplers, 1, &sampler);
    nvnSamplerPoolRegisterSamplerBuilder(&samplers, 0, &s);
    const auto image_table = texture_pool->texture_snapshot();
    const auto sampler_table = sampler_pool->sampler_snapshot();
    nvnTexturePoolRegisterTexture(&textures, 0, &alias, nullptr);
    nvnSamplerPoolRegisterSampler(&samplers, 0, &sampler);
    if ((*sampler_table)[0] || (*image_table)[0].texture || !(*texture_pool->texture_snapshot())[0].texture ||
        (*sampler_pool->sampler_snapshot())[0].get() != find_sampler(&sampler)) return 13;
    const CopyRegion box{0, 0, 0, 64, 64, 1};
    const float red[]{1, 0, 0, 1};
    nvnCommandBufferBeginRecording(&cmd);
    nvnCommandBufferSetTexturePool(&cmd, &textures);
    nvnCommandBufferSetSamplerPool(&cmd, &samplers);
    void* target = &image;
    nvnCommandBufferSetRenderTargets(&cmd, 1, &target, nullptr, nullptr, nullptr);
    nvnCommandBufferClearColor(&cmd, 0, red, 15);
    nvnCommandBufferClearTexture(&cmd, &image, nullptr, &box, red, 15);
    const auto list = nvnCommandBufferEndRecording(&cmd);
    if (!list) return 6;
    nvnTextureFinalize(&image);
    if (!ref || texture_pool->texture_at(1).texture.get() != g_textures.get(&alias)) return 7;
    {
        // The queued replay cannot start until both guest addresses have been reused.
        std::lock_guard lock(executor_mutex());
        nvnQueueSubmitCommands(&queue, 1, &list);
        nvnTextureFinalize(&alias);
        nvnSamplerFinalize(&sampler);
        if (!original.expired() || ref || texture_pool->texture_at(1).texture || sampler_pool->sampler_at(1) ||
            (*image_table)[1].texture || (*sampler_table)[1]) return 8;
        if (!nvnTextureInitialize(&image, &d) || !nvnSamplerInitialize(&sampler, &s) ||
            texture_pool->texture_at(1).texture || sampler_pool->sampler_at(1))
            return 9;
        nvnSamplerPoolRegisterSamplerBuilder(&samplers, 0, &s);
        const auto builder_table = sampler_pool->sampler_snapshot();
        if (!(*builder_table)[0]) return 14;
        nvnTexturePoolFinalize(&textures);
        nvnSamplerPoolFinalize(&samplers);
        if (!nvnTexturePoolInitialize(&textures, &pool, 0, 2) || !nvnSamplerPoolInitialize(&samplers, &pool, 0, 2) ||
            texture_pool || sampler_pool || (*builder_table)[0])
            return 10;
    }
    nvnQueueFinish(&queue);
    nvnQueueSubmitCommands(&queue, 1, &list);
    nvnQueueFinish(&queue);
    nvnTextureReadTexels(&image, nullptr, &box, pixels.data());
    if (!std::all_of(pixels.begin(), pixels.end(), [](uint32_t v) { return v == 0xff00ff00; })) return 11;
    auto live = g_textures.reference(&image);
    d.width = 32;
    if (!nvnTextureInitialize(&image, &d) || !live || live->desc.width != 32) return 12;
    nvnCommandBufferFinalize(&cmd);
    nvnTexturePoolFinalize(&textures);
    nvnSamplerPoolFinalize(&samplers);
    nvnSamplerFinalize(&sampler);
    nvnTextureFinalize(&image);
    nvnQueueFinalize(&queue);
    nvnMemoryPoolFinalize(&pool);
    return 0;
}
