// Tests the TFLite glue against a test double of the C API (see tests/fake_tflite).
// They prove the SDK's own logic: ownership, shape/type handling, pre/post-processing and
// error paths. They do not prove behaviour on the real runtime or a real model.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "fake_tflite_control.h"
#include "golden_support.h"
#include "magic_portal/tflite_person_segmenter.h"
#include "test_harness.h"

using namespace magic_portal;

namespace {

TflitePersonSegmenterConfig from_text(const std::string &text, std::vector<std::uint8_t> *keep) {
    keep->assign(text.begin(), text.end());
    TflitePersonSegmenterConfig config;
    config.model_data = keep->data();
    config.model_size = keep->size();
    return config;
}

RgbaFrame flat_frame(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    RgbaFrame frame(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            frame.set_pixel(x, y, {r, g, b, 255});
        }
    }
    return frame;
}

float float_in(const std::vector<std::uint8_t> &bytes, std::size_t index) {
    float v = 0.0F;
    std::memcpy(&v, bytes.data() + index * 4U, 4U);
    return v;
}

const char *kFloatModel =
    "input float32 0 0 1 4 4 3\n"
    "output float32 0 0 1 4 4 1\n"
    "fill 0.8\n";

}  // namespace

MT_TEST("tflite: loads a model from a memory buffer and does not rely on the caller keeping it") {
    fake_tflite::reset();
    std::unique_ptr<TflitePersonSegmenter> segmenter;
    {
        std::vector<std::uint8_t> buffer;
        TflitePersonSegmenterConfig config = from_text(kFloatModel, &buffer);
        segmenter = std::make_unique<TflitePersonSegmenter>(config);
        MT_CHECK(segmenter->valid());
        MT_CHECK(fake_tflite::observed().models_from_buffer == 1);
    }  // caller's buffer is freed here
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(segmenter->segment(flat_frame(8, 8, 10, 20, 30), 0, &mask, &error));
    MT_CHECK(mask.width() == 4 && mask.height() == 4);
    MT_CHECK_NEAR(mask.at(2, 2), 0.8, 1e-6);
}

MT_TEST("tflite: loads a model from a file path") {
    fake_tflite::reset();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mp_fake_model.txt";
    {
        std::ofstream out(path, std::ios::binary);
        out << kFloatModel;
    }
    TflitePersonSegmenterConfig config;
    config.model_path = path.string();
    TflitePersonSegmenter segmenter(config);
    MT_CHECK(segmenter.valid());
    MT_CHECK(fake_tflite::observed().models_from_file == 1);
    std::filesystem::remove(path);
}

MT_TEST("tflite: reports clear initialization errors") {
    fake_tflite::reset();
    TflitePersonSegmenterConfig none;
    TflitePersonSegmenter a(none);
    MT_CHECK(!a.valid() && !a.initialization_error().empty());

    TflitePersonSegmenterConfig missing;
    missing.model_path = "/nonexistent/model.tflite";
    TflitePersonSegmenter b(missing);
    MT_CHECK(!b.valid() && b.initialization_error().find("open") != std::string::npos);

    std::vector<std::uint8_t> buffer;
    TflitePersonSegmenterConfig zero_std = from_text(kFloatModel, &buffer);
    zero_std.input_std = 0.0F;
    TflitePersonSegmenter c(zero_std);
    MT_CHECK(!c.valid());

    TflitePersonSegmenterConfig two_inputs = from_text(std::string(kFloatModel) + "extra_input 1\n", &buffer);
    TflitePersonSegmenter d(two_inputs);
    MT_CHECK(!d.valid());

    TflitePersonSegmenterConfig bad_alloc =
        from_text(std::string(kFloatModel) + "fail_allocate 1\n", &buffer);
    TflitePersonSegmenter e(bad_alloc);
    MT_CHECK(!e.valid() && e.initialization_error().find("allocate") != std::string::npos);

    ConfidenceMask mask;
    std::string error;
    MT_CHECK(!e.segment(flat_frame(4, 4, 0, 0, 0), 0, &mask, &error));
    MT_CHECK(!error.empty());
    // Every failed construction released everything it created.
    MT_CHECK(fake_tflite::observed().models_live == 0);
    MT_CHECK(fake_tflite::observed().options_live == 0);
    MT_CHECK(fake_tflite::observed().interpreters_live == 0);
}

