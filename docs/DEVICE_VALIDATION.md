# Device validation plan

The automated tests prove the SDK's logic, but they run without a GPU, a camera, the real
ViroCore, the real TFLite runtime, or a real model. These checks close that gap. Record the
results in `docs/LAUNCH_READINESS.md` before shipping.

## 1. Build against the real dependencies

- [ ] Compile `adapters/viro` inside the ViroCore renderer you ship with. The adapter uses
      `VROMaterial::removeShaderModifier`, `VROTexture`/`VROTextureSubstrate::updateR32FData`,
      `VROShaderModifier::setRequiresCameraTexture` and the ARCore camera accessors. The stub
      headers in `tests/viro_stubs` are written from the API the adapter expects, not copied
      from ViroCore, so signature mismatches show up here.
- [ ] Build with `MAGIC_PORTAL_WITH_TFLITE=ON` against the LiteRT/TFLite C library you ship.
- [ ] Run `ctest` for the Android ABIs you support if you run tests on-device.

## 2. Shader correctness on a GPU

Automated part: open `tools/shader_parity/index.html` (see its README) in Chrome on each
device. It compiles the generated shader, compares 5,504 hash cells bit for bit and 24 frames
against the CPU reference, and produces a result JSON to attach to the matrix in §7. Pass
means: compiles, hash exact, no frame graded fail. This exercises the phone's WebGL2 stack,
not ViroCore, so the on-device items below still apply.

- [ ] The generated modifier compiles and links inside ViroCore on each GPU family below (no
      driver warnings in logcat), including the `camera_texture` declaration (see
      `adapters/viro/ANDROID_INTEGRATION.md`, "Known integration risk").
- [ ] Parity inside ViroCore: show a fixed scene and fixed transition states (progress 0, 0.25,
      0.5, 0.75, 1) on device and compare screenshots with `PortalCompositor`. Differences
      should be limited to float rounding and texture filtering. Record the maximum
      per-channel difference.
- [ ] With `effect_enable = 0` the output is bit-for-bit the unmodified camera frame
      (`tools/shader_parity` already proves this for the shader on desktop GPUs).
- [ ] `GL_OES_texture_float_linear` present? If not, confirm the nearest-filter fallback is
      visually acceptable (the parity page reports which filter it used).
- [ ] Background textures appear upright (the shader expects bottom row first; see the
      orientation section of the integration guide).

## 3. Orientation and alignment

- [ ] Front camera and rear camera, portrait and landscape, each rotation: raise one hand on
      one side only; the cut-out must be on the same side. If it is mirrored, set
      `mirror_segmentation_input`.
- [ ] Viewport crop (aspect ratio mismatch between camera image and screen): the cut-out must
      line up with the person at all four screen edges.

## 4. Model quality (use real, labelled footage)

- [ ] Build a labelled clip set that matches your users (lighting, skin tones, hair, clothing,
      glasses, backgrounds, motion). Export frames as described in `tools/eval_masks.cpp` and
      run `magic_portal_eval --dataset <dir>`.
- [ ] Record IoU, boundary F1 and flicker for raw vs refined masks and agree thresholds with
      product. The synthetic gate only checks that refinement behaves as designed.
- [ ] Review failure cases visually: fast motion, two people, person leaving the frame,
      cluttered backgrounds, low light.

## 5. Performance and thermals

Measure on the weakest device you support, in a Release build, with the real model:

| Metric | How | Budget (agree with product) |
|---|---|---|
| Render frame time p50 / p95 | Viro/GL frame timing or `dumpsys gfxinfo` | within the display refresh interval |
| Inference latency | `mp_stats.last_segment_ms`, `last_refine_ms`, `max_total_ms` | below `inference_interval_ms` |
| End-to-end mask latency | timestamp at capture vs mask upload | measure and decide |
| Memory | Android Studio profiler over 10 minutes | no growth |
| Battery / thermal | 10-minute run, record throttling events and fps drop | no sustained fps collapse |

## 6. Lifecycle and failure injection

- [ ] Pause/resume, screen rotation, app backgrounding, camera permission revoked and
      re-granted, AR session rebuilt: no crash, no stale mask, listener removed before the
      renderer is torn down.
- [ ] Cover the camera, kill the segmenter (bad model file), return an error from inference:
      the screen shows the plain camera within `stale_after_ms`, and recovers when masks resume.
- [ ] Trigger transitions repeatedly and during a running transition: no crash; second trigger
      is rejected with `kInvalidState`.
- [ ] Register, replace and remove backgrounds while running.
- [ ] Run with a sanitizer-enabled native build (HWASan or ASan on Android) for at least one
      full session.

## 7. Device matrix

Cover at least six devices across GPU vendors and Android versions, for example one each of:
Qualcomm Adreno (current and 2-3 years old), ARM Mali, Samsung Xclipse, a low-end device with
2-3 GB RAM, and a tablet. Record device, OS, GPU, result, and the build hash.

| Device | OS | GPU | Compiles | Hash exact | Parity (parity page) | In-ViroCore parity | Orientation | Perf budget | Lifecycle | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| Apple M2 (desktop, WebGL2 via ANGLE Metal) | macOS | Apple M2 | yes | yes (5,504 / 5,504) | pass 16 / 16, worst mean error 0.73 | n/a | n/a | n/a | n/a | Not a phone; evidence for the shader logic only |
| | | | | | | | | | | |
