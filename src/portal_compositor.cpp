#include "magic_portal/portal_compositor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "magic_portal/portal_constants.h"
#include "magic_portal/portal_geometry.h"
#include "magic_portal/portal_hash.h"

namespace magic_portal {
namespace {

using namespace constants;

float smoothstep(float edge0, float edge1, float value) {
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0F : 0.0F;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

float mix(float a, float b, float amount) {
    return a + (b - a) * amount;
}

Rgba mix_color(Rgba a, Rgba b, float amount) {
    const auto channel = [amount](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(
            std::clamp(std::lround(mix(static_cast<float>(x), static_cast<float>(y), amount)), 0L, 255L));
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a)};
}

Rgba screen_blend(Rgba base, Rgba light, float strength) {
    strength = std::clamp(strength, 0.0F, 1.0F);
    const auto channel = [strength](std::uint8_t base_channel, std::uint8_t light_channel) {
        const float b = static_cast<float>(base_channel) / 255.0F;
        const float l = static_cast<float>(light_channel) / 255.0F * strength;
        const float result = 1.0F - (1.0F - b) * (1.0F - l);
        return static_cast<std::uint8_t>(std::clamp(std::lround(result * 255.0F), 0L, 255L));
    };
    return {channel(base.r, light.r), channel(base.g, light.g), channel(base.b, light.b), base.a};
}

Rgba sample_aspect_fill(const RgbaFrame &image, float u, float v, int output_width, int output_height) {
    const float image_aspect = static_cast<float>(image.width()) / static_cast<float>(image.height());
    const float output_aspect = static_cast<float>(output_width) / static_cast<float>(output_height);
    if (image_aspect > output_aspect) {
        u = 0.5F + (u - 0.5F) * (output_aspect / image_aspect);
    } else if (image_aspect < output_aspect) {
        v = 0.5F + (v - 0.5F) * (image_aspect / output_aspect);
    }
    return sample_bilinear(image, u, v);
}

}  // namespace

void validate_style(const PortalStyle &style) {
    const auto in_range = [](float value, float lo, float hi) {
        return std::isfinite(value) && value >= lo && value <= hi;
    };
    // The lower bounds of the width-like values are exclusive: zero would divide by zero in
    // both the shader and the reference. The person guard also needs ordered smoothstep
    // edges: its upper edge must be strictly above kPersonGuardStart.
    const bool valid =
        style.ring_width > 0.0F && in_range(style.ring_width, 0.0F, 0.5F) && style.reveal_feather > 0.0F &&
        in_range(style.reveal_feather, 0.0F, 0.5F) && in_range(style.ring_strength, 0.0F, 8.0F) &&
        in_range(style.particle_strength, 0.0F, 8.0F) && in_range(style.silhouette_glow, 0.0F, 8.0F) &&
        style.person_protection > constants::kPersonGuardStart &&
        in_range(style.person_protection, 0.0F, 1.0F);
    if (!valid) {
        throw std::invalid_argument(
            "invalid portal visual style (finite values only: ring_width and reveal_feather in "
            "(0, 0.5], strengths in [0, 8], person_protection above the shader guard threshold and at "
            "most 1)");
    }
}

PortalCompositor::PortalCompositor(PortalStyle style) : style_(style) {
    validate_style(style_);
}

RgbaFrame PortalCompositor::compose(const CompositeInput &input) const {
    if (input.camera == nullptr || input.next_background == nullptr || input.person == nullptr ||
        input.camera->empty() || input.next_background->empty() || input.person->alpha.empty()) {
        throw std::invalid_argument("compositor input is incomplete");
    }
    if (input.current_background != nullptr && input.current_background->empty()) {
        throw std::invalid_argument("current background cannot be empty");
    }
    for (float value : input.mask_crop) {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
            throw std::invalid_argument("mask crop must lie inside [0, 1]");
        }
    }
    if (input.mask_crop[2] <= input.mask_crop[0] || input.mask_crop[3] <= input.mask_crop[1]) {
        throw std::invalid_argument("mask crop is empty");
    }

