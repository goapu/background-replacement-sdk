#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "magic_portal/mask_refiner.h"
#include "magic_portal/pipeline.h"
#include "magic_portal/portal_compositor.h"
#include "magic_portal/portal_timeline.h"

namespace {

using magic_portal::CompositeInput;
using magic_portal::ConfidenceMask;
using magic_portal::MaskRefiner;
using magic_portal::MaskRefinerConfig;
using magic_portal::PersonBounds;
using magic_portal::PortalCompositor;
using magic_portal::PortalTimeline;
using magic_portal::PortalTimelineConfig;
using magic_portal::PortalVisualState;
using magic_portal::RefinedPersonMask;
using magic_portal::Rgba;
using magic_portal::RgbaFrame;

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool equals(Rgba left, Rgba right) {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

RgbaFrame solid(int width, int height, Rgba color) {
    RgbaFrame frame(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            frame.set_pixel(x, y, color);
        }
    }
    return frame;
}

void test_frame_contract() {
    bool threw = false;
    try {
        const RgbaFrame invalid(0, 10);
        (void)invalid;
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    check(threw, "zero-sized frames must be rejected");
    RgbaFrame frame(2, 2);
    const Rgba expected{12, 34, 56, 78};
    frame.set_pixel(1, 0, expected);
    check(equals(frame.pixel(1, 0), expected), "RGBA frame must round-trip exact bytes");
}

void test_mask_refinement_and_bounds() {
    const RgbaFrame camera = solid(64, 64, {90, 100, 110, 255});
    ConfidenceMask raw(16, 16);
    for (int y = 3; y < 15; ++y) {
        for (int x = 5; x < 12; ++x) {
            raw.set(x, y, 1.0F);
        }
    }
    MaskRefinerConfig config;
    config.morphology_radius = 0;
    config.bilateral_radius = 0;
    config.low_confidence = 0.2F;
    config.high_confidence = 0.8F;
    MaskRefiner refiner(config);
    const RefinedPersonMask refined = refiner.refine(camera, raw, 1000);
    check(refined.alpha.at(7, 7) == 1.0F, "solid person confidence must become exact foreground");
    check(refined.alpha.at(0, 0) == 0.0F, "solid background confidence must become exact background");
    check(refined.bounds.valid, "person bounds must be reported");
    check(std::abs(refined.bounds.min_x - 5.0F / 16.0F) < 0.001F,
          "person minimum x bound must be normalized");
    check(std::abs(refined.bounds.max_y - 15.0F / 16.0F) < 0.001F,
          "person maximum y bound must be normalized");
}

void test_temporal_asymmetry() {
    const RgbaFrame camera = solid(16, 16, {100, 100, 100, 255});
    MaskRefinerConfig config;
    config.morphology_radius = 0;
    config.bilateral_radius = 0;
    config.low_confidence = 0.0F;
    config.high_confidence = 1.0F;
    config.exact_background_cutoff = 0.0F;
    config.exact_person_cutoff = 1.0F;
    config.temporal_attack = 0.8F;
    config.temporal_release = 0.2F;
    MaskRefiner refiner(config);
    ConfidenceMask off(4, 4);
    ConfidenceMask on(4, 4);
    for (float &value : on.values()) {
        value = 1.0F;
    }
    (void)refiner.refine(camera, off, 1000);
    const float rising = refiner.refine(camera, on, 1033).alpha.at(1, 1);
    const float falling = refiner.refine(camera, off, 1066).alpha.at(1, 1);
    check(rising > 0.80F, "mask attack must respond quickly when the person enters a pixel");
    check(falling > 0.50F, "mask release must be slower to prevent holes and hair flicker");
}

void test_timeline() {
    PortalTimeline timeline(PortalTimelineConfig{});
    check(!timeline.state_at(100).complete, "inactive timeline must not report completion");
    timeline.start(1000);
    const PortalVisualState start = timeline.state_at(1000);
    const PortalVisualState middle = timeline.state_at(1900);
    const PortalVisualState end = timeline.state_at(3000);
    check(start.progress == 0.0F, "timeline must start at zero");
    check(middle.ring_alpha > 0.0F && middle.reveal > 0.0F,
          "middle of transition must show ring and background reveal");
    check(start.person_visibility == 1.0F, "person must be visible before the vanish phase");
    check(middle.person_visibility == 0.0F, "person must be fully absent during the magical transition");
    check(end.person_visibility == 1.0F, "person must reappear by the end of the transition");
    check(end.complete && end.reveal == 1.0F, "timeline must finish with a complete reveal");
}

void test_person_can_vanish() {
    const RgbaFrame camera = solid(32, 32, {40, 80, 120, 255});
    const RgbaFrame next = solid(32, 32, {8, 210, 44, 255});
    ConfidenceMask alpha(8, 8);
    for (int y = 2; y < 7; ++y) {
        for (int x = 2; x < 6; ++x) {
            alpha.set(x, y, 1.0F);
        }
    }
    RefinedPersonMask person{alpha, PersonBounds{0.25F, 0.25F, 0.75F, 0.875F, true}, 2000};
    PortalVisualState state;
    state.progress = 0.5F;
    state.reveal = 0.35F;
    state.ring_scale = 1.0F;
    state.ring_alpha = 0.0F;
    state.person_visibility = 0.0F;
    CompositeInput input{&camera, nullptr, &next, &person, state};
    const RgbaFrame output = PortalCompositor().compose(input);
    check(equals(output.pixel(16, 16), Rgba{8, 210, 44, 255}),
          "vanished person pixels must expose the destination background");
}

void test_person_pixels_are_preserved() {
    RgbaFrame camera = solid(32, 32, {40, 80, 120, 255});
    camera.set_pixel(16, 16, {17, 33, 201, 255});
    const RgbaFrame next = solid(12, 20, {8, 210, 44, 255});
    ConfidenceMask alpha(8, 8);
    for (int y = 2; y < 7; ++y) {
        for (int x = 2; x < 6; ++x) {
            alpha.set(x, y, 1.0F);
        }
    }
    RefinedPersonMask person{alpha, PersonBounds{0.25F, 0.25F, 0.75F, 0.875F, true}, 2000};
    PortalVisualState state;
    state.progress = 1.0F;
    state.reveal = 1.0F;
    state.ring_scale = 1.0F;
    state.complete = true;
    CompositeInput input{&camera, nullptr, &next, &person, state};
    const RgbaFrame output = PortalCompositor().compose(input);
    check(equals(output.pixel(16, 16), camera.pixel(16, 16)),
          "high-confidence person pixels must be copied byte-for-byte");
    check(equals(output.pixel(0, 0), Rgba{8, 210, 44, 255}),
          "completed transition must replace pixels outside the person mask");
}

void test_missing_person_is_an_empty_scene() {
    const RgbaFrame camera = solid(16, 16, {33, 44, 55, 255});
    const RgbaFrame next = solid(16, 16, {220, 10, 90, 255});
    ConfidenceMask alpha(4, 4);
    RefinedPersonMask person{alpha, PersonBounds{}, 2000};
    PortalVisualState state;
    state.progress = 1.0F;
    state.reveal = 1.0F;
    state.complete = true;
    CompositeInput input{&camera, nullptr, &next, &person, state};
    const RgbaFrame output = PortalCompositor().compose(input);
    check(equals(output.pixel(0, 0), Rgba{220, 10, 90, 255}) &&
              equals(output.pixel(15, 15), Rgba{220, 10, 90, 255}),
          "with nobody in view the completed transition replaces the whole view, as on the GPU");
}

void test_pipeline_end_to_end() {
    const RgbaFrame camera = solid(48, 64, {40, 40, 40, 255});
    const RgbaFrame next = solid(48, 64, {180, 20, 90, 255});
    ConfidenceMask raw(12, 16);
    for (int y = 2; y < 15; ++y) {
        for (int x = 4; x < 9; ++x) {
            raw.set(x, y, 1.0F);
        }
    }
    magic_portal::MagicPortalPipeline pipeline;
    pipeline.start_transition(0);
    const auto output = pipeline.process(camera, raw, nullptr, next, 1800);
    check(output.transition.complete, "pipeline must propagate completed timeline state");
    check(output.frame.width() == camera.width() && output.frame.height() == camera.height(),
          "pipeline output must match camera dimensions");
}

}  // namespace

int main() {
    try {
        test_frame_contract();
        test_mask_refinement_and_bounds();
        test_temporal_asymmetry();
        test_timeline();
        test_person_pixels_are_preserved();
        test_person_can_vanish();
        test_missing_person_is_an_empty_scene();
        test_pipeline_end_to_end();
    } catch (const std::exception &exception) {
        std::cerr << "Unexpected exception: " << exception.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All magic portal tests passed\n";
    return 0;
}
