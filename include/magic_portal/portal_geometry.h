#pragma once

#include "magic_portal/frame.h"
#include "magic_portal/portal_timeline.h"

namespace magic_portal {

// Values derived from the subject bounds and the transition state. Coordinates are
// normalized screen units with a top-left origin; distances are aspect-corrected.
struct PortalGeometry {
    float center_x = 0.5F;
    float center_y = 0.5F;
    float body_radius = 0.0F;
    float ring_radius = 0.0F;
    float max_radius = 0.0F;  // farthest-corner distance plus feather margin
    float reveal_radius = 0.0F;
    float reveal_gate = 0.0F;
    float spin_radians = 0.0F;
};

namespace detail {

inline float mix(float a, float b, float amount) {
    return a + (b - a) * amount;
}

}  // namespace detail

// `aspect` is viewport width divided by height. `reveal_feather` is PortalStyle's value.
[[nodiscard]] PortalGeometry compute_portal_geometry(const PersonBounds &bounds, float aspect,
                                                     const PortalVisualState &transition,
                                                     float reveal_feather);

// Farthest distance from (center_x, center_y) to any corner of the unit square, with the
// x axis scaled by `aspect`.
[[nodiscard]] float farthest_corner_distance(float center_x, float center_y, float aspect);

}  // namespace magic_portal
