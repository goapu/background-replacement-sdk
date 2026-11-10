// Regression test against recorded outputs. Run with `--update <dir>` to
// re-record, or `--check <dir>` (default in CTest) to compare.
//
// Provenance: refine.bin and timeline.bin were recorded from the original implementation
// before any refactoring and have not changed since. compose.bin was re-recorded once when
// the dissolve/spark noise moved from a sine hash to an exact integer hash and the CPU
// reference adopted the shader's bottom-left convention for its angle and noise terms. The
// first and last frames of that scenario (live camera, finished transition) are byte-identical
// to the original recording; only the noise and ring pixels in between changed.
//
// Byte blobs are compared with a small per-byte tolerance and float blobs with a
// small absolute tolerance, so results stay portable across compilers and libm
// versions while still catching any real behavioural change.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "golden_support.h"
#include "magic_portal/mask_refiner.h"
#include "magic_portal/portal_compositor.h"
#include "magic_portal/portal_timeline.h"

namespace {

using magic_portal::CompositeInput;
using magic_portal::ConfidenceMask;
using magic_portal::MaskRefiner;
using magic_portal::PortalCompositor;
using magic_portal::PortalTimeline;
using magic_portal::PortalVisualState;
using magic_portal::RefinedPersonMask;
using magic_portal::RgbaFrame;

constexpr int kByteTolerance = 3;
constexpr float kFloatTolerance = 2.0e-4F;

std::vector<std::uint8_t> scenario_refine() {
    const RgbaFrame camera = mp_test::make_camera(96, 128, 11U);
    MaskRefiner refiner;
    RefinedPersonMask last;
    for (int i = 0; i < 5; ++i) {
        const ConfidenceMask raw =
            mp_test::make_ellipse_mask(24, 32, 0.40F + 0.04F * static_cast<float>(i), 0.55F, 0.26F, 0.40F,
                                       100U + static_cast<std::uint32_t>(i));
        last = refiner.refine(camera, raw, 1000 + 66 * i);
    }
    std::vector<std::uint8_t> bytes;
    for (float value : last.alpha.values()) {
        bytes.push_back(static_cast<std::uint8_t>(value * 255.0F + 0.5F));
    }
    mp_test::append_float(&bytes, last.bounds.valid ? 1.0F : 0.0F);
    mp_test::append_float(&bytes, last.bounds.min_x);
    mp_test::append_float(&bytes, last.bounds.min_y);
    mp_test::append_float(&bytes, last.bounds.max_x);
    mp_test::append_float(&bytes, last.bounds.max_y);
    return bytes;
}

std::vector<std::uint8_t> scenario_compose() {
    const RgbaFrame camera = mp_test::make_camera(64, 96, 5U);
    const RgbaFrame next = mp_test::make_background(80, 60, 0);
    const RgbaFrame previous = mp_test::make_background(40, 90, 1);
    const ConfidenceMask raw = mp_test::make_ellipse_mask(16, 24, 0.45F, 0.58F, 0.24F, 0.38F, 7U);
    MaskRefiner refiner;
    const RefinedPersonMask person = refiner.refine(camera, raw, 500);

    PortalTimeline timeline;
    timeline.start(0);
    const PortalCompositor compositor;
    std::vector<std::uint8_t> bytes;
    const int stamps[] = {0, 360, 720, 1080, 1440, 1800};
    for (int stamp : stamps) {
        CompositeInput input{&camera, nullptr, &next, &person, timeline.state_at(stamp)};
        const RgbaFrame out = compositor.compose(input);
        bytes.insert(bytes.end(), out.pixels().begin(), out.pixels().end());
    }
    CompositeInput with_previous{&camera, &previous, &next, &person, timeline.state_at(1080)};
    const RgbaFrame out = compositor.compose(with_previous);
    bytes.insert(bytes.end(), out.pixels().begin(), out.pixels().end());
    return bytes;
}

std::vector<std::uint8_t> scenario_timeline() {
    PortalTimeline timeline;
    timeline.start(250);
    std::vector<std::uint8_t> bytes;
    for (int stamp = 200; stamp <= 2300; stamp += 100) {
        const PortalVisualState s = timeline.state_at(stamp);
        for (float v : {s.progress, s.reveal, s.ring_scale, s.ring_alpha, s.spin_turns, s.person_visibility,
                        s.complete ? 1.0F : 0.0F}) {
            mp_test::append_float(&bytes, v);
        }
    }
    return bytes;
}

struct Scenario {
    const char *name;
    bool is_float;
    std::vector<std::uint8_t> (*run)();
};

const Scenario kScenarios[] = {
    {"refine.bin", false, scenario_refine},
    {"compose.bin", false, scenario_compose},
    {"timeline.bin", true, scenario_timeline},
};

bool compare(const Scenario &scenario, const std::vector<std::uint8_t> &actual,
             const std::vector<std::uint8_t> &expected) {
    if (actual.size() != expected.size()) {
        std::fprintf(stderr, "FAIL %s: size %zu != golden %zu\n", scenario.name, actual.size(),
                     expected.size());
        return false;
    }
    if (scenario.is_float) {
        float worst = 0.0F;
        for (std::size_t i = 0; i < actual.size() / sizeof(float); ++i) {
            worst =
                std::max(worst, std::abs(mp_test::read_float(actual, i) - mp_test::read_float(expected, i)));
        }
        if (worst > kFloatTolerance) {
            std::fprintf(stderr, "FAIL %s: max float diff %g\n", scenario.name, static_cast<double>(worst));
            return false;
        }
        std::printf("ok   %s (max float diff %g)\n", scenario.name, static_cast<double>(worst));
        return true;
    }
    // refine.bin ends with five floats; compare the byte part and the float tail separately.
    const std::size_t tail = std::strcmp(scenario.name, "refine.bin") == 0 ? 5U * sizeof(float) : 0U;
    const std::size_t body = actual.size() - tail;
    int worst = 0;
    std::size_t over = 0;
    for (std::size_t i = 0; i < body; ++i) {
        const int diff = std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i]));
        worst = std::max(worst, diff);
        over += diff > kByteTolerance ? 1U : 0U;
    }
    float worst_tail = 0.0F;
    for (std::size_t i = 0; i < tail / sizeof(float); ++i) {
        const std::vector<std::uint8_t> a(actual.begin() + static_cast<std::ptrdiff_t>(body), actual.end());
        const std::vector<std::uint8_t> e(expected.begin() + static_cast<std::ptrdiff_t>(body),
                                          expected.end());
        worst_tail = std::max(worst_tail, std::abs(mp_test::read_float(a, i) - mp_test::read_float(e, i)));
    }
    if (over != 0 || worst_tail > kFloatTolerance) {
        std::fprintf(stderr, "FAIL %s: %zu bytes over tolerance (worst %d), tail diff %g\n", scenario.name,
                     over, worst, static_cast<double>(worst_tail));
        return false;
    }
    std::printf("ok   %s (max byte diff %d)\n", scenario.name, worst);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3 || (std::strcmp(argv[1], "--update") != 0 && std::strcmp(argv[1], "--check") != 0)) {
        std::fprintf(stderr, "usage: %s --update|--check <golden-dir>\n", argv[0]);
        return 2;
    }
    const bool update = std::strcmp(argv[1], "--update") == 0;
    const std::string dir = argv[2];
    int failures = 0;
    for (const Scenario &scenario : kScenarios) {
        const std::vector<std::uint8_t> actual = scenario.run();
        const std::string path = dir + "/" + scenario.name;
        if (update) {
            if (!mp_test::write_file(path, actual)) {
                std::fprintf(stderr, "cannot write %s\n", path.c_str());
                return 2;
            }
            std::printf("wrote %s (%zu bytes)\n", path.c_str(), actual.size());
            continue;
        }
        std::vector<std::uint8_t> expected;
        if (!mp_test::read_file(path, &expected)) {
            std::fprintf(stderr, "FAIL %s: golden file missing\n", path.c_str());
            ++failures;
            continue;
        }
        failures += compare(scenario, actual, expected) ? 0 : 1;
    }
    return failures == 0 ? 0 : 1;
}
