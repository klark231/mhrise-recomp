// Offscreen Android regression. Link against the NVN runtime and call bench() from auditrun.
#include "nnsdk/nvn/commands.h"
#include "nnsdk/nvn/driver.h"
#include "nnsdk/nvn/objects.h"
#include <array>
#include <cstdio>
#include <vector>

using namespace nvn;
extern "C" {
int nvnMemoryPoolInitialize(void*, const void*);
void nvnTextureBuilderSetDefaults(TextureDesc*);
int nvnTextureInitialize(void*, const TextureDesc*);
void nvnTextureFinalize(void*);
int nvnTextureGetDebugID(const void*);
void nvnTextureInvalidateTexels(const void*, const TextureViewDesc*, const CopyRegion*);
void nvnTextureFlushTexels(const void*, const TextureViewDesc*, const CopyRegion*);
void nvnTextureReadTexels(const void*, const TextureViewDesc*, const CopyRegion*, void*);
void nvnTextureReadTexelsStrided(const void*, const TextureViewDesc*, const CopyRegion*, void*, int64_t, int64_t);
void nvnTextureWriteTexels(const void*, const TextureViewDesc*, const CopyRegion*, const void*);
int nvnCommandBufferInitialize(void*, void*);
void nvnCommandBufferBeginRecording(void*);
uint64_t nvnCommandBufferEndRecording(void*);
void nvnCommandBufferSetRenderTargets(void*, int, void* const*, const void* const*, void*, const void*);
void nvnCommandBufferClearColor(void*, int, const float*, int);
void nvnCommandBufferSetScissor(void*, int, int, int, int);
void nvnCommandBufferBarrier(void*, int);
void nvnCommandBufferCopyTextureToTexture(void*, const void*, const void*, const CopyRegion*, const void*, const void*, const CopyRegion*, int);
}

namespace {
using Pixel = std::array<uint8_t, 4>;
constexpr Pixel red{255, 0, 0, 255}, gray{17, 17, 17, 17}, green{0, 255, 0, 255};
int cmd_key, errors;
void submit() {
    auto* list = reinterpret_cast<CommandList*>(nvnCommandBufferEndRecording(&cmd_key));
    execute({list});
    wait_idle();
}
void check(const char* name, Pixel got, Pixel expected) {
    printf("%s = %u,%u,%u,%u %s\n", name, got[0], got[1], got[2], got[3], got == expected ? "PASS" : "FAIL");
    errors += got != expected;
}
Pixel read(void* tex, CopyRegion r) {
    Pixel p{};
    nvnTextureReadTexels(tex, nullptr, &r, p.data());
    return p;
}
}

