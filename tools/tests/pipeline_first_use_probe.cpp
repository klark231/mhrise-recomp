#include "nnsdk/nvn/vk.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <thread>

// Device-only regression probe. Build in place of the executor TU. Calls go to the actual Vulkan driver.
namespace {
std::mutex gate_mutex;
std::condition_variable gate_cv;
bool entered = false, release_build = false;
std::atomic<unsigned> creates{}, successes{}, oom_results{}, refused_allocations{}, diagnostics{};
void* VKAPI_PTR refuse(void*, size_t, size_t, VkSystemAllocationScope) {
    ++refused_allocations;
    return nullptr;
}
void* VKAPI_PTR refuse_resize(void*, void*, size_t, size_t, VkSystemAllocationScope) {
    ++refused_allocations;
    return nullptr;
}
void VKAPI_PTR release(void*, void*) {}
VkResult observe_create(VkDevice device, VkPipelineCache cache, uint32_t count,
                        const VkGraphicsPipelineCreateInfo* info, const VkAllocationCallbacks* allocator,
                        VkPipeline* out) {
    const auto* blend = count == 1 ? info->pColorBlendState : nullptr;
    if (!blend || blend->attachmentCount != 2 || blend->pAttachments[0].colorWriteMask != 15 ||
        blend->pAttachments[1].colorWriteMask != 15)
        return vkCreateGraphicsPipelines(device, cache, count, info, allocator, out);
    // Production's post-failure diagnostic variants are discarded and use no driver cache.
    if (!cache) {
        ++diagnostics;
        return vkCreateGraphicsPipelines(device, cache, count, info, allocator, out);
    }
    unsigned attempt = ++creates;
    if (getenv("JOIN_WARM") && attempt == 1) {
        std::unique_lock lock(gate_mutex);
        entered = true;
        gate_cv.notify_all();
        if (!gate_cv.wait_for(lock, std::chrono::seconds(8), [] { return release_build; })) {
            fprintf(stderr, "fixture: warm join deadline exceeded\n");
            abort();
        }
    }
    VkAllocationCallbacks refuse_memory{nullptr, refuse, refuse_resize, release, nullptr, nullptr};
    bool fail = getenv("PERMANENT_ERROR") || (getenv("RECOVER_ERROR") && attempt == 1);
    auto start = std::chrono::steady_clock::now();
    auto status = vkCreateGraphicsPipelines(device, cache, count, info, fail ? &refuse_memory : allocator, out);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    char thread[16]{};
    pthread_getname_np(pthread_self(), thread, sizeof thread);
    printf("fixture Vulkan graphics call=%u thread=%s status=%d handle=%p driver_us=%lld\n", attempt, thread,
           int(status), (void*)*out, (long long)us);
    if (status == VK_SUCCESS)
        ++successes;
    if (status == VK_ERROR_OUT_OF_HOST_MEMORY)
        ++oom_results;
    return status;
}
} // namespace
#define vkCreateGraphicsPipelines observe_create
#include "../../runtime/src/nnsdk/nvn/executor.cpp"
#undef vkCreateGraphicsPipelines

extern "C" int fixture_arm_warm() {
    std::unique_lock lock(gate_mutex);
    if (!gate_cv.wait_for(lock, std::chrono::seconds(5), [] { return entered; }))
        return 0;
    uint64_t key;
    {
        std::lock_guard registry(nvn::g_executor.warm_mutex_);
        if (nvn::g_executor.warm_builds_.size() != 1)
            return 0;
        key = nvn::g_executor.warm_builds_.begin()->first;
    }
    std::thread([key] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            {
                std::lock_guard registry(nvn::g_executor.warm_mutex_);
                if (nvn::g_executor.warm_done_.count(key))
                    break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                abort();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard lock(gate_mutex);
        release_build = true;
        gate_cv.notify_all();
    }).detach();
    return 1;
}
extern "C" int fixture_report() {
    using namespace nvn;
    printf("fixture creates=%u successes=%u real_oom=%u refused_allocations=%u diagnostics=%u warm_joins=%llu\n",
           creates.load(), successes.load(), oom_results.load(), refused_allocations.load(), diagnostics.load(),
           (unsigned long long)g_executor.prof_warm_waits_);
    if (getenv("JOIN_WARM") && g_executor.prof_warm_waits_ == 0)
        return 0;
    if (getenv("PERMANENT_ERROR"))
        return creates == 1 && successes == 0 && oom_results == 1 && refused_allocations > 0;
    if (getenv("RECOVER_ERROR"))
        return creates == 2 && successes == 1 && oom_results == 1 && refused_allocations > 0;
    return creates == 1 && successes == 1;
}
