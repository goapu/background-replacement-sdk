#include "magic_portal/portal_controller.h"

namespace magic_portal {

PortalController::PortalController(PortalTimelineConfig config) : timeline_(config) {}

bool PortalController::begin(std::int64_t now_ms, int next_background_id) noexcept {
    if (next_background_id < 0 || phase_ == PortalPhase::kTransitioning) {
        return false;
    }
    next_ = next_background_id;
    phase_ = PortalPhase::kTransitioning;
    timeline_.start(now_ms);
    return true;
}

void PortalController::reset() noexcept {
    timeline_.cancel();
    phase_ = PortalPhase::kLive;
    current_ = kNoBackground;
    next_ = kNoBackground;
}

PortalFrameState PortalController::update(std::int64_t now_ms) noexcept {
    PortalFrameState state;
    if (phase_ == PortalPhase::kTransitioning) {
        const PortalVisualState visual = timeline_.state_at(now_ms);
        if (!visual.complete) {
            state.visual = visual;
            state.phase = PortalPhase::kTransitioning;
            state.current_background = current_;
            state.next_background = next_;
            state.camera_is_current = current_ == kNoBackground;
            return state;
        }
        // The reveal finished: the new background takes over as the current one.
        timeline_.cancel();
        current_ = next_;
        next_ = kNoBackground;
        phase_ = PortalPhase::kReplaced;
    }
    state.phase = phase_;
    state.current_background = current_;
    state.next_background = current_;
    state.camera_is_current = current_ == kNoBackground;
    return state;
}

}  // namespace magic_portal
