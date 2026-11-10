# C interface reference

Header: `magic_portal/magic_portal.h`. Valid C11. All functions are `extern "C"`; no
exception crosses the boundary.

## Versioning

- `MP_VERSION_*` / `mp_version_string()`: release version.
- `MP_ABI_VERSION` / `mp_abi_version()`: bumped only for incompatible binary changes. Check
  `mp_abi_version() == MP_ABI_VERSION` once at startup when loading a shared library.
- Every struct starts with `uint32_t struct_size`. Set it with `sizeof` (or let
  `MP_ENGINE_CONFIG_INIT` do it). Later releases append fields; an old caller's smaller struct
  is still accepted, and output structs are written only up to the size the caller declared.
- `mp_engine_config_init` writes the whole struct of the header the library was built with.
  A client compiled against an older, smaller header must use `mp_engine_config_init_sized`
  (the `MP_ENGINE_CONFIG_INIT` macro does this with the caller's own `sizeof`), otherwise the
  call could write past the end of its struct.
- 0.2.0 added, without an ABI break: `mp_frame.session_id`, `mp_render_state.mask_timestamp_ms`,
  `mp_stats.frames_rejected_out_of_order`, `mp_stats.last_mask_timestamp_ms`,
  `mp_engine_session_id` and `mp_engine_config_init_sized`. A client built against 0.1 that
  keeps its smaller structs keeps working.

## Status codes

`MP_OK`, `MP_NO_PERSON` (success: the frame was processed and nobody is in it), and negative
errors: `MP_ERROR_INVALID_ARGUMENT`, `_INVALID_STATE`, `_UNSUPPORTED`, `_MODEL`, `_INFERENCE`,
`_OUT_OF_MEMORY`, `_INTERNAL`. `mp_status_string()` returns a short description;
`mp_engine_copy_last_error()` returns the detailed message of the last failure.

## Lifecycle

```c
mp_engine_config cfg;  MP_ENGINE_CONFIG_INIT(&cfg);
/* cfg.model_path = "...";  or cfg.model_data/model_size for an in-memory model */
mp_engine *engine;     char err[256];
mp_status s = mp_engine_create(&cfg, &engine, err, sizeof err);
...
mp_engine_destroy(engine);
```

With no model configured the engine accepts caller-supplied masks only
(`mp_engine_process_frame(engine, &frame, &raw_mask)`): useful when segmentation comes from
another SDK. If a model is configured but the build has no TFLite backend, creation fails
with `MP_ERROR_UNSUPPORTED` and an explanatory message (`mp_has_tflite_backend()` tells you
beforehand).

## Per-frame calls

| Function | Thread | Purpose |
|---|---|---|
| `mp_engine_process_frame` | worker | Segment (or take `raw_mask`), refine, publish. Blocking. |
| `mp_engine_get_render_state` | render | Everything needed to draw one frame. |
| `mp_engine_begin_transition` | any | Start the portal toward background id `>= 0`. |
| `mp_engine_reset` | any | Back to the live camera; discards the mask; starts a new session. |
| `mp_engine_session_id` | any | Current session id (never 0); changes on every reset. |
| `mp_engine_get_stats` | any | Counters and last stage timings. |

Rules:

- Pass one monotonic clock (milliseconds) to every call; use the same clock for
  `mp_frame.timestamp_ms` and the `now_ms` arguments. Frame timestamps must not decrease: an
  older frame returns `MP_ERROR_INVALID_ARGUMENT` and is counted in
  `frames_rejected_out_of_order`.
- To make `mp_engine_reset` safe against frames that are already in flight, read
  `mp_engine_session_id()` just before you capture a frame and put it in
  `mp_frame.session_id`. A frame captured before a reset is then dropped (it returns `MP_OK`
  and increments `frames_discarded_by_reset`) instead of reviving the old scene. Leave the
  field 0 to skip the check.
- Size limits: frames up to 16384 pixels per side and 64 megapixels, raw masks up to 4096 per
  side and 16 megapixels. Larger or inconsistent inputs return `MP_ERROR_INVALID_ARGUMENT`
  without reading the buffer. Look parameters (ring width, feather, strengths, colours) must be
  finite and in range, or `mp_engine_create` fails.
- `mp_frame.pixels` is RGBA8, display-oriented, rows `stride_bytes` apart (0 = tight). The
  engine copies it, so the buffer may be reused as soon as the call returns.
- `mp_frame.crop` is the visible part of the unmirrored frame (left, top, right, bottom in
  [0, 1]). `mirror_horizontally` mirrors the pixels and the crop together.
- `mp_render_state.mask` points to an engine-owned float buffer
  (`mask_width * mask_height`, values in [0, 1]); it stays valid until the next
  `mp_engine_get_render_state` or `mp_engine_destroy`; `mp_engine_reset` does not invalidate
  it. Upload it to a texture only when `mask_updated` is 1. `mask_timestamp_ms` is the capture
  time of that mask (-1 when there is none).
- Do not call `mp_engine_destroy` while another call on the same engine is in flight.

## Rendering contract

When `effect_enable == 0`, draw the camera untouched. That is the fail-safe: it is what you get
for the live phase, a stale or future-stamped mask, a failed inference and after a reset. A
successful frame with nobody in it is different: `effect_enable` stays 1 and the mask is all
zero, so the destination fills the view. Otherwise set the shader uniforms from
the state (names in `portal_shader.h`, GLSL via `mp_shader_source`):

| Uniform | Source |
|---|---|
| `mp_person_mask` | texture from `mask` |
| `mp_current_background` / `mp_next_background` | textures you registered for `current_background` / `next_background`; when `camera_is_current` is 1 the old side is the live camera (`mp_use_camera_old = 1`) |
| `mp_reveal`, `mp_ring_scale`, `mp_ring_alpha`, `mp_spin_turns`, `mp_person_visibility`, `mp_progress` | same-named fields |
| `mp_person_bounds`, `mp_mask_crop` | `person_bounds`, `mask_crop` |
| `mp_style_a`, `mp_style_b`, `mp_warm`, `mp_cool` | `style_a`, `style_b`, `warm`, `cool` |
| `mp_effect_enable` | `effect_enable` |

Orientation the shader expects (the Viro adapter does this for you; do it yourself when you
bind the shader elsewhere): the camera texture is the unflipped top-first image sampled through
`camera_image_transform`; the mask is uploaded unflipped and addressed through `mp_mask_crop`;
background textures must be stored **bottom row first**, because the shader samples them at
bottom-left screen coordinates. `person_bounds` is already in the bottom-left screen space of
`gl_FragCoord`. `tools/shader_parity` is a working reference for all three.

The mask is a single-channel float texture. Sampling it with linear filtering needs the
`GL_OES_texture_float_linear` extension on OpenGL ES; the Viro adapter checks for it and falls
back to nearest filtering with a recorded warning.
