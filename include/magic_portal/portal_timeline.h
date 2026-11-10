#pragma once

#include <cstdint>

namespace magic_portal {

struct PortalTimelineConfig {
    std::int64_t duration_ms = 1800;
    // All fractions lie in [0, 1]. Each interval's end must be greater than its start;
    // ring_buildup_end is positive, ring_fade_start is below 1, and reappearance follows vanish.
    float ring_buildup_end = 0.24F;
    float reveal_start = 0.20F;
    float reveal_end = 0.82F;
    float ring_fade_start = 0.72F;
    float vanish_start = 0.12F;
    float vanish_end = 0.30F;
    float reappear_start = 0.58F;
    float reappear_end = 0.84F;
};

struct PortalVisualState {
    float progress = 0.0F;
    float reveal = 0.0F;
    float ring_scale = 0.0F;
    float ring_alpha = 0.0F;
    float spin_turns = 0.0F;
    float person_visibility = 1.0F;
    bool complete = false;
};

class PortalTimeline {
public:
    explicit PortalTimeline(PortalTimelineConfig config = {});

    void start(std::int64_t timestamp_ms) noexcept;
    void cancel() noexcept;
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] PortalVisualState state_at(std::int64_t timestamp_ms) const noexcept;

private:
    PortalTimelineConfig config_;
    std::int64_t start_timestamp_ms_ = 0;
    bool active_ = false;
};

}  // namespace magic_portal