MT_TEST("tflite: no handles leak after destruction, moves keep a single owner") {
    fake_tflite::reset();
    std::vector<std::uint8_t> buffer;
    {
        TflitePersonSegmenter first(from_text(kFloatModel, &buffer));
        TflitePersonSegmenter second(std::move(first));
        MT_CHECK(second.valid());
        MT_CHECK(fake_tflite::observed().interpreters_live == 1);
    }
    MT_CHECK(fake_tflite::observed().models_live == 0);
    MT_CHECK(fake_tflite::observed().interpreters_live == 0);
}

MT_TEST("tflite: float input is normalized and thread count is applied") {
    fake_tflite::reset();
    std::vector<std::uint8_t> buffer;
    TflitePersonSegmenterConfig config = from_text(kFloatModel, &buffer);
    config.input_mean = 127.5F;
    config.input_std = 127.5F;
    config.num_threads = 0;  // clamped to 1
    TflitePersonSegmenter segmenter(config);
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(segmenter.segment(flat_frame(8, 8, 255, 0, 51), 0, &mask, &error));
    const auto &input = fake_tflite::observed().last_input;
    MT_CHECK(input.size() == 4U * 4U * 3U * 4U);
    MT_CHECK_NEAR(float_in(input, 0), 1.0, 1e-6);
    MT_CHECK_NEAR(float_in(input, 1), -1.0, 1e-6);
    MT_CHECK_NEAR(float_in(input, 2), (51.0 - 127.5) / 127.5, 1e-6);
    MT_CHECK(fake_tflite::observed().last_num_threads == 1);
}

MT_TEST("tflite: uint8 and int8 inputs") {
    std::vector<std::uint8_t> buffer;
    {
        fake_tflite::reset();
        TflitePersonSegmenter segmenter(
            from_text("input uint8 0 0 1 2 2 3\noutput float32 0 0 1 2 2 1\nfill 0.5\n", &buffer));
        ConfidenceMask mask;
        std::string error;
        MT_CHECK(segmenter.segment(flat_frame(6, 6, 9, 99, 199), 0, &mask, &error));
        const auto &in = fake_tflite::observed().last_input;
        MT_CHECK(in.size() == 12U && in[0] == 9 && in[1] == 99 && in[2] == 199);
    }
    {
        fake_tflite::reset();
        TflitePersonSegmenterConfig config =
            from_text("input int8 0.0078125 0 1 2 2 3\noutput float32 0 0 1 2 2 1\nfill 0.5\n", &buffer);
        config.input_mean = 127.5F;
        config.input_std = 127.5F;
        TflitePersonSegmenter segmenter(config);
        ConfidenceMask mask;
        std::string error;
        MT_CHECK(segmenter.segment(flat_frame(6, 6, 255, 0, 128), 0, &mask, &error));
        const auto &in = fake_tflite::observed().last_input;
        MT_CHECK(static_cast<std::int8_t>(in[0]) == 127);
        MT_CHECK(static_cast<std::int8_t>(in[1]) == -128);
    }
}

MT_TEST("tflite: RGBA input tensors carry the alpha channel") {
    fake_tflite::reset();
    std::vector<std::uint8_t> buffer;
    TflitePersonSegmenter segmenter(
        from_text("input uint8 0 0 1 2 2 4\noutput float32 0 0 1 2 2 1\nfill 0.5\n", &buffer));
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(segmenter.segment(flat_frame(4, 4, 1, 2, 3), 0, &mask, &error));
    const auto &in = fake_tflite::observed().last_input;
    MT_CHECK(in.size() == 16U && in[3] == 255);
}

