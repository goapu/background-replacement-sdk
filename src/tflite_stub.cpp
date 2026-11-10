#include "magic_portal/tflite_person_segmenter.h"

namespace magic_portal {

std::unique_ptr<PersonSegmenter> create_tflite_segmenter(const TflitePersonSegmenterConfig &,
                                                         std::string *error) noexcept {
    try {
        if (error != nullptr) {
            *error = "this build of the SDK was compiled without the TFLite backend";
        }
    } catch (...) {
        // Best effort: this path cannot report further failures.
    }
    return nullptr;
}

}  // namespace magic_portal
