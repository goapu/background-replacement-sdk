// Unit tests for frame utilities, the portal controller, mask freshness and geometry.
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <limits>
#include <stdexcept>

#include "golden_support.h"
#include "magic_portal/engine.h"
#include "magic_portal/frame.h"
#include "magic_portal/limits.h"
#include "magic_portal/mask_freshness.h"
#include "magic_portal/mask_refiner.h"
#include "magic_portal/portal_compositor.h"
#include "magic_portal/portal_constants.h"
#include "magic_portal/portal_controller.h"
#include "magic_portal/portal_geometry.h"
#include "magic_portal/portal_hash.h"
#include "magic_portal/portal_timeline.h"
#include "test_harness.h"

using namespace magic_portal;

namespace {

std::vector<std::uint8_t> ramp_rgba(int width, int height, int stride_bytes) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stride_bytes) * static_cast<std::size_t>(height),
                                    0xEE);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t *p = bytes.data() +
                              static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes) +
                              static_cast<std::size_t>(x) * 4U;
            p[0] = static_cast<std::uint8_t>(x);
            p[1] = static_cast<std::uint8_t>(y);
            p[2] = static_cast<std::uint8_t>(x + y);
            p[3] = 255;
        }
    }
    return bytes;
}

}  // namespace

MT_TEST("frame: strided copy ignores row padding") {
    const auto bytes = ramp_rgba(5, 3, 32);  // 20 bytes of pixels, 12 bytes of padding per row
    RgbaFrame frame;
    copy_rgba_strided(bytes.data(), 5, 3, 32, false, &frame);
    MT_CHECK(frame.width() == 5 && frame.height() == 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) {
            const Rgba p = frame.pixel(x, y);
            MT_CHECK(p.r == x && p.g == y && p.b == x + y && p.a == 255);
        }
    }
}

MT_TEST("frame: strided copy with mirroring flips each row only") {
    const auto bytes = ramp_rgba(5, 3, 24);
    RgbaFrame frame;
    copy_rgba_strided(bytes.data(), 5, 3, 24, true, &frame);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 5; ++x) {
            const Rgba p = frame.pixel(x, y);
            MT_CHECK(p.r == 4 - x && p.g == y);
        }
    }
}

MT_TEST("frame: in-place mirror matches mirrored copy and is an involution") {
    const auto bytes = ramp_rgba(7, 4, 28);
    RgbaFrame a;
    RgbaFrame b;
    copy_rgba_strided(bytes.data(), 7, 4, 28, false, &a);
    copy_rgba_strided(bytes.data(), 7, 4, 28, true, &b);
    mirror_horizontal(&a);
    MT_CHECK(a.pixels() == b.pixels());
    mirror_horizontal(&a);
    RgbaFrame c;
    copy_rgba_strided(bytes.data(), 7, 4, 28, false, &c);
    MT_CHECK(a.pixels() == c.pixels());
}

MT_TEST("frame: reshape reuses capacity when shrinking") {
    RgbaFrame frame(64, 64);
    const std::uint8_t *before = frame.data();
    frame.reshape(32, 32);
    MT_CHECK(frame.data() == before);
    MT_CHECK(frame.width() == 32 && frame.height() == 32);
    MT_CHECK(frame.byte_size() == 32U * 32U * 4U);
}

