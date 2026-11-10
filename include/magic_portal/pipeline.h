#pragma once

#include <cstdint>

#include "magic_portal/mask_refiner.h"
#include "magic_portal/portal_compositor.h"
#include "magic_portal/portal_timeline.h"

namespace magic_portal {

struct PipelineOutput {
    RgbaFrame frame;
    RefinedPersonMask person;
    PortalVisualState transition;
};

class MagicPortalPipeline {
public:
    MagicPortalPipeline(MaskRefinerConfig mask_config = {}, PortalTimelineConfig timeline_config = {},
                        PortalStyle style = {});

    void start_transition(std::int64_t timestamp_ms) noexcept;
    void reset() noexcept;

    [[nodiscard]] PipelineOutput process(const RgbaFrame &camera, const ConfidenceMask &raw_person_confidence,
                                         const RgbaFrame *current_background,
                                         const RgbaFrame &next_background, std::int64_t timestamp_ms);

private:
    MaskRefiner refiner_;
    PortalTimeline timeline_;
    PortalCompositor compositor_;
};

}  // namespace magic_portal