    // Invalid bounds mean "nobody is in view": the engine publishes an all-zero mask with the
    // fallback bounds for that case, and the shader receives exactly that. The reference does
    // the same, so the destination fills the view and nothing is cut out of the camera. (A
    // mask that is stale or failed never reaches this function: the engine switches the
    // effect off and the camera is shown untouched.)
    const bool has_person = input.person->bounds.valid;
    ConfidenceMask no_one;
    if (!has_person) {
        no_one = ConfidenceMask(input.person->alpha.width(), input.person->alpha.height());
    }
    const ConfidenceMask &alpha = has_person ? input.person->alpha : no_one;

    const int width = input.camera->width();
    const int height = input.camera->height();
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const PortalGeometry geometry =
        compute_portal_geometry(input.person->bounds, aspect, input.transition, style_.reveal_feather);
    const float center_x = geometry.center_x;
    const float center_y = geometry.center_y;
    const float ring_radius = geometry.ring_radius;
    const float reveal_radius = geometry.reveal_radius;
    const float reveal_gate = geometry.reveal_gate;
    const float spin = geometry.spin_radians;
    const float mask_dx = 1.0F / static_cast<float>(alpha.width());
    const float mask_dy = 1.0F / static_cast<float>(alpha.height());

    // The shader evaluates the noise, angle and ring terms from gl_FragCoord, whose origin is
    // the bottom-left corner. The reference does the same so that both paths draw one pattern;
    // image sampling below keeps the top-left convention of the frames.
    const float center_y_gl = 1.0F - center_y;

