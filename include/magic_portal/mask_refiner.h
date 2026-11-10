#pragma once

#include <cstdint>
#include <vector>

#include "magic_portal/frame.h"

namespace magic_portal {

// Upper limits accepted for the filter radii (pixels of the model mask) and the two sigmas.
// The cost of a frame grows with the square of a radius, so larger values only make sense as
// mistakes.
constexpr int kMaxRefinerRadius = 16;
constexpr float kMaxRefinerSigma = 1000.0F;

struct MaskRefinerConfig {
    int morphology_radius = 1;
    int bilateral_radius = 2;
    float spatial_sigma = 1.6F;
    float color_sigma = 42.0F;
    float temporal_attack = 0.72F;
    float temporal_release = 0.28F;
    float low_confidence = 0.20F;
    float high_confidence = 0.78F;
    float exact_background_cutoff = 0.015F;
    float exact_person_cutoff = 0.985F;
    float bounds_threshold = 0.55F;
    float minimum_person_area_fraction = 0.01F;
};

struct RefinedPersonMask {
    ConfidenceMask alpha;
    PersonBounds bounds;
    std::int64_t timestamp_ms = 0;
};

// Not thread-safe: one instance must be driven from a single thread at a time.
class MaskRefiner {
public:
    explicit MaskRefiner(MaskRefinerConfig config = {});

    [[nodiscard]] RefinedPersonMask refine(const RgbaFrame &display_oriented_frame,
                                           const ConfidenceMask &raw_confidence, std::int64_t timestamp_ms);
    void reset() noexcept;

private:
    MaskRefinerConfig config_;
    ConfidenceMask previous_;
    std::int64_t previous_timestamp_ms_ = -1;

    // Per-instance scratch space, reused across frames to avoid allocation churn.
    ConfidenceMask scratch_a_;
    ConfidenceMask scratch_b_;
    ConfidenceMask scratch_c_;
    ConfidenceMask current_;
    std::vector<float> guide_;
};

}  // namespace magic_portal
