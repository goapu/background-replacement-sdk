#pragma once

#include <cstdint>
#include <string>

#include "magic_portal/frame.h"

namespace magic_portal {

class PersonSegmenter {
public:
    virtual ~PersonSegmenter() = default;

    // The frame must already be rotated and mirrored exactly as it will be displayed.
    // Implementations may drop frames internally, but a successful call must return a mask.
    virtual bool segment(const RgbaFrame &display_oriented_frame, std::int64_t timestamp_ms,
                         ConfidenceMask *person_confidence, std::string *error) = 0;
};

}  // namespace magic_portal