MT_TEST("frame: invalid strided copy is rejected") {
    RgbaFrame frame;
    std::uint8_t pixel[4] = {1, 2, 3, 4};
    bool threw = false;
    try {
        copy_rgba_strided(pixel, 1, 1, 2, false, &frame);  // stride smaller than a row
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    MT_CHECK(threw);
    threw = false;
    try {
        copy_rgba_strided(nullptr, 1, 1, 4, false, &frame);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    MT_CHECK(threw);
}

MT_TEST("controller: live until begin, then transitions and commits the new background") {
    PortalTimelineConfig cfg;
    cfg.duration_ms = 1000;
    PortalController controller(cfg);

    PortalFrameState state = controller.update(0);
    MT_CHECK(state.phase == PortalPhase::kLive);
    MT_CHECK(state.camera_is_current);
    MT_CHECK(state.current_background == kNoBackground);

    MT_CHECK(controller.begin(100, 7));
    state = controller.update(600);
    MT_CHECK(state.phase == PortalPhase::kTransitioning);
    MT_CHECK(state.camera_is_current);
    MT_CHECK(state.next_background == 7);
    MT_CHECK(state.visual.reveal > 0.0F && state.visual.reveal < 1.0F);

    state = controller.update(1200);
    MT_CHECK(state.phase == PortalPhase::kReplaced);
    MT_CHECK(state.current_background == 7);
    MT_CHECK(!state.camera_is_current);
    MT_CHECK(controller.phase() == PortalPhase::kReplaced);
}

MT_TEST("controller: second transition starts from the replaced background") {
    PortalTimelineConfig cfg;
    cfg.duration_ms = 1000;
    PortalController controller(cfg);
    MT_CHECK(controller.begin(0, 1));
    (void)controller.update(2000);
    MT_CHECK(controller.begin(2100, 2));
    const PortalFrameState state = controller.update(2600);
    MT_CHECK(state.phase == PortalPhase::kTransitioning);
    MT_CHECK(!state.camera_is_current);
    MT_CHECK(state.current_background == 1);
    MT_CHECK(state.next_background == 2);
}

MT_TEST("controller: rejects overlapping and invalid transitions without side effects") {
    PortalTimelineConfig cfg;
    cfg.duration_ms = 1000;
    PortalController controller(cfg);
    MT_CHECK(!controller.begin(0, -1));
    MT_CHECK(controller.phase() == PortalPhase::kLive);
    MT_CHECK(controller.begin(0, 3));
    MT_CHECK(!controller.begin(10, 4));
    const PortalFrameState state = controller.update(500);
    MT_CHECK(state.next_background == 3);
}

MT_TEST("controller: reset returns to the live camera") {
    PortalController controller;
    MT_CHECK(controller.begin(0, 5));
    (void)controller.update(5000);
    controller.reset();
    const PortalFrameState state = controller.update(5100);
    MT_CHECK(state.phase == PortalPhase::kLive);
    MT_CHECK(state.camera_is_current);
    MT_CHECK(state.current_background == kNoBackground);
    MT_CHECK(controller.begin(5200, 5));  // usable again
}

MT_TEST("freshness: fail-closed before the first mask and after the limit") {
    MaskFreshnessConfig cfg;
    cfg.stale_after_ms = 300;
    MaskFreshness freshness(cfg);
    MT_CHECK(!freshness.is_fresh(0));
    MT_CHECK(freshness.age_ms(0) == -1);
    freshness.on_valid_mask(1000);
    MT_CHECK(freshness.is_fresh(1000));
    MT_CHECK(freshness.is_fresh(1300));
    MT_CHECK(!freshness.is_fresh(1301));
    MT_CHECK(freshness.age_ms(1250) == 250);
    freshness.on_valid_mask(1400);
    MT_CHECK(freshness.is_fresh(1500));
    freshness.reset();
    MT_CHECK(!freshness.has_mask());
    MT_CHECK(!freshness.is_fresh(1500));
}

MT_TEST("freshness: rejects a non-positive limit") {
    MaskFreshnessConfig cfg;
    cfg.stale_after_ms = 0;
    MT_CHECK_THROWS(MaskFreshness(cfg), std::invalid_argument);
}

MT_TEST("geometry: max radius reaches the farthest corner for any centre and aspect") {
    for (float aspect : {0.5F, 0.75F, 1.0F, 1.7778F}) {
        for (float cx : {0.0F, 0.25F, 0.5F, 0.9F, 1.0F}) {
            for (float cy : {0.0F, 0.4F, 0.5F, 1.0F}) {
                float best = 0.0F;
                for (float x : {0.0F, 1.0F}) {
                    for (float y : {0.0F, 1.0F}) {
                        const float dx = (x - cx) * aspect;
                        const float dy = y - cy;
                        best = std::fmax(best, std::sqrt(dx * dx + dy * dy));
                    }
                }
                MT_CHECK_NEAR(farthest_corner_distance(cx, cy, aspect), best, 1e-6);
            }
        }
    }
}

MT_TEST("geometry: reveal radius spans start to fully covered") {
    PersonBounds bounds;
    bounds.valid = true;
    bounds.min_x = 0.3F;
    bounds.max_x = 0.6F;
    bounds.min_y = 0.2F;
    bounds.max_y = 0.9F;
    PortalVisualState start;
    start.reveal = 0.0F;
    PortalVisualState end;
    end.reveal = 1.0F;
    const PortalGeometry a = compute_portal_geometry(bounds, 0.75F, start, 0.045F);
    const PortalGeometry b = compute_portal_geometry(bounds, 0.75F, end, 0.045F);
    MT_CHECK(a.reveal_radius < b.reveal_radius);
    MT_CHECK_NEAR(b.reveal_radius, b.max_radius, 1e-5);
    MT_CHECK(b.max_radius >= farthest_corner_distance(a.center_x, a.center_y, 0.75F));
}

MT_TEST("geometry: invalid bounds fall back to the default body size") {
    PersonBounds none;  // valid = false
    PortalVisualState state;
    const PortalGeometry geometry = compute_portal_geometry(none, 1.0F, state, 0.045F);
    MT_CHECK(geometry.body_radius >= constants::kBodyRadiusMin - 1e-6F);
    MT_CHECK(geometry.body_radius <= constants::kBodyRadiusMax + 1e-6F);
    MT_CHECK_NEAR(geometry.center_x, 0.5, 1e-6);
}

// Known answers computed independently (Python integers) from the published constants.
MT_TEST("hash: pcg round matches independently computed vectors") {
    const std::pair<std::uint32_t, std::uint32_t> expected[] = {{0U, 129708002U},
                                                                {1U, 2831084092U},
                                                                {2U, 2055130248U},
                                                                {4294967295U, 3861530882U},
                                                                {123456789U, 4272394698U}};
    for (const auto &[input, output] : expected) {
        MT_CHECK(magic_portal::pcg_round(input) == output);
    }
}

MT_TEST("hash: 2-D hash matches independently computed vectors and is exact in float") {
    struct Case {
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t value24;
    };
    const Case cases[] = {{0U, 0U, 3194371U},
                          {1U, 0U, 6185729U},
                          {0U, 1U, 15280721U},
                          {63U, 85U, 15443593U},
                          {359U, 639U, 6171549U},
                          {144U, 90U, 13251878U},
                          {4294967295U, 4294967295U, 12088085U},
                          {12345U, 67890U, 6841651U}};
    for (const Case &c : cases) {
        const float value = magic_portal::hash_unit(c.x, c.y);
        MT_CHECK(value >= 0.0F && value < 1.0F);
        MT_CHECK(value * 16777216.0F == static_cast<float>(c.value24));  // 24-bit value, no rounding
    }
}

MT_TEST("hash: behaves like noise over a screen-sized grid") {
    double sum = 0.0;
    int equal_neighbours = 0;
    float previous = -1.0F;
    for (std::uint32_t y = 0; y < 90U; ++y) {
        for (std::uint32_t x = 0; x < 64U; ++x) {
            const float value = magic_portal::hash_unit(x, y);
            sum += static_cast<double>(value);
            equal_neighbours += value == previous ? 1 : 0;
            previous = value;
        }
    }
    const double mean = sum / (64.0 * 90.0);
    MT_CHECK(mean > 0.48 && mean < 0.52);
    MT_CHECK(equal_neighbours == 0);
}

MT_TEST("hash: negative coordinates map to cell zero") {
    MT_CHECK(magic_portal::hash_cell(-1.0F) == 0U);
    MT_CHECK(magic_portal::hash_cell(0.0F) == 0U);
    MT_CHECK(magic_portal::hash_cell(7.0F) == 7U);
}

MT_TEST("freshness: extreme timestamps neither overflow nor look fresh") {
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    MaskFreshness oldest;
    oldest.on_valid_mask(kMin);
    MT_CHECK(!oldest.is_fresh(kMax));
    MT_CHECK(oldest.age_ms(kMax) == kMax);  // saturates instead of wrapping around
    MaskFreshness newest;
    newest.on_valid_mask(kMax);
    MT_CHECK(!newest.is_fresh(kMin));
    MT_CHECK(!newest.is_fresh(0));  // stamped in the future
    MaskFreshness normal;
    normal.on_valid_mask(1000);
    MT_CHECK(normal.is_fresh(1000));
    MT_CHECK(normal.is_fresh(1500));
    MT_CHECK(!normal.is_fresh(1501));
}

MT_TEST("timeline: extreme elapsed times complete or clamp without overflowing") {
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    PortalTimeline timeline;
    timeline.start(kMin);
    const PortalVisualState done = timeline.state_at(kMax);
    MT_CHECK(done.complete);
    MT_CHECK(done.progress == 1.0F && done.reveal == 1.0F && done.person_visibility == 1.0F);

    timeline.start(kMax);
    const PortalVisualState before = timeline.state_at(kMin);
    MT_CHECK(!before.complete && before.progress == 0.0F);
    MT_CHECK(before.person_visibility == 1.0F);

    timeline.start(1000);
    MT_CHECK(timeline.state_at(999).progress == 0.0F);
    MT_CHECK(timeline.state_at(2800).complete);
}

MT_TEST("refiner: extreme forward time gaps use the same bounded weight as ordinary long gaps") {
    MaskRefinerConfig config;
    config.morphology_radius = 0;
    config.bilateral_radius = 0;
    const RgbaFrame camera(4, 4);
    const ConfidenceMask empty(4, 4);
    const ConfidenceMask full(4, 4, std::vector<float>(16U, 1.0F));
    MaskRefiner ordinary(config);
    MaskRefiner extreme(config);
    (void)ordinary.refine(camera, empty, 0);
    (void)extreme.refine(camera, empty, std::numeric_limits<std::int64_t>::min());
    const auto expected = ordinary.refine(camera, full, 10000);
    const auto actual = extreme.refine(camera, full, std::numeric_limits<std::int64_t>::max());
    MT_CHECK(actual.alpha.values() == expected.alpha.values());
    MT_CHECK(actual.bounds.valid == expected.bounds.valid);

    MaskRefiner fresh(config);
    const auto unfiltered = fresh.refine(camera, empty, 100);
    const auto backwards = extreme.refine(camera, empty, std::numeric_limits<std::int64_t>::min());
    MT_CHECK(backwards.alpha.values() == unfiltered.alpha.values());
}

MT_TEST("timeline: every transition fraction is constrained to the unit interval") {
    float PortalTimelineConfig::*fractions[] = {
        &PortalTimelineConfig::ring_buildup_end, &PortalTimelineConfig::reveal_start,
        &PortalTimelineConfig::reveal_end,       &PortalTimelineConfig::ring_fade_start,
        &PortalTimelineConfig::vanish_start,     &PortalTimelineConfig::vanish_end,
        &PortalTimelineConfig::reappear_start,   &PortalTimelineConfig::reappear_end};
    for (auto field : fractions) {
        for (float invalid : {-0.01F, 1.01F}) {
            PortalTimelineConfig config;
            config.*field = invalid;
            MT_CHECK_THROWS(PortalTimeline{config}, std::invalid_argument);
        }
    }
    PortalTimelineConfig endpoints;
    endpoints.ring_buildup_end = 1.0F;
    endpoints.reveal_start = 0.0F;
    endpoints.reveal_end = 1.0F;
    endpoints.vanish_start = 0.0F;
    endpoints.reappear_end = 1.0F;
    PortalTimeline timeline(endpoints);
    timeline.start(0);
    MT_CHECK(timeline.state_at(endpoints.duration_ms).complete);
}

MT_TEST("style: every accepted person threshold has ordered shader guard edges") {
    for (float threshold : {0.0F, 0.5F, constants::kPersonGuardStart}) {
        PortalStyle style;
        style.person_protection = threshold;
        MT_CHECK_THROWS(PortalCompositor{style}, std::invalid_argument);
        EngineConfig config;
        config.style = style;
        MT_CHECK_THROWS(Engine{config}, std::invalid_argument);
    }
    for (float threshold : {std::nextafter(constants::kPersonGuardStart, 1.0F), 0.8F, 1.0F}) {
        PortalStyle style;
        style.person_protection = threshold;
        validate_style(style);
        EngineConfig config;
        config.style = style;
        Engine engine(config);
        MT_CHECK(engine.style().person_protection > constants::kPersonGuardStart);
    }
}

MT_TEST("compositor: invalid person bounds mean an empty scene, as in the engine and the shader") {
    const RgbaFrame camera = mp_test::make_camera(32, 32, 3);
    RgbaFrame destination(32, 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            destination.set_pixel(x, y, Rgba{200, 10, 10, 255});
        }
    }
    const RefinedPersonMask nobody{ConfidenceMask(8, 8), PersonBounds{}, 0};  // bounds.valid == false
    PortalVisualState done;
    done.progress = 1.0F;
    done.reveal = 1.0F;
    done.complete = true;
    CompositeInput input{&camera, nullptr, &destination, &nobody, done};
    const RgbaFrame out = PortalCompositor().compose(input);
    bool all_destination = true;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const Rgba a = out.pixel(x, y);
            all_destination = all_destination && a.r == 200 && a.g == 10 && a.b == 10;
        }
    }
    MT_CHECK(all_destination);  // nobody to protect: the whole view is the destination

    PortalVisualState middle;
    middle.progress = 0.5F;
    middle.reveal = 0.6F;
    middle.ring_scale = 1.0F;
    middle.ring_alpha = 0.8F;
    input.transition = middle;
    const RgbaFrame mid = PortalCompositor().compose(input);
    int changed = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const Rgba a = mid.pixel(x, y);
            const Rgba b = camera.pixel(x, y);
            changed += (a.r != b.r || a.g != b.g || a.b != b.b) ? 1 : 0;
        }
    }
    MT_CHECK(changed > 100);  // the effect plays instead of returning the plain camera
}

