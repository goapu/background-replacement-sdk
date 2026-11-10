# Magic Portal SDK

A portable C++17 core for a "magic portal" background-replacement effect on a live camera
feed: the person is cut out with a segmentation model, a swirling portal opens around them,
and a new background is revealed behind them. The person's own pixels are never recolored
or regenerated; only the soft edge is blended.

```text
camera frame ──► Engine::process_frame  (worker thread)
                   copy/mirror → segment (TFLite or caller-supplied mask)
                   → refine (close holes, edge-aware filter, temporal filter)
                   → publish mask + bounds
                                              │
render thread:  Engine::render_state(now) ◄───┘   fresh mask? phase? uniforms?
                   │
                   ├─► GPU:  generated GLSL fragment modifier (Viro adapter)
                   └─► CPU:  PortalCompositor reference renderer (tests, previews)
```

## What is in the box

| Piece | Where | Notes |
|---|---|---|
| Stable C interface | `include/magic_portal/magic_portal.h` | Opaque handle, status codes, versioned structs, no exceptions across the boundary. Use this from Kotlin/JNI, Swift, or any FFI. |
| C++ engine | `include/magic_portal/engine.h` | Thread-safe hand-off between a worker and the render thread; fail-safe to the plain camera. |
| Mask refinement | `mask_refiner.h` | Morphology, camera-guided bilateral filter, frame-rate-independent temporal filter. |
| Transition logic | `portal_controller.h`, `portal_timeline.h` | Explicit state machine: live → transitioning → replaced. |
| Shader | `portal_shader.h` | GLSL is generated from the same constants table as the CPU renderer. |
| CPU reference | `portal_compositor.h` | Dependency-free renderer used for tests and previews. |
| TFLite backend | `tflite_person_segmenter.h` | Optional (`MAGIC_PORTAL_WITH_TFLITE`). Model from file or memory, uint8/int8/float32, multiclass outputs. |
| Android adapter | `adapters/viro/` | ViroCore integration over the engine. |

## Build and test

```bash
cmake --preset release            # or: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build --preset release
ctest --preset release
scripts/ci.sh                     # the full matrix: gcc, clang, ASan/UBSan, TSan, shared, reproducibility
```

See [docs/BUILD.md](docs/BUILD.md) for options, sanitizers, TFLite, Android, and installing the
package for `find_package(magic_portal)`.

## Minimal use (C)

```c
#include "magic_portal/magic_portal.h"

mp_engine_config config;
mp_engine_config_init(&config);
config.model_path = "selfie_segmenter.tflite";      /* needs a TFLite build */

char error[256];
mp_engine *engine = NULL;
if (mp_engine_create(&config, &engine, error, sizeof error) != MP_OK) { /* report error */ }

/* worker thread, ~15 times per second */
mp_engine_process_frame(engine, &frame, NULL);

/* render thread, every frame */
mp_render_state state = {0};
state.struct_size = sizeof state;
mp_engine_get_render_state(engine, now_ms, &state);
```

The complete reference, including threading and versioning rules, is in
[docs/C_API.md](docs/C_API.md).

## Behaviour you can rely on

- **Fails safe.** The effect runs only over a recent mask (default: younger than 500 ms).
  When segmentation stalls or fails, the unmodified camera is shown; a stale cut-out is
  never held on screen.
- **Starts only with a mask.** A transition cannot begin until a fresh mask exists.
- **No hidden threads in the engine.** `Engine` is driven by the caller; `PortalRunner`
  (used by the Android adapter) adds the worker thread and drops frames rather than queueing
  them.
- **Deterministic.** Time is passed in explicitly; given the same inputs the engine produces
  the same output. Golden files guard the numerics, and the effect's noise is an integer hash
  that is bit-identical on the CPU and in GLSL.

## Status

Read [docs/LAUNCH_READINESS.md](docs/LAUNCH_READINESS.md) before shipping. It separates what
is verified by the test suite from what can only be validated on real devices, with a real
model, and against the real ViroCore (the shader on phone GPUs, the ViroCore signatures, the
TFLite runtime, measured performance, and iOS are not covered by the automated tests). The
shader itself is checked against the CPU reference on desktop GPUs with `tools/shader_parity`.

## Repository layout

```text
include/magic_portal/   public headers (C and C++)
src/                    implementation
adapters/viro/          Android ViroCore adapter and integration guide
tests/                  unit, golden, C, adapter and packaging tests (+ test doubles)
tools/                  demo renderer, benchmark, mask-quality evaluation, shader parity check (GPU vs CPU)
docs/                   architecture, build, C API, device validation, launch readiness
scripts/                ci.sh (verification matrix), check_hygiene.sh, format.sh
```
