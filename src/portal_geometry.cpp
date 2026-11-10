#include "magic_portal/portal_geometry.h"

#include <algorithm>
#include <cmath>

#include "magic_portal/portal_constants.h"

namespace magic_portal {
namespace {

float smoothstep_local(float edge0, float edge1, float value) {
    if (edge1 <= edge0) {
        return value >= edge1 ? 1.0F : 0.0F;
    }
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

}  // namespace

float farthest_corner_distance(float center_x, float center_y, float aspect) {
    float farthest = 0.0F;
    for (float y : {0.0F, 1.0F}) {
        for (float x : {0.0F, 1.0F}) {
            const float dx = (x - center_x) * aspect;
            const float dy = y - center_y;
            farthest = std::max(farthest, std::sqrt(dx * dx + dy * dy));
        }
    }
    return farthest;
}

PortalGeometry compute_portal_geometry(const PersonBounds &bounds, float aspect,
                                       const PortalVisualState &transition, float reveal_feather) {
    using namespace constants;
    PortalGeometry g;
    g.center_x = bounds.valid ? bounds.center_x() : 0.5F;
    g.center_y = bounds.valid ? bounds.center_y() : 0.5F;
    const float body_width = bounds.valid ? std::max(bounds.width(), kMinBodyWidth) : kFallbackBodyWidth;
    const float body_height = bounds.valid ? std::max(bounds.height(), kMinBodyHeight) : kFallbackBodyHeight;
    g.body_radius = std::clamp(kBodyRadiusScale * std::sqrt((body_width * aspect) * (body_width * aspect) +
                                                            body_height * body_height),
                               kBodyRadiusMin, kBodyRadiusMax);
    g.ring_radius = g.body_radius * detail::mix(kRingStartScale, 1.0F, transition.ring_scale);
    g.max_radius = farthest_corner_distance(g.center_x, g.center_y, aspect) + 2.0F * reveal_feather;
    g.reveal_radius = detail::mix(kRevealStartRadiusScale * g.body_radius, g.max_radius, transition.reveal);
    g.reveal_gate = smoothstep_local(0.0F, kRevealGateEnd, transition.reveal);
    g.spin_radians = transition.spin_turns * 2.0F * kPi;
    return g;
}

}  // namespace magic_portal
