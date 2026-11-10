# Changelog

## 0.2.0

Reliability release. It resolves the findings of the lead engineering review of 0.1.0
(failure handling, ordering, validation, evaluation honesty). The C interface changes are
additive; `MP_ABI_VERSION` stays 1.

### Fixed
- **The macOS reproducibility check failed on archive timestamps.** CMake now runs the
  toolchain's archive creation, append and indexing commands with `ZERO_AR_DATE=1`, so the
  fix applies to normal builds as well as CI.
- **Extreme timestamp differences could still overflow.** Timeline elapsed time and the
  temporal refiner now use the same saturating subtraction as freshness. Forward intervals
  spanning the int64 limits complete normally; reversed timeline time clamps to zero.
- **Accepted styles could invoke undefined GLSL interpolation.** `person_protection` must
  exceed `kPersonGuardStart` (currently 0.72). The engine, compositor and Viro style setter all
  enforce the same constraint before use; invalid Viro settings leave uniforms unchanged.
- Timeline fractions are now all constrained to [0, 1], in addition to interval ordering.
- The declared minimum CMake version is 3.21, matching `PROJECT_IS_TOP_LEVEL` and the presets.
  A separate workflow job builds and tests the package with CMake 3.21.4.
- UBSan diagnostics now terminate the process, preventing a test with undefined behavior from
  being reported as passed. The CI script reports a Clang-backed `g++` as a skipped GCC run.
- **A failed mask upload no longer keeps the effect running on a stale GPU mask.** The Viro
  adapter tracks whether the mask on the GPU is the one the engine published. After a failed
  upload it fails safe (plain camera), retries on the next frame, and exposes
  `mask_upload_failures()` / `consecutive_mask_upload_failures()`. The new texture is committed
  only after the upload succeeded.
- **Evaluation could pass a bad model.** Boundary F1 gave two opposite masks a score of 1.0
  because pixels outside the image were ignored. Out-of-image neighbours are now
  background. `magic_portal_eval --dataset --gate` also exited 0 without checking anything;
  it now evaluates every clip, applies the thresholds (`--min-iou`, `--min-boundary-f1`,
  `--max-flicker`, `--max-iou-loss`), and exits 1 on a failed threshold and on unusable data (2 for bad usage).
  Per-clip results are reported for datasets that contain sub-folders.
- **An older frame could replace a newer mask.** Timestamps must now be non-decreasing: an
  older frame is rejected with `kInvalidArgument` / `MP_ERROR_INVALID_ARGUMENT` and counted in
  `frames_rejected_out_of_order`. Freshness is judged on the timestamp of the mask that is on
  screen, and a mask stamped more than `future_tolerance_ms` (default 100) ahead of `now` is
  treated as stale. Age arithmetic saturates instead of overflowing.
- **A frame queued before `reset()` could be published after it.** Frames carry a session id
  (`Engine::session_id()`, `mp_engine_session_id()`, `mp_frame.session_id`); a frame from a
  previous session is dropped and counted in `frames_discarded_by_reset`. The runner reads the
  session before it captures and `PortalRunner::reset()` drops its pending frame. The Android
  frame listener's `reset()` uses it.
- **Untrusted sizes and styles.** Frame dimensions are limited to 16384 per side and 64
  megapixels, masks to 4096 per side and 16 megapixels, and every size, stride and buffer
  computation is done in 64-bit arithmetic (a signed overflow in `width * 4` and in the runner
  is gone). `PortalStyle` is validated (finite values; `ring_width` and `reveal_feather` in
  (0, 0.5], strengths in [0, 8], `person_protection` in (0.72, 1]), and so are the refiner
  parameters (radius up to 16, finite sigmas) and timeline fractions. NaN and infinity are
  rejected by the engine, the C interface and the compositor.
- **The CPU compositor disagreed with the engine and the shader for a missing person.** It
  returned the plain camera when the bounds were invalid, while the engine and shader treat a
  successful empty scene as "nobody in view" (the destination fills the view). The three now
  agree; see "Changed". The compositor also honours the mask crop that the shader applies.
- **The runner kept the older pending frame although it advertises "latest wins".** It now
  captures only when the worker is idle, so the frame that is processed is the newest one, and
  an inconsistent frame from the source (wrong size, stride or buffer) is counted as a failed
  capture instead of being read.
- **The borrowed mask pointer could dangle across `reset()`.** `reset()` no longer frees the
  render thread's copy; it is released by the next `render_state()` on the render thread, so the
  documented lifetime ("valid until the next render-state call") holds.
- Size-unaware `mp_engine_config_init` could overrun a config struct from an older header; see
  `mp_engine_config_init_sized` below.

