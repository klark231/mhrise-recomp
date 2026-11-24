// Android auditrun fixture. AImageReader consumes real swapchain buffers off screen.
#include <android/hardware_buffer.h>
#include <media/NdkImageReader.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>
#include "nnsdk/host.h"
#include "nnsdk/nvn/commands.h"

extern "C" {
void nvnWindowBuilderSetDefaults(void*);
void nvnWindowBuilderSetNativeWindow(void*, void*);
void nvnWindowBuilderSetTextures(void*, int, void* const*);
void nvnWindowBuilderSetPresentInterval(void*, int);
int nvnWindowInitialize(void*, const void*);
void nvnWindowFinalize(void*);
int nvnMemoryPoolInitialize(void*, const void*);
void nvnMemoryPoolFinalize(void*);
void nvnTextureBuilderSetDefaults(nvn::TextureDesc*);
int nvnTextureInitialize(void*, const nvn::TextureDesc*);
void nvnTextureFinalize(void*);
int nvnCommandBufferInitialize(void*, void*);
void nvnCommandBufferBeginRecording(void*);
uint64_t nvnCommandBufferEndRecording(void*);
void nvnCommandBufferFinalize(void*);
void nvnCommandBufferSetRenderTargets(void*, int, void* const*, const void* const*, void*, const void*);
void nvnCommandBufferClearColor(void*, int, const float*, int);
void nvnCommandBufferCopyTextureToTexture(void*, const void*, const void*, const nvn::CopyRegion*, const void*, const void*, const nvn::CopyRegion*, int);
void nvnQueuePresentTexture(void*, void*, int);
}

namespace {
using Pixel = std::array<uint8_t, 4>;
using Clock = std::chrono::steady_clock;
using Frame = std::unique_ptr<AImage, decltype(&AImage_delete)>;
constexpr Pixel hud_back{12, 12, 14, 255};

bool exercise(NnsdkInput& input, int width, int height, NnsdkWindow* state = nullptr) {
    AImageReader* reader = nullptr;
    if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_RGBA_8888,
                                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 3, &reader) != AMEDIA_OK) return false;
    std::unique_ptr<AImageReader, decltype(&AImageReader_delete)> consumer(reader, AImageReader_delete);
    ANativeWindow* native = nullptr;
    if (AImageReader_getWindow(reader, &native) != AMEDIA_OK) return false;
    int key, texture_key;
    void* textures[]{&texture_key};
    alignas(void*) std::array<uint8_t, 128> builder{};
    nvnWindowBuilderSetDefaults(builder.data());
    nvnWindowBuilderSetNativeWindow(builder.data(), native);
    nvnWindowBuilderSetTextures(builder.data(), 1, textures);
    if (!nvnWindowInitialize(&key, builder.data())) return false;
    if (state) {
        state->window = native;
        ++state->serial;
    }

    auto& window = *nvn::g_windows.get(&key);
    nvn::g_textures.bind(&texture_key, {});
    auto& texture = *nvn::g_textures.get(&texture_key);
    texture.cleared = true;
    Frame held(nullptr, AImage_delete);
    unsigned wrong = 0, checks = 0;
    std::vector<double> times;
    for (int frame = 0; frame < 42; ++frame) {
        if (state && frame == 22) {
            // Background and resume with a different ANativeWindow but the same NVNwindow.
            held.reset();
            state->window = nullptr;
            ++state->serial;
            nvn::present(window, 0);
            if (window.swapchain || state->done != state->serial.load()) return false;
            consumer.reset();
            if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_RGBA_8888,
                                          AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 3, &reader) != AMEDIA_OK) return false;
            consumer.reset(reader);
            if (AImageReader_getWindow(reader, &native) != AMEDIA_OK) return false;
            state->window = native;
            ++state->serial;
        }
        if (frame == 5 || frame == 26) {
            const uint64_t y = uint64_t(height / 2) << 32;
            input.touch_start = y | (frame == 5 ? width - 8 : width / 3);
            input.touch_last = y | (frame == 5 ? width / 2 : width / 3);
            input.touch_state = frame == 5 ? 1 : 0;
            ++input.touch_serial;
        }
        const Pixel background{static_cast<uint8_t>(20 + frame), 90, 170, 255};
        for (int i = 0; i < 4; ++i) texture.clear_color[i] = background[i] / 255.0f;
        auto start = Clock::now();
        nvn::present(window, 0);
        AImage* raw = nullptr;
        media_status_t status;
        do {
            status = AImageReader_acquireNextImage(reader, &raw);
            if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE && Clock::now() - start < std::chrono::seconds(2));
        if (status != AMEDIA_OK || !raw) { printf("missing image frame=%d status=%d\n", frame, status); return false; }
        Frame delivered(raw, AImage_delete);
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        uint8_t* bytes = nullptr;
        int length = 0, stride = 0, step = 0;
        if (AImage_getPlaneData(raw, 0, &bytes, &length) != AMEDIA_OK ||
            AImage_getPlaneRowStride(raw, 0, &stride) != AMEDIA_OK ||
            AImage_getPlanePixelStride(raw, 0, &step) != AMEDIA_OK || step != 4) return false;
        auto expect = [&](int x, int y, Pixel p) {
            ++checks;
            const int at = y * stride + x * step;
            if (at + 4 > length || !std::equal(p.begin(), p.end(), bytes + at)) ++wrong;
        };
        // The entire central strip is outside both overlays, even while the panel slides.
        for (int y = 0; y < height; ++y)
            for (int x = width / 3; x < width * 2 / 3; ++x) expect(x, y, background);
        expect(8, 8, hud_back);
        if (frame < 5 || frame >= 40) expect(width-1, 3, background);
        if (frame >= 20 && frame < 26) expect(width-1, 3, hud_back);
        // Hold one buffer across several submissions so image and command-slot order can diverge.
        if (frame % 7 == 0) held = std::move(delivered);
        if (frame % 7 == 4) held.reset();
    }
    held.reset();
    nvnWindowFinalize(&key);
    nvn::g_textures.unbind(&texture_key);
    std::sort(times.begin(), times.end());
    printf("Android WSI %dx%d: frames=%zu checked=%u mismatches=%u paced_delivery_median_ms=%.3f\n",
           width, height, times.size(), checks, wrong, times[times.size()/2]);
    return wrong == 0;
}

