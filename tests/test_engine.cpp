// Engine behaviour: publishing, freshness, transitions, reset, error handling, threading.
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "golden_support.h"
#include "magic_portal/engine.h"
#include "magic_portal/mask_refiner.h"
#include "test_harness.h"

using namespace magic_portal;

namespace {

constexpr int kW = 64;
constexpr int kH = 48;

class ScriptedSegmenter final : public PersonSegmenter {
public:
    explicit ScriptedSegmenter(ConfidenceMask mask) : mask_(std::move(mask)) {}
    bool segment(const RgbaFrame &, std::int64_t, ConfidenceMask *out, std::string *error) override {
        ++calls;
        if (hook) {
            hook();
        }
        if (fail) {
            *error = "scripted failure";
            return false;
        }
        *out = mask_;
        return true;
    }
    std::function<void()> hook;
    std::atomic<bool> fail{false};
    std::atomic<int> calls{0};

private:
    ConfidenceMask mask_;
};

ConfidenceMask person_mask() {
    return mp_test::make_ellipse_mask(32, 32, 0.5F, 0.5F, 0.25F, 0.4F, 7);
}
ConfidenceMask empty_mask() {
    return ConfidenceMask(32, 32);
}

FrameInput input_for(const RgbaFrame &frame, std::int64_t t) {
    FrameInput in;
    in.rgba = frame.data();
    in.width = frame.width();
    in.height = frame.height();
    in.stride_bytes = frame.width() * 4;
    in.timestamp_ms = t;
    return in;
}

EngineConfig fast_config() {
    EngineConfig config;
    config.timeline.duration_ms = 1000;
    config.freshness.stale_after_ms = 400;
    return config;
}

}  // namespace

MT_TEST("engine: publishes a refined mask that matches the standalone refiner bit for bit") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 3);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());
    MT_CHECK(engine.process_frame(input_for(camera, 100), &raw) == Status::kOk);

    MaskRefiner reference(fast_config().mask_refiner);
    const RefinedPersonMask expected = reference.refine(camera, raw, 100);

    const RenderState state = engine.render_state(100);
    MT_CHECK(state.mask_updated);
    MT_CHECK(state.mask != nullptr);
    MT_CHECK(state.has_person);
    MT_CHECK(state.mask->values() == expected.alpha.values());
}

MT_TEST("engine: mask_updated is reported once per published mask") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 3);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());
    MT_CHECK(!engine.render_state(0).mask_updated);
    MT_CHECK(engine.process_frame(input_for(camera, 10), &raw) == Status::kOk);
    const RenderState first = engine.render_state(10);
    const RenderState second = engine.render_state(11);
    MT_CHECK(first.mask_updated && !second.mask_updated);
    MT_CHECK(first.mask_version == second.mask_version);
    MT_CHECK(engine.process_frame(input_for(camera, 20), &raw) == Status::kOk);
    const RenderState third = engine.render_state(20);
    MT_CHECK(third.mask_updated && third.mask_version > second.mask_version);
}

MT_TEST("engine: rejects bad input with a message and does not publish") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 3);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());

    FrameInput bad = input_for(camera, 1);
    bad.rgba = nullptr;
    MT_CHECK(engine.process_frame(bad, &raw) == Status::kInvalidArgument);
    bad = input_for(camera, 1);
    bad.stride_bytes = kW * 4 - 1;
    MT_CHECK(engine.process_frame(bad, &raw) == Status::kInvalidArgument);
    bad = input_for(camera, 1);
    bad.crop_right = 1.5F;
    MT_CHECK(engine.process_frame(bad, &raw) == Status::kInvalidArgument);
    bad = input_for(camera, 1);
    bad.crop_left = 0.5F;
    bad.crop_right = 0.5F;
    MT_CHECK(engine.process_frame(bad, &raw) == Status::kInvalidArgument);
    MT_CHECK(!engine.last_error().empty());
    MT_CHECK(engine.stats().frames_processed == 0);
    MT_CHECK(engine.process_frame(input_for(camera, 1), nullptr) == Status::kInvalidState);  // no model
    MT_CHECK(engine.render_state(1).mask == nullptr);
}

MT_TEST("engine: row padding does not change the result") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 11);
    const ConfidenceMask raw = person_mask();
    std::vector<std::uint8_t> padded(static_cast<std::size_t>(kW * 4 + 24) * kH, 0xAB);
    for (int y = 0; y < kH; ++y) {
        std::copy(camera.data() + static_cast<std::size_t>(y) * kW * 4,
                  camera.data() + static_cast<std::size_t>(y + 1) * kW * 4,
                  padded.begin() + static_cast<std::ptrdiff_t>(y) * (kW * 4 + 24));
    }
    Engine a(fast_config());
    Engine b(fast_config());
    FrameInput tight = input_for(camera, 5);
    FrameInput loose = tight;
    loose.rgba = padded.data();
    loose.stride_bytes = kW * 4 + 24;
    MT_CHECK(a.process_frame(tight, &raw) == Status::kOk);
    MT_CHECK(b.process_frame(loose, &raw) == Status::kOk);
    MT_CHECK(a.render_state(5).mask->values() == b.render_state(5).mask->values());
}