    RgbaFrame output(width, height);
    for (int y = 0; y < height; ++y) {
        const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
        const float v_gl = (static_cast<float>(height - 1 - y) + 0.5F) / static_cast<float>(height);
        for (int x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
            const Rgba camera = input.camera->pixel(x, y);
            // The same mapping as the shader: screen position -> position inside the cropped mask.
            const float mask_u = mix(input.mask_crop[0], input.mask_crop[2], u);
            const float mask_v = mix(input.mask_crop[1], input.mask_crop[3], v);
            const float person_alpha = sample_bilinear(alpha, mask_u, mask_v);
            const float person_visibility = std::clamp(input.transition.person_visibility, 0.0F, 1.0F);

            // A stable spatial threshold makes the person dissolve into particles rather
            // than becoming a translucent ghost. The end states remain exact.
            const float dissolve_noise =
                kNoiseHashWeight *
                    hash_unit(
                        hash_cell(std::floor(u * static_cast<float>(width) / kDissolveCellPixels)),
                        hash_cell(std::floor(v_gl * static_cast<float>(height) / kDissolveCellPixels))) +
                kNoiseWaveWeight * (0.5F + 0.5F * std::sin(u * kNoiseWaveAX + v_gl * kNoiseWaveAY)) +
                kNoiseWaveWeight * (0.5F + 0.5F * std::sin(u * kNoiseWaveBX - v_gl * kNoiseWaveBY));
            float person_gate = 1.0F;
            if (person_visibility <= kVisibilityEpsLow) {
                person_gate = 0.0F;
            } else if (person_visibility < kVisibilityEpsHigh) {
                const float threshold = 1.0F - person_visibility;
                person_gate =
                    smoothstep(threshold - kDissolveBand, threshold + kDissolveBand, dissolve_noise);
            }
            const float visible_person_alpha = person_alpha * person_gate;

            // High-confidence person pixels are copied byte-for-byte. This is the invariant
            // that keeps the face, hair, clothes, and body unchanged outside the intentional
            // vanish interval.
            if (person_visibility >= kVisibilityEpsHigh && person_alpha >= style_.person_protection) {
                output.set_pixel(x, y, camera);
                continue;
            }

            const Rgba old_background =
                input.current_background == nullptr
                    ? camera
                    : sample_aspect_fill(*input.current_background, u, v, width, height);
            const Rgba next_background = sample_aspect_fill(*input.next_background, u, v, width, height);

            const float dx = (u - center_x) * aspect;
            const float dy = v_gl - center_y_gl;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float angle = std::atan2(dy, dx);
            float background_mix =
                input.transition.complete
                    ? 1.0F
                    : (1.0F - smoothstep(reveal_radius - style_.reveal_feather,
                                         reveal_radius + style_.reveal_feather, distance)) *
                          reveal_gate;
            // Reveal the destination behind every dissolved body fragment immediately.
            // This prevents the person from remaining baked into the old camera image.
            background_mix = std::max(background_mix, person_alpha * (1.0F - person_gate));
            // The mask controls only the foreground cutout. Transition math never darkens it.
            const Rgba virtual_background = mix_color(old_background, next_background, background_mix);
            Rgba composed = mix_color(virtual_background, camera, visible_person_alpha);

            const float wobble =
                style_.ring_width * (kWobbleAmpA * std::sin(angle * kWobbleFreqA - spin * kWobbleSpinA) +
                                     kWobbleAmpB * std::sin(angle * kWobbleFreqB + spin * kWobbleSpinB));
            const float ring_delta = distance - (ring_radius + wobble);
            const float ring_band =
                std::exp(-(ring_delta * ring_delta) / (2.0F * style_.ring_width * style_.ring_width));
            const float arc_texture =
                kArcBase +
                kArcAmp * std::pow(0.5F + 0.5F * std::sin(angle * kArcFreq - spin * kArcSpin), kArcPower);

            const float cell = std::max(std::floor((angle + kPi) * kSparkCells), 0.0F);
            const float time_step = std::max(std::floor(input.transition.progress * kSparkTimeSteps), 0.0F);
            const float spark_noise = hash_unit(hash_cell(cell), hash_cell(time_step));
            const float spark_distance =
                std::abs(ring_delta - (spark_noise - 0.5F) * style_.ring_width * kSparkOffsetScale);
            const float spark = spark_noise > kSparkThreshold
                                    ? std::exp(-spark_distance * spark_distance /
                                               (style_.ring_width * style_.ring_width * kSparkWidthFactor))
                                    : 0.0F;

            const float left = sample_bilinear(alpha, mask_u - mask_dx, mask_v);
            const float right = sample_bilinear(alpha, mask_u + mask_dx, mask_v);
            const float up = sample_bilinear(alpha, mask_u, mask_v - mask_dy);
            const float down = sample_bilinear(alpha, mask_u, mask_v + mask_dy);
            const float silhouette_edge =
                std::clamp((std::abs(right - left) + std::abs(down - up)) * kSilhouetteGain, 0.0F, 1.0F);

            const float person_guard =
                1.0F - smoothstep(kPersonGuardStart, style_.person_protection, visible_person_alpha);
            const float dissolve_threshold = 1.0F - person_visibility;
            const float dissolve_delta = dissolve_noise - dissolve_threshold;
            const float dissolve_edge =
                person_visibility > kVisibilityEpsLow && person_visibility < kVisibilityEpsHigh
                    ? person_alpha * std::exp(-(dissolve_delta * dissolve_delta) / kDissolveEdgeVariance)
                    : 0.0F;
            const float energy =
                input.transition.ring_alpha * person_guard *
                std::clamp(ring_band * arc_texture * style_.ring_strength + spark * style_.particle_strength +
                               silhouette_edge * style_.silhouette_glow + dissolve_edge * kDissolveEdgeGain,
                           0.0F, 1.0F);
            const float warm_to_cool =
                smoothstep(center_x - kWarmCoolHalfWidth, center_x + kWarmCoolHalfWidth, u);
            const Rgba energy_color = mix_color(style_.warm_color, style_.cool_color, warm_to_cool);
            composed = screen_blend(composed, energy_color, energy);
            output.set_pixel(x, y, composed);
        }
    }
    return output;
}

}  // namespace magic_portal
