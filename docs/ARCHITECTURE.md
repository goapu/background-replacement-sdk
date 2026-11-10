# Architecture

## Layers

```text
 host app / adapter  (Android: ViroMagicPortalFrameListener + ViroMagicPortalMaterial)
        │ uses
 PortalRunner  (optional)   worker thread + one-slot "latest frame wins" hand-off
        │ drives
 Engine                     mask pipeline, freshness, controller, render state
        │ composed of
 PersonSegmenter ─ MaskRefiner ─ MaskFreshness ─ PortalController ─ PortalTimeline
        │
 C interface (magic_portal.h) wraps Engine for non-C++ clients
```

`Engine` has no threads of its own and no clock: every call that depends on time takes
`now_ms`. That is what makes it testable and reproducible.

## Threads

| Thread | Calls |
|---|---|
| Worker | `Engine::process_frame` (segmentation, refinement, publish) |
| Render | `render_state`, `begin_transition`, `reset`, texture upload via the adapter |

`process_frame` is serialized internally, so two accidental concurrent callers cannot corrupt
state. The worker and render threads share only a small published snapshot guarded by one
mutex; the mask itself changes hands by swapping a buffer, so the render thread never copies
or allocates.

`reset()` can arrive while a frame is being processed, or after a frame was captured but before
it was processed. Two mechanisms cover both cases:

- A generation counter makes the in-flight frame discard its result instead of resurrecting
  the old session.
- **Session ids.** `Engine::session_id()` changes on every `reset()` and is never `kAnySession`
  (0). The host reads it *before* capturing a frame and stores it in `FrameInput.session_id`
  (`mp_frame.session_id` in C). A frame whose session is no longer current is dropped, counted
  in `frames_discarded_by_reset`, and reported as success (it is not an error: the frame was
  simply obsolete). `kAnySession` skips the check, which keeps older callers working.
  `PortalRunner` does this for you and `PortalRunner::reset()` also drops the frame it was
  holding.

Timestamps must be non-decreasing. A frame older than the newest accepted frame is rejected
(`kInvalidArgument`, counted in `frames_rejected_out_of_order`) so that a late frame can never
replace a newer mask. The record of the newest timestamp is cleared with the session.

`render_state()` returns a pointer to the mask copy that the render thread owns. That copy is
replaced only inside `render_state()`, and `reset()` merely marks the published mask as gone,
so a pointer obtained from one `render_state()` call stays valid until the next call on the
render thread, even if `reset()` runs in between.

## Fail-safe rules

1. `effect_enable` is true only when the phase is not *live*, a mask exists, **and** that mask
   is fresh. Otherwise the shader outputs the camera unchanged. "Fresh" is judged on the
   timestamp of the mask that is on screen (`mask_timestamp_ms`): not older than
   `stale_after_ms`, and not stamped more than `future_tolerance_ms` ahead of `now`.
2. `begin_transition` fails with `kInvalidState` until a fresh mask exists.
3. A frame where nobody is visible publishes an explicit all-zero mask with the fallback
   bounds (status `kNoPerson`), not residual noise. This is a *successful empty scene*: the
   effect runs and the destination fills the view. A failure (as opposed to an empty scene)
   lets the mask age out and shows the camera.
4. If a transition references a background id the host never registered, the adapter disables
   the effect and records an error instead of drawing a wrong texture.
5. If uploading the mask texture fails, the adapter disables the effect until an upload
   succeeds, so the GPU never draws with a mask that is older than the one the engine
   published. It retries on the next frame and counts the failures.
6. The adapter never lets an exception escape into the host renderer's loop.

What each situation shows (the engine, the shader and the CPU reference agree on all rows):

| Situation | Output |
|---|---|
| Live phase | camera |
| Mask fresh, person found | effect runs, person kept |
| Mask fresh, nobody in view | effect runs with a zero mask; destination fills the view |
| Mask stale, from the future, failed inference, or session reset | camera (fail-safe) |
| GPU mask upload failed | camera until an upload succeeds |

## Input limits

Inputs come from outside the SDK, so they are bounded before any memory is touched
(`limits.h`, all arithmetic in 64 bits): frames up to 16384 pixels per side and 64 megapixels,
masks up to 4096 per side and 16 megapixels, strides at least one row and not beyond what the
buffer can hold. Configuration is validated at construction: style values must be finite and in
range (`validate_style`), with `person_protection` strictly above the shader's guard threshold
(currently 0.72), refiner radius is at most 16 with finite sigmas, and timeline fractions are
finite, in [0, 1] and ordered. Timestamp differences saturate at the int64 limits in freshness,
the timeline and temporal refinement. A violation throws `std::invalid_argument` (C++) or returns
`MP_ERROR_INVALID_ARGUMENT` (C); it never reaches the render loop.