MT_TEST("engine: mirroring flips the crop and the mask orientation") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask raw = person_mask();
    FrameInput in = input_for(camera, 5);
    in.crop_left = 0.1F;
    in.crop_right = 0.6F;
    in.mirror_horizontally = true;
    Engine engine(fast_config());
    MT_CHECK(engine.process_frame(in, &raw) == Status::kOk);
    const RenderState state = engine.render_state(5);
    MT_CHECK_NEAR(state.mask_crop[0], 0.4, 1e-6);
    MT_CHECK_NEAR(state.mask_crop[2], 0.9, 1e-6);
    for (float v : state.person_bounds) {
        MT_CHECK(v >= 0.0F && v <= 1.0F);
    }
    MT_CHECK(state.person_bounds[0] <= state.person_bounds[2]);
    MT_CHECK(state.person_bounds[1] <= state.person_bounds[3]);
}

MT_TEST("engine: transitions need a fresh mask and complete into the replaced phase") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());
    MT_CHECK(engine.begin_transition(0, 3) == Status::kInvalidState);  // no mask yet
    MT_CHECK(engine.process_frame(input_for(camera, 100), &raw) == Status::kOk);
    MT_CHECK(engine.begin_transition(100, -1) == Status::kInvalidArgument);
    MT_CHECK(!engine.render_state(100).effect_enable);  // live: camera is untouched
    MT_CHECK(engine.begin_transition(100, 3) == Status::kOk);
    MT_CHECK(engine.begin_transition(110, 4) == Status::kInvalidState);  // already running

    RenderState state = engine.render_state(150);
    MT_CHECK(state.phase == PortalPhase::kTransitioning);
    MT_CHECK(state.effect_enable);
    MT_CHECK(state.camera_is_current);
    MT_CHECK(state.next_background == 3);

    // Keep the mask fresh through the end of the transition.
    for (std::int64_t t = 200; t <= 1200; t += 100) {
        MT_CHECK(engine.process_frame(input_for(camera, t), &raw) == Status::kOk);
    }
    state = engine.render_state(1200);
    MT_CHECK(state.phase == PortalPhase::kReplaced);
    MT_CHECK(state.current_background == 3);
    MT_CHECK(!state.camera_is_current);
    MT_CHECK(state.effect_enable);  // background stays replaced behind the person
}

MT_TEST("engine: a stale mask fails safe to the camera and recovers") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());
    MT_CHECK(engine.process_frame(input_for(camera, 100), &raw) == Status::kOk);
    MT_CHECK(engine.begin_transition(100, 1) == Status::kOk);
    MT_CHECK(engine.render_state(300).effect_enable);
    MT_CHECK(!engine.render_state(501).effect_enable);  // older than 400 ms
    MT_CHECK(engine.process_frame(input_for(camera, 600), &raw) == Status::kOk);
    MT_CHECK(engine.render_state(610).effect_enable);
}

MT_TEST("engine: an empty scene publishes a blank mask, stays fresh and reports no person") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask nobody = empty_mask();
    Engine engine(fast_config());
    MT_CHECK(engine.process_frame(input_for(camera, 50), &nobody) == Status::kNoPerson);
    const RenderState state = engine.render_state(60);
    MT_CHECK(state.mask != nullptr && !state.has_person);
    bool all_zero = true;
    for (float v : state.mask->values()) {
        all_zero = all_zero && v == 0.0F;
    }
    MT_CHECK(all_zero);
    MT_CHECK(engine.begin_transition(60, 2) == Status::kOk);  // fully replaces the scene
    MT_CHECK(engine.stats().frames_without_person == 1);
}

MT_TEST("engine: segmenter failure keeps the previous mask until it ages out") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    auto owned = std::make_unique<ScriptedSegmenter>(person_mask());
    ScriptedSegmenter *segmenter = owned.get();
    Engine engine(fast_config(), std::move(owned));
    MT_CHECK(engine.process_frame(input_for(camera, 100)) == Status::kOk);
    MT_CHECK(engine.begin_transition(100, 1) == Status::kOk);
    segmenter->fail = true;
    MT_CHECK(engine.process_frame(input_for(camera, 150)) == Status::kInferenceError);
    MT_CHECK(engine.last_error() == "scripted failure");
    MT_CHECK(engine.stats().frames_failed == 1);
    MT_CHECK(engine.render_state(300).effect_enable);   // still within the freshness window
    MT_CHECK(!engine.render_state(600).effect_enable);  // aged out
    segmenter->fail = false;
    MT_CHECK(engine.process_frame(input_for(camera, 700)) == Status::kOk);
    MT_CHECK(engine.last_error().empty());
    MT_CHECK(engine.render_state(710).effect_enable);
}