MT_TEST("compositor: mask_crop selects the visible part of the mask, as the shader does") {
    const RgbaFrame camera = mp_test::make_camera(32, 32, 9);
    RgbaFrame destination(32, 32);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            destination.set_pixel(x, y, Rgba{10, 200, 10, 255});
        }
    }
    ConfidenceMask left_half(8, 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 4; ++x) {
            left_half.set(x, y, 1.0F);
        }
    }
    const RefinedPersonMask person{left_half, PersonBounds{0.0F, 0.0F, 0.5F, 1.0F, true}, 0};
    PortalVisualState done;
    done.progress = 1.0F;
    done.reveal = 1.0F;
    done.complete = true;
    CompositeInput input{&camera, nullptr, &destination, &person, done};

    const RgbaFrame whole = PortalCompositor().compose(input);  // default: the whole mask
    MT_CHECK(whole.pixel(4, 16).r == camera.pixel(4, 16).r);    // left half: the person
    MT_CHECK(whole.pixel(28, 16).g == 200);                     // right half: destination

    input.mask_crop[0] = 0.5F;  // only the right half of the mask is visible: nobody on screen
    const RgbaFrame cropped = PortalCompositor().compose(input);
    // Probe away from the mask edge, where the silhouette glow legitimately applies.
    MT_CHECK(cropped.pixel(12, 16).g == 200);
    MT_CHECK(cropped.pixel(28, 16).g == 200);

    input.mask_crop[0] = 0.0F;
    input.mask_crop[2] = 0.5F;  // only the left half is visible: the person fills the screen
    const RgbaFrame zoomed = PortalCompositor().compose(input);
    MT_CHECK(zoomed.pixel(20, 16).r == camera.pixel(20, 16).r);
    MT_CHECK(zoomed.pixel(2, 16).r == camera.pixel(2, 16).r);

    input.mask_crop[2] = 0.0F;  // empty rectangle
    MT_CHECK_THROWS(PortalCompositor().compose(input), std::invalid_argument);
    input.mask_crop[2] = std::numeric_limits<float>::quiet_NaN();
    MT_CHECK_THROWS(PortalCompositor().compose(input), std::invalid_argument);
}

