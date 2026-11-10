#include "magic_portal/pipeline.h"

#include <utility>

namespace magic_portal {

MagicPortalPipeline::MagicPortalPipeline(MaskRefinerConfig mask_config, PortalTimelineConfig timeline_config,
                                         PortalStyle style)
    : refiner_(mask_config), timeline_(timeline_config), compositor_(style) {}

void MagicPortalPipeline::start_transition(std::int64_t timestamp_ms) noexcept {
    timeline_.start(timestamp_ms);
}

void MagicPortalPipeline::reset() noexcept {
    timeline_.cancel();
    refiner_.reset();
}

PipelineOutput MagicPortalPipeline::process(const RgbaFrame &camera,
                                            const ConfidenceMask &raw_person_confidence,
                                            const RgbaFrame *current_background,
                                            const RgbaFrame &next_background, std::int64_t timestamp_ms) {
    RefinedPersonMask person = refiner_.refine(camera, raw_person_confidence, timestamp_ms);
    const PortalVisualState transition = timeline_.state_at(timestamp_ms);
    CompositeInput input;
    input.camera = &camera;
    input.current_background = current_background;
    input.next_background = &next_background;
    input.person = &person;
    input.transition = transition;
    RgbaFrame frame = compositor_.compose(input);
    return {std::move(frame), std::move(person), transition};
}

}  // namespace magic_portal
