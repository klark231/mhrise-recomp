// Fixed RGB and three guest alpha values, presented by the shipping NVN backend.
#include "nnsdk/nvn/commands.h"
#include "nnsdk/nvn/driver.h"
#include <android/native_window_jni.h>
#include <array>
#include <chrono>
#include <jni.h>
#include "scene_shaders.h"
extern "C" {
int nvnMemoryPoolInitialize(void*, const void*);
void nvnTextureBuilderSetDefaults(nvn::TextureDesc*);
int nvnTextureInitialize(void*, const nvn::TextureDesc*);
void nvnTextureWriteTexels(const void*, const nvn::TextureViewDesc*, const nvn::CopyRegion*, const void*);
void nvnTextureReadTexels(const void*, const nvn::TextureViewDesc*, const nvn::CopyRegion*, void*);
void nvnWindowBuilderSetDefaults(void*);
void nvnWindowBuilderSetNativeWindow(void*, void*);
void nvnWindowBuilderSetTextures(void*, int, void* const*);
int nvnWindowInitialize(void*, const void*);
void nvnQueuePresentTexture(void*, void*, int);
void nvnWindowFinalize(void*);
void nvnTextureFinalize(void*);
void nvnMemoryPoolFinalize(void*);
}
namespace {
int pool, tex, output, window, depth, color;
ANativeWindow* surface;
std::vector<uint8_t> memory;
nvn::Program prepass, paint;
nvn::DescriptorPool images, samplers;
nvn::Sampler nearest{};
nvn::CommandList scene;
nvn::CommandList lightScene;
bool useLightScene = false;
alignas(16) std::array<uint64_t, 4> reports{};
uint64_t gpuSerial = 0;
constexpr int W = 1280, H = 720;
using Pixel = std::array<uint8_t, 4>;
bool covered(int x, int y, bool contrast) {
    if (!contrast)
        return (x / 16) % 2 != 0;
    return ((x + y) % 13 < 2 || (x - y + 144) % 9 == 0);
}

template <size_t N>
bool stage(nvn::Program& program, bool vertex, const uint32_t (&spv)[N], bool exportColor = false) {
    auto& s = program.stages.emplace_back();
    s.shader.stage = vertex ? nvnshader::Stage::Vertex : nvnshader::Stage::Fragment;
    s.shader.spirv.assign(spv, spv + N);
    s.shader.color_outputs = exportColor;
    if (!vertex)
        s.shader.resources.push_back({nvnshader::Resource::Texture, 0, 2, 0x168, 2});
    s.spirv_id = nvn::spirv_identity(s.shader);
    s.tried = true;
    return nvn::create_shader_module(nvn::vk()->device, s.shader.spirv, &s.module) == VK_SUCCESS;
}

bool record_scene(int passes, bool contrast) {
    using namespace nvn;
    if (!stage(prepass, true, scene_vert) || !stage(prepass, false, scene_depth) ||
        !stage(paint, true, scene_vert) || !stage(paint, false, scene_color, true))
        return false;
    images.textures.resize(2);
    images.register_texture(1, {g_textures.get(&tex), {}});
    nearest.desc.max_anisotropy = 1;
    samplers.samplers.push_back(&nearest);
    cmd::SetRenderTargets z{}, rt{};
    z.depth = rt.depth = g_textures.get(&depth);
    rt.count = 1;
    rt.colors[0] = g_textures.get(&color);
    DepthStencilState write{}, equal{};
    write.depth_test = write.depth_write = 1;
    // The full-screen vertex shader writes depth zero, so clear to one and use LESS.
    write.depth_func = 2;
    equal.depth_test = 1;
    equal.depth_func = 3;
    const cmd::Draw tri{4, -1, 3, 0, 0, 0, 0, 1};
    cmd::CopyTexture copy{};
    copy.src = rt.colors[0];
    copy.dst = g_textures.get(&output);
    copy.src_region = copy.dst_region = {0, 0, 0, W, H, 1};
    const cmd::ClearColor sky = contrast ? cmd::ClearColor{0, {0, 0, 0, 1}, 15}
                                         : cmd::ClearColor{0, {32 / 255.f, 64 / 255.f, 128 / 255.f, 1}, 15};
    const std::vector<Command> draws{z,
                                     cmd::Viewport{0, 0, 0, W, H},
                                     cmd::Scissor{0, 0, 0, W, H},
                                     cmd::DepthRange{0, 1},
                                     cmd::ClearDepthStencil{1, true, 0, 255},
                                     write,
                                     cmd::BindProgram{&prepass, 3},
                                     cmd::SetPools{&images, &samplers},
                                     cmd::BindTexture{1, 0, 1, true},
                                     PolygonState{0, 1, 2, 0},
                                     ColorState{0, 0, 8, 0},
                                     ChannelMaskState{15},
                                     tri,
                                     rt,
                                     sky,
                                     equal,
                                     cmd::BindProgram{&paint, 3},
                                     ColorState{1, 0, 8, 0},
                                     BlendState{0, 5, 6, 1, 2, 1, 1, 0},
                                     tri,
                                     cmd::SetRenderTargets{},
                                     copy};
    scene.commands.emplace_back(cmd::ReportCounter{0, reinterpret_cast<uintptr_t>(reports.data())});
    lightScene.commands = scene.commands;
    lightScene.commands.insert(lightScene.commands.end(), draws.begin(), draws.end());
    lightScene.commands.emplace_back(cmd::ReportCounter{0, reinterpret_cast<uintptr_t>(reports.data() + 2)});
    for (int i = 0; i < passes; ++i)
        scene.commands.insert(scene.commands.end(), draws.begin(), draws.end());
    scene.commands.emplace_back(cmd::ReportCounter{0, reinterpret_cast<uintptr_t>(reports.data() + 2)});
    execute({&scene});
    wait_idle();
    std::vector<Pixel> pixels(W * H);
    nvnTextureReadTexels(&output, nullptr, &copy.dst_region, pixels.data());
    unsigned wrong = 0;
    for (int i = 0; i < W * H; ++i) {
        const bool visible = covered(i % W % 128, i / W % 72, contrast);
        const Pixel want = contrast ? (visible ? Pixel{127, 127, 127, 255} : Pixel{0, 0, 0, 255})
                                    : (visible ? Pixel{48, 112, 176, 255} : Pixel{32, 64, 128, 255});
        for (int c = 0; c < 4; ++c)
            if (std::abs(int(pixels[i][c]) - int(want[c])) > 1) {
                ++wrong;
                break;
            }
    }
    printf("prepass -> EQUAL/blend -> copy: %u/%d incorrect (%u,%u,%u,%u at x=16)\n", wrong, W * H,
           pixels[16][0], pixels[16][1], pixels[16][2], pixels[16][3]);
    return !wrong;
}
} // namespace
extern "C" JNIEXPORT jint JNICALL Java_dev_rise_audit_panel_Probe_start(JNIEnv* env, jclass, jobject holder,
                                                                        jstring folder, jstring mode,
                                                                        jboolean textured, jboolean contrast,
                                                                        jint passes) {
    const char* dir = env->GetStringUTFChars(folder, nullptr);
    freopen((std::string(dir) + "/probe-nvn.log").c_str(), "w", stdout);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setenv("NIER_DATA", dir, 1);
    setenv("NIER_INTERNO", dir, 1);
    env->ReleaseStringUTFChars(folder, dir);
    const char* tu = env->GetStringUTFChars(mode, nullptr);
    setenv("TU_DEBUG", tu, 1);
    env->ReleaseStringUTFChars(mode, tu);
    if (!nvn::vk() || !nvn::driver::custom())
        return 1;
    surface = ANativeWindow_fromSurface(env, holder);
    constexpr int span = 4 << 20;
    memory.resize(textured ? 4 * span : 131072);
    struct {
        void* device;
        void* storage;
        size_t size;
        int flags;
    } builder{nullptr, memory.data(), memory.size(), 0};
    if (!surface || !nvnMemoryPoolInitialize(&pool, &builder))
        return 2;
    nvn::TextureDesc d;
    nvnTextureBuilderSetDefaults(&d);
    d.pool = &pool;
    d.width = 128;
    d.height = 72;
    d.format = 0x25;
    if (!nvnTextureInitialize(&tex, &d))
        return 3;
    std::array<std::array<uint8_t, 4>, 128 * 72> pixels;
    for (unsigned i = 0; i < pixels.size(); ++i) {
        pixels[i] = contrast ? Pixel{254, 254, 254, 255} : Pixel{64, 160, 224, 255};
        pixels[i][3] = textured ? uint8_t(covered(i % 128, i / 128, contrast) ? 255 : 0)
                                : uint8_t((i / 128 < 24)   ? 0
                                          : (i / 128 < 48) ? 128
                                                           : 255);
    }
    nvn::CopyRegion area{0, 0, 0, 128, 72, 1};
    nvnTextureWriteTexels(&tex, nullptr, &area, pixels.data());
    // Copy through a second guest texture to establish the real display image before presentation.
    if (textured) {
        d.width = W;
        d.height = H;
    }
    d.offset = textured ? span : 40960;
    if (!nvnTextureInitialize(&output, &d))
        return 4;
    nvn::cmd::CopyTexture copy{};
    copy.src = nvn::g_textures.get(&tex);
    copy.dst = nvn::g_textures.get(&output);
    copy.src_region = copy.dst_region = area;
    nvn::CommandList list;
    list.commands = {copy};
    if (textured) {
        d.offset = 2 * span;
        if (!nvnTextureInitialize(&color, &d))
            return 6;
        d.offset = 3 * span;
        d.format = 0x35;
        if (!nvnTextureInitialize(&depth, &d) || !record_scene(passes, contrast))
            return 7;
    } else {
        nvn::execute({&list});
        nvn::wait_idle();
    }
    alignas(void*) std::array<uint8_t, 128> wb{};
    void* textures[]{&output};
    nvnWindowBuilderSetDefaults(wb.data());
    nvnWindowBuilderSetNativeWindow(wb.data(), surface);
    nvnWindowBuilderSetTextures(wb.data(), 1, textures);
    return nvnWindowInitialize(&window, wb.data()) ? 0 : 5;
}
extern "C" JNIEXPORT void JNICALL Java_dev_rise_audit_panel_Probe_frame(JNIEnv*, jclass) {
    if (!scene.commands.empty())
        nvn::execute({useLightScene ? &lightScene : &scene});
    nvnQueuePresentTexture(nullptr, &window, 0);
    int64_t ns;
    if (nvn::game_gpu_frame(gpuSerial, ns)) {
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
        printf("scene_gpu time_ns=%lld duration_ns=%lld\n", (long long)now, (long long)ns);
    }
}
extern "C" JNIEXPORT void JNICALL Java_dev_rise_audit_panel_Probe_light(JNIEnv*, jclass, jboolean enable) {
    useLightScene = enable;
}
extern "C" JNIEXPORT void JNICALL Java_dev_rise_audit_panel_Probe_stop(JNIEnv*, jclass) {
    nvn::wait_idle();
    nvnWindowFinalize(&window);
    if (!scene.commands.empty()) {
        nvnTextureFinalize(&depth);
        nvnTextureFinalize(&color);
    }
    nvnTextureFinalize(&output);
    nvnTextureFinalize(&tex);
    nvnMemoryPoolFinalize(&pool);
    ANativeWindow_release(surface);
}