## Transition state machine

```text
kLive ──begin(id)──► kTransitioning ──timeline completes──► kReplaced(id)
  ▲                         │                                   │
  └───────── reset() ───────┴───────────── reset() ◄────────────┘
                                   kReplaced ──begin(id2)──► kTransitioning (old = id)
```

The replaced background becomes the "current" one inside the same `update()` call in which the
timeline completes, so there is no frame where the old state is shown again.

## One source of truth for the look

`portal_constants.h` holds every numeric constant of the effect in one X-macro table. It
generates the C++ `constants::` values used by the CPU renderer **and** is substituted into the
GLSL template in `portal_shader.cpp`. Values are printed with 9 significant digits, so each
literal in the shader denotes exactly the same `float` as the C++ constant (a test checks the
round trip). Previously the CPU and GPU paths had drifted (hard-coded `1.55` max radius,
different spark hash); both now derive `max_radius` from the farthest corner of the screen,
and the noise comes from one integer hash (next section).

Shader modifier bodies in ViroCore cannot define functions (only statements and uniform
declarations), which is why the template is written as straight-line code.

## Noise hash

The dissolve and spark effects need a stable pseudo-random value per screen cell. A common
shader hash, `fract(sin(dot(cell, k)) * 43758.5)`, is **not reproducible**: the large multiplier
turns a one-step difference in the sine argument (a fused multiply-add, another vendor's sine
routine) into an unrelated value, so two GPUs, or a GPU and the CPU, disagree on most cells
(measured on a software GPU: 4.4 % of cells agreed within tolerance, about what unrelated
values give by chance).

`portal_hash.h` therefore defines the noise with 32-bit unsigned integer arithmetic only (a
PCG-style permutation). Integer multiplication wraps identically on every CPU and in GLSL ES
3.00 (`highp uint`), and the 24-bit result converts to `float` exactly. The C++ reference calls
`pcg_round()`; `portal_shader.cpp` emits the same constants and operations as straight-line
GLSL statements (no functions, see above). Known-answer tests pin the CPU values, and the shader
parity tool checks 5,504 cells bit for bit on a GPU.

The sine terms that remain (soft wave bands, ring edges) are smooth, so small differences stay
small and are covered by the parity tolerances, not by exact comparison.

## Coordinate conventions

- Frames are display-oriented RGBA8 (already rotated), top-left origin.
- The mask crop selects which part of the mask is visible: screen `(u, v)` maps to mask
  `(mix(crop.left, crop.right, u), mix(crop.top, crop.bottom, v))` in top-left terms. The shader
  and `CompositeInput::mask_crop` use the same mapping; the parity tool has cropped scenarios.
- `FrameInput` carries a crop rectangle (visible region of the unmirrored frame). If
  `mirror_horizontally` is set, the engine mirrors the pixels and mirrors the crop.
- `RenderState::person_bounds` and `mask_crop` are already converted for the shader:
  bounds in screen space with a bottom-left origin (`gl_FragCoord`), crop as left/top/right/bottom.
- The GPU works in a bottom-left screen space; the CPU frames are top-left. The CPU
  compositor reads images and the mask in top-left order but evaluates every term that depends
  on screen position (angle, noise, ring distance) with `v = 1 - v_top`, so a pixel gets the
  same value on both sides. Anything added to the effect must follow the same rule, otherwise
  rings and sparks appear mirrored between the CPU reference and the device.
- Texture orientation expected by the shader (and used by the parity tool): the camera texture
  is the unflipped top-first image, mapped by `camera_image_transform`; the mask texture is
  uploaded unflipped and addressed through `mp_mask_crop`; background textures are stored
  bottom row first because the shader samples them at bottom-left coordinates.

## Performance design

- Full-resolution buffers are recycled: `PortalRunner` swaps three frame buffers, the engine
  reuses its scratch frame, and the refiner reuses its scratch masks. What remains per inference
  are a few model-resolution allocations (the raw and refined masks, about 256 KB each at
  256x256) on the worker thread, never on the render thread.
- Mirroring is folded into the single strided copy that already happens on the worker.
- Morphology is separable (two 1-D passes instead of a full window).
- On the render thread the camera read-back is the only heavy step; mirror and allocation were
  removed from it.

Measured numbers (development machine, not a phone) are produced by `magic_portal_bench`.
They only show relative cost; phone numbers must come from the device validation plan.

## Compatibility policy

- C interface: `MP_ABI_VERSION` changes only for incompatible changes. Structs start with
  `struct_size`; new fields are appended and older callers keep working (outputs are written
  only up to the caller's declared size).
- C++ headers are for static linking at the same version; a shared build exports only `mp_*`.
