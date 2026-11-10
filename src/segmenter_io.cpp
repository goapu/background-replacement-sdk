#include "magic_portal/segmenter_io.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace magic_portal {
namespace {

void set_error(std::string *error, const char *message) {
    if (error != nullptr) {
        *error = message;
    }
}

float sigmoid(float value) {
    if (value >= 0.0F) {
        const float exponential = std::exp(-value);
        return 1.0F / (1.0F + exponential);
    }
    const float exponential = std::exp(value);
    return exponential / (1.0F + exponential);
}

// Box-averages the source pixels that fall under one output pixel.
Rgba area_sample(const RgbaFrame &frame, int x, int y, int out_width, int out_height) {
    const double scale_x = static_cast<double>(frame.width()) / static_cast<double>(out_width);
    const double scale_y = static_cast<double>(frame.height()) / static_cast<double>(out_height);
    const int x0 =
        std::clamp(static_cast<int>(std::floor(static_cast<double>(x) * scale_x)), 0, frame.width() - 1);
    const int y0 =
        std::clamp(static_cast<int>(std::floor(static_cast<double>(y) * scale_y)), 0, frame.height() - 1);
    const int x1 =
        std::clamp(static_cast<int>(std::ceil(static_cast<double>(x + 1) * scale_x)), x0 + 1, frame.width());
    const int y1 =
        std::clamp(static_cast<int>(std::ceil(static_cast<double>(y + 1) * scale_y)), y0 + 1, frame.height());
    std::uint64_t sum[4] = {0, 0, 0, 0};
    const std::uint8_t *base = frame.data();
    for (int sy = y0; sy < y1; ++sy) {
        const std::uint8_t *row =
            base + static_cast<std::size_t>(sy) * static_cast<std::size_t>(frame.width()) * 4U;
        for (int sx = x0; sx < x1; ++sx) {
            const std::uint8_t *p = row + static_cast<std::size_t>(sx) * 4U;
            sum[0] += p[0];
            sum[1] += p[1];
            sum[2] += p[2];
            sum[3] += p[3];
        }
    }
    const std::uint64_t count = static_cast<std::uint64_t>(x1 - x0) * static_cast<std::uint64_t>(y1 - y0);
    const auto average = [count](std::uint64_t total) {
        return static_cast<std::uint8_t>((total + count / 2U) / count);
    };
    return {average(sum[0]), average(sum[1]), average(sum[2]), average(sum[3])};
}

Rgba sample_input(const RgbaFrame &frame, const InputSpec &spec, int x, int y) {
    const bool downscale = spec.resize == ResizeMode::kArea && frame.width() >= spec.shape.width &&
                           frame.height() >= spec.shape.height;
    if (downscale) {
        return area_sample(frame, x, y, spec.shape.width, spec.shape.height);
    }
    const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(spec.shape.width);
    const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(spec.shape.height);
    return sample_bilinear(frame, u, v);
}

std::int8_t quantize_channel(std::uint8_t channel, const InputSpec &spec) {
    const float normalized = (static_cast<float>(channel) - spec.mean) / spec.stddev;
    const long quantized =
        std::lround(normalized / spec.quant.scale + static_cast<float>(spec.quant.zero_point));
    return static_cast<std::int8_t>(std::clamp(quantized, -128L, 127L));
}

float read_element(const std::uint8_t *source, std::size_t index, const OutputSpec &spec) {
    switch (spec.element) {
        case TensorElement::kFloat32: {
            float value = 0.0F;
            std::memcpy(&value, source + index * sizeof(float), sizeof(float));
            return value;
        }
        case TensorElement::kUInt8:
            return static_cast<float>(static_cast<int>(source[index]) - spec.quant.zero_point) *
                   spec.quant.scale;
        case TensorElement::kInt8:
            return static_cast<float>(static_cast<int>(static_cast<std::int8_t>(source[index])) -
                                      spec.quant.zero_point) *
                   spec.quant.scale;
    }
    return 0.0F;
}

}  // namespace

ImageShape image_shape_from_dims(const int *dims, int count) {
    if (dims == nullptr) {
        return {};
    }
    ImageShape shape;
    if (count == 4 && dims[0] == 1) {
        shape = {dims[2], dims[1], dims[3], true};
    } else if (count == 3) {
        shape = {dims[1], dims[0], dims[2], true};
    } else if (count == 2) {
        shape = {dims[1], dims[0], 1, true};
    } else {
        return {};
    }
    if (shape.width <= 0 || shape.height <= 0 || shape.channels <= 0) {
        return {};
    }
    return shape;
}

std::size_t element_size(TensorElement element) noexcept {
    return element == TensorElement::kFloat32 ? sizeof(float) : 1U;
}

