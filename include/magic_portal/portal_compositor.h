#pragma once

#include "magic_portal/frame.h"
#include "magic_portal/mask_refiner.h"
#include "magic_portal/portal_timeline.h"

namespace magic_portal {

struct PortalStyle {
    Rgba warm_color{255, 126, 22, 255};
    Rgba cool_color{143, 111, 255, 255};
    float ring_width = 0.014F;
    float ring_strength = 0.92F;
    float particle_strength = 0.58F;
    float silhouette_glow = 0.38F;
    float reveal_feather = 0.045F;
    float person_protection = 0.985F;
};

// Throws std::invalid_argument unless every value is finite and inside its supported range:
// ring_width and reveal_feather in (0, 0.5], the three strengths in [0, 8],
// person_protection in (constants::kPersonGuardStart, 1] (currently (0.72, 1]). Used by the
// compositor, engine and Viro adapter, so accepted styles have defined CPU and shader math.
void validate_style(const PortalStyle &style);

struct CompositeInput {
    const RgbaFrame *camera = nullptr;
    const RgbaFrame *current_background = nullptr;  // null means the unmodified camera
    const RgbaFrame *next_background = nullptr;
    const RefinedPersonMask *person = nullptr;
    PortalVisualState transition;
    // The part of the mask that is visible on screen: left, top, right, bottom in [0, 1] of
    // the mask, top-left origin. This is RenderState::mask_crop; the shader maps the screen
    // onto the mask through the same rectangle. Default: the whole mask.
    float mask_crop[4] = {0.0F, 0.0F, 1.0F, 1.0F};
};

class PortalCompositor {
public:
    explicit PortalCompositor(PortalStyle style = {});

    // Dependency-free CPU reference path. The Viro adapter performs the same math on GPU.
    [[nodiscard]] RgbaFrame compose(const CompositeInput &input) const;

private:
    PortalStyle style_;
};

}  // namespace magic_portal
