// Micro-benchmarks for the per-frame stages, on deterministic synthetic input.
//
// Numbers are only comparable on the same machine and build type. They are NOT a substitute
// for measuring on target phones with the real model; they exist to catch regressions and to
// show where CPU time goes. Usage: magic_portal_bench [--frames N] [--json]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "golden_support.h"
#include "magic_portal/engine.h"
#include "magic_portal/mask_refiner.h"
#include "magic_portal/portal_compositor.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Result {
    std::string name;
    double mean_ms = 0.0;
    double best_ms = 0.0;
};

template <typename Fn>
Result measure(const std::string &name, int frames, Fn &&fn) {
    fn();  // warm-up
    double total = 0.0;
    double best = 1.0e30;
    for (int i = 0; i < frames; ++i) {
        const auto start = Clock::now();
        fn();
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        total += ms;
        best = ms < best ? ms : best;
    }
    return {name, total / static_cast<double>(frames), best};
}

}  // namespace

int main(int argc, char **argv) {
    int frames = 30;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--json") == 0) {
            json = true;
        } else {
            std::fprintf(stderr, "usage: %s [--frames N] [--json]\n", argv[0]);
            return 2;
        }
    }

    using namespace magic_portal;
    const RgbaFrame full = mp_test::make_camera(1080, 1440, 1);
    const RgbaFrame small = mp_test::make_camera(256, 256, 2);
    const ConfidenceMask raw = mp_test::make_ellipse_mask(256, 256, 0.5F, 0.55F, 0.28F, 0.42F, 3);
    const RgbaFrame background = mp_test::make_background(540, 720, 1);
    const RgbaFrame camera540 = mp_test::make_camera(540, 720, 4);

    std::vector<Result> results;
    RgbaFrame scratch;
    results.push_back(measure("copy 1080x1440 (strided)", frames, [&] {
        copy_rgba_strided(full.data(), 1080, 1440, 1080 * 4, false, &scratch);
    }));
    results.push_back(measure("copy+mirror 1080x1440", frames,
                              [&] { copy_rgba_strided(full.data(), 1080, 1440, 1080 * 4, true, &scratch); }));
    MaskRefiner refiner;
    results.push_back(measure("refine 256x256", frames, [&] { (void)refiner.refine(small, raw, 0); }));
    Engine engine{EngineConfig{}};
    std::int64_t t = 0;
    FrameInput input;
    input.rgba = full.data();
    input.width = 1080;
    input.height = 1440;
    input.stride_bytes = 1080 * 4;
    results.push_back(measure("engine.process_frame 1080x1440 + 256x256 mask", frames, [&] {
        input.timestamp_ms = (t += 66);
        (void)engine.process_frame(input, &raw);
    }));
    results.push_back(measure("engine.render_state", frames * 100, [&] { (void)engine.render_state(t); }));
    {
        PortalCompositor compositor;
        MaskRefiner local;
        const RefinedPersonMask person = local.refine(camera540, raw, 0);
        PortalVisualState state;
        state.progress = 0.5F;
        state.reveal = 0.5F;
        state.ring_scale = 0.7F;
        state.ring_alpha = 0.8F;
        state.spin_turns = 0.6F;
        CompositeInput composite;
        composite.camera = &camera540;
        composite.next_background = &background;
        composite.person = &person;
        composite.transition = state;
        results.push_back(measure("cpu reference compose 540x720", std::max(3, frames / 6),
                                  [&] { (void)compositor.compose(composite); }));
    }

    if (json) {
        std::printf("[\n");
        for (std::size_t i = 0; i < results.size(); ++i) {
            std::printf("  {\"stage\": \"%s\", \"mean_ms\": %.4f, \"best_ms\": %.4f}%s\n",
                        results[i].name.c_str(), results[i].mean_ms, results[i].best_ms,
                        i + 1 < results.size() ? "," : "");
        }
        std::printf("]\n");
    } else {
        std::printf("%-48s %12s %12s\n", "stage", "mean (ms)", "best (ms)");
        for (const Result &r : results) {
            std::printf("%-48s %12.3f %12.3f\n", r.name.c_str(), r.mean_ms, r.best_ms);
        }
    }
    return 0;
}
