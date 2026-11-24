#include "../../runtime/src/nnsdk/activity.h"

#include <cassert>
#include <future>
#include <thread>

using namespace std::chrono_literals;

int main() {
    nnsdk::ActivityState state;
    assert(state.foreground());
    assert(state.update(false, false));
    const auto frozen = state.active_time();

    auto parked_frame = [&] {
        std::promise<void> entered;
        auto started = entered.get_future();
        auto result = std::async(std::launch::async, [&state, entered = std::move(entered)]() mutable {
            entered.set_value();
            state.checkpoint();
        });
        started.get();
        assert(result.wait_for(20ms) == std::future_status::timeout);
        return result;
    };

    auto frame = parked_frame();
    assert(!state.update(false, true)); // release the surface while audio and active time stay paused
    assert(frame.wait_for(2s) == std::future_status::ready);
    frame.get();
    assert(!state.foreground());
    assert(state.active_time() == frozen);
    assert(!state.update(false, false));

    auto main_thread = parked_frame();
    auto worker = parked_frame();
    assert(!state.update(false, false));
    assert(state.active_time() == frozen);
    assert(state.update(true, false));
    assert(main_thread.wait_for(2s) == std::future_status::ready);
    assert(worker.wait_for(2s) == std::future_status::ready);
    assert(state.foreground());
    std::this_thread::sleep_for(5ms);
    assert(state.active_time() > frozen);

    for (int i = 0; i < 100; ++i) {
        state.update(false, false);
        auto pending = std::async(std::launch::async, [&] { state.checkpoint(); });
        state.update(true, false);
        assert(pending.wait_for(2s) == std::future_status::ready);
    }
}
