#include "magic_portal/portal_timeline.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "magic_portal/limits.h"

namespace magic_portal {
namespace {

float smoothstep(float edge0, float edge1, float value) {
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0F : 0.0F;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

}  // namespace

PortalTimeline::PortalTimeline(PortalTimelineConfig config) : config_(config) {
    const PortalTimelineConfig &c = config_;
    const float all[] = {c.ring_buildup_end, c.reveal_start, c.reveal_end,     c.ring_fade_start,
                         c.vanish_start,     c.vanish_end,   c.reappear_start, c.reappear_end};
    bool fractions_in_range = true;
    for (float value : all) {
        fractions_in_range = fractions_in_range && std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    }
    if (!fractions_in_range || c.duration_ms <= 0 || c.ring_buildup_end <= 0.0F ||
        c.reveal_end <= c.reveal_start || c.ring_fade_start < c.reveal_start || c.ring_fade_start >= 1.0F ||
        c.vanish_end <= c.vanish_start || c.reappear_start < c.vanish_end ||
        c.reappear_end <= c.reappear_start) {
        throw std::invalid_argument("invalid portal timeline configuration");
    }
}

void PortalTimeline::start(std::int64_t timestamp_ms) noexcept {
    start_timestamp_ms_ = timestamp_ms;
    active_ = true;
}

void PortalTimeline::cancel() noexcept {
    active_ = false;
}

PortalVisualState PortalTimeline::state_at(std::int64_t timestamp_ms) const noexcept {
    if (!active_) {
        return {};
    }
    const std::int64_t elapsed = std::max<std::int64_t>(0, saturating_sub(timestamp_ms, start_timestamp_ms_));
    const float progress =
        std::clamp(static_cast<float>(elapsed) / static_cast<float>(config_.duration_ms), 0.0F, 1.0F);

    PortalVisualState state;
    state.progress = progress;
    state.ring_scale = smoothstep(0.0F, config_.ring_buildup_end, progress);
    state.reveal = smoothstep(config_.reveal_start, config_.reveal_end, progress);
    const float fade = 1.0F - smoothstep(config_.ring_fade_start, 1.0F, progress);
    state.ring_alpha = smoothstep(0.0F, 0.10F, progress) * fade;
    state.spin_turns = 2.75F * progress + 0.35F * std::sin(progress * 3.14159265F);
    if (progress < config_.vanish_start) {
        state.person_visibility = 1.0F;
    } else if (progress < config_.vanish_end) {
        state.person_visibility = 1.0F - smoothstep(config_.vanish_start, config_.vanish_end, progress);
    } else if (progress < config_.reappear_start) {
        state.person_visibility = 0.0F;
    } else {
        state.person_visibility = smoothstep(config_.reappear_start, config_.reappear_end, progress);
    }
    state.complete = progress >= 1.0F;
    return state;
}

}  // namespace magic_portal
