#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "magic_portal/person_segmenter.h"
#include "magic_portal/segmenter_io.h"

namespace magic_portal {

struct TflitePersonSegmenterConfig {
    // Provide the model either as a file path or as an in-memory buffer (for example a
    // packaged Android asset). The buffer is copied; it need not outlive the constructor.
    std::string model_path;
    const void *model_data = nullptr;
    std::size_t model_size = 0;

    int num_threads = 2;
    int person_class_index = 0;
    float input_mean = 0.0F;
    float input_std = 255.0F;
    bool output_is_logits = false;

    // Multiclass models (for example background + several person parts) can expose the
    // person as "one minus the background probability".
    OutputMode output_mode = OutputMode::kSingleClass;
    int background_class_index = 0;

    ResizeMode resize_mode = ResizeMode::kBilinear;
};

// Optional native backend. Build with MAGIC_PORTAL_WITH_TFLITE=ON and link LiteRT/TFLite C.
// Not thread-safe: call segment() from one thread at a time.
class TflitePersonSegmenter final : public PersonSegmenter {
public:
    explicit TflitePersonSegmenter(TflitePersonSegmenterConfig config);
    ~TflitePersonSegmenter() override;
    TflitePersonSegmenter(TflitePersonSegmenter &&) noexcept;
    TflitePersonSegmenter &operator=(TflitePersonSegmenter &&) noexcept;
    TflitePersonSegmenter(const TflitePersonSegmenter &) = delete;
    TflitePersonSegmenter &operator=(const TflitePersonSegmenter &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::string &initialization_error() const noexcept;

    bool segment(const RgbaFrame &display_oriented_frame, std::int64_t timestamp_ms,
                 ConfidenceMask *person_confidence, std::string *error) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Creates a ready-to-use segmenter, or returns nullptr and fills `error` when this build has
// no TFLite backend or the model cannot be loaded. Never throws.
[[nodiscard]] std::unique_ptr<PersonSegmenter> create_tflite_segmenter(
    const TflitePersonSegmenterConfig &config, std::string *error) noexcept;

}  // namespace magic_portal
