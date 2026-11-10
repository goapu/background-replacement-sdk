#include "magic_portal/portal_runner.h"

#include <cstddef>
#include <stdexcept>
#include <utility>

#include "magic_portal/limits.h"

namespace magic_portal {

PortalRunner::PortalRunner(Engine *engine, FrameSource *source, PortalRunnerConfig config)
    : engine_(engine), source_(source), config_(config) {
    if (engine_ == nullptr || source_ == nullptr) {
        throw std::invalid_argument("PortalRunner needs an engine and a frame source");
    }
    if (config_.inference_interval_ms <= 0) {
        throw std::invalid_argument("inference interval must be positive");
    }
    worker_ = std::thread(&PortalRunner::worker_loop, this);
}

PortalRunner::~PortalRunner() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

PortalRunnerStats PortalRunner::stats() const noexcept {
    PortalRunnerStats stats;
    stats.frames_submitted = frames_submitted_.load(std::memory_order_relaxed);
    stats.ticks_skipped_interval = skipped_interval_.load(std::memory_order_relaxed);
    stats.ticks_skipped_busy = skipped_busy_.load(std::memory_order_relaxed);
    stats.captures_failed = captures_failed_.load(std::memory_order_relaxed);
    return stats;
}

void PortalRunner::tick(std::int64_t now_ms) {
    const std::int64_t last = last_capture_ms_.load(std::memory_order_relaxed);
    if (last >= 0 && saturating_sub(now_ms, last) < config_.inference_interval_ms) {
        skipped_interval_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_pending_ || worker_busy_) {
            // Capturing now would only park an old frame behind the one being processed;
            // by the time the worker got to it, it would be stale. Wait for an idle worker.
            skipped_busy_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    filling_.mirror_horizontally = false;
    filling_.crop_left = 0.0F;
    filling_.crop_top = 0.0F;
    filling_.crop_right = 1.0F;
    filling_.crop_bottom = 1.0F;
    // Read before the pixels are taken: if reset() lands while capturing, the frame belongs
    // to the previous session and the engine will drop it.
    const std::uint64_t session = engine_->session_id();
    if (!source_->capture(&filling_)) {
        captures_failed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::int64_t stride = 0;
    if (!frame_size_ok(filling_.width, filling_.height) ||
        !resolve_rgba_stride(filling_.width, filling_.stride_bytes, &stride) ||
        filling_.rgba.size() <
            static_cast<std::size_t>(stride) * static_cast<std::size_t>(filling_.height - 1) +
                4U * static_cast<std::size_t>(filling_.width)) {
        captures_failed_.fetch_add(1, std::memory_order_relaxed);  // sizes and buffer disagree
        return;
    }
    filling_.stride_bytes = static_cast<int>(stride);
    filling_.timestamp_ms = now_ms;
    filling_.session_id = session;
    last_capture_ms_.store(now_ms, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::swap(pending_, filling_);  // O(1); both keep their allocations
        has_pending_ = true;
    }
    frames_submitted_.fetch_add(1, std::memory_order_relaxed);
    condition_.notify_one();
}

void PortalRunner::reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        has_pending_ = false;  // a waiting frame belongs to the scene being thrown away
    }
    engine_->reset();
    last_capture_ms_.store(-1, std::memory_order_relaxed);  // capture again on the next tick
}

void PortalRunner::worker_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stop_ || has_pending_; });
            if (stop_) {
                return;
            }
            std::swap(processing_, pending_);
            has_pending_ = false;
            worker_busy_ = true;
        }
        FrameInput input;
        input.rgba = processing_.rgba.data();
        input.width = processing_.width;
        input.height = processing_.height;
        input.stride_bytes = processing_.stride_bytes;  // validated by tick()
        input.session_id = processing_.session_id;
        input.mirror_horizontally = processing_.mirror_horizontally;
        input.timestamp_ms = processing_.timestamp_ms;
        input.crop_left = processing_.crop_left;
        input.crop_top = processing_.crop_top;
        input.crop_right = processing_.crop_right;
        input.crop_bottom = processing_.crop_bottom;
        (void)engine_->process_frame(input);  // failures are recorded by the engine
        std::lock_guard<std::mutex> lock(mutex_);
        worker_busy_ = false;
    }
}

}  // namespace magic_portal
