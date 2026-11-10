#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "magic_portal/segmenter_io.h"
#include "test_harness.h"

using namespace magic_portal;

namespace {

RgbaFrame solid(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    RgbaFrame frame(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            frame.set_pixel(x, y, {r, g, b, 255});
        }
    }
    return frame;
}

float float_at(const std::vector<std::uint8_t> &bytes, std::size_t index) {
    float value = 0.0F;
    std::memcpy(&value, bytes.data() + index * sizeof(float), sizeof(float));
    return value;
}

std::vector<std::uint8_t> float_bytes(const std::vector<float> &values) {
    std::vector<std::uint8_t> bytes(values.size() * sizeof(float));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

}  // namespace

MT_TEST("io: shape parsing accepts NHWC, HWC and HW only") {
    const int nhwc[] = {1, 4, 6, 3};
    ImageShape shape = image_shape_from_dims(nhwc, 4);
    MT_CHECK(shape.valid && shape.width == 6 && shape.height == 4 && shape.channels == 3);
    const int hwc[] = {5, 7, 1};
    shape = image_shape_from_dims(hwc, 3);
    MT_CHECK(shape.valid && shape.width == 7 && shape.height == 5 && shape.channels == 1);
    const int hw[] = {8, 9};
    shape = image_shape_from_dims(hw, 2);
    MT_CHECK(shape.valid && shape.width == 9 && shape.height == 8);
    const int batched[] = {2, 4, 4, 3};
    MT_CHECK(!image_shape_from_dims(batched, 4).valid);
    const int bad[] = {1, 0, 4, 3};
    MT_CHECK(!image_shape_from_dims(bad, 4).valid);
    MT_CHECK(!image_shape_from_dims(nullptr, 4).valid);
}

MT_TEST("io: float input is normalized with mean and std") {
    InputSpec spec;
    spec.shape = {2, 2, 3, true};
    spec.mean = 127.5F;
    spec.stddev = 127.5F;
    std::vector<std::uint8_t> out(input_byte_size(spec));
    std::string error;
    MT_CHECK(encode_input(solid(8, 8, 255, 0, 51), spec, out.data(), out.size(), &error));
    MT_CHECK_NEAR(float_at(out, 0), 1.0, 1e-6);
    MT_CHECK_NEAR(float_at(out, 1), -1.0, 1e-6);
    MT_CHECK_NEAR(float_at(out, 2), (51.0 - 127.5) / 127.5, 1e-6);
}

MT_TEST("io: uint8 input passes channel values through") {
    InputSpec spec;
    spec.shape = {3, 2, 4, true};
    spec.element = TensorElement::kUInt8;
    std::vector<std::uint8_t> out(input_byte_size(spec));
    std::string error;
    MT_CHECK(encode_input(solid(9, 6, 10, 20, 30), spec, out.data(), out.size(), &error));
    MT_CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 255);
}

MT_TEST("io: int8 input is quantized and clamped") {
    InputSpec spec;
    spec.shape = {2, 2, 3, true};
    spec.element = TensorElement::kInt8;
    spec.quant = {1.0F / 128.0F, 0};
    spec.mean = 127.5F;
    spec.stddev = 127.5F;
    std::vector<std::uint8_t> out(input_byte_size(spec));
    std::string error;
    MT_CHECK(encode_input(solid(4, 4, 255, 0, 128), spec, out.data(), out.size(), &error));
    MT_CHECK(static_cast<std::int8_t>(out[0]) == 127);   // 1.0 / (1/128) = 128 -> clamped
    MT_CHECK(static_cast<std::int8_t>(out[1]) == -128);  // -1.0 -> -128
    MT_CHECK(static_cast<std::int8_t>(out[2]) == 0 || static_cast<std::int8_t>(out[2]) == 1);

    spec.quant.scale = 0.0F;
    MT_CHECK(!encode_input(solid(4, 4, 1, 2, 3), spec, out.data(), out.size(), &error));
    MT_CHECK(!error.empty());
}

MT_TEST("io: input validation reports clear errors") {
    InputSpec spec;
    spec.shape = {2, 2, 3, true};
    std::vector<std::uint8_t> out(input_byte_size(spec));
    std::string error;
    MT_CHECK(!encode_input(RgbaFrame(), spec, out.data(), out.size(), &error));
    MT_CHECK(!encode_input(solid(4, 4, 1, 1, 1), spec, out.data(), out.size() - 1, &error));
    spec.stddev = 0.0F;
    MT_CHECK(!encode_input(solid(4, 4, 1, 1, 1), spec, out.data(), out.size(), &error));
    spec.stddev = 255.0F;
    spec.shape.channels = 2;
    MT_CHECK(!encode_input(solid(4, 4, 1, 1, 1), spec, out.data(), 16, &error));
}

