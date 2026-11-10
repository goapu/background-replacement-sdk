#include "magic_portal/mask_refiner.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "magic_portal/limits.h"

namespace magic_portal {
namespace {

float smoothstep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

float sanitize(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0F, 1.0F) : 0.0F;
}

std::size_t index_of(int x, int y, int width) {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
}

// One separable pass of a square max/min filter with clamp-to-edge borders. Two passes
// (horizontal then vertical) are exactly equivalent to the full (2r+1)^2 window.
void morphology_pass(const ConfidenceMask &source, ConfidenceMask *destination, int radius, bool dilate,
                     bool horizontal) {
    const int width = source.width();
    const int height = source.height();
    destination->reshape(width, height);
    const float *in = source.data();
    float *out = destination->data();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float best = dilate ? 0.0F : 1.0F;
            for (int d = -radius; d <= radius; ++d) {
                const int sx = horizontal ? std::clamp(x + d, 0, width - 1) : x;
                const int sy = horizontal ? y : std::clamp(y + d, 0, height - 1);
                const float value = in[index_of(sx, sy, width)];
                best = dilate ? std::max(best, value) : std::min(best, value);
            }
            out[index_of(x, y, width)] = sanitize(best);
        }
    }
}

// Morphological closing (dilate then erode). Returns the buffer holding the result; when
// the radius is zero that is the input itself.
const ConfidenceMask *close_mask(const ConfidenceMask &raw, int radius, ConfidenceMask *a, ConfidenceMask *b,
                                 ConfidenceMask *c) {
    if (radius <= 0) {
        return &raw;
    }
    morphology_pass(raw, a, radius, true, true);
    morphology_pass(*a, b, radius, true, false);
    morphology_pass(*b, a, radius, false, true);
    morphology_pass(*a, c, radius, false, false);
    return c;
}

void make_guide(const RgbaFrame &camera, int width, int height, std::vector<float> *guide) {
    guide->resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
    float *out = guide->data();
    for (int y = 0; y < height; ++y) {
        const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
        for (int x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
            const Rgba color = sample_bilinear(camera, u, v);
            float *pixel = out + index_of(x, y, width) * 3U;
            pixel[0] = static_cast<float>(color.r);
            pixel[1] = static_cast<float>(color.g);
            pixel[2] = static_cast<float>(color.b);
        }
    }
}

void joint_bilateral(const ConfidenceMask &source, const RgbaFrame &camera, int radius, float spatial_sigma,
                     float color_sigma, std::vector<float> *guide, ConfidenceMask *destination) {
    const int width = source.width();
    const int height = source.height();
    destination->reshape(width, height);
    float *out = destination->data();
    const float *in = source.data();
    if (radius <= 0) {
        std::copy(in, in + source.size(), out);
        return;
    }
    make_guide(camera, width, height, guide);
    const float *g = guide->data();
    const float spatial_denominator = std::max(2.0F * spatial_sigma * spatial_sigma, 1.0e-6F);
    const float color_denominator = std::max(2.0F * color_sigma * color_sigma, 1.0e-6F);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float *center = g + index_of(x, y, width) * 3U;
            float weighted_sum = 0.0F;
            float weight_total = 0.0F;
            for (int dy = -radius; dy <= radius; ++dy) {
                const int sy = std::clamp(y + dy, 0, height - 1);
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int sx = std::clamp(x + dx, 0, width - 1);
                    const float *neighbor = g + index_of(sx, sy, width) * 3U;
                    const float spatial_distance = static_cast<float>(dx * dx + dy * dy);
                    const float dr = center[0] - neighbor[0];
                    const float dg = center[1] - neighbor[1];
                    const float db = center[2] - neighbor[2];
                    const float color_distance = (dr * dr + dg * dg + db * db) / 3.0F;
                    const float weight = std::exp(-spatial_distance / spatial_denominator -
                                                  color_distance / color_denominator);
                    weighted_sum += in[index_of(sx, sy, width)] * weight;
                    weight_total += weight;
                }
            }
            out[index_of(x, y, width)] =
                sanitize(weight_total > 0.0F ? weighted_sum / weight_total : in[index_of(x, y, width)]);
        }
    }
}

float frame_rate_independent_weight(float nominal_weight, std::int64_t delta_ms) {
    const float frames_at_30_hz = std::clamp(static_cast<float>(delta_ms) / 33.3333F, 0.25F, 4.0F);
    return 1.0F - std::pow(1.0F - std::clamp(nominal_weight, 0.0F, 1.0F), frames_at_30_hz);
}