MT_TEST("engine: reset returns to live and discards a frame that was already in flight") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    auto owned = std::make_unique<ScriptedSegmenter>(person_mask());
    ScriptedSegmenter *segmenter = owned.get();
    Engine engine(fast_config(), std::move(owned));
    MT_CHECK(engine.process_frame(input_for(camera, 100)) == Status::kOk);
    MT_CHECK(engine.begin_transition(100, 1) == Status::kOk);

    segmenter->hook = [&engine] { engine.reset(); };  // reset lands during inference
    MT_CHECK(engine.process_frame(input_for(camera, 200)) == Status::kOk);
    segmenter->hook = nullptr;
    MT_CHECK(engine.stats().frames_discarded_by_reset == 1);
    const RenderState state = engine.render_state(210);
    MT_CHECK(state.phase == PortalPhase::kLive);
    MT_CHECK(state.mask == nullptr);
    MT_CHECK(!state.effect_enable);
    MT_CHECK(engine.begin_transition(210, 2) == Status::kInvalidState);  // needs a new mask
    MT_CHECK(engine.process_frame(input_for(camera, 300)) == Status::kOk);
    MT_CHECK(engine.begin_transition(300, 2) == Status::kOk);
}

MT_TEST("engine: an older frame never replaces a newer mask") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask newer = person_mask();
    const ConfidenceMask older = empty_mask();
    Engine engine(fast_config());
    MT_CHECK(engine.process_frame(input_for(camera, 2000), &newer) == Status::kOk);
    const RenderState before = engine.render_state(2000);
    MT_CHECK(before.mask != nullptr && before.has_person);
    const std::uint64_t version = before.mask_version;
    const std::vector<float> values = before.mask->values();

    // A late or reordered frame (timestamp 0 after 2000) must not win.
    MT_CHECK(engine.process_frame(input_for(camera, 0), &older) == Status::kInvalidArgument);
    MT_CHECK(!engine.last_error().empty());
    const RenderState after = engine.render_state(2010);
    MT_CHECK(!after.mask_updated);
    MT_CHECK(after.mask_version == version);
    MT_CHECK(after.has_person);
    MT_CHECK(after.mask != nullptr && after.mask->values() == values);
    MT_CHECK(engine.stats().last_mask_timestamp_ms == 2000);

    // The same timestamp is a duplicate, not an older frame, and a later one moves on.
    MT_CHECK(engine.process_frame(input_for(camera, 2000), &newer) == Status::kOk);
    MT_CHECK(engine.process_frame(input_for(camera, 2033), &newer) == Status::kOk);
}

MT_TEST("engine: a mask stamped in the future cannot keep the effect alive") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask raw = person_mask();
    {
        Engine engine(fast_config());
        MT_CHECK(engine.process_frame(input_for(camera, 900), &raw) == Status::kOk);
        MT_CHECK(engine.begin_transition(900, 1) == Status::kOk);
        MT_CHECK(engine.render_state(950).effect_enable);
        // A frame from a wrong clock (ten seconds ahead) must not look "fresh" at now = 1000.
        MT_CHECK(engine.process_frame(input_for(camera, 10000), &raw) == Status::kOk);
        MT_CHECK(!engine.render_state(1000).effect_enable);
    }
    {
        Engine engine(fast_config());
        MT_CHECK(engine.process_frame(input_for(camera, 10000), &raw) == Status::kOk);
        MT_CHECK(engine.begin_transition(1000, 1) == Status::kInvalidState);
    }
}

MT_TEST("engine: frames from before a reset are dropped by session id") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());
    const std::uint64_t first = engine.session_id();
    MT_CHECK(first != kAnySession);
    FrameInput captured = input_for(camera, 100);
    captured.session_id = first;
    MT_CHECK(engine.process_frame(captured, &raw) == Status::kOk);
    MT_CHECK(engine.render_state(100).mask != nullptr);

    engine.reset();
    MT_CHECK(engine.session_id() != first);
    MT_CHECK(engine.render_state(110).mask == nullptr);
    captured.timestamp_ms = 120;
    MT_CHECK(engine.process_frame(captured, &raw) == Status::kOk);  // late frame of the old session
    MT_CHECK(engine.stats().frames_discarded_by_reset == 1);
    MT_CHECK(engine.render_state(130).mask == nullptr);

    FrameInput current = input_for(camera, 140);
    current.session_id = engine.session_id();
    MT_CHECK(engine.process_frame(current, &raw) == Status::kOk);
    MT_CHECK(engine.render_state(150).mask != nullptr);
    // kAnySession (the default) is never dropped.
    MT_CHECK(engine.process_frame(input_for(camera, 160), &raw) == Status::kOk);
    MT_CHECK(engine.stats().frames_discarded_by_reset == 1);
}