MT_TEST("io: area resize averages the covered pixels; bilinear point-samples") {
    // 4x4 frame, left half black, right half white, resized to 2x2.
    RgbaFrame frame(4, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::uint8_t v = x < 2 ? 0 : 255;
            frame.set_pixel(x, y, {v, v, v, 255});
        }
    }
    InputSpec spec;
    spec.shape = {2, 2, 3, true};
    spec.element = TensorElement::kUInt8;
    spec.resize = ResizeMode::kArea;
    std::vector<std::uint8_t> out(input_byte_size(spec));
    std::string error;
    MT_CHECK(encode_input(frame, spec, out.data(), out.size(), &error));
    MT_CHECK(out[0] == 0);
    MT_CHECK(out[3] == 255);

    // 3 -> 1 downscale of a checker: area gives the mean, not a single tap.
    RgbaFrame stripes(4, 1);
    for (int x = 0; x < 4; ++x) {
        const std::uint8_t v = (x % 2) == 0 ? 0 : 200;
        stripes.set_pixel(x, 0, {v, v, v, 255});
    }
    spec.shape = {1, 1, 3, true};
    out.assign(input_byte_size(spec), 0);
    MT_CHECK(encode_input(stripes, spec, out.data(), out.size(), &error));
    MT_CHECK(out[0] == 100);
}

MT_TEST("io: single-class float output clamps to [0, 1]") {
    OutputSpec spec;
    spec.shape = {2, 1, 2, true};
    spec.person_class_index = 1;
    const auto bytes = float_bytes({0.2F, 0.9F, 0.4F, 1.7F});
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(decode_output(bytes.data(), bytes.size(), spec, &mask, &error));
    MT_CHECK(mask.width() == 2 && mask.height() == 1);
    MT_CHECK_NEAR(mask.at(0, 0), 0.9, 1e-6);
    MT_CHECK_NEAR(mask.at(1, 0), 1.0, 1e-6);
}

MT_TEST("io: logits are squashed with a sigmoid") {
    OutputSpec spec;
    spec.shape = {2, 1, 1, true};
    spec.is_logits = true;
    const auto bytes = float_bytes({0.0F, 40.0F});
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(decode_output(bytes.data(), bytes.size(), spec, &mask, &error));
    MT_CHECK_NEAR(mask.at(0, 0), 0.5, 1e-6);
    MT_CHECK_NEAR(mask.at(1, 0), 1.0, 1e-6);
}

MT_TEST("io: multiclass 'one minus background' (probabilities)") {
    OutputSpec spec;
    spec.shape = {1, 1, 3, true};
    spec.mode = OutputMode::kOneMinusBackground;
    spec.background_class_index = 0;
    const auto bytes = float_bytes({0.25F, 0.5F, 0.25F});
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(decode_output(bytes.data(), bytes.size(), spec, &mask, &error));
    MT_CHECK_NEAR(mask.at(0, 0), 0.75, 1e-6);
}

MT_TEST("io: multiclass logits use a softmax over channels") {
    OutputSpec spec;
    spec.shape = {1, 1, 2, true};
    spec.mode = OutputMode::kOneMinusBackground;
    spec.background_class_index = 0;
    spec.is_logits = true;
    const auto bytes = float_bytes({0.0F, std::log(3.0F)});  // softmax = {0.25, 0.75}
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(decode_output(bytes.data(), bytes.size(), spec, &mask, &error));
    MT_CHECK_NEAR(mask.at(0, 0), 0.75, 1e-5);
}

MT_TEST("io: quantized output is dequantized") {
    OutputSpec spec;
    spec.shape = {2, 1, 1, true};
    spec.element = TensorElement::kUInt8;
    spec.quant = {1.0F / 255.0F, 0};
    const std::vector<std::uint8_t> bytes = {0, 255};
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(decode_output(bytes.data(), bytes.size(), spec, &mask, &error));
    MT_CHECK_NEAR(mask.at(0, 0), 0.0, 1e-6);
    MT_CHECK_NEAR(mask.at(1, 0), 1.0, 1e-6);

    spec.element = TensorElement::kInt8;
    spec.quant = {1.0F / 255.0F, -128};
    const std::vector<std::uint8_t> signed_bytes = {0x80, 0x7F};  // -128, 127
    MT_CHECK(decode_output(signed_bytes.data(), signed_bytes.size(), spec, &mask, &error));
    MT_CHECK_NEAR(mask.at(0, 0), 0.0, 1e-6);
    MT_CHECK_NEAR(mask.at(1, 0), 1.0, 1e-6);
}

MT_TEST("io: output validation rejects bad class indices and sizes") {
    OutputSpec spec;
    spec.shape = {1, 1, 2, true};
    spec.person_class_index = 2;
    const auto bytes = float_bytes({0.1F, 0.2F});
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(!decode_output(bytes.data(), bytes.size(), spec, &mask, &error));
    spec.person_class_index = 1;
    MT_CHECK(!decode_output(bytes.data(), bytes.size() - 4U, spec, &mask, &error));
    MT_CHECK(!decode_output(nullptr, bytes.size(), spec, &mask, &error));
    spec.mode = OutputMode::kOneMinusBackground;
    spec.background_class_index = 5;
    MT_CHECK(!decode_output(bytes.data(), bytes.size(), spec, &mask, &error));
    spec.mode = OutputMode::kSingleClass;
    spec.element = TensorElement::kUInt8;
    spec.quant = {0.0F, 0};
    const std::vector<std::uint8_t> q = {1, 2};
    MT_CHECK(!decode_output(q.data(), q.size(), spec, &mask, &error));
}