MT_TEST("freshness: a mask from the future is not trusted beyond the configured tolerance") {
    MaskFreshnessConfig config;
    config.stale_after_ms = 400;
    config.future_tolerance_ms = 50;
    MaskFreshness freshness(config);
    freshness.on_valid_mask(1000);
    MT_CHECK(freshness.is_fresh(950));   // 50 ms ahead of "now": within tolerance
    MT_CHECK(!freshness.is_fresh(949));  // 51 ms ahead
    MT_CHECK(freshness.is_fresh(1400));
    MT_CHECK(!freshness.is_fresh(1401));
    MT_CHECK(freshness.age_ms(900) == -100);  // negative: stamped in the future
    MT_CHECK(freshness.fresh_at(1000, 1000) && !freshness.fresh_at(1000, 5000));
    config.future_tolerance_ms = -1;
    MT_CHECK_THROWS(MaskFreshness(config), std::invalid_argument);
}

MT_TEST("limits: sizes are range-checked in 64-bit arithmetic") {
    MT_CHECK(frame_size_ok(1920, 1080) && frame_size_ok(16384, 1));
    MT_CHECK(!frame_size_ok(0, 10) && !frame_size_ok(10, -1));
    MT_CHECK(!frame_size_ok(16385, 1));
    MT_CHECK(!frame_size_ok(10000, 10000));  // within each side limit, beyond the pixel budget
    MT_CHECK(!frame_size_ok(INT32_MAX, INT32_MAX));
    MT_CHECK(mask_size_ok(256, 256) && !mask_size_ok(4097, 1) && !mask_size_ok(0, 1));
    std::int64_t stride = 0;
    MT_CHECK(resolve_rgba_stride(100, 0, &stride) && stride == 400);
    MT_CHECK(resolve_rgba_stride(100, 512, &stride) && stride == 512);
    MT_CHECK(!resolve_rgba_stride(100, 399, &stride));
    MT_CHECK(!resolve_rgba_stride(100, -400, &stride));
    MT_CHECK(!resolve_rgba_stride(INT32_MAX, 0, &stride));
    MT_CHECK(saturating_sub(INT64_MAX, -1) == INT64_MAX && saturating_sub(INT64_MIN, 1) == INT64_MIN);
    MT_CHECK(saturating_sub(10, 3) == 7 && saturating_sub(-10, -3) == -7);
}

