# Background Replacement SDK

[![CI](https://github.com/goapu/background-replacement-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/goapu/background-replacement-sdk/actions/workflows/ci.yml)

A C++17 SDK for camera background replacement using person segmentation, mask refinement,
and configurable portal transitions.

The SDK processes camera frames and publishes the masks and transition state needed by a
renderer. Applications provide camera capture, background images, and rendering resources.
The CMake package, headers, and C++ namespace use the name `magic_portal`; C functions use
the `mp_` prefix.

## Features

- C++ engine and a versioned C API for native applications and language bindings.
- Application-provided confidence masks or an optional TensorFlow Lite segmentation backend.
- Morphological closing, camera-guided bilateral filtering, and temporal mask smoothing.
- Configurable transitions, a generated GLSL ES 3.00 fragment modifier, and a CPU reference renderer.
- An optional worker-thread runner and an Android ViroCore adapter.
- Mask freshness checks, session-aware resets, and camera fallback when the effect cannot run.

## Requirements

- CMake 3.21 or later.
- A compiler with C++17 support.
- Ninja is optional.

The default build has no third-party runtime dependencies. TensorFlow Lite and ViroCore are
optional integrations and are not bundled. No segmentation model is included.

## Build and test

```bash
git clone https://github.com/goapu/background-replacement-sdk.git
cd background-replacement-sdk

cmake --preset release
cmake --build --preset release --parallel
ctest --preset release
```

This builds the static library, development tools, and tests. TensorFlow Lite inference is
disabled by default; the engine can accept masks from another segmentation system.

Additional presets are available for `debug-asan-ubsan`, `debug-tsan`, and `shared` builds.
See the [build guide](docs/BUILD.md) for configuration options and platform requirements.

## Use with CMake

Install the Release build to a local prefix:

```bash
cmake --install build/release --prefix "$PWD/build/install"
```

Set the consuming project's `CMAKE_PREFIX_PATH` to that installation directory, then link
the exported target:

```cmake
find_package(magic_portal 0.2 CONFIG REQUIRED)
target_link_libraries(app PRIVATE magic_portal::magic_portal)
```

Static linking requires C++ support in the consuming project, including for C applications.
The C++ API requires the matching SDK version; shared builds export the C API only.

## Integration

The following helper creates an engine configured to accept application-provided masks:

```c
#include <stdio.h>
#include "magic_portal/magic_portal.h"

mp_engine *create_engine(void) {
    mp_engine_config config;
    MP_ENGINE_CONFIG_INIT(&config);

    mp_engine *engine = NULL;
    char error[256] = {0};
    mp_status status = mp_engine_create(&config, &engine, error, sizeof error);
    if (status != MP_OK) {
        fprintf(stderr, "Engine initialization failed: %s\n", error);
        return NULL;
    }
    return engine;
}
```

The application then drives the engine:

1. On a worker thread, pass each camera frame and raw confidence mask to
   `mp_engine_process_frame`.
2. Once a fresh mask is available, call `mp_engine_begin_transition` with an application-assigned
   background ID.
3. On the render thread, call `mp_engine_get_render_state` and apply its mask and shader
   uniforms. Draw the camera unchanged when `effect_enable` is zero.
4. After processing and rendering have stopped, release the engine with `mp_engine_destroy`.

Frames use display-oriented RGBA8 pixels. Masks contain floating-point confidence values in
`[0, 1]`. Capture and render timestamps must use the same monotonic clock.
Negative status codes indicate errors; `MP_NO_PERSON` indicates a successful frame with no
detected person.

`Engine` is caller-driven; `PortalRunner` provides optional worker scheduling. Masks older
than the configured limit, 500 ms by default, disable the effect. Inference failures retain
the previous mask until it expires. An empty scene is a valid segmentation result.

See the [C API reference](docs/C_API.md) for struct initialization, buffer ownership,
threading, session IDs, and rendering conventions.

## TensorFlow Lite backend

Build with `MAGIC_PORTAL_WITH_TFLITE=ON` and set `TFLITE_INCLUDE_DIR` and `TFLITE_LIBRARY`.
Configure a model file or memory buffer before engine creation, then pass `NULL` as the raw
mask to `mp_engine_process_frame`.

The backend supports float32, uint8, and int8 tensors. Models must expose one RGB or RGBA
image input in NHWC or HWC layout; only the first output tensor is read. Input normalization,
class indices, and logits handling must match the model. See the
[TFLite build instructions](docs/BUILD.md#tensorflow-lite--litert).

## Validation and project status

Run the local verification matrix with:

```bash
bash scripts/ci.sh
```

The matrix checks formatting, static analysis, sanitizers, regression tests, installation,
Linux shared-library exports, and reproducibility. Unavailable tools are reported as skipped.
GitHub Actions also cross-compiles Android ARM64 and verifies CMake 3.21.4 compatibility.

This is a development SDK. Automated TFLite and ViroCore tests use test doubles; model
accuracy, phone GPU rendering, performance, and lifecycle behavior need device validation.
Android JNI/Kotlin/AAR packaging and iOS capture and rendering bridges are not included.

See [launch readiness](docs/LAUNCH_READINESS.md) and the
[device validation plan](docs/DEVICE_VALIDATION.md) for the remaining release work.

## Documentation

| Reference | Content |
|---|---|
| [Architecture](docs/ARCHITECTURE.md) | Processing stages, threading, state, and coordinate conventions |
| [Build guide](docs/BUILD.md) | Build options, sanitizers, dependencies, installation, and Android builds |
| [C API](docs/C_API.md) | Lifecycle, status codes, ABI versioning, and rendering contract |
| [Android integration](adapters/viro/ANDROID_INTEGRATION.md) | ViroCore adapter setup and lifecycle |
| [Shader parity](tools/shader_parity/README.md) | Comparing GPU output with the CPU reference |

## License

A project license file has not yet been added. Optional runtime and model dependencies are
listed in [third-party notices](THIRD_PARTY_NOTICES.md).
