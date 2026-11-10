// Observation and control hooks for the TFLite test double.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fake_tflite {

struct Observed {
    int models_live = 0;
    int options_live = 0;
    int interpreters_live = 0;
    int models_from_file = 0;
    int models_from_buffer = 0;
    int last_num_threads = 0;
    int invoke_count = 0;
    std::vector<std::uint8_t> last_input;  // bytes written into the input tensor
};

// Clears counters and the recorded input.
void reset();
const Observed &observed();

}  // namespace fake_tflite

// Model description format (plain text, one directive per line):
//   input  <float32|uint8|int8> <scale> <zero_point> <dim> <dim> ...
//   output <float32|uint8|int8> <scale> <zero_point> <dim> <dim> ...
//   fill   <v0> <v1> ...   per-pixel channel values (cycled over the output channels)
//   extra_input 1          report two input tensors (invalid for the SDK)
//   fail_allocate 1        TfLiteInterpreterAllocateTensors fails
//   fail_invoke 1          TfLiteInterpreterInvoke fails
