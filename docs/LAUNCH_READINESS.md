# Launch readiness

"Ready to launch" is a claim about evidence. This page lists what has evidence and what does
not. A gate marked **open** must be closed (or consciously accepted by the owner) before a
public release.

Legend: **verified** = covered by an automated check you can re-run; **open** = needs real
devices, real dependencies, or an owner decision.

## Verified (re-run with `scripts/ci.sh`)

| Claim | Evidence |
|---|---|
| Behaviour of the refiner, timeline and CPU compositor is unchanged by the refactor | Golden files recorded from the original code before changes (`ctest -R golden`); legacy tests still pass. `compose.bin` was re-recorded once for the noise-hash change (see `CHANGELOG.md`); its live and fully-replaced frames are byte-identical to the original and `refine.bin` / `timeline.bin` are untouched |
| Unit behaviour of frame utilities, controller, freshness, geometry, segmenter I/O, engine, runner | `ctest -R unit` |
| No data races in the engine and runner | `scripts/ci.sh` TSan stage |
| No memory errors / undefined behaviour in tests | ASan + UBSan stage |
| Builds warning-free with `-Werror` on GCC and Clang; sources match `.clang-format`; clang-tidy (repo `.clang-tidy`, warnings are errors) is clean on `src/` | `scripts/ci.sh` format, release and static-analysis stages (also run by the workflow) |
| Public C header is valid C11; the C interface works end to end without C++ in the client | `ctest -R c_api_smoke` |
| Every public header compiles standalone | `magic_portal_header_check` target |
| Package installs and a separate project can `find_package` and link it (C and C++) | `ctest -R consumer_package` |
| A shared build exports only `mp_*` symbols | `scripts/ci.sh` shared stage |
| Two static-library builds in different directories are byte-identical on a given toolchain; macOS archive timestamps are suppressed by the CMake build rules | `scripts/ci.sh` reproducibility stage |
| Generated GLSL has no unresolved tokens, balanced braces, every uniform declared once, constants identical to C++ | `ctest -R unit` (shader tests) |
| The noise hash is reproducible: integer-only, known-answer values pinned in C++, and bit-identical on a GPU (5,504 of 5,504 cells) | `ctest -R unit`; `tools/shader_parity` on software GL (SwiftShader) and on an Apple M2 (WebGL2 over ANGLE/Metal) |
| The generated shader compiles and matches the CPU reference on software GL (SwiftShader, WebGL2): 24 of 24 scenarios pass, worst mean error 0.86 of 255, the noise hash is exact (5,504 of 5,504 cells), and the bypass paths (live camera, stale-mask fallback) are bit-identical. The scenarios include an empty scene during and after a transition, a person re-entering, and cropped views | `tools/shader_parity` (`run` steps in its README). The Apple M2 run (WebGL2 over ANGLE/Metal) was made with the earlier 16 scenarios; repeat it with the 24-scenario fixtures to refresh that evidence |
| Failure handling: a failed mask upload fails safe and recovers on retry; an older frame never replaces a newer mask; a frame queued before `reset()` is never published after it; a mask from the future is not trusted; none of these can be bypassed by a late frame through the runner | `ctest -R unit` (engine, runner, freshness), `ctest -R viro_adapter_stubs`, `ctest -R c_api_smoke`. The regression tests for these cases were run against the 0.1.0 code first: 11 unit tests and 1 adapter test failed there |
| Hostile or corrupt inputs are rejected before memory is touched: frame and mask dimensions, strides and buffer sizes are checked in 64-bit arithmetic; NaN, infinity and out-of-range style, refiner and timeline values are rejected | `ctest -R unit`, `ctest -R c_api_smoke`, ASan/UBSan stage (the 0.1.0 code showed signed overflow and an oversize allocation under these inputs) |
| Timeline and temporal-refiner timestamp differences cannot overflow; accepted styles have ordered shader guard edges; timeline fractions are constrained to [0, 1]; the Viro style setter validates before writing uniforms | `ctest -R unit`, `ctest -R viro_adapter_stubs`, ASan/UBSan with non-recovering diagnostics. Five unit regressions and one adapter regression failed before these fixes |
| The declared CMake minimum configures installation rules and passes the installed-consumer test | CMake 3.21.4 build and `consumer_package`; separate minimum-version workflow job |
| The CPU compositor, the engine and the shader agree on a missing person (destination fills the view), on a stale or failed mask (plain camera) and on mask crops | `ctest -R unit`; the empty-scene and cropped scenarios in `tools/shader_parity` |
| The quality gate can fail: boundary F1 does not reward opposite masks, and `magic_portal_eval --dataset --gate` exits 1 on a failed threshold or unusable data | `ctest -R unit` (evaluation tests) |
| Builds and passes all tests on ARM64 (Linux VM on Apple silicon), re-run for 0.2.0: 88 of 88 unit tests, the legacy, C-interface, glue and adapter tests and the quality gate pass; goldens match with zero difference; ASan/UBSan and TSan unit runs are clean; and the 24-scenario shader parity fixtures (engine state, adapter uniforms, CPU reference frames, generated GLSL) are byte-identical to those produced on x86-64 | Re-run on an ARM64 host: build `magic_portal_shader_parity_gen`, run it, compare the SHA-256 of `fixtures.json` with an x86-64 build |
| TFLite glue: ownership of in-memory models (a mutation that stops copying the buffer is caught by ASan), tensor types, normalization, multiclass, quantization, error paths | `ctest -R tflite_glue` against a test double of the C API |
| Viro adapter logic: uniforms, mask texture lifecycle, background registry, fail-safe, detach, end-to-end flow | `ctest -R viro_adapter_stubs` against stub headers |
| Mask refinement reduces flicker and does not lose accuracy on controlled synthetic noise | `magic_portal_eval --gate` |
| Repository has no machine-specific paths, large files or media | `scripts/check_hygiene.sh` |