bool composed_frames(bool generated, int height, const std::string& reference) {
    using namespace nvn;
    int pool, tex[2], cb, key;
    std::vector<uint8_t> storage(2*65536);
    struct { void* device; void* data; size_t size; int flags; } mem{nullptr, storage.data(), storage.size(), 0};
    if (!nvnMemoryPoolInitialize(&pool, &mem)) return false;
    TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.pool = &pool;
    d.width = 128;
    d.height = 72;
    d.format = 0x25;
    for (int i = 0; i < 2; ++i) {
        d.offset = i*65536;
        if (!nvnTextureInitialize(&tex[i], &d)) return false;
    }
    if (!nvnCommandBufferInitialize(&cb, nullptr)) return false;
    void* target = &tex[0];
    const float red[]{1, 0, 0, 1};
    const CopyRegion full{0,0,0,128,72,1};
    nvnCommandBufferBeginRecording(&cb);
    nvnCommandBufferSetRenderTargets(&cb, 1, &target, nullptr, nullptr, nullptr);
    nvnCommandBufferClearColor(&cb, 0, red, 15);
    nvnCommandBufferSetRenderTargets(&cb, 0, nullptr, nullptr, nullptr, nullptr);
    nvnCommandBufferCopyTextureToTexture(&cb, target, nullptr, &full, &tex[1], nullptr, &full, 0);
    auto* list = reinterpret_cast<CommandList*>(nvnCommandBufferEndRecording(&cb));
    execute({list});
    wait_idle();
    if (!g_textures.get(&tex[1])->image) return false;

    constexpr int width = 512, real_frames = 48;
    const int pad = (width - static_cast<int>(std::lround(height*16.0/9.0))) / 2;
    AImageReader* reader = nullptr;
    if (AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_RGBA_8888, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                  3, &reader) != AMEDIA_OK) return false;
    std::unique_ptr<AImageReader, decltype(&AImageReader_delete)> consumer(reader, AImageReader_delete);
    ANativeWindow* native = nullptr;
    if (AImageReader_getWindow(reader, &native) != AMEDIA_OK) return false;
    alignas(void*) std::array<uint8_t,128> builder{};
    void* textures[]{&tex[1]};
    nvnWindowBuilderSetDefaults(builder.data());
    nvnWindowBuilderSetNativeWindow(builder.data(), native);
    nvnWindowBuilderSetTextures(builder.data(), 1, textures);
    const bool extras = generated && getenv("WSI_EXTRA_FRAMES");
    nvnWindowBuilderSetPresentInterval(builder.data(), extras ? 4 : 2);
    if (!nvnWindowInitialize(&key, builder.data())) return false;
    std::atomic_uint delivered{0};
    unsigned wrong = 0, checked = 0;
    std::vector<uint8_t> golden;
    const std::string path = reference + std::to_string(height) + ".rgba";
    if (!reference.empty() && !getenv("WSI_CAPTURE_REFERENCE")) {
        std::ifstream file(path, std::ios::binary);
        golden.assign(std::istreambuf_iterator<char>(file), {});
        if (golden.size() != 16*width*4) return false;
    }
    std::atomic_bool done{false};
    std::thread read([&] {
        auto last = Clock::now();
        for (;;) {
            AImage* raw = nullptr;
            const auto rc = AImageReader_acquireNextImage(reader, &raw);
            if (rc == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
                if (done.load() && Clock::now()-last > std::chrono::milliseconds(150)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (rc != AMEDIA_OK) { ++wrong; break; }
            Frame frame(raw, AImage_delete);
            last = Clock::now();
            ++delivered;
            uint8_t* bytes;
            int length, stride;
            if (AImage_getPlaneData(raw, 0, &bytes, &length) != AMEDIA_OK ||
                AImage_getPlaneRowStride(raw, 0, &stride) != AMEDIA_OK) { ++wrong; continue; }
            // Capture the bottom strip once on the signed baseline. FSR has an approximate
            // reciprocal; compare its filtered pixels byte-for-byte at each precision.
            if (length < (height-1)*stride + width*4) { ++wrong; continue; }
            if (!reference.empty() && golden.empty()) {
                for (int y = height-16; y < height; ++y)
                    golden.insert(golden.end(), bytes+y*stride, bytes+y*stride+width*4);
                std::ofstream file(path, std::ios::binary);
                if (!file.write(reinterpret_cast<const char*>(golden.data()), golden.size())) ++wrong;
            }
            for (int y = height-16; y < height; ++y) for (int x = 0; x < width; ++x) {
                const int at = y*stride + 4*x;
                const bool inside = x >= pad && x < width-pad;
                const auto i = ((y-(height-16))*width + x)*4;
                const uint8_t r = inside ? 255 : 0;
                ++checked;
                bool correct = bytes[at] == r && !bytes[at+1] && !bytes[at+2];
                if (!golden.empty()) correct = std::equal(bytes+at, bytes+at+4, golden.data()+i) &&
                    (!inside ? bytes[at] == 0 : bytes[at] >= 254) && !bytes[at+1] && !bytes[at+2];
                if (!correct) {
                    if (!wrong) {
                        Pixel expected{r,0,0,static_cast<uint8_t>(inside ? 255 : 0)};
                        if (!golden.empty()) std::copy_n(golden.data()+i, 4, expected.begin());
                        printf("first pixel mismatch at %d,%d: RGBA %u,%u,%u,%u expected %u,%u,%u,%u\n",
                               x, y, bytes[at], bytes[at+1], bytes[at+2], bytes[at+3],
                               expected[0], expected[1], expected[2], expected[3]);
                    }
                    ++wrong;
                }
            }
        }
    });
    for (int i = 0; i < real_frames; ++i) {
        nvnQueuePresentTexture(nullptr, &key, 0);
        if (!generated) {
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (delivered.load() < unsigned(i+1) && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (delivered.load() < unsigned(i+1)) break;
        }
    }
    // Finalize also drains the optional async presenter before the window memory is recycled.
    wait_idle();
    nvnWindowFinalize(&key);
    done = true;
    read.join();
    nvnCommandBufferFinalize(&cb);
    for (int& texture : tex) nvnTextureFinalize(&texture);
    nvnMemoryPoolFinalize(&pool);
    printf("NVN attachment -> copy -> Android Surface: submitted=%d delivered=%u checked=%u mismatches=%u\n",
           real_frames, delivered.load(), checked, wrong);
    return wrong == 0 && (generated ? delivered > real_frames*(extras ? 2u : 1u) : delivered == real_frames);
}
}

extern "C" int bench() {
    NnsdkInput input;
    NnsdkWindow state;
    NnsdkHost host{};
    host.input = &input;
    if (getenv("WSI_NATIVE_REPLACE")) host.window_state = &state;
    nnsdk_attach(&host);
    if (!nvn::vk()) return 1;
    if (const char* mode = getenv("WSI_COMPOSE")) {
        const bool generated = strcmp(mode, "generated") == 0;
        const char* ref = getenv("WSI_FSR_REFERENCE");
        const int height = getenv("WSI_TALL") ? 252 : 216;
        bool ok = composed_frames(generated, height, ref ? ref : "");
        if (ok && getenv("WSI_RESIZE")) ok = composed_frames(generated, 252, ref ? ref : "");
        return ok ? 0 : 3;
    }
    const bool first = exercise(input, 1152, 384, host.window_state);
    const bool second = exercise(input, 1200, 432, host.window_state);
    return first && second ? 0 : 2;
}