extern "C" int bench() {
    auto* ctx = vk();
    if (!ctx || !driver::custom()) return 2;
    printf("GPU %s driver 0x%x\n", ctx->props.deviceName, ctx->props.driverVersion);
    static std::vector<uint8_t> bytes(8 * 65536, 17);
    static int pool, keys[9];
    struct { void *device, *storage; size_t size; int flags; } builder{nullptr, bytes.data(), bytes.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &builder) || !nvnCommandBufferInitialize(&cmd_key, nullptr)) return 3;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.width = d.height = 64;
    d.pool = &pool;
    d.format = 0x25;
    for (int i = 0; i < 9; ++i) {
        d.offset = std::max(i - 1, 0) * 65536;
        if (!nvnTextureInitialize(&keys[i], &d)) return 4;
    }
    errors += nvnTextureGetDebugID(&keys[0]) == nvnTextureGetDebugID(&keys[1]);
    CopyRegion full{0, 0, 0, 64, 64, 1}, pixel{0, 0, 0, 1, 1, 1};
    void* a = &keys[0];
    nvnCommandBufferBeginRecording(&cmd_key);
    nvnCommandBufferSetRenderTargets(&cmd_key, 1, &a, nullptr, nullptr, nullptr);
    const float color[]{1, 0, 0, 1};
    nvnCommandBufferClearColor(&cmd_key, 0, color, 15);
    nvnCommandBufferSetRenderTargets(&cmd_key, 0, nullptr, nullptr, nullptr, nullptr);
    nvnCommandBufferBarrier(&cmd_key, 0xff);
    submit();
    // Snapshot A, B and a disjoint gray source on the GPU before *any* CPU invalidation/read.
    nvnCommandBufferBeginRecording(&cmd_key);
    for (auto [src, dst] : {std::pair{0,2}, {1,3}, {4,5}}) {
        nvnCommandBufferCopyTextureToTexture(&cmd_key, &keys[src], nullptr, &full, &keys[dst], nullptr, &full, 0);
        nvnCommandBufferBarrier(&cmd_key, 0xff);
    }
    submit();
    // Use the Vulkan images actually bound by the runtime. Independent of NVN's CPU read implementation.
    nvnCommandBufferBeginRecording(&cmd_key);
    void* handles[6];
    for (int i = 0; i < 6; ++i) handles[i] = &keys[i];
    nvnCommandBufferSetRenderTargets(&cmd_key, 6, handles, nullptr, nullptr, nullptr);
    auto* list = reinterpret_cast<CommandList*>(nvnCommandBufferEndRecording(&cmd_key));
    const auto& rt = std::get<cmd::SetRenderTargets>(list->commands.front());
    VolkDeviceTable v{};
    volkLoadDeviceTable(&v, ctx->device);
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = 96;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (v.vkCreateBuffer(ctx->device, &bc, nullptr, &buf)) return 5;
    VkMemoryRequirements req;
    v.vkGetBufferMemoryRequirements(ctx->device, buf, &req);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    const auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (; ma.memoryTypeIndex < ctx->memory.memoryTypeCount; ++ma.memoryTypeIndex)
        if ((req.memoryTypeBits & (1u << ma.memoryTypeIndex)) &&
            (ctx->memory.memoryTypes[ma.memoryTypeIndex].propertyFlags & want) == want) break;
    VkDeviceMemory mem;
    if (ma.memoryTypeIndex == ctx->memory.memoryTypeCount || v.vkAllocateMemory(ctx->device, &ma, nullptr, &mem) ||
        v.vkBindBufferMemory(ctx->device, buf, mem, 0)) return 6;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = ctx->queue_family;
    VkCommandPool cp;
    if (v.vkCreateCommandPool(ctx->device, &pc, nullptr, &cp)) return 7;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = cp;
    ca.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (v.vkAllocateCommandBuffers(ctx->device, &ca, &cb)) return 8;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (v.vkBeginCommandBuffer(cb, &bi)) return 9;
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    v.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    for (int i = 0; i < 6; ++i) {
        if (!rt.colors[i] || !rt.colors[i]->image) return 10;
        VkBufferImageCopy r{};
        r.bufferOffset = i * 16;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {1,1,1};
        v.vkCmdCopyImageToBuffer(cb, rt.colors[i]->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    v.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (v.vkEndCommandBuffer(cb) || v.vkQueueSubmit(ctx->queue, 1, &si, VK_NULL_HANDLE) || v.vkQueueWaitIdle(ctx->queue)) return 11;
    void* mapped;
    if (v.vkMapMemory(ctx->device, mem, 0, VK_WHOLE_SIZE, 0, &mapped)) return 12;
    const char* labels[]{"GPU A","GPU B","GPU A->C","GPU B->D","GPU E","GPU E->F"};
    for (int i = 0; i < 6; ++i) {
        Pixel p;
        memcpy(p.data(), static_cast<uint8_t*>(mapped) + i * 16, 4);
        check(labels[i], p, i < 4 ? red : gray);
    }
    v.vkUnmapMemory(ctx->device, mem);
    v.vkDestroyCommandPool(ctx->device, cp, nullptr);
    v.vkDestroyBuffer(ctx->device, buf, nullptr);
    v.vkFreeMemory(ctx->device, mem, nullptr);
    nvnTextureInvalidateTexels(&keys[1], nullptr, &pixel);
    Pixel pool_pixel;
    memcpy(pool_pixel.data(), bytes.data(), 4);
    check("mapped pool after invalidate B", pool_pixel, red);
    check("ReadTexels A", read(a, pixel), red);
    check("ReadTexels B", read(&keys[1], pixel), red);
    check("ReadTexels D", read(&keys[3], pixel), red);
    check("ReadTexels E", read(&keys[4], pixel), gray);
    CopyRegion offset{7,11,0,1,1,1};
    nvnTextureWriteTexels(&keys[1], nullptr, &offset, green.data());
    CopyRegion column{7,10,0,1,2,1};
    std::array<Pixel,3> rows{gray, gray, gray};
    nvnTextureReadTexelsStrided(a, nullptr, &column, rows.data(), 8, 16);
    check("strided neighbor", rows[0], red);
    check("stride padding", rows[1], gray);
    check("CPU B->A", rows[2], green);
    nvnTextureFinalize(a);
    check("B after finalize A", read(&keys[1], pixel), red);
    check("B retained CPU patch", read(&keys[1], offset), green);
    // G has never been bound. A first, partial GPU copy must retain the other guest texels.
    nvnCommandBufferBeginRecording(&cmd_key);
    nvnCommandBufferCopyTextureToTexture(&cmd_key, &keys[1], nullptr, &pixel, &keys[6], nullptr, &offset, 0);
    nvnCommandBufferBarrier(&cmd_key, 0xff);
    submit();
    check("partial GPU destination", read(&keys[6], offset), red);
    check("partial GPU untouched", read(&keys[6], pixel), gray);
    // Register a new alias *after* the clear. Identity copies across equal handles are no-ops.
    d.offset = 0;
    if (!nvnTextureInitialize(a, &d)) return 13;
    nvnCommandBufferBeginRecording(&cmd_key);
    nvnCommandBufferCopyTextureToTexture(&cmd_key, a, nullptr, &pixel, &keys[1], nullptr, &pixel, 0);
    nvnCommandBufferBarrier(&cmd_key, 0xff);
    submit();
    check("late alias", read(a, offset), green);
    memcpy(bytes.data(), green.data(), 4);
    nvnTextureFlushTexels(a, nullptr, &pixel);
    nvnCommandBufferBeginRecording(&cmd_key);
    nvnCommandBufferCopyTextureToTexture(&cmd_key, &keys[1], nullptr, &pixel, &keys[3], nullptr, &pixel, 0);
    nvnCommandBufferBarrier(&cmd_key, 0xff);
    submit();
    check("mapped flush A -> GPU B -> D", read(&keys[3], pixel), green);
    CopyRegion first_pair{0,0,0,2,1,1}, shifted{1,0,0,2,1,1};
    nvnCommandBufferBeginRecording(&cmd_key);
    nvnCommandBufferCopyTextureToTexture(&cmd_key, &keys[1], nullptr, &first_pair, a, nullptr, &shifted, 0);
    nvnCommandBufferBarrier(&cmd_key, 0xff);
    submit();
    check("overlap first", read(a, {1,0,0,1,1,1}), green);
    check("overlap second", read(&keys[1], {2,0,0,1,1,1}), red);
    d.offset = 3 * 65536; // E, disjoint from B
    if (!nvnTextureInitialize(a, &d)) return 14;
    check("rebound A", read(a, pixel), gray);
    check("B after rebind", read(&keys[1], pixel), green);
    // First use of two CPU-backed colors. Clear one texel, then copy each attachment
    // before any CPU read can influence the result.
    void* colors[] = {&keys[7], &keys[8]};
    nvnCommandBufferBeginRecording(&cmd_key);
    nvnCommandBufferSetRenderTargets(&cmd_key, 2, colors, nullptr, nullptr, nullptr);
    nvnCommandBufferSetScissor(&cmd_key, offset.x, offset.y, 1, 1);
    nvnCommandBufferClearColor(&cmd_key, 0, color, 15);
    nvnCommandBufferSetRenderTargets(&cmd_key, 0, nullptr, nullptr, nullptr, nullptr);
    nvnCommandBufferCopyTextureToTexture(&cmd_key, colors[0], nullptr, &full, &keys[2], nullptr, &full, 0);
    nvnCommandBufferCopyTextureToTexture(&cmd_key, colors[1], nullptr, &full, &keys[3], nullptr, &full, 0);
    submit();
    check("scissored attachment", read(&keys[2], offset), red);
    check("untouched texel in written attachment", read(&keys[2], pixel), gray);
    check("unwritten sibling attachment", read(&keys[3], offset), gray);
    printf("failed pixel/identity checks: %d\n", errors);
    return errors ? 1 : 0;
}