MT_TEST("tflite: multiclass output and class selection") {
    std::vector<std::uint8_t> buffer;
    {
        TflitePersonSegmenterConfig config =
            from_text("input float32 0 0 1 2 2 3\noutput float32 0 0 1 2 2 2\nfill 0.1 0.9\n", &buffer);
        config.person_class_index = 1;
        TflitePersonSegmenter segmenter(config);
        ConfidenceMask mask;
        std::string error;
        MT_CHECK(segmenter.segment(flat_frame(4, 4, 0, 0, 0), 0, &mask, &error));
        MT_CHECK_NEAR(mask.at(1, 1), 0.9, 1e-6);
    }
    {
        TflitePersonSegmenterConfig config =
            from_text("input float32 0 0 1 2 2 3\noutput float32 0 0 1 2 2 3\nfill 0.2 0.3 0.5\n", &buffer);
        config.output_mode = OutputMode::kOneMinusBackground;
        config.background_class_index = 0;
        TflitePersonSegmenter segmenter(config);
        ConfidenceMask mask;
        std::string error;
        MT_CHECK(segmenter.segment(flat_frame(4, 4, 0, 0, 0), 0, &mask, &error));
        MT_CHECK_NEAR(mask.at(0, 0), 0.8, 1e-6);
    }
    {
        TflitePersonSegmenterConfig config =
            from_text("input float32 0 0 1 2 2 3\noutput float32 0 0 1 2 2 2\nfill 0.1 0.9\n", &buffer);
        config.person_class_index = 5;  // out of range
        TflitePersonSegmenter segmenter(config);
        ConfidenceMask mask;
        std::string error;
        MT_CHECK(!segmenter.segment(flat_frame(4, 4, 0, 0, 0), 0, &mask, &error));
        MT_CHECK(error.find("class index") != std::string::npos);
    }
}

MT_TEST("tflite: quantized outputs are dequantized") {
    std::vector<std::uint8_t> buffer;
    TflitePersonSegmenter segmenter(
        from_text("input float32 0 0 1 2 2 3\noutput uint8 0.00390625 0 1 2 2 1\nfill 0.75\n", &buffer));
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(segmenter.segment(flat_frame(4, 4, 0, 0, 0), 0, &mask, &error));
    MT_CHECK_NEAR(mask.at(0, 0), 0.75, 1e-6);
}

MT_TEST("tflite: inference failure is reported, not thrown") {
    std::vector<std::uint8_t> buffer;
    TflitePersonSegmenter segmenter(from_text(std::string(kFloatModel) + "fail_invoke 1\n", &buffer));
    MT_CHECK(segmenter.valid());
    ConfidenceMask mask;
    std::string error;
    MT_CHECK(!segmenter.segment(flat_frame(4, 4, 0, 0, 0), 0, &mask, &error));
    MT_CHECK(error.find("inference failed") != std::string::npos);
    MT_CHECK(!segmenter.segment(RgbaFrame(), 0, &mask, &error));
    MT_CHECK(!segmenter.segment(flat_frame(4, 4, 0, 0, 0), 0, nullptr, &error));
}

MT_TEST("tflite: factory returns null with a message instead of throwing") {
    TflitePersonSegmenterConfig config;
    config.model_path = "/nonexistent/model.tflite";
    std::string error;
    MT_CHECK(create_tflite_segmenter(config, &error) == nullptr);
    MT_CHECK(!error.empty());
    MT_CHECK(create_tflite_segmenter(config, nullptr) == nullptr);

    std::vector<std::uint8_t> buffer;
    TflitePersonSegmenterConfig good = from_text(kFloatModel, &buffer);
    auto segmenter = create_tflite_segmenter(good, &error);
    MT_CHECK(segmenter != nullptr);
}

MT_TEST("tflite: area resizing differs from point sampling on high-frequency input") {
    std::vector<std::uint8_t> buffer;
    RgbaFrame stripes(8, 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const std::uint8_t v = (x % 2) == 0 ? 0 : 200;
            stripes.set_pixel(x, y, {v, v, v, 255});
        }
    }
    const std::string model = "input uint8 0 0 1 2 2 3\noutput float32 0 0 1 2 2 1\nfill 0.5\n";
    ConfidenceMask mask;
    std::string error;

    fake_tflite::reset();
    TflitePersonSegmenterConfig area = from_text(model, &buffer);
    area.resize_mode = ResizeMode::kArea;
    TflitePersonSegmenter area_segmenter(area);
    MT_CHECK(area_segmenter.segment(stripes, 0, &mask, &error));
    const std::uint8_t area_value = fake_tflite::observed().last_input[0];

    fake_tflite::reset();
    TflitePersonSegmenterConfig point = from_text(model, &buffer);
    TflitePersonSegmenter point_segmenter(point);
    MT_CHECK(point_segmenter.segment(stripes, 0, &mask, &error));
    const std::uint8_t point_value = fake_tflite::observed().last_input[0];

    MT_CHECK(area_value == 100);          // mean of 0 and 200
    MT_CHECK(point_value != area_value);  // bilinear point sample of the same pixels
}
