#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "magic_portal/engine.h"

namespace magic_portal {

// A camera frame the host wants segmented. Buffers are recycled between captures, so a
// source should resize `rgba` (a no-op once the size is stable) and overwrite it in place.
struct CapturedFrame {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
    bool mirror_horizontally = false;
    float crop_left = 0.0F;
    float crop_top = 0.0F;
    float crop_right = 1.0F;
    float crop_bottom = 1.0F;
    // Set by the runner after capture(); a source does not need to fill them in.
    std::int64_t timestamp_ms = 0;
    std::uint64_t session_id = kAnySession;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;
    // Called on the thread that calls PortalRunner::tick() (the render thread). Return false
    // when no usable frame is available right now. `stride_bytes` may be 0 (tightly packed);
    // the dimensions, stride and buffer size must agree or the frame is counted as a failed
    // capture.
    virtual bool capture(CapturedFrame *frame) = 0;
};

struct PortalRunnerConfig {
    std::int64_t inference_interval_ms = 66;  // ~15 inferences per second
};

struct PortalRunnerStats {
    std::uint64_t frames_submitted = 0;
    std::uint64_t ticks_skipped_interval = 0;
    std::uint64_t ticks_skipped_busy = 0;  // the worker was still busy with the previous frame
    std::uint64_t captures_failed = 0;     // no frame, or the source returned an inconsistent one
};

// Runs Engine::process_frame on a worker thread. The render thread never blocks on inference
// and never queues stale frames: a frame is captured only when the worker is idle, so the
// frame the worker receives is always the newest one available. Ticks that arrive while the
// worker is busy are skipped without touching the camera. Buffers are recycled, so
// steady-state operation allocates nothing.
//
// Every frame carries the engine's session id from the moment it was captured, so a frame that
// was captured (or queued) before a reset is dropped by the engine instead of being published
// after it.
class PortalRunner {
public:
    // `engine` and `source` must outlive the runner.
    PortalRunner(Engine *engine, FrameSource *source, PortalRunnerConfig config = {});
    ~PortalRunner();
    PortalRunner(const PortalRunner &) = delete;
    PortalRunner &operator=(const PortalRunner &) = delete;

    // Render thread, once per frame. Cheap unless it decides to capture.
    void tick(std::int64_t now_ms);

    // Resets the engine (see Engine::reset) and forgets any frame that is waiting for the
    // worker. A frame already being processed is discarded by the engine when it finishes.
    // Callable from any thread.
    void reset();

    [[nodiscard]] PortalRunnerStats stats() const noexcept;

private:
    void worker_loop();

    Engine *engine_;
    FrameSource *source_;
    PortalRunnerConfig config_;

    std::mutex mutex_;
    std::condition_variable condition_;
    CapturedFrame filling_;     // render thread only
    CapturedFrame pending_;     // guarded by mutex_
    CapturedFrame processing_;  // worker only
    bool has_pending_ = false;
    bool stop_ = false;
    bool worker_busy_ = false;
    std::atomic<std::int64_t> last_capture_ms_{-1};
    std::thread worker_;

    std::atomic<std::uint64_t> frames_submitted_{0};
    std::atomic<std::uint64_t> skipped_interval_{0};
    std::atomic<std::uint64_t> skipped_busy_{0};
    std::atomic<std::uint64_t> captures_failed_{0};
};

}  // namespace magic_portal
