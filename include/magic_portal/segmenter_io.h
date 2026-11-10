#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "magic_portal/frame.h"

namespace magic_portal {

enum class TensorElement { kFloat32, kUInt8, kInt8 };

// How camera pixels are resampled to the model input size.
enum class ResizeMode {
    kBilinear,  // legacy behaviour: bilinear point sampling
    kArea,      // box average when downscaling (less aliasing); bilinear when upscaling
};

// How the person confidence is read from the model output.
enum class OutputMode {
    kSingleClass,         // person probability is channel `person_class_index`
    kOneMinusBackground,  // person = 1 - probability of channel `background_class_index`
};

struct QuantParams {
    float scale = 0.0F;
    int zero_point = 0;
};

// Image-like tensor interpreted as NHWC.
struct ImageShape {
    int width = 0;
    int height = 0;
    int channels = 0;
    bool valid = false;
};

// Accepts {1,H,W,C}, {H,W,C} and {H,W}.
[[nodiscard]] ImageShape image_shape_from_dims(const int *dims, int count);

[[nodiscard]] std::size_t element_size(TensorElement element) noexcept;

struct InputSpec {
    ImageShape shape;
    TensorElement element = TensorElement::kFloat32;
    QuantParams quant;
    float mean = 0.0F;
    float stddev = 255.0F;
    ResizeMode resize = ResizeMode::kBilinear;
};

[[nodiscard]] std::size_t input_byte_size(const InputSpec &spec) noexcept;

// Resamples `frame` to the model input size and writes it in the tensor's element type.
// `destination_bytes` must equal input_byte_size(spec).
bool encode_input(const RgbaFrame &frame, const InputSpec &spec, std::uint8_t *destination,
                  std::size_t destination_bytes, std::string *error);

struct OutputSpec {
    ImageShape shape;
    TensorElement element = TensorElement::kFloat32;
    QuantParams quant;
    OutputMode mode = OutputMode::kSingleClass;
    int person_class_index = 0;
    int background_class_index = 0;
    bool is_logits = false;
};

[[nodiscard]] std::size_t output_byte_size(const OutputSpec &spec) noexcept;

// Converts the raw output tensor into a person-confidence mask in [0, 1].
bool decode_output(const std::uint8_t *source, std::size_t source_bytes, const OutputSpec &spec,
                   ConfidenceMask *person_confidence, std::string *error);

}  // namespace magic_portal
