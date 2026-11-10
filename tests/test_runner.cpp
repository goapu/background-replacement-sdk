#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>

#include "golden_support.h"
#include "magic_portal/portal_runner.h"
#include "test_harness.h"

using namespace magic_portal;

namespace {

class GateSegmenter final : public PersonSegmenter {
public:
    bool segment(const RgbaFrame &, std::int64_t, ConfidenceMask *out, std::string *) override {
        {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait(lock, [this] { return open; });
        }
        *out = mp_test::make_ellipse_mask(32, 32, 0.5F, 0.5F, 0.25F, 0.4F, 7);
        ++done;
        return true;
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return entered; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        cv.notify_all();
    }
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    bool entered = false;
    std::atomic<int> done{0};
};

class CountingSource final : public FrameSource {
public:
    bool capture(CapturedFrame *frame) override {
        ++calls;
        if (!available) {
            return false;
        }
        frame->width = 64;
        frame->height = 48;
        frame->stride_bytes = 64 * 4;
        frame->rgba.resize(64U * 48U * 4U);
        std::fill(frame->rgba.begin(), frame->rgba.end(), static_cast<std::uint8_t>(calls));
        frame->mirror_horizontally = mirror;
        frame->crop_left = 0.1F;
        frame->crop_right = 0.9F;
        buffers.insert(frame->rgba.data());
        return true;
    }
    std::atomic<int> calls{0};
    std::atomic<bool> available{true};
    bool mirror = false;
    std::set<const std::uint8_t *> buffers;
};

template <typename Pred>
bool wait_for(Pred pred) {
    for (int i = 0; i < 4000; ++i) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

MT_TEST("runner: captures on the interval and feeds the engine") {
    auto gate = std::make_unique<GateSegmenter>();
    gate->release();
    Engine open_engine(EngineConfig{}, std::move(gate));
    CountingSource source;
    PortalRunnerConfig config;
    config.inference_interval_ms = 50;
    {
        PortalRunner runner(&open_engine, &source, config);
        runner.tick(1000);  // captured
        runner.tick(1020);  // too soon
        runner.tick(1049);  // too soon
        MT_CHECK(wait_for([&] { return open_engine.stats().frames_processed == 1; }));
        runner.tick(1050);  // captured
        MT_CHECK(wait_for([&] { return open_engine.stats().frames_processed == 2; }));
        const PortalRunnerStats stats = runner.stats();
        MT_CHECK(stats.frames_submitted == 2);
        MT_CHECK(stats.ticks_skipped_interval == 2);
    }
    MT_CHECK(source.calls == 2);
    const RenderState state = open_engine.render_state(1050);
    MT_CHECK(state.mask != nullptr);
    MT_CHECK_NEAR(state.mask_crop[0], 0.1, 1e-6);  // crop travels with the frame
    MT_CHECK_NEAR(state.mask_crop[2], 0.9, 1e-6);
}

MT_TEST("runner: a busy worker never builds a queue and the render thread does not block") {
    auto owned = std::make_unique<GateSegmenter>();
    GateSegmenter *gate = owned.get();
    Engine engine(EngineConfig{}, std::move(owned));
    CountingSource source;
    PortalRunnerConfig config;
    config.inference_interval_ms = 10;
    {
        PortalRunner runner(&engine, &source, config);
        runner.tick(0);        // goes straight to the worker
        gate->wait_entered();  // worker is now stuck inside inference
        const int captured_before = source.calls;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 1; i <= 50; ++i) {
            runner.tick(i * 20);  // all skipped: the worker is busy
        }
        const auto elapsed = std::chrono::steady_clock::now() - start;
        MT_CHECK(source.calls == captured_before);  // the camera is not even asked
        MT_CHECK(elapsed < std::chrono::milliseconds(200));
        const PortalRunnerStats stats = runner.stats();
        MT_CHECK(stats.frames_submitted == 1);
        MT_CHECK(stats.ticks_skipped_busy == 50);
        gate->release();
        MT_CHECK(wait_for([&] { return gate->done == 1; }));
        MT_CHECK(wait_for([&] { return engine.stats().frames_processed == 1; }));
        runner.tick(2000);  // idle again: the next capture is taken now
        MT_CHECK(wait_for([&] { return gate->done == 2; }));
        MT_CHECK(runner.stats().frames_submitted == 2);
    }
}

namespace {

// A source whose capture() lets a reset land after the pixels were taken, which is what a
// user pressing "reset" on another thread looks like from the runner's point of view.
class ResettingSource final : public FrameSource {
public:
    explicit ResettingSource(Engine *target) : engine(target) {}
    bool capture(CapturedFrame *frame) override {
        const bool ok = inner.capture(frame);
        if (ok && reset_after_capture) {
            engine->reset();
        }
        return ok;
    }
    Engine *engine;
    CountingSource inner;
    bool reset_after_capture = true;
};

}  // namespace

MT_TEST("runner: a frame captured before reset() is never published after it") {
    auto owned = std::make_unique<GateSegmenter>();
    owned->release();
    Engine engine(EngineConfig{}, std::move(owned));
    ResettingSource source(&engine);
    PortalRunnerConfig config;
    config.inference_interval_ms = 10;
    {
        PortalRunner runner(&engine, &source, config);
        runner.tick(1000);
        MT_CHECK(wait_for([&] {
            const EngineStats s = engine.stats();
            return s.frames_processed + s.frames_discarded_by_reset >= 1;
        }));
    }
    const RenderState state = engine.render_state(1000);
    MT_CHECK(state.mask == nullptr);  // the pre-reset frame must not resurrect a mask
    MT_CHECK(!state.effect_enable);
    MT_CHECK(engine.stats().frames_discarded_by_reset == 1);
}

MT_TEST("runner: the worker only ever sees a freshly captured frame") {
    auto owned = std::make_unique<GateSegmenter>();
    GateSegmenter *gate = owned.get();
    Engine engine(EngineConfig{}, std::move(owned));
    CountingSource source;
    PortalRunnerConfig config;
    config.inference_interval_ms = 10;
    {
        PortalRunner runner(&engine, &source, config);
        runner.tick(0);
        gate->wait_entered();
        runner.tick(20);  // the worker is busy: this frame would be stale by the time it ran
        runner.tick(40);
        gate->release();
        MT_CHECK(wait_for([&] { return engine.stats().frames_processed >= 1; }));
        MT_CHECK(wait_for([&] { return engine.stats().last_mask_timestamp_ms == 0; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // No frame captured at 20 or 40 was queued behind the first one.
        MT_CHECK(engine.stats().frames_processed == 1);
        MT_CHECK(engine.stats().last_mask_timestamp_ms == 0);
        runner.tick(400);  // idle again: this capture is current
        MT_CHECK(wait_for([&] { return engine.stats().last_mask_timestamp_ms == 400; }));
    }
}

MT_TEST("runner: steady state reuses a fixed set of buffers") {
    auto owned = std::make_unique<GateSegmenter>();
    owned->release();
    Engine engine(EngineConfig{}, std::move(owned));
    CountingSource source;
    PortalRunnerConfig config;
    config.inference_interval_ms = 1;
    {
        PortalRunner runner(&engine, &source, config);
        for (int i = 0; i < 40; ++i) {
            runner.tick(i * 10);
            wait_for([&] { return engine.stats().frames_processed >= static_cast<std::uint64_t>(i + 1); });
        }
    }
    MT_CHECK(source.buffers.size() <= 3);  // filling / pending / processing
}

MT_TEST("runner: capture failures are counted and retried on the next tick") {
    auto owned = std::make_unique<GateSegmenter>();
    owned->release();
    Engine engine(EngineConfig{}, std::move(owned));
    CountingSource source;
    source.available = false;
    PortalRunner runner(&engine, &source, PortalRunnerConfig{});
    runner.tick(0);
    runner.tick(1);
    MT_CHECK(runner.stats().captures_failed == 2);
    source.available = true;
    runner.tick(2);
    MT_CHECK(runner.stats().frames_submitted == 1);
}

MT_TEST("runner: rejects invalid construction and shuts down cleanly with work pending") {
    Engine engine(EngineConfig{});
    CountingSource source;
    MT_CHECK_THROWS(PortalRunner(nullptr, &source), std::invalid_argument);
    MT_CHECK_THROWS(PortalRunner(&engine, nullptr), std::invalid_argument);
    PortalRunnerConfig bad;
    bad.inference_interval_ms = 0;
    MT_CHECK_THROWS(PortalRunner(&engine, &source, bad), std::invalid_argument);

    auto owned = std::make_unique<GateSegmenter>();
    GateSegmenter *gate = owned.get();
    Engine blocked_engine(EngineConfig{}, std::move(owned));
    std::thread releaser;
    {
        PortalRunner runner(&blocked_engine, &source);
        runner.tick(0);
        gate->wait_entered();
        releaser = std::thread([gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            gate->release();
        });
        // The destructor runs while the worker is mid-inference: it must wait for it.
    }
    releaser.join();
    MT_CHECK(gate->done == 1);
}

MT_TEST("runner: reset() drops a waiting frame, starts a new session and captures again at once") {
    auto owned = std::make_unique<GateSegmenter>();
    GateSegmenter *gate = owned.get();
    Engine engine(EngineConfig{}, std::move(owned));
    CountingSource source;
    PortalRunnerConfig config;
    config.inference_interval_ms = 10000;  // a reset must not wait for the interval to pass
    {
        PortalRunner runner(&engine, &source, config);
        const std::uint64_t before = engine.session_id();
        runner.tick(0);
        gate->wait_entered();  // frame 1 is being processed
        runner.reset();        // lands mid-inference
        MT_CHECK(engine.session_id() != before);
        gate->release();
        MT_CHECK(wait_for([&] { return engine.stats().frames_discarded_by_reset == 1; }));
        MT_CHECK(engine.render_state(0).mask == nullptr);
        runner.tick(1);  // interval ignored after a reset
        MT_CHECK(wait_for([&] { return engine.stats().frames_processed == 1; }));
        MT_CHECK(engine.render_state(1).mask != nullptr);
    }
}

namespace {

class OddSource final : public FrameSource {
public:
    bool capture(CapturedFrame *frame) override {
        frame->width = width;
        frame->height = height;
        frame->stride_bytes = stride;
        frame->rgba.assign(bytes, 7);
        return true;
    }
    int width = 64;
    int height = 48;
    int stride = 0;
    std::size_t bytes = 64U * 48U * 4U;
};

}  // namespace

MT_TEST("runner: an inconsistent frame from the source is a failed capture, never a crash") {
    auto owned = std::make_unique<GateSegmenter>();
    owned->release();
    Engine engine(EngineConfig{}, std::move(owned));
    OddSource source;
    PortalRunner runner(&engine, &source, PortalRunnerConfig{});
    std::int64_t t = 0;
    const auto attempt = [&] {
        t += 1000;
        runner.tick(t);
    };
    source.width = 2147483647;  // width * 4 overflows int
    attempt();
    source.width = 64;
    source.stride = 100;  // shorter than one row
    attempt();
    source.stride = -256;
    attempt();
    source.stride = 0;
    source.bytes = 100;  // buffer smaller than the dimensions claim
    attempt();
    source.bytes = 64U * 48U * 4U;
    source.height = 0;
    attempt();
    MT_CHECK(runner.stats().captures_failed == 5);
    MT_CHECK(runner.stats().frames_submitted == 0);
    source.height = 48;
    attempt();  // a consistent frame still goes through
    MT_CHECK(runner.stats().frames_submitted == 1);
}
