/* Test double for the subset of the TensorFlow Lite C API used by the SDK glue.
 * It is NOT TensorFlow Lite: it exists so that the glue code (tensor shape handling,
 * pre/post-processing, ownership and error paths) can be tested without the real runtime.
 * The declarations follow the public C API; keep them in sync with the calls made by
 * src/tflite_person_segmenter.cpp. */
#ifndef FAKE_TENSORFLOW_LITE_C_C_API_H_
#define FAKE_TENSORFLOW_LITE_C_C_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum TfLiteStatus { kTfLiteOk = 0, kTfLiteError = 1 } TfLiteStatus;

typedef enum TfLiteType {
    kTfLiteNoType = 0,
    kTfLiteFloat32 = 1,
    kTfLiteInt32 = 2,
    kTfLiteUInt8 = 3,
    kTfLiteInt64 = 4,
    kTfLiteInt8 = 9
} TfLiteType;

typedef struct TfLiteQuantizationParams {
    float scale;
    int32_t zero_point;
} TfLiteQuantizationParams;

typedef struct TfLiteModel TfLiteModel;
typedef struct TfLiteInterpreterOptions TfLiteInterpreterOptions;
typedef struct TfLiteInterpreter TfLiteInterpreter;
typedef struct TfLiteTensor TfLiteTensor;

TfLiteModel *TfLiteModelCreate(const void *model_data, size_t model_size);
TfLiteModel *TfLiteModelCreateFromFile(const char *model_path);
void TfLiteModelDelete(TfLiteModel *model);

TfLiteInterpreterOptions *TfLiteInterpreterOptionsCreate(void);
void TfLiteInterpreterOptionsDelete(TfLiteInterpreterOptions *options);
void TfLiteInterpreterOptionsSetNumThreads(TfLiteInterpreterOptions *options, int32_t num_threads);

TfLiteInterpreter *TfLiteInterpreterCreate(const TfLiteModel *model,
                                           const TfLiteInterpreterOptions *optional_options);
void TfLiteInterpreterDelete(TfLiteInterpreter *interpreter);
TfLiteStatus TfLiteInterpreterAllocateTensors(TfLiteInterpreter *interpreter);
int32_t TfLiteInterpreterGetInputTensorCount(const TfLiteInterpreter *interpreter);
TfLiteTensor *TfLiteInterpreterGetInputTensor(const TfLiteInterpreter *interpreter, int32_t input_index);
TfLiteStatus TfLiteInterpreterInvoke(TfLiteInterpreter *interpreter);
int32_t TfLiteInterpreterGetOutputTensorCount(const TfLiteInterpreter *interpreter);
const TfLiteTensor *TfLiteInterpreterGetOutputTensor(const TfLiteInterpreter *interpreter,
                                                     int32_t output_index);

TfLiteType TfLiteTensorType(const TfLiteTensor *tensor);
int32_t TfLiteTensorNumDims(const TfLiteTensor *tensor);
int32_t TfLiteTensorDim(const TfLiteTensor *tensor, int32_t dim_index);
size_t TfLiteTensorByteSize(const TfLiteTensor *tensor);
TfLiteQuantizationParams TfLiteTensorQuantizationParams(const TfLiteTensor *tensor);
TfLiteStatus TfLiteTensorCopyFromBuffer(TfLiteTensor *tensor, const void *input_data, size_t input_data_size);
TfLiteStatus TfLiteTensorCopyToBuffer(const TfLiteTensor *output_tensor, void *output_data,
                                      size_t output_data_size);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_TENSORFLOW_LITE_C_C_API_H_ */
