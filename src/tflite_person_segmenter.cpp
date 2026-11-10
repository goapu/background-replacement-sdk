#include "magic_portal/tflite_person_segmenter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

#include <tensorflow/lite/c/c_api.h>

namespace magic_portal {
namespace {

ImageShape tensor_image_shape(const TfLiteTensor *tensor) {
    if (tensor == nullptr) {
        return {};
    }
    int dims[8] = {0};
    const int count = TfLiteTensorNumDims(tensor);
    if (count < 2 || count > 8) {
        return {};
    }
    for (int i = 0; i < count; ++i) {
        dims[i] = TfLiteTensorDim(tensor, i);
    }
    return image_shape_from_dims(dims, count);
}

bool tensor_element(TfLiteType type, TensorElement *element) {
    switch (type) {
        case kTfLiteFloat32:
            *element = TensorElement::kFloat32;
            return true;
        case kTfLiteUInt8:
            *element = TensorElement::kUInt8;
            return true;
        case kTfLiteInt8:
            *element = TensorElement::kInt8;
            return true;
        default:
            return false;
    }
}

void assign(std::string *target, const char *message) {
    if (target != nullptr) {
        *target = message;
    }
}

}  // namespace

struct TflitePersonSegmenter::Impl {
    explicit Impl(TflitePersonSegmenterConfig value) : config(std::move(value)) {}

    TflitePersonSegmenterConfig config;
    std::vector<std::uint8_t> model_bytes;  // owned copy; TFLite keeps a pointer into it
    TfLiteModel *model = nullptr;
    TfLiteInterpreterOptions *options = nullptr;
    TfLiteInterpreter *interpreter = nullptr;
    std::string initialization_error;
    std::vector<std::uint8_t> input_buffer;   // reused across frames
    std::vector<std::uint8_t> output_buffer;  // reused across frames

    ~Impl() { release(); }

    void release() {
        // The interpreter must be destroyed before the model and its backing buffer.
        if (interpreter != nullptr) {
            TfLiteInterpreterDelete(interpreter);
            interpreter = nullptr;
        }
        if (options != nullptr) {
            TfLiteInterpreterOptionsDelete(options);
            options = nullptr;
        }
        if (model != nullptr) {
            TfLiteModelDelete(model);
            model = nullptr;
        }
    }