## Open gates

| # | Gate | Why it is open | How to close |
|---|---|---|---|
| 1 | GLSL compiles and renders correctly on **phone** GPUs and inside ViroCore | Verified on desktop-class GL through WebGL2 only (see above); no phone GPU or native GLES driver was available | Run `tools/shader_parity` on each device and the in-ViroCore checks, `docs/DEVICE_VALIDATION.md` §2 |
| 2 | Adapter matches the real ViroCore API, including how `camera_texture` is declared (plain `sampler2D` vs external texture) and the background texture orientation | Tests use stub headers written from the expected API; the shader declares its own `camera_texture` | §1; compile in the Viro renderer; `adapters/viro/ANDROID_INTEGRATION.md` ("Known integration risk") |
| 3 | TFLite glue works with the real runtime and a real model | Tests use a test double of the C API; no model is bundled | §1, §4 |
| 4 | Segmentation quality on real people | Only synthetic data was available | §4 |
| 5 | Performance and thermals on phones | Benchmarks ran on a development VM | §5 |
| 6 | Lifecycle, orientation, device matrix | Needs devices | §3, §6, §7 |
| 7 | Android packaging (AAR / JNI / Kotlin API) | Not included: the C interface is the intended binding surface, but no JNI layer is provided | Add a thin JNI + Kotlin wrapper over `magic_portal.h` |
| 8 | iOS | Not started. The engine and C interface are portable, but an ARKit/AVFoundation capture bridge and a Metal/GLSL shader port are required | Plan a separate phase |
| 9 | CI workflow on GitHub | `.github/workflows/ci.yml` has not been executed; it only calls `scripts/ci.sh` (which was run) and an NDK cross-compile | Push and fix any runner differences (macOS compiler, NDK path) |
| 10 | License and IP ownership | No `LICENSE` is included; choosing one is the owner's decision | Decide, add `LICENSE`, update `THIRD_PARTY_NOTICES.md` |
| 11 | Model licensing and provenance | No model is bundled | Confirm the license of the model you ship and record it |
| 12 | Privacy statement | The SDK processes camera frames in memory on-device and has no network code, but the app's disclosures are the owner's | Review store/privacy disclosures |

## Known limitations

- The shader parity check runs through the browser's WebGL2 stack. It proves the shader logic
  and the integer hash on two implementations, but a phone's native OpenGL ES driver inside
  ViroCore can still differ (precision, extension support); gate 1 stays open for that reason.

- The effect needs `GL_OES_texture_float_linear` for smooth mask edges; without it the adapter
  falls back to nearest filtering (blockier edges) and reports it.
- The mask texture is R32F (4 bytes per texel). A smaller format would save bandwidth but was
  not changed because the corresponding ViroCore API could not be checked here.
- Only the first model output tensor is used.
- The shader runs the full effect path for every pixel while a transition is active; a
  tile-skipping optimisation was not attempted without a GPU to profile on.
- `MaskRefiner` takes about 14 ms for a 256x256 mask on the x86-64 development VM and about
  8 ms on the ARM64 VM (Release); neither is a phone. It runs
  on the worker thread, but on slower phones it may limit the achievable inference rate.
  Optimising it must keep the golden files within tolerance.

## Sign-off record

| Gate | Owner | Date | Evidence link |
|---|---|---|---|
| | | | |
