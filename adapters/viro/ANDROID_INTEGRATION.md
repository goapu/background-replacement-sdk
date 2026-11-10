# Android ViroCore integration

The adapter is compiled inside the native ViroCore renderer, where the AR camera-background
material, the driver and the camera image already exist. It is a thin layer over the SDK's
`Engine`: it reads camera frames, hands them to the worker, and writes the engine's
`RenderState` into the camera-background material every frame.

> Verification status: the adapter's logic is tested against stub ViroCore headers
> (`tests/viro_stubs`). The generated shader has been compiled and run on a desktop-class GPU
> through WebGL2 (`tools/shader_parity`), with the adapter's real uniform logic feeding it.
> The adapter has not been compiled against, or run inside, the real ViroCore, and the shader
> has not run on a phone GPU. Follow `docs/DEVICE_VALIDATION.md` §1 and §2 before relying on it.

### Known integration risk: `camera_texture`

The modifier declares `uniform sampler2D camera_texture;` and samples it with `texture()`.
ViroCore's ARCore camera-background material supplies the camera image itself, and on Android
the camera image is often an external texture (`samplerExternalOES`) rather than a plain
`sampler2D`. If the real material declares `camera_texture` with a different type, the shader
fails to compile (redeclaration) or samples the wrong kind of texture. The stubs cannot tell.
On the first device build, check the compiled material's own declaration; if it differs, adapt
the one declaration and the one `texture(camera_texture, ...)` call in
`src/portal_shader.cpp` (the rest of the effect does not depend on it) and re-run
`tools/shader_parity` to confirm nothing else moved. Watch logcat for shader compile errors
when the material is first drawn (or at `prewarm`, if you call it).

## 1. Add native sources

Add to the Viro renderer's native build (or link the `magic_portal` static library built with
the NDK):

```text
include/                                  (header search path)
src/*.cpp except tflite_stub.cpp          (or the installed libmagic_portal.a)
src/tflite_person_segmenter.cpp           (instead of tflite_stub.cpp; define MAGIC_PORTAL_WITH_TFLITE=1)
adapters/viro/ViroMagicPortalMaterial.cpp
adapters/viro/android/ViroMagicPortalFrameListener.cpp
```

`portal_compositor.cpp` and `pipeline.cpp` are the CPU reference path and are not needed at
runtime on a device, but they are small and harmless to include.

The adapter needs these ViroCore pieces: `VROMaterial` (`addShaderModifier`,
`removeShaderModifier`, `setShaderUniform`, `prewarm`), `VROShaderModifier`, `VROTexture`,
`VROTextureSubstrate::updateR32FData`, `VROVector4f`, `VROFrameListener`, and the ARCore
camera accessors used in `ViroMagicPortalFrameListener::capture`. Check each signature against
your ViroCore version.

## 2. Create and attach (render thread, GL context current)

The hook is `VROSceneRendererARCore::initARSession`, right after the camera background and its
first material exist.

```cpp
#include "ViroMagicPortalFrameListener.h"
#include "ViroMagicPortalMaterial.h"
#include "magic_portal/tflite_person_segmenter.h"

using namespace magic_portal;
using namespace magic_portal::viro;

std::shared_ptr<VROMaterial> cameraMaterial = _cameraBackground->getMaterials()[0];

TflitePersonSegmenterConfig model;
model.model_path = resolvedModelPath;      // or model.model_data / model.model_size (copied)
model.num_threads = 2;
model.input_mean = 0.0F;                   // bytes -> [0, 1], MediaPipe selfie convention
model.input_std = 255.0F;
model.resize_mode = ResizeMode::kArea;     // better when the camera frame is much larger than the model input
std::string error;
std::unique_ptr<PersonSegmenter> segmenter = create_tflite_segmenter(model, &error);
if (!segmenter) { /* log `error`, leave the camera untouched */ return; }

_magicPortalMaterial = ViroMagicPortalMaterial::create(cameraMaterial, _driver);
_magicPortalMaterial->set_background(/*id=*/1, roomTexture);   // any thread; ids are yours

ViroMagicPortalConfig config;
config.mirror_segmentation_input = false;  // see "Orientation" below
config.engine.timeline.duration_ms = 1800;
config.engine.freshness.stale_after_ms = 500;

_magicPortalListener = std::make_shared<ViroMagicPortalFrameListener>(
    shared_from_this(), _driver, _magicPortalMaterial, std::move(segmenter), config);
_renderer->getFrameSynchronizer()->addFrameListener(_magicPortalListener);
```

Keep both objects as renderer members so their lifetime matches the AR view.

## 3. Trigger from the app

Expose one native command that calls:

```cpp
Status s = _magicPortalListener->trigger_transition(/*background_id=*/1);
```

`kOk` starts the portal. `kInvalidState` means "no fresh person mask yet" or "a transition is
already running": disable the button or retry after the next frame. Do not bridge RGBA frames,
masks or per-frame values to JavaScript.

`reset()` returns to the live camera. `last_error()`, `engine_stats()` and `runner_stats()`
help with diagnostics (inference timings, dropped frames).

## 4. Teardown (render thread)

1. Remove the listener from the frame synchronizer.
2. Release the listener. Its destructor stops the worker thread first and then removes the shader
   modifier, so the camera background renders normally again.
3. Release the material wrapper, then destroy the session/camera.

On pause or GL context loss, do the same and recreate everything with the new camera
background. All GL-touching calls (`create`, `apply` via the listener, destruction) belong to
the render thread.

## 5. Orientation

If the front preview is mirrored but `getRotatedImageData()` is not, set
`mirror_segmentation_input = true`. Verify with a raised hand on one side only: a wrong guess
produces a visibly offset cut-out. The mirror is applied on the worker, not the render thread.

Vertical convention: the shader works in the bottom-left screen space of `gl_FragCoord`, and
samples the background textures at those coordinates with no flip, so a background texture
must be stored bottom row first. Many image loaders decode top row first; whether ViroCore's
texture loader flips for you is not verified here. Test with an image that has an obvious
top (text, a horizon): if the background appears upside down, flip it when you create the
texture, or flip the sampling coordinate in one place in the template. `tools/shader_parity`
shows the convention the CPU reference assumes.

## 6. What the adapter does for you

- Mask texture is created at the model's output size and rebuilt if that size changes.
- Missing `GL_OES_texture_float_linear` is detected; nearest filtering is used and a warning
  is recorded in `last_error()`.
- A transition that names an unregistered background disables the effect instead of drawing a
  wrong texture.
- Any exception in `apply` or `onFrameWillRender` is contained; the effect is switched off and
  the camera shows through.
- Stale masks (older than `stale_after_ms`) switch the effect off, showing the plain camera.