    void fail(const char *message) {
        initialization_error = message;
        release();
    }
};

TflitePersonSegmenter::TflitePersonSegmenter(TflitePersonSegmenterConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
    Impl &impl = *impl_;
    const bool has_path = !impl.config.model_path.empty();
    const bool has_buffer = impl.config.model_data != nullptr && impl.config.model_size > 0U;
    if (!has_path && !has_buffer) {
        impl.initialization_error = "no segmentation model was provided (path or buffer)";
        return;
    }
    if (impl.config.input_std == 0.0F) {
        impl.initialization_error = "input_std cannot be zero";
        return;
    }
    if (has_buffer) {
        const auto *begin = static_cast<const std::uint8_t *>(impl.config.model_data);
        impl.model_bytes.assign(begin, begin + impl.config.model_size);
        impl.model = TfLiteModelCreate(impl.model_bytes.data(), impl.model_bytes.size());
    } else {
        impl.model = TfLiteModelCreateFromFile(impl.config.model_path.c_str());
    }
    if (impl.model == nullptr) {
        impl.initialization_error = "failed to open the TFLite person-segmentation model";
        return;
    }
    impl.options = TfLiteInterpreterOptionsCreate();
    if (impl.options == nullptr) {
        impl.fail("failed to create TFLite interpreter options");
        return;
    }
    TfLiteInterpreterOptionsSetNumThreads(impl.options, std::max(1, impl.config.num_threads));
    impl.interpreter = TfLiteInterpreterCreate(impl.model, impl.options);
    if (impl.interpreter == nullptr) {
        impl.fail("failed to create the TFLite interpreter");
        return;
    }
    if (TfLiteInterpreterAllocateTensors(impl.interpreter) != kTfLiteOk) {
        impl.fail("failed to allocate TFLite tensors");
        return;
    }
    if (TfLiteInterpreterGetInputTensorCount(impl.interpreter) != 1 ||
        TfLiteInterpreterGetOutputTensorCount(impl.interpreter) < 1) {
        impl.fail("the model must have one image input and at least one output");
    }
}

TflitePersonSegmenter::~TflitePersonSegmenter() = default;
TflitePersonSegmenter::TflitePersonSegmenter(TflitePersonSegmenter &&) noexcept = default;
TflitePersonSegmenter &TflitePersonSegmenter::operator=(TflitePersonSegmenter &&) noexcept = default;

bool TflitePersonSegmenter::valid() const noexcept {
    return impl_ != nullptr && impl_->interpreter != nullptr;
}

const std::string &TflitePersonSegmenter::initialization_error() const noexcept {
    static const std::string empty;
    return impl_ == nullptr ? empty : impl_->initialization_error;
}

bool TflitePersonSegmenter::segment(const RgbaFrame &frame, std::int64_t /*timestamp_ms*/,
                                    ConfidenceMask *person_confidence, std::string *error) {
    if (!valid()) {
        if (error != nullptr) {
            *error = initialization_error();
        }
        return false;
    }
    if (person_confidence == nullptr || frame.empty()) {
        assign(error, "segmentation requires a non-empty frame and output mask");
        return false;
    }
    Impl &impl = *impl_;

    TfLiteTensor *input = TfLiteInterpreterGetInputTensor(impl.interpreter, 0);
    InputSpec input_spec;
    input_spec.shape = tensor_image_shape(input);
    if (!tensor_element(TfLiteTensorType(input), &input_spec.element)) {
        assign(error, "unsupported input tensor type; use float32, uint8, or int8");
        return false;
    }
    const TfLiteQuantizationParams input_quant = TfLiteTensorQuantizationParams(input);
    input_spec.quant = {input_quant.scale, input_quant.zero_point};
    input_spec.mean = impl.config.input_mean;
    input_spec.stddev = impl.config.input_std;
    input_spec.resize = impl.config.resize_mode;

    impl.input_buffer.resize(input_byte_size(input_spec));
    if (!encode_input(frame, input_spec, impl.input_buffer.data(), impl.input_buffer.size(), error)) {
        return false;
    }
    if (TfLiteTensorByteSize(input) != impl.input_buffer.size()) {
        assign(error, "preprocessed input size does not match model input tensor");
        return false;
    }
    if (TfLiteTensorCopyFromBuffer(input, impl.input_buffer.data(), impl.input_buffer.size()) != kTfLiteOk) {
        assign(error, "failed to copy the camera frame into the model input tensor");
        return false;
    }
    if (TfLiteInterpreterInvoke(impl.interpreter) != kTfLiteOk) {
        assign(error, "TFLite person-segmentation inference failed");
        return false;
    }

    const TfLiteTensor *output = TfLiteInterpreterGetOutputTensor(impl.interpreter, 0);
    OutputSpec output_spec;
    output_spec.shape = tensor_image_shape(output);
    if (!tensor_element(TfLiteTensorType(output), &output_spec.element)) {
        assign(error, "unsupported output tensor type");
        return false;
    }
    const TfLiteQuantizationParams output_quant = TfLiteTensorQuantizationParams(output);
    output_spec.quant = {output_quant.scale, output_quant.zero_point};
    output_spec.mode = impl.config.output_mode;
    output_spec.person_class_index = impl.config.person_class_index;
    output_spec.background_class_index = impl.config.background_class_index;
    output_spec.is_logits = impl.config.output_is_logits;

    impl.output_buffer.resize(TfLiteTensorByteSize(output));
    if (TfLiteTensorCopyToBuffer(output, impl.output_buffer.data(), impl.output_buffer.size()) != kTfLiteOk) {
        assign(error, "failed to read the segmentation output tensor");
        return false;
    }
    return decode_output(impl.output_buffer.data(), impl.output_buffer.size(), output_spec, person_confidence,
                         error);
}

std::unique_ptr<PersonSegmenter> create_tflite_segmenter(const TflitePersonSegmenterConfig &config,
                                                         std::string *error) noexcept {
    try {
        auto segmenter = std::make_unique<TflitePersonSegmenter>(config);
        if (!segmenter->valid()) {
            if (error != nullptr) {
                *error = segmenter->initialization_error();
            }
            return nullptr;
        }
        return segmenter;
    } catch (const std::exception &exception) {
        try {
            if (error != nullptr) {
                *error = exception.what();
            }
        } catch (...) {
            // Best effort: this path cannot report further failures.
        }
    } catch (...) {
        // Best effort: this path cannot report further failures.
    }
    return nullptr;
}

}  // namespace magic_portal
