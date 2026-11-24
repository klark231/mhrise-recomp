// Android offscreen fixture, linked against the packaged backend and invoked by auditrun.
#include "nnsdk/nvn/commands.h"
#include <chrono>
#include <future>

namespace {
struct ShaderData { uint64_t code; const void* control; };
extern "C" int nvnProgramInitialize(void*, void*);
extern "C" int nvnProgramSetShaders(void*, int, const ShaderData*);
extern "C" void nvnProgramFinalize(void*);
}

extern "C" int bench() {
    using namespace std::chrono_literals;
    int key = 0, first = 1, second = 2;
    ShaderData stages[]{{0, &first}, {0, &second}};
    if (!nvnProgramInitialize(&key, nullptr)) return 1;
    std::unique_lock replay(nvn::executor_mutex());
    auto init = std::async(std::launch::async, [&] { return nvnProgramSetShaders(&key, 2, stages); });
    if (init.wait_for(2s) != std::future_status::ready) {
        replay.unlock();
        init.wait();
        return 2;
    }
    if (!init.get()) return 3;
    auto* program = nvn::find_program(&key);
    if (!program || program->stages.size() != 2 || program->stages[1].control != &second) return 4;
    const auto generation = program->generation;
    std::promise<void> started;
    auto update = std::async(std::launch::async, [&] {
        started.set_value();
        return nvnProgramSetShaders(&key, 1, stages + 1);
    });
    started.get_future().wait();
    const bool serialized = update.wait_for(50ms) == std::future_status::timeout;
    replay.unlock();
    if (!update.get() || !serialized || program->generation == generation ||
        program->stages.size() != 1 || program->stages.front().control != &second) return 5;
    nvnProgramFinalize(&key);
    return nvn::find_program(&key) ? 6 : 0;
}
