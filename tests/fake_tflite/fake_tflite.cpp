#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "fake_tflite_control.h"
#include "tensorflow/lite/c/c_api.h"

namespace {

fake_tflite::Observed g_observed;

struct TensorSpec {
    TfLiteType type = kTfLiteFloat32;
    float scale = 0.0F;
    int zero_point = 0;
    std::vector<int> dims;
    bool present = false;
};

struct ModelSpec {
    TensorSpec input;
    TensorSpec output;
    std::vector<float> fill;
    bool extra_input = false;
    bool fail_allocate = false;
    bool fail_invoke = false;
    bool valid = false;
};

bool parse_type(const std::string &text, TfLiteType *type) {
    if (text == "float32") {
        *type = kTfLiteFloat32;
        return true;
    }
    if (text == "uint8") {
        *type = kTfLiteUInt8;
        return true;
    }
    if (text == "int8") {
        *type = kTfLiteInt8;
        return true;
    }
    return false;
}

ModelSpec parse(const std::string &text) {
    ModelSpec spec;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::istringstream words(line);
        std::string key;
        if (!(words >> key)) {
            continue;
        }
        if (key == "input" || key == "output") {
            TensorSpec tensor;
            std::string type;
            if (!(words >> type >> tensor.scale >> tensor.zero_point) || !parse_type(type, &tensor.type)) {
                return {};
            }
            int dim = 0;
            while (words >> dim) {
                tensor.dims.push_back(dim);
            }
            tensor.present = true;
            (key == "input" ? spec.input : spec.output) = tensor;
        } else if (key == "fill") {
            float value = 0.0F;
            while (words >> value) {
                spec.fill.push_back(value);
            }
        } else if (key == "extra_input") {
            spec.extra_input = true;
        } else if (key == "fail_allocate") {
            spec.fail_allocate = true;
        } else if (key == "fail_invoke") {
            spec.fail_invoke = true;
        }
    }
    spec.valid = spec.input.present && spec.output.present && !spec.fill.empty();
    return spec;
}

size_t element_bytes(TfLiteType type) {
    return type == kTfLiteFloat32 ? 4U : 1U;
}

size_t element_count(const std::vector<int> &dims) {
    size_t count = 1;
    for (int d : dims) {
        count *= static_cast<size_t>(d);
    }
    return count;
}

}  // namespace

struct TfLiteTensor {
    TensorSpec spec;
    std::vector<std::uint8_t> bytes;
};

struct TfLiteModel {
    std::string path;
    const std::uint8_t *data = nullptr;  // like real TFLite: the caller's buffer is NOT copied
    size_t size = 0;
    bool from_file = false;
    std::string file_text;

    std::string text() const {
        if (from_file) {
            return file_text;
        }
        return std::string(reinterpret_cast<const char *>(data), size);
    }
};

struct TfLiteInterpreterOptions {
    int threads = 1;
};

struct TfLiteInterpreter {
    const TfLiteModel *model = nullptr;
    ModelSpec spec;
    TfLiteTensor input;
    TfLiteTensor output;
    bool allocated = false;
};

namespace fake_tflite {
void reset() {
    g_observed = Observed();
}
const Observed &observed() {
    return g_observed;
}
}  // namespace fake_tflite

