#pragma once

#include <cstdint>

#include "magic_portal/portal_timeline.h"

namespace magic_portal {

inline constexpr int kNoBackground = -1;

enum class PortalPhase {
    kLive,           // the live camera is the background
    kTransitioning,  // a portal transition is playing
    kReplaced,       // a replacement background is showing
};

// Everything a renderer needs for one frame. Background ids are chosen by the caller and
// refer to textures the caller owns.
struct PortalFrameState {
    PortalVisualState visual;
    PortalPhase phase = PortalPhase::kLive;
    int current_background = kNoBackground;  // the "old" background of the effect
    int next_background = kNoBackground;     // the background being revealed
    bool camera_is_current = true;           // true: old background is the live camera
};

// Owns the transition life cycle so adapters do not have to:
//
//   kLive --begin(id)--> kTransitioning --(timeline completes)--> kReplaced(id)
//   kReplaced --begin(id2)--> kTransitioning (old background = id, not the camera)
//   any --reset()--> kLive
//
// Not thread-safe; callers serialize access.
class PortalController {
public:
    explicit PortalController(PortalTimelineConfig config = {});

    // Starts a transition to `next_background_id` (>= 0). Returns false, and changes nothing,
    // when a transition is already running or the id is invalid.
    [[nodiscard]] bool begin(std::int64_t now_ms, int next_background_id) noexcept;

    // Returns to the live camera immediately, cancelling any running transition.
    void reset() noexcept;

    // Advances to `now_ms`. When the timeline finishes, the revealed background becomes the
    // current one within the same call, so there is no visible seam.
    [[nodiscard]] PortalFrameState update(std::int64_t now_ms) noexcept;

    [[nodiscard]] PortalPhase phase() const noexcept { return phase_; }

private:
    PortalTimeline timeline_;
    PortalPhase phase_ = PortalPhase::kLive;
    int current_ = kNoBackground;
    int next_ = kNoBackground;
};

}  // namespace magic_portal
