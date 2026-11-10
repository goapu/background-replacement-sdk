/*
 * magic_portal - stable C interface.
 *
 * Lifecycle (typical):
 *   mp_engine_config_init(&cfg);  cfg.model_path = "...";  mp_engine_create(&cfg, &engine, err, sizeof err);
 *   worker thread : mp_engine_process_frame(engine, &frame, NULL);
 *   render thread : mp_engine_begin_transition(...);  mp_engine_get_render_state(...);
 *   shutdown      : mp_engine_destroy(engine);
 *
 * Rules:
 *   - No C++ exception crosses this boundary; every failure is a negative mp_status.
 *   - Structs carry `struct_size`; callers set it (the *_init helpers do) so fields can be
 *     appended in later minor versions without breaking compiled clients. Fields beyond the
 *     caller's struct_size are never read or written. Use mp_engine_config_init_sized when
 *     the config struct comes from a client compiled against an older header.
 *   - Time is passed explicitly as milliseconds on one monotonic clock of the caller's choice.
 *   - mp_engine_process_frame may run on one worker thread while the other mp_engine_*
 *     functions run on another (typically the render) thread. Do not call
 *     mp_engine_destroy while any other call on the same engine is in flight.
 */
#ifndef MAGIC_PORTAL_H
#define MAGIC_PORTAL_H

#include <stddef.h>
#include <stdint.h>

#define MP_VERSION_MAJOR 0
#define MP_VERSION_MINOR 2
#define MP_VERSION_PATCH 0
#define MP_VERSION_STRING "0.2.0"
/* Bumped only when the binary interface changes incompatibly. */
#define MP_ABI_VERSION 1

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(MAGIC_PORTAL_BUILDING_SHARED)
#define MP_API __declspec(dllexport)
#elif defined(MAGIC_PORTAL_SHARED)
#define MP_API __declspec(dllimport)
#else
#define MP_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MP_API __attribute__((visibility("default")))
#else
#define MP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mp_engine mp_engine;

typedef enum mp_status {
    MP_OK = 0,
    MP_NO_PERSON = 1, /* frame processed successfully; nobody is in it */
    MP_ERROR_INVALID_ARGUMENT = -1,
    MP_ERROR_INVALID_STATE = -2,
    MP_ERROR_UNSUPPORTED = -3,
    MP_ERROR_MODEL = -4,
    MP_ERROR_INFERENCE = -5,
    MP_ERROR_OUT_OF_MEMORY = -6,
    MP_ERROR_INTERNAL = -7
} mp_status;

typedef enum mp_phase {
    MP_PHASE_LIVE = 0, /* the live camera is shown */
    MP_PHASE_TRANSITIONING = 1,
    MP_PHASE_REPLACED = 2 /* a replacement background is shown behind the person */
} mp_phase;

typedef enum mp_output_mode {
    MP_OUTPUT_SINGLE_CLASS = 0,        /* person = channel person_class_index */
    MP_OUTPUT_ONE_MINUS_BACKGROUND = 1 /* person = 1 - background_class_index */
} mp_output_mode;

typedef enum mp_resize_mode {
    MP_RESIZE_BILINEAR = 0,
    MP_RESIZE_AREA = 1 /* box filter when downscaling; less aliasing */
} mp_resize_mode;

/* ---- configuration -------------------------------------------------------------------- */

typedef struct mp_engine_config {
    uint32_t struct_size; /* set by mp_engine_config_init */

    /* Segmentation model. Provide at most one of model_path / model_data. With neither, the
     * engine accepts only caller-supplied masks (mp_engine_process_frame's raw_mask). */
    const char *model_path;
    const void *model_data; /* copied; need not outlive mp_engine_create */
    size_t model_size;
    int32_t num_threads;
    int32_t output_mode; /* mp_output_mode */
    int32_t person_class_index;
    int32_t background_class_index;
    int32_t output_is_logits; /* 0 / 1 */
    int32_t resize_mode;      /* mp_resize_mode */
    float input_mean;
    float input_std;

    /* Freshness and timeline. */
    int64_t stale_after_ms; /* masks older than this disable the effect (show camera) */
    int64_t transition_duration_ms;

    /* Mask refinement. */
    int32_t morphology_radius;
    int32_t bilateral_radius;
    float spatial_sigma;
    float color_sigma;
    float temporal_attack;
    float temporal_release;
    float low_confidence;
    float high_confidence;
    float exact_background_cutoff;
    float exact_person_cutoff;
    float bounds_threshold;
    float minimum_person_area_fraction;

    /* Look. Colors are RGBA in [0, 1]. */
    float warm_color[4];
    float cool_color[4];
    float ring_width;
    float ring_strength;
    float particle_strength;
    float silhouette_glow;
    float reveal_feather;
    float person_protection; /* (0.72, 1], above the shader's person guard threshold */
} mp_engine_config;

/* ---- per-frame input and output ------------------------------------------------------- */

typedef struct mp_frame {
    uint32_t struct_size;
    const uint8_t *pixels; /* RGBA8, display orientation */
    int32_t width;
    int32_t height;
    int32_t stride_bytes; /* >= 4 * width; 0 means tightly packed */
    int32_t mirror_horizontally;
    int64_t timestamp_ms; /* must not decrease from one frame to the next; see below */
    /* Visible region of the (unmirrored) frame: left, top, right, bottom in [0, 1]. */
    float crop[4];
    /* Optional (struct_size must cover it): the value of mp_engine_session_id() read just
     * before this frame's pixels were captured. A frame captured before a reset is then
     * dropped instead of being published after it. 0 skips the check. */
    uint64_t session_id;
} mp_frame;