PersonBounds measure_bounds(const ConfidenceMask &mask, float threshold, float minimum_area_fraction) {
    const int width = mask.width();
    const int height = mask.height();
    const float *values = mask.data();
    int min_x = width;
    int min_y = height;
    int max_x = -1;
    int max_y = -1;
    std::size_t foreground_pixels = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (values[index_of(x, y, width)] >= threshold) {
                ++foreground_pixels;
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
        }
    }
    const float area_fraction = static_cast<float>(foreground_pixels) / static_cast<float>(mask.size());
    if (max_x < min_x || max_y < min_y || area_fraction < minimum_area_fraction) {
        return {};
    }
    const float inverse_width = 1.0F / static_cast<float>(width);
    const float inverse_height = 1.0F / static_cast<float>(height);
    PersonBounds bounds;
    bounds.min_x = std::clamp(static_cast<float>(min_x) * inverse_width, 0.0F, 1.0F);
    bounds.min_y = std::clamp(static_cast<float>(min_y) * inverse_height, 0.0F, 1.0F);
    bounds.max_x = std::clamp(static_cast<float>(max_x + 1) * inverse_width, 0.0F, 1.0F);
    bounds.max_y = std::clamp(static_cast<float>(max_y + 1) * inverse_height, 0.0F, 1.0F);
    bounds.valid = true;
    return bounds;
}

}  // namespace

namespace {

// Comparisons with NaN are false, so a NaN fails every range test below.
bool in_range(float value, float lo, float hi) {
    return std::isfinite(value) && value >= lo && value <= hi;
}

}  // namespace

MaskRefiner::MaskRefiner(MaskRefinerConfig config) : config_(config) {
    const MaskRefinerConfig &c = config_;
    const bool valid =
        c.morphology_radius >= 0 && c.morphology_radius <= kMaxRefinerRadius && c.bilateral_radius >= 0 &&
        c.bilateral_radius <= kMaxRefinerRadius && in_range(c.spatial_sigma, 1.0e-3F, kMaxRefinerSigma) &&
        in_range(c.color_sigma, 1.0e-3F, kMaxRefinerSigma) && in_range(c.temporal_attack, 0.0F, 1.0F) &&
        in_range(c.temporal_release, 0.0F, 1.0F) && in_range(c.low_confidence, 0.0F, 1.0F) &&
        in_range(c.high_confidence, 0.0F, 1.0F) && c.high_confidence > c.low_confidence &&
        in_range(c.exact_background_cutoff, 0.0F, 1.0F) && in_range(c.exact_person_cutoff, 0.0F, 1.0F) &&
        c.exact_background_cutoff < c.exact_person_cutoff && in_range(c.bounds_threshold, 0.0F, 1.0F) &&
        in_range(c.minimum_person_area_fraction, 0.0F, 1.0F);
    if (!valid) {
        throw std::invalid_argument(
            "invalid person-mask refinement configuration (values must be finite, in range, and radii "
            "at most 16)");
    }
}

RefinedPersonMask MaskRefiner::refine(const RgbaFrame &display_oriented_frame,
                                      const ConfidenceMask &raw_confidence, std::int64_t timestamp_ms) {
    if (display_oriented_frame.empty() || raw_confidence.empty()) {
        throw std::invalid_argument("mask refinement requires a camera frame and confidence mask");
    }

    const ConfidenceMask *closed =
        close_mask(raw_confidence, config_.morphology_radius, &scratch_a_, &scratch_b_, &scratch_c_);
    joint_bilateral(*closed, display_oriented_frame, config_.bilateral_radius, config_.spatial_sigma,
                    config_.color_sigma, &guide_, &current_);

    const bool can_temporally_filter = !previous_.empty() && previous_.width() == current_.width() &&
                                       previous_.height() == current_.height() &&
                                       timestamp_ms > previous_timestamp_ms_;
    if (can_temporally_filter) {
        const std::int64_t delta_ms = saturating_sub(timestamp_ms, previous_timestamp_ms_);
        const float attack = frame_rate_independent_weight(config_.temporal_attack, delta_ms);
        const float release = frame_rate_independent_weight(config_.temporal_release, delta_ms);
        float *now = current_.data();
        const float *old_values = previous_.data();
        for (std::size_t i = 0; i < current_.size(); ++i) {
            const float current = now[i];
            const float old = old_values[i];
            const float weight = current >= old ? attack : release;
            now[i] = old + (current - old) * weight;
        }
    }

    ConfidenceMask alpha(current_.width(), current_.height());
    const float *filtered = current_.data();
    float *alpha_values = alpha.data();
    for (std::size_t i = 0; i < current_.size(); ++i) {
        float value = smoothstep(config_.low_confidence, config_.high_confidence, filtered[i]);
        if (value <= config_.exact_background_cutoff) {
            value = 0.0F;
        } else if (value >= config_.exact_person_cutoff) {
            value = 1.0F;
        }
        alpha_values[i] = value;
    }

    const PersonBounds bounds =
        measure_bounds(alpha, config_.bounds_threshold, config_.minimum_person_area_fraction);
    std::swap(previous_, current_);
    previous_timestamp_ms_ = timestamp_ms;
    return {std::move(alpha), bounds, timestamp_ms};
}

void MaskRefiner::reset() noexcept {
    previous_ = {};
    previous_timestamp_ms_ = -1;
}

}  // namespace magic_portal
