# Build guide

Requirements: CMake 3.21+, a C++17 compiler (GCC, Clang, MSVC), and optionally Ninja.
The core has no third-party dependencies.

CMake 3.21 is required for `PROJECT_IS_TOP_LEVEL`, which controls the default installation
rules. CI also builds and runs the installed-consumer tests with CMake 3.21.4.

## Everyday

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Presets: `release`, `debug-asan-ubsan`, `debug-tsan`, `shared`. All enable
`MAGIC_PORTAL_WARNINGS_AS_ERRORS`. Without presets:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Options

| Option | Default | Meaning |
|---|---|---|
| `MAGIC_PORTAL_BUILD_TESTS` | ON | Unit, golden, C, adapter-stub, TFLite-glue and packaging tests |
| `MAGIC_PORTAL_BUILD_TOOLS` | ON | Demo renderer, benchmark, mask evaluation, shader parity fixture generator |
| `MAGIC_PORTAL_BUILD_SHARED` | OFF | Shared library exporting only the C interface (Linux uses a version script) |
| `MAGIC_PORTAL_WITH_TFLITE` | OFF | Native TFLite segmenter; needs `TFLITE_INCLUDE_DIR` and `TFLITE_LIBRARY` |
| `MAGIC_PORTAL_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` |
| `MAGIC_PORTAL_SANITIZE` | "" | e.g. `address;undefined` or `thread` |

`-ffp-contract=off` is applied to the library, tests and tools so floating-point results do
not depend on a compiler's fused multiply-add default.

On macOS, the archive creation and indexing rules set `ZERO_AR_DATE=1`, so static libraries
do not include changing member timestamps. This applies to ordinary CMake builds as well as CI.
UBSan builds use `-fno-sanitize-recover=undefined`: a diagnostic fails the process and CTest.

Style settings are validated by the engine, CPU compositor and Viro adapter. In particular,
`person_protection` must be greater than the shader guard threshold (currently 0.72) and at
most 1. All timeline fractions must lie in [0, 1] and their intervals must remain ordered.

## TensorFlow Lite / LiteRT

```bash
cmake -S . -B build -DMAGIC_PORTAL_WITH_TFLITE=ON \
  -DTFLITE_INCLUDE_DIR=/path/to/include -DTFLITE_LIBRARY=/path/to/libtensorflowlite_c.so
```

The glue is tested against a test double of the C API (`tests/fake_tflite`). It has **not**
been run against the real runtime or a real model in this repository; do that with the
checks in `docs/DEVICE_VALIDATION.md`.

Model contract: one image input (NHWC, RGB or RGBA; float32, uint8 or int8), first output is
an NHWC mask. Configure `person_class_index`, `input_mean`/`input_std`, logits vs probabilities,
`output_mode` (single class, or one-minus-background for multiclass models) and `resize_mode`.

## Android (NDK)

```bash
cmake -S . -B build/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
  -DMAGIC_PORTAL_BUILD_TESTS=OFF -DMAGIC_PORTAL_BUILD_TOOLS=OFF -DMAGIC_PORTAL_BUILD_SHARED=ON
```

The ViroCore adapter is compiled inside the Viro renderer; see
`adapters/viro/ANDROID_INTEGRATION.md`.

## Installing and consuming

```bash
cmake --install build --prefix /opt/magic_portal
```

```cmake
find_package(magic_portal 0.1 REQUIRED)
target_link_libraries(app PRIVATE magic_portal::magic_portal)
```

The test `consumer_package` does exactly this against a scratch prefix on every run.

Without CMake, link the static library together with the C++ runtime, the math library and
threads, for example `cc app.c -lmagic_portal -lstdc++ -lm -lpthread` (the order matters for
static libraries: the SDK first, then its dependencies). The CMake package declares these for
you. The shared build exports only the `mp_*` C interface and needs nothing extra.

## Shader parity on a GPU

The effect shader cannot be validated without a GPU, so the SDK ships a checker that runs the
generated GLSL in any WebGL2 browser and compares it with the CPU reference:

```bash
cmake --build build --target magic_portal_shader_parity_gen
build/magic_portal_shader_parity_gen tools/shader_parity/fixtures.json
cd tools/shader_parity && python3 -m http.server 8000     # open the page on the device
```

See `tools/shader_parity/README.md` for what it verifies and the pass criteria. Run it for
each GPU family in the device matrix and keep the result JSON as evidence.

## Verification matrix

`scripts/ci.sh` runs, from clean build directories: Release with GCC and Clang (plus the
mask-quality gate), ASan+UBSan, TSan, the shared build (and checks that only `mp_*` symbols
are exported), and a reproducibility check (two builds in different directories must produce
a byte-identical `libmagic_portal.a`). `scripts/check_hygiene.sh` rejects machine-specific
paths, large files and media. `scripts/format.sh` applies `.clang-format`.
Unavailable tools are reported as skipped. In particular, macOS's `g++` alias for Clang is not
counted as a GCC run; real GCC coverage comes from the Linux workflow.

## Golden files

`tests/golden/*.bin` were recorded from the original implementation before any refactoring
and are compared with a small tolerance (±3 per byte, 2e-4 per float). If a change is
*meant* to alter output, run `cmake --build build --target update_goldens`, review the
difference, and record why in `CHANGELOG.md`.

`compose.bin` was re-recorded once, for the noise-hash and vertical-convention change listed in
the changelog (0.1.0). `refine.bin` and `timeline.bin` are unchanged from the original
recordings, and the live and fully-replaced frames inside `compose.bin` are byte-identical to
the original recording; only the frames that show dissolve noise, sparks or ring position
differ (see the header comment of `tests/golden_test.cpp`).