typedef struct mp_raw_mask {
    uint32_t struct_size;
    const float *confidence; /* width * height person probabilities in [0, 1] */
    int32_t width;
    int32_t height;
} mp_raw_mask;

typedef struct mp_render_state {
    uint32_t struct_size;
    int32_t effect_enable;      /* 0: draw the camera untouched */
    int32_t phase;              /* mp_phase */
    int32_t current_background; /* caller-assigned id, or -1 */
    int32_t next_background;    /* caller-assigned id, or -1 */
    int32_t camera_is_current;  /* 1: the "old" background is the live camera */
    int32_t has_person;
    int32_t mask_updated; /* 1: `mask` differs from the previous call */
    uint64_t mask_version;

    float progress;
    float reveal;
    float ring_scale;
    float ring_alpha;
    float spin_turns;
    float person_visibility;

    float person_bounds[4]; /* min_x, min_y, max_x, max_y; screen space, bottom-left origin */
    float mask_crop[4];     /* left, top, right, bottom */
    float style_a[4];       /* ring_width, ring_strength, particle_strength, silhouette_glow */
    float style_b[4];       /* reveal_feather, person_protection, 0, 0 */
    float warm[4];
    float cool[4];

    /* Single-channel float mask; owned by the engine and valid until the next call to
     * mp_engine_get_render_state or mp_engine_destroy on this engine. mp_engine_reset and
     * mp_engine_process_frame, from any thread, do not invalidate it. NULL until the first
     * mask exists (and again after a reset, from the next get_render_state on). */
    const float *mask;
    int32_t mask_width;
    int32_t mask_height;
    int64_t mask_timestamp_ms; /* capture time of `mask`; -1 when there is none */
} mp_render_state;

typedef struct mp_stats {
    uint32_t struct_size;
    uint32_t reserved0;
    uint64_t frames_processed;
    uint64_t frames_without_person;
    uint64_t frames_failed;
    uint64_t frames_discarded_by_reset;
    double last_copy_ms;
    double last_segment_ms;
    double last_refine_ms;
    double max_total_ms;
    int64_t last_mask_timestamp_ms;        /* -1 before the first mask */
    uint64_t frames_rejected_out_of_order; /* older timestamp than an accepted frame */
} mp_stats;

/* ---- functions ------------------------------------------------------------------------ */

MP_API uint32_t mp_abi_version(void); /* compare with MP_ABI_VERSION at startup */
MP_API const char *mp_version_string(void);
MP_API const char *mp_status_string(mp_status status);
MP_API int32_t mp_has_tflite_backend(void); /* 1 when built with the TFLite segmenter */

/* Fills every field with its documented default. Writes sizeof(mp_engine_config) bytes of
 * the header this library was built with; the config must be that large. */
MP_API void mp_engine_config_init(mp_engine_config *config);

/* Same, for a config whose storage is `size` bytes (the sizeof of the struct as the caller
 * compiled it). Only the fields that fit are written and struct_size is set to `size`, so a
 * client built against an older, smaller header cannot be overrun. Sizes below the v1 layout
 * leave the struct untouched. Prefer the MP_ENGINE_CONFIG_INIT macro. */
MP_API void mp_engine_config_init_sized(mp_engine_config *config, size_t size);
#define MP_ENGINE_CONFIG_INIT(config_ptr) mp_engine_config_init_sized((config_ptr), sizeof(*(config_ptr)))

/* On failure *out_engine is NULL and, when error_buffer is non-NULL, it receives a
 * NUL-terminated message (truncated to error_capacity). */
MP_API mp_status mp_engine_create(const mp_engine_config *config, mp_engine **out_engine, char *error_buffer,
                                  size_t error_capacity);
MP_API void mp_engine_destroy(mp_engine *engine);

/* Identifies the current session; changes on every mp_engine_reset and is never 0. Read it
 * just before capturing a frame and store it in mp_frame.session_id. Returns 0 for a NULL
 * engine. */
MP_API uint64_t mp_engine_session_id(const mp_engine *engine);

/* Worker thread. If raw_mask is NULL the built-in model segments the frame.
 * Limits: frames up to 16384 pixels per side and 64 megapixels, raw masks up to 4096 per side
 * (anything larger is MP_ERROR_INVALID_ARGUMENT before any memory is touched). Timestamps
 * must be non-decreasing: a frame older than one already accepted is rejected with
 * MP_ERROR_INVALID_ARGUMENT and does not change the published mask. */
MP_API mp_status mp_engine_process_frame(mp_engine *engine, const mp_frame *frame,
                                         const mp_raw_mask *raw_mask);

/* Render thread (or any other thread). */
MP_API mp_status mp_engine_begin_transition(mp_engine *engine, int64_t now_ms, int32_t next_background_id);
MP_API mp_status mp_engine_reset(mp_engine *engine);
MP_API mp_status mp_engine_get_render_state(mp_engine *engine, int64_t now_ms, mp_render_state *out_state);
MP_API mp_status mp_engine_get_stats(const mp_engine *engine, mp_stats *out_stats);

/* Copies the engine's most recent error text. Returns the full message length (excluding
 * the NUL), so a result >= capacity means the text was truncated. */
MP_API size_t mp_engine_copy_last_error(const mp_engine *engine, char *buffer, size_t capacity);

/* GLSL ES 3.00 fragment-modifier source generated from the same constants as the CPU
 * reference renderer. Returns the length (excluding NUL); pass a NULL buffer to query it. */
MP_API size_t mp_shader_source(char *buffer, size_t capacity);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGIC_PORTAL_H */
