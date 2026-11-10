# Shader parity check

The portal effect runs on the GPU as a fragment-shader modifier, and the CPU compositor
(`PortalCompositor`) is the reference implementation of the same maths. This tool runs the
generated shader on a real graphics device and compares it with the reference, frame by frame.
Use it on every device model you intend to support, and again whenever the shader or the
effect constants change.

## What it checks

| Check | Criterion |
| --- | --- |
| Shader compilation | The generated GLSL ES 3.00 compiles and links on this GPU |
| Noise hash | Every cell of a 64 x 86 grid is **bit-identical** between GPU and CPU |
| Frames | 24 engine states: live camera, a full transition, a second transition, the stale-mask fallback, an empty scene during and after a transition, a person re-entering, and cropped views (a non-trivial `mp_mask_crop`); each GPU frame is compared with the CPU reference |

Frame grades: **pass** = mean error up to 2.5 of 255 and fewer than 1.5 % of pixels differing
by more than 24; **warn** = mean error up to 6 and fewer than 5 %; anything worse is **fail**.
The overall verdict fails if any frame fails or the hash is not exact. The remaining small
differences (edges of rings and sparks) come from texture filtering and float rounding and are
expected.

A mask that is stale, failed or reset must give the plain camera on both sides (the bypass
frames are bit-identical); a fresh mask with nobody in it must fill the view with the
destination on both sides.

The inputs come from the real code path: the engine produces the render state, the Viro adapter
(compiled against the stub ViroCore headers) decides which uniforms the shader sees, and the
CPU reference is rendered from the same state. The page uploads the camera, mask and
backgrounds with the same orientation conventions the adapter relies on (see
`docs/ARCHITECTURE.md`, "Coordinate conventions").

## Run it

1. Generate the fixtures (any machine that can build the SDK):

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --target magic_portal_shader_parity_gen
   build/magic_portal_shader_parity_gen tools/shader_parity/fixtures.json
   ```

   `fixtures.json` is about 5.5 MB and is deterministic; it is not checked in.

2. Serve this directory and open the page on the device under test. The page needs WebGL2:

   ```sh
   cd tools/shader_parity && python3 -m http.server 8000
   # then open http://<host>:8000/ in Chrome on the device
   ```

   Browsers do not allow `fetch` from `file://`. If the page cannot load `fixtures.json` it
   offers a file picker instead.

3. Read the verdict at the top of the page. **Copy result JSON** or **Download result JSON**
   keeps the evidence (renderer string, GLSL version, per-frame numbers) for the device matrix
   in `docs/DEVICE_VALIDATION.md`.

For automation, the finished result object is available as `window.__MP_RESULT`.

## What it does not check

* It runs through the browser's WebGL2 stack (ANGLE on most platforms). A phone's native
  OpenGL ES driver inside ViroCore can differ; the on-device checklist in
  `docs/DEVICE_VALIDATION.md` covers that.
* It does not exercise ViroCore itself, so the `camera_texture` sampler type and the real
  camera-image transform still need a device run.
* The hash probe proves integer maths is exact; the frame comparison tolerates float-level
  differences, so a visible regression in look still needs a person to watch the demo.