MT_TEST("config validation: NaN, infinity and out-of-range values are rejected everywhere") {
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInf = std::numeric_limits<float>::infinity();
    PortalStyle style;
    MT_CHECK_THROWS(validate_style([&] {
                        auto s = style;
                        s.ring_width = 0.0F;
                        return s;
                    }()),
                    std::invalid_argument);
    MT_CHECK_THROWS(validate_style([&] {
                        auto s = style;
                        s.ring_width = kNaN;
                        return s;
                    }()),
                    std::invalid_argument);
    MT_CHECK_THROWS(validate_style([&] {
                        auto s = style;
                        s.ring_width = 0.9F;
                        return s;
                    }()),
                    std::invalid_argument);
    MT_CHECK_THROWS(validate_style([&] {
                        auto s = style;
                        s.reveal_feather = kInf;
                        return s;
                    }()),
                    std::invalid_argument);
    MT_CHECK_THROWS(validate_style([&] {
                        auto s = style;
                        s.ring_strength = -0.1F;
                        return s;
                    }()),
                    std::invalid_argument);
    MT_CHECK_THROWS(validate_style([&] {
                        auto s = style;
                        s.person_protection = 0.0F;
                        return s;
                    }()),
                    std::invalid_argument);
    validate_style(style);  // the defaults are fine

    MaskRefinerConfig refiner;
    refiner.spatial_sigma = kNaN;
    MT_CHECK_THROWS(MaskRefiner{refiner}, std::invalid_argument);
    refiner = MaskRefinerConfig{};
    refiner.bilateral_radius = kMaxRefinerRadius + 1;
    MT_CHECK_THROWS(MaskRefiner{refiner}, std::invalid_argument);
    refiner = MaskRefinerConfig{};
    refiner.temporal_attack = kNaN;
    MT_CHECK_THROWS(MaskRefiner{refiner}, std::invalid_argument);

    PortalTimelineConfig timeline;
    timeline.reveal_end = kNaN;
    MT_CHECK_THROWS(PortalTimeline{timeline}, std::invalid_argument);

    // The engine applies all of them at construction.
    EngineConfig engine;
    engine.style.ring_width = 0.0F;
    MT_CHECK_THROWS(Engine{engine}, std::invalid_argument);
    engine = EngineConfig{};
    engine.mask_refiner.spatial_sigma = kNaN;
    MT_CHECK_THROWS(Engine{engine}, std::invalid_argument);
}