std::size_t input_byte_size(const InputSpec &spec) noexcept {
    return static_cast<std::size_t>(spec.shape.width) * static_cast<std::size_t>(spec.shape.height) *
           static_cast<std::size_t>(spec.shape.channels) * element_size(spec.element);
}

std::size_t output_byte_size(const OutputSpec &spec) noexcept {
    return static_cast<std::size_t>(spec.shape.width) * static_cast<std::size_t>(spec.shape.height) *
           static_cast<std::size_t>(spec.shape.channels) * element_size(spec.element);
}

bool encode_input(const RgbaFrame &frame, const InputSpec &spec, std::uint8_t *destination,
                  std::size_t destination_bytes, std::string *error) {
    if (frame.empty() || destination == nullptr) {
        set_error(error, "segmentation requires a non-empty frame and an input buffer");
        return false;
    }
    if (!spec.shape.valid || (spec.shape.channels != 3 && spec.shape.channels != 4)) {
        set_error(error, "unsupported input tensor; expected NHWC RGB or RGBA");
        return false;
    }
    if (spec.stddev == 0.0F) {
        set_error(error, "input_std cannot be zero");
        return false;
    }
    if (destination_bytes != input_byte_size(spec)) {
        set_error(error, "preprocessed input size does not match model input tensor");
        return false;
    }
    if (spec.element == TensorElement::kInt8 && spec.quant.scale <= 0.0F) {
        set_error(error, "quantized input tensor has invalid scale metadata");
        return false;
    }

    const std::size_t channels = static_cast<std::size_t>(spec.shape.channels);
    for (int y = 0; y < spec.shape.height; ++y) {
        for (int x = 0; x < spec.shape.width; ++x) {
            const Rgba color = sample_input(frame, spec, x, y);
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(spec.shape.width) +
                 static_cast<std::size_t>(x)) *
                channels;
            const std::uint8_t values[4] = {color.r, color.g, color.b, color.a};
            for (std::size_t c = 0; c < channels; ++c) {
                const std::size_t index = pixel + c;
                switch (spec.element) {
                    case TensorElement::kFloat32: {
                        const float value = (static_cast<float>(values[c]) - spec.mean) / spec.stddev;
                        std::memcpy(destination + index * sizeof(float), &value, sizeof(float));
                        break;
                    }
                    case TensorElement::kUInt8:
                        destination[index] = values[c];
                        break;
                    case TensorElement::kInt8:
                        destination[index] = static_cast<std::uint8_t>(quantize_channel(values[c], spec));
                        break;
                }
            }
        }
    }
    return true;
}

bool decode_output(const std::uint8_t *source, std::size_t source_bytes, const OutputSpec &spec,
                   ConfidenceMask *person_confidence, std::string *error) {
    if (source == nullptr || person_confidence == nullptr) {
        set_error(error, "decoding requires an output tensor and a destination mask");
        return false;
    }
    if (!spec.shape.valid) {
        set_error(error, "unsupported output tensor or invalid person class index");
        return false;
    }
    const int channels = spec.shape.channels;
    const bool single = spec.mode == OutputMode::kSingleClass;
    if (spec.person_class_index < 0 || spec.person_class_index >= channels ||
        (!single && (spec.background_class_index < 0 || spec.background_class_index >= channels))) {
        set_error(error, "unsupported output tensor or invalid person class index");
        return false;
    }
    if (spec.element != TensorElement::kFloat32 && spec.quant.scale <= 0.0F) {
        set_error(error, "quantized output tensor has invalid scale metadata");
        return false;
    }
    if (source_bytes != output_byte_size(spec)) {
        set_error(error, "output tensor size does not match its shape");
        return false;
    }

    ConfidenceMask result(spec.shape.width, spec.shape.height);
    std::vector<float> scores(static_cast<std::size_t>(channels));
    for (int y = 0; y < spec.shape.height; ++y) {
        for (int x = 0; x < spec.shape.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(spec.shape.width) +
                 static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(channels);
            float person = 0.0F;
            if (single) {
                const float raw =
                    read_element(source, pixel + static_cast<std::size_t>(spec.person_class_index), spec);
                person = spec.is_logits ? sigmoid(raw) : std::clamp(raw, 0.0F, 1.0F);
            } else {
                for (int c = 0; c < channels; ++c) {
                    scores[static_cast<std::size_t>(c)] =
                        read_element(source, pixel + static_cast<std::size_t>(c), spec);
                }
                float background = scores[static_cast<std::size_t>(spec.background_class_index)];
                if (spec.is_logits) {
                    const float peak = *std::max_element(scores.begin(), scores.end());
                    float total = 0.0F;
                    for (float score : scores) {
                        total += std::exp(score - peak);
                    }
                    background = std::exp(background - peak) / total;
                }
                person = 1.0F - std::clamp(background, 0.0F, 1.0F);
            }
            result.set(x, y, person);
        }
    }
    *person_confidence = std::move(result);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace magic_portal