extern "C" {

TfLiteModel *TfLiteModelCreate(const void *model_data, size_t model_size) {
    if (model_data == nullptr || model_size == 0U) {
        return nullptr;
    }
    auto *model = new TfLiteModel();
    model->data = static_cast<const std::uint8_t *>(model_data);
    model->size = model_size;
    ++g_observed.models_live;
    ++g_observed.models_from_buffer;
    return model;
}

TfLiteModel *TfLiteModelCreateFromFile(const char *model_path) {
    std::ifstream in(model_path == nullptr ? "" : model_path, std::ios::binary);
    if (!in) {
        return nullptr;
    }
    auto *model = new TfLiteModel();
    model->from_file = true;
    model->path = model_path;
    model->file_text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    ++g_observed.models_live;
    ++g_observed.models_from_file;
    return model;
}

void TfLiteModelDelete(TfLiteModel *model) {
    if (model != nullptr) {
        --g_observed.models_live;
        delete model;
    }
}

TfLiteInterpreterOptions *TfLiteInterpreterOptionsCreate(void) {
    ++g_observed.options_live;
    return new TfLiteInterpreterOptions();
}

void TfLiteInterpreterOptionsDelete(TfLiteInterpreterOptions *options) {
    if (options != nullptr) {
        --g_observed.options_live;
        delete options;
    }
}

void TfLiteInterpreterOptionsSetNumThreads(TfLiteInterpreterOptions *options, int32_t num_threads) {
    options->threads = num_threads;
}

TfLiteInterpreter *TfLiteInterpreterCreate(const TfLiteModel *model,
                                           const TfLiteInterpreterOptions *options) {
    ModelSpec spec = parse(model->text());
    if (!spec.valid) {
        return nullptr;
    }
    auto *interpreter = new TfLiteInterpreter();
    interpreter->model = model;
    interpreter->spec = spec;
    interpreter->input.spec = spec.input;
    interpreter->output.spec = spec.output;
    g_observed.last_num_threads = options != nullptr ? options->threads : 0;
    ++g_observed.interpreters_live;
    return interpreter;
}

void TfLiteInterpreterDelete(TfLiteInterpreter *interpreter) {
    if (interpreter != nullptr) {
        --g_observed.interpreters_live;
        delete interpreter;
    }
}

TfLiteStatus TfLiteInterpreterAllocateTensors(TfLiteInterpreter *interpreter) {
    if (interpreter->spec.fail_allocate) {
        return kTfLiteError;
    }
    interpreter->input.bytes.assign(
        element_count(interpreter->input.spec.dims) * element_bytes(interpreter->input.spec.type), 0);
    interpreter->output.bytes.assign(
        element_count(interpreter->output.spec.dims) * element_bytes(interpreter->output.spec.type), 0);
    interpreter->allocated = true;
    return kTfLiteOk;
}

int32_t TfLiteInterpreterGetInputTensorCount(const TfLiteInterpreter *interpreter) {
    return interpreter->spec.extra_input ? 2 : 1;
}

TfLiteTensor *TfLiteInterpreterGetInputTensor(const TfLiteInterpreter *interpreter, int32_t index) {
    return index == 0 ? const_cast<TfLiteTensor *>(&interpreter->input) : nullptr;
}

int32_t TfLiteInterpreterGetOutputTensorCount(const TfLiteInterpreter *) {
    return 1;
}

const TfLiteTensor *TfLiteInterpreterGetOutputTensor(const TfLiteInterpreter *interpreter, int32_t index) {
    return index == 0 ? &interpreter->output : nullptr;
}

TfLiteStatus TfLiteInterpreterInvoke(TfLiteInterpreter *interpreter) {
    ++g_observed.invoke_count;
    // Re-read the model description on every call: if the glue handed us a buffer it did
    // not keep alive, this read is a use-after-free that AddressSanitizer reports.
    const ModelSpec spec = parse(interpreter->model->text());
    if (!interpreter->allocated || !spec.valid || spec.fail_invoke) {
        return kTfLiteError;
    }
    g_observed.last_input = interpreter->input.bytes;

    TfLiteTensor &out = interpreter->output;
    const size_t channels = static_cast<size_t>(out.spec.dims.back());
    const size_t pixels = element_count(out.spec.dims) / channels;
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        for (size_t c = 0; c < channels; ++c) {
            const float value = spec.fill[c % spec.fill.size()];
            const size_t index = pixel * channels + c;
            switch (out.spec.type) {
                case kTfLiteFloat32:
                    std::memcpy(out.bytes.data() + index * 4U, &value, 4U);
                    break;
                case kTfLiteUInt8: {
                    const long q = std::lround(value / out.spec.scale) + out.spec.zero_point;
                    out.bytes[index] = static_cast<std::uint8_t>(std::clamp(q, 0L, 255L));
                    break;
                }
                case kTfLiteInt8: {
                    const long q = std::lround(value / out.spec.scale) + out.spec.zero_point;
                    out.bytes[index] =
                        static_cast<std::uint8_t>(static_cast<std::int8_t>(std::clamp(q, -128L, 127L)));
                    break;
                }
                default:
                    return kTfLiteError;
            }
        }
    }
    return kTfLiteOk;
}

TfLiteType TfLiteTensorType(const TfLiteTensor *tensor) {
    return tensor->spec.type;
}
int32_t TfLiteTensorNumDims(const TfLiteTensor *tensor) {
    return static_cast<int32_t>(tensor->spec.dims.size());
}
int32_t TfLiteTensorDim(const TfLiteTensor *tensor, int32_t index) {
    return tensor->spec.dims[static_cast<size_t>(index)];
}
size_t TfLiteTensorByteSize(const TfLiteTensor *tensor) {
    return tensor->bytes.size();
}

TfLiteQuantizationParams TfLiteTensorQuantizationParams(const TfLiteTensor *tensor) {
    return {tensor->spec.scale, tensor->spec.zero_point};
}

TfLiteStatus TfLiteTensorCopyFromBuffer(TfLiteTensor *tensor, const void *data, size_t size) {
    if (size != tensor->bytes.size()) {
        return kTfLiteError;
    }
    std::memcpy(tensor->bytes.data(), data, size);
    return kTfLiteOk;
}

TfLiteStatus TfLiteTensorCopyToBuffer(const TfLiteTensor *tensor, void *data, size_t size) {
    if (size != tensor->bytes.size()) {
        return kTfLiteError;
    }
    std::memcpy(data, tensor->bytes.data(), size);
    return kTfLiteOk;
}

}  // extern "C"
