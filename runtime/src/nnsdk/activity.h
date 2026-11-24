#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace nnsdk {

// Only guest entry points park. The Android looper, Vulkan submission worker and
// driver's threads must remain available to finish work and return the surface.
class ActivityState {
public:
    using Clock = std::chrono::steady_clock;

    bool update(bool resumed, bool releasing_surface) {
        std::lock_guard lock(mutex_);
        const bool changed = resumed != foreground_;
        if (changed) {
            const auto now = Clock::now();
            if (resumed) suspended_ += now - paused_at_;
            else paused_at_ = now;
            foreground_ = resumed;
        }
        const bool park = !resumed && !releasing_surface;
        if (parked_.exchange(park) && !park) wake_.notify_all();
        return changed;
    }

    void checkpoint() {
        if (!parked_.load(std::memory_order_acquire)) return;
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return !parked_.load(); });
    }

    Clock::duration active_time() {
        std::lock_guard lock(mutex_);
        return (foreground_ ? Clock::now() : paused_at_) - start_ - suspended_;
    }

    bool foreground() const { return foreground_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> parked_{false}, foreground_{true};
    const Clock::time_point start_ = Clock::now();
    Clock::time_point paused_at_{};
    Clock::duration suspended_{};
};

ActivityState& activity_state();
void update_audio_activity(bool resumed);

}  // namespace nnsdk
