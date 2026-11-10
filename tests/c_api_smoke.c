/* Exercises the public C header from a C11 translation unit: this proves the header is
 * valid C and that the ABI works without any C++ in the client. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magic_portal/magic_portal.h"

static int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++failures;                                                               \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        }                                                                             \
    } while (0)

enum { W = 48, H = 36, MW = 24, MH = 24 };

static void fill_scene(uint8_t *pixels, float *mask, int with_person) {
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint8_t *p = pixels + (size_t)(y * W + x) * 4U;
            p[0] = (uint8_t)(x * 5);
            p[1] = (uint8_t)(y * 7);
            p[2] = (uint8_t)(120 + ((x + y) & 31));
            p[3] = 255;
        }
    }
    for (int y = 0; y < MH; ++y) {
        for (int x = 0; x < MW; ++x) {
            const int dx = x - MW / 2;
            const int dy = y - MH / 2;
            const int inside = dx * dx * 4 + dy * dy < 90;
            mask[y * MW + x] = with_person && inside ? 0.95F : 0.02F;
        }
    }
}

int main(void) {
    CHECK(mp_abi_version() == MP_ABI_VERSION);
    CHECK(strcmp(mp_version_string(), MP_VERSION_STRING) == 0);
    CHECK(strcmp(mp_status_string(MP_ERROR_INVALID_STATE), "invalid state") == 0);

    mp_engine_config config;
    MP_ENGINE_CONFIG_INIT(&config);
    CHECK(config.struct_size == sizeof(config));
    CHECK(config.stale_after_ms > 0 && config.transition_duration_ms > 0);
    config.transition_duration_ms = 1000;

    /* Argument validation. */
    char error[256];
    mp_engine *engine = NULL;
    CHECK(mp_engine_create(NULL, &engine, error, sizeof error) == MP_ERROR_INVALID_ARGUMENT);
    CHECK(engine == NULL && error[0] != '\0');
    mp_engine_config tiny = config;
    tiny.struct_size = 8;
    CHECK(mp_engine_create(&tiny, &engine, error, sizeof error) == MP_ERROR_INVALID_ARGUMENT);
    mp_engine_config both = config;
    both.model_path = "model.tflite";
    both.model_data = "x";
    both.model_size = 1;
    CHECK(mp_engine_create(&both, &engine, error, sizeof error) == MP_ERROR_INVALID_ARGUMENT);
    mp_engine_config bad_refiner = config;
    bad_refiner.morphology_radius = -3;
    CHECK(mp_engine_create(&bad_refiner, &engine, error, sizeof error) < 0);
    CHECK(engine == NULL);

    if (!mp_has_tflite_backend()) {
        mp_engine_config with_model = config;
        with_model.model_path = "does-not-matter.tflite";
        CHECK(mp_engine_create(&with_model, &engine, error, sizeof error) == MP_ERROR_UNSUPPORTED);
        CHECK(engine == NULL && strstr(error, "TFLite") != NULL);
    }

    CHECK(mp_engine_create(&config, &engine, error, sizeof error) == MP_OK);
    CHECK(engine != NULL);

    uint8_t *pixels = (uint8_t *)malloc((size_t)W * H * 4U);
    float *mask = (float *)malloc((size_t)MW * MH * sizeof(float));
    CHECK(pixels != NULL && mask != NULL);
    fill_scene(pixels, mask, 1);

    mp_frame frame;
    memset(&frame, 0, sizeof frame);
    frame.struct_size = sizeof frame;
    frame.pixels = pixels;
    frame.width = W;
    frame.height = H;
    frame.stride_bytes = 0; /* tightly packed */
    frame.timestamp_ms = 1000;
    frame.crop[0] = 0.0F;
    frame.crop[1] = 0.0F;
    frame.crop[2] = 1.0F;
    frame.crop[3] = 1.0F;
    mp_raw_mask raw;
    memset(&raw, 0, sizeof raw);
    raw.struct_size = sizeof raw;
    raw.confidence = mask;
    raw.width = MW;
    raw.height = MH;

    /* No model and no mask is a state error, not a crash. */
    CHECK(mp_engine_process_frame(engine, &frame, NULL) == MP_ERROR_INVALID_STATE);
    CHECK(mp_engine_copy_last_error(engine, error, sizeof error) > 0);

    CHECK(mp_engine_begin_transition(engine, 1000, 7) == MP_ERROR_INVALID_STATE); /* no mask yet */
    CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_OK);

    mp_render_state state;
    memset(&state, 0, sizeof state);
    state.struct_size = sizeof state;
    CHECK(mp_engine_get_render_state(engine, 1000, &state) == MP_OK);
    CHECK(state.mask_updated == 1 && state.mask != NULL);
    CHECK(state.mask_width == MW && state.mask_height == MH);
    CHECK(state.has_person == 1);
    CHECK(state.effect_enable == 0 && state.phase == MP_PHASE_LIVE);

    CHECK(mp_engine_begin_transition(engine, 1000, 7) == MP_OK);
    CHECK(mp_engine_begin_transition(engine, 1010, 8) == MP_ERROR_INVALID_STATE);
    CHECK(mp_engine_get_render_state(engine, 1400, &state) == MP_OK);
    CHECK(state.phase == MP_PHASE_TRANSITIONING && state.effect_enable == 1);
    CHECK(state.next_background == 7 && state.camera_is_current == 1);
    CHECK(state.mask_updated == 0);
    CHECK(state.reveal > 0.0F && state.reveal < 1.0F);
    CHECK(state.person_bounds[0] < state.person_bounds[2]);

    /* Shorter caller struct: only the fields it knows about are written. */
    mp_render_state shorter;
    memset(&shorter, 0x5A, sizeof shorter);
    shorter.struct_size = (uint32_t)(offsetof(mp_render_state, mask_height) + sizeof(int32_t));
    CHECK(mp_engine_get_render_state(engine, 1400, &shorter) == MP_OK);
    CHECK(shorter.struct_size == (uint32_t)(offsetof(mp_render_state, mask_height) + sizeof(int32_t)));

    for (int64_t t = 1100; t <= 2100; t += 100) {
        frame.timestamp_ms = t;
        CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_OK);
    }
    CHECK(mp_engine_get_render_state(engine, 2100, &state) == MP_OK);
    CHECK(state.phase == MP_PHASE_REPLACED && state.current_background == 7);

    /* An empty scene is reported, not treated as an error. */
    fill_scene(pixels, mask, 0);
    frame.timestamp_ms = 2200;
    CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_NO_PERSON);

    mp_stats stats;
    memset(&stats, 0, sizeof stats);
    stats.struct_size = sizeof stats;
    CHECK(mp_engine_get_stats(engine, &stats) == MP_OK);
    CHECK(stats.frames_processed >= 12 && stats.frames_without_person == 1);

    /* Shader text is available through the C API. */
    const size_t shader_length = mp_shader_source(NULL, 0);
    CHECK(shader_length > 1000);
    char *shader = (char *)malloc(shader_length + 1U);
    CHECK(shader != NULL && mp_shader_source(shader, shader_length + 1U) == shader_length);
    CHECK(strstr(shader, "mp_effect_enable") != NULL && strchr(shader, '@') == NULL);
    free(shader);

    /* Invalid calls never crash. */
    CHECK(mp_engine_process_frame(NULL, &frame, &raw) == MP_ERROR_INVALID_ARGUMENT);
    CHECK(mp_engine_process_frame(engine, NULL, &raw) == MP_ERROR_INVALID_ARGUMENT);
    mp_frame bad = frame;
    bad.crop[2] = 2.0F;
    CHECK(mp_engine_process_frame(engine, &bad, &raw) == MP_ERROR_INVALID_ARGUMENT);
    raw.width = 0;
    CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_ERROR_INVALID_ARGUMENT);
    CHECK(mp_engine_get_render_state(engine, 0, NULL) == MP_ERROR_INVALID_ARGUMENT);

    CHECK(mp_engine_reset(engine) == MP_OK);
    CHECK(mp_engine_get_render_state(engine, 2300, &state) == MP_OK);
    CHECK(state.phase == MP_PHASE_LIVE && state.mask == NULL && state.effect_enable == 0);

    /* A frame captured before a reset is dropped instead of being published after it. */
    raw.width = MW;
    fill_scene(pixels, mask, 1);
    const uint64_t before_reset = mp_engine_session_id(engine);
    CHECK(before_reset != 0);
    frame.session_id = before_reset;
    frame.timestamp_ms = 3000;
    CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_OK);
    CHECK(mp_engine_reset(engine) == MP_OK);
    CHECK(mp_engine_session_id(engine) != before_reset);
    CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_OK); /* accepted, then dropped */
    CHECK(mp_engine_get_render_state(engine, 3000, &state) == MP_OK && state.mask == NULL);
    CHECK(mp_engine_get_stats(engine, &stats) == MP_OK && stats.frames_discarded_by_reset == 1);

    /* A client built against the older header (no session field) is never dropped by mistake. */
    mp_frame legacy = frame;
    legacy.struct_size = (uint32_t)offsetof(mp_frame, session_id);
    legacy.session_id = before_reset; /* garbage as far as this size is concerned */
    CHECK(mp_engine_process_frame(engine, &legacy, &raw) == MP_OK);
    CHECK(mp_engine_get_render_state(engine, 3000, &state) == MP_OK && state.mask != NULL);
    CHECK(state.mask_timestamp_ms == 3000);

    /* Timestamps must not go backwards. */
    frame.session_id = mp_engine_session_id(engine);
    frame.timestamp_ms = 3100;
    CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_OK);
    frame.timestamp_ms = 3000;
    CHECK(mp_engine_process_frame(engine, &frame, &raw) == MP_ERROR_INVALID_ARGUMENT);
    CHECK(mp_engine_get_stats(engine, &stats) == MP_OK && stats.frames_rejected_out_of_order == 1);

    /* Oversized dimensions are refused before any arithmetic or allocation. */
    mp_frame huge = frame;
    huge.timestamp_ms = 3200;
    huge.width = 2147483647;
    huge.stride_bytes = 0;
    CHECK(mp_engine_process_frame(engine, &huge, &raw) == MP_ERROR_INVALID_ARGUMENT);

    /* A config struct from an older, smaller header is initialised without being overrun. */
    {
        const size_t v1_size = offsetof(mp_engine_config, person_protection) + sizeof(float);
        union {
            mp_engine_config config;
            unsigned char bytes[sizeof(mp_engine_config) + 64U];
        } storage;
        memset(&storage, 0xA5, sizeof storage);
        mp_engine_config_init_sized(&storage.config, v1_size);
        CHECK(storage.config.struct_size == v1_size);
        CHECK(storage.config.ring_width == config.ring_width);
        CHECK(storage.config.person_protection == config.person_protection);
        int overrun = 0;
        for (size_t i = v1_size; i < sizeof storage; ++i) {
            overrun += storage.bytes[i] != 0xA5;
        }
        CHECK(overrun == 0);
        mp_engine *from_old_client = NULL;
        CHECK(mp_engine_create(&storage.config, &from_old_client, error, sizeof error) == MP_OK);
        mp_engine_destroy(from_old_client);

        memset(&storage, 0xA5, sizeof storage);
        mp_engine_config_init_sized(&storage.config, 8U); /* smaller than the v1 layout */
        int touched = 0;
        for (size_t i = 0; i < sizeof storage; ++i) {
            touched += storage.bytes[i] != 0xA5;
        }
        CHECK(touched == 0);
    }

    mp_engine_destroy(engine);
    mp_engine_destroy(NULL); /* allowed */
    free(pixels);
    free(mask);

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("c api smoke test passed\n");
    return 0;
}