### Added
- C interface (additive): `mp_engine_session_id`, `mp_frame.session_id`,
  `mp_render_state.mask_timestamp_ms`, `mp_stats.frames_rejected_out_of_order`,
  `mp_engine_config_init_sized` and the `MP_ENGINE_CONFIG_INIT` macro (initialises exactly the
  bytes the caller's struct has).
- `include/magic_portal/limits.h`: the size limits and the checked arithmetic helpers.
- `PortalRunner::reset()`, `CompositeInput::mask_crop`, `validate_style()`,
  `RenderState::mask_timestamp_ms`, `EngineStats::frames_rejected_out_of_order`.
- Tests for every item above, including a C-interface test (`test_c_api.cpp`) and a test of the
  evaluation command line (`test_eval_cli.cpp`); the evaluation logic moved to
  `tools/eval_cli.h` so it can be tested.
- Shader parity: 24 scenarios (was 16), adding the empty scene during a transition and after
  completion, a person re-entering, and cropped views. All 24 pass on software GL (SwiftShader).
- Verification: `scripts/ci.sh` and the workflow now run the formatter check and clang-tidy
  (warnings are errors), not only the build and tests.

### Changed
- **Behaviour:** a fresh mask with nobody in it (invalid bounds) is a successful empty scene.
  The effect runs with a zero mask, so the destination fills the view. Previously the CPU
  reference showed the plain camera in this case. A stale, failed or reset mask still shows the
  plain camera. If you relied on the old CPU-reference output for this case, update it.
- **Behaviour:** frames must have non-decreasing timestamps (see Fixed).
- **Behaviour:** `reset()` is lazy for the render thread's copy of the mask (see Fixed), and
  the runner skips a capture while the worker is busy.
- The golden files are unchanged.
- Version bumped to 0.2.0 (`CMakeLists.txt` and `magic_portal.h` are checked against each
  other at configure time).

## 0.1.0

First packaged release of the SDK.

### Added
- Stable C interface (`magic_portal.h`): opaque engine handle, status codes, versioned structs,
  ABI version, optional shared build exporting only `mp_*`.
- `Engine`: thread-safe worker/render hand-off, explicit timestamps, stale-mask fail-safe,
  reset handling for in-flight frames, statistics.
- `PortalController` state machine (live / transitioning / replaced) and `MaskFreshness`.
- `PortalRunner`: worker thread with a one-slot latest-frame hand-off and recycled buffers.
- Shared constants table and generated GLSL, so CPU and GPU paths use identical values.
- TFLite glue: model from memory or file, uint8/int8/float32 tensors, multiclass
  (one-minus-background) outputs, area resizing, shared pre/post-processing (`segmenter_io`).
- CMake: install and export (`magic_portal::magic_portal`), presets, sanitizer option,
  warnings-as-errors, `-ffp-contract=off`.
- Tests: golden regression files, unit tests, C11 smoke test, TFLite glue tests against a test
  double, Viro adapter tests against stub headers, install-and-consume test.
- Tools: benchmark, mask-quality evaluation (synthetic gate and dataset mode), and a shader
  parity check (`tools/shader_parity`) that runs the generated shader on a GPU through WebGL2
  and compares it with the CPU reference, including a bit-exact test of the noise hash.
- Scripts and configuration: verification matrix, hygiene check, formatter, CI workflow.

### Changed
- The visible dissolve noise and spark pattern differ from earlier internal builds (a
  consequence of the hash replacement). `tests/golden/compose.bin` was re-recorded; the live and
  fully-replaced frames in it are byte-identical to the original recording, and `refine.bin`
  and `timeline.bin` are unchanged.
- Viro adapter rewritten over the engine: background registry with ids, shader modifier
  removed on teardown, exceptions never escape into the render loop, `GL_OES_texture_float_linear`
  detected (nearest-filter fallback), mirror work moved off the render thread, style uniforms
  now reach the shader.
- Mask refiner: separable morphology and raw-pointer loops (numerically identical to the
  recorded golden files).
- A transition now requires a fresh mask and a mask that stops arriving no longer stays on
  screen: the plain camera is shown after `stale_after_ms` (default 500 ms).

### Fixed
- Shader maximum reveal radius was a hard-coded constant that disagreed with the CPU path;
  both now use the farthest-corner distance.
- Spark hash differed between CPU and GPU; both now use the same function of progress.
- The dissolve and spark noise used `fract(sin(dot(cell, k)) * 43758.5)`, which is not
  reproducible: a one-step difference in the sine (another GPU, a fused multiply-add, the CPU)
  changes the result completely, so the noise pattern differed from device to device and from
  the CPU reference. It is replaced by a 32-bit integer hash (`portal_hash.h`) that is exact on
  every CPU and in GLSL ES 3.00; the shader text is generated from the same constants.
- The CPU reference evaluated angle, noise and ring terms in top-left screen space while the
  shader uses the bottom-left `gl_FragCoord` space, so rings and sparks were mirrored between
  the two. The CPU reference now uses the GPU convention.
- The static library did not declare its dependency on libm; a plain C link of the static
  library failed on some toolchains. It is declared now (non-Apple, non-Android Unix).
- A second transition used the live camera as the "old" background instead of the previously
  revealed one.
- The replaced background was never committed after a transition completed.