MT_TEST("engine: a reset does not free the mask the render thread is still reading") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());
    MT_CHECK(engine.process_frame(input_for(camera, 100), &raw) == Status::kOk);
    const RenderState state = engine.render_state(100);
    MT_CHECK(state.mask != nullptr);
    const std::vector<float> before = state.mask->values();

    engine.reset();  // from another thread in real use; the pointer must stay readable
    MT_CHECK(state.mask->values() == before);

    const RenderState after = engine.render_state(110);  // the old mask is released here
    MT_CHECK(after.mask == nullptr && !after.mask_updated && after.mask_timestamp_ms == -1);
    MT_CHECK(after.mask_version > state.mask_version);  // versions only ever move forward
}

MT_TEST("engine: the mask timestamp is reported and freshness follows the mask on screen") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    const ConfidenceMask raw = person_mask();
    Engine engine(fast_config());
    MT_CHECK(engine.render_state(0).mask_timestamp_ms == -1);
    MT_CHECK(engine.process_frame(input_for(camera, 700), &raw) == Status::kOk);
    const RenderState state = engine.render_state(710);
    MT_CHECK(state.mask_timestamp_ms == 700);
    MT_CHECK(engine.begin_transition(710, 1) == Status::kOk);
    MT_CHECK(engine.render_state(1000).effect_enable);   // 300 ms old
    MT_CHECK(!engine.render_state(1200).effect_enable);  // 500 ms old, limit is 400
}

MT_TEST("engine: rejects frames that are too large or have inconsistent sizes without touching memory") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    Engine engine(fast_config());
    FrameInput huge = input_for(camera, 100);
    huge.width = 2'000'000'000;
    huge.stride_bytes = 2'000'000'000;
    MT_CHECK(engine.process_frame(huge) == Status::kInvalidArgument);
    FrameInput wide = input_for(camera, 100);
    wide.width = kMaxFrameDimension + 1;
    wide.stride_bytes = (kMaxFrameDimension + 1) * 4;
    MT_CHECK(engine.process_frame(wide) == Status::kInvalidArgument);
    ConfidenceMask too_big(kMaxMaskDimension + 1, 1);
    MT_CHECK(engine.process_frame(input_for(camera, 100), &too_big) == Status::kInvalidArgument);
}

MT_TEST("engine: identical inputs give identical outputs across instances") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 21);
    const ConfidenceMask raw = person_mask();
    Engine a(fast_config());
    Engine b(fast_config());
    for (std::int64_t t = 0; t < 500; t += 100) {
        MT_CHECK(a.process_frame(input_for(camera, t), &raw) == Status::kOk);
        MT_CHECK(b.process_frame(input_for(camera, t), &raw) == Status::kOk);
    }
    const RenderState sa = a.render_state(400);
    const RenderState sb = b.render_state(400);
    MT_CHECK(sa.mask->values() == sb.mask->values());
    for (int i = 0; i < 4; ++i) {
        MT_CHECK(sa.person_bounds[i] == sb.person_bounds[i]);
    }
}

MT_TEST("engine: concurrent worker and render threads stay consistent") {
    const RgbaFrame camera = mp_test::make_camera(kW, kH, 5);
    auto owned = std::make_unique<ScriptedSegmenter>(person_mask());
    Engine engine(fast_config(), std::move(owned));
    MT_CHECK(engine.process_frame(input_for(camera, 0)) == Status::kOk);

    std::atomic<bool> stop{false};
    std::atomic<std::int64_t> clock{0};
    std::thread worker([&] {
        for (int i = 1; i <= 150; ++i) {
            clock = i * 10;
            (void)engine.process_frame(input_for(camera, i * 10));
        }
        stop = true;
    });
    std::uint64_t last_version = 0;
    bool monotonic = true;
    int begun = 0;
    while (!stop) {
        const std::int64_t now = clock.load();
        const RenderState state = engine.render_state(now);
        monotonic = monotonic && state.mask_version >= last_version;
        last_version = state.mask_version;
        if (state.mask != nullptr) {
            monotonic = monotonic && state.mask->size() == 32U * 32U;
        }
        if (state.phase != PortalPhase::kTransitioning) {
            begun += engine.begin_transition(now, begun) == Status::kOk ? 1 : 0;
        }
        if (begun % 7 == 6) {
            engine.reset();
            begun += 1;
        }
    }
    worker.join();
    MT_CHECK(monotonic);
    MT_CHECK(engine.stats().frames_processed + engine.stats().frames_discarded_by_reset >= 1);
}
