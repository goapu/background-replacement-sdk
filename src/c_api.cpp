#include "magic_portal/magic_portal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "magic_portal/engine.h"
#include "magic_portal/limits.h"
#include "magic_portal/portal_shader.h"
#include "magic_portal/tflite_person_segmenter.h"

namespace mp = magic_portal;

static_assert(static_cast<int>(mp::Status::kOk) == MP_OK, "status mapping");
static_assert(static_cast<int>(mp::Status::kNoPerson) == MP_NO_PERSON, "status mapping");
static_assert(static_cast<int>(mp::Status::kInvalidArgument) == MP_ERROR_INVALID_ARGUMENT, "status mapping");
static_assert(static_cast<int>(mp::Status::kInvalidState) == MP_ERROR_INVALID_STATE, "status mapping");
static_assert(static_cast<int>(mp::Status::kUnsupported) == MP_ERROR_UNSUPPORTED, "status mapping");
static_assert(static_cast<int>(mp::Status::kModelError) == MP_ERROR_MODEL, "status mapping");
static_assert(static_cast<int>(mp::Status::kInferenceError) == MP_ERROR_INFERENCE, "status mapping");
static_assert(static_cast<int>(mp::Status::kOutOfMemory) == MP_ERROR_OUT_OF_MEMORY, "status mapping");
static_assert(static_cast<int>(mp::Status::kInternal) == MP_ERROR_INTERNAL, "status mapping");
static_assert(static_cast<int>(mp::PortalPhase::kLive) == MP_PHASE_LIVE, "phase mapping");
static_assert(static_cast<int>(mp::PortalPhase::kTransitioning) == MP_PHASE_TRANSITIONING, "phase mapping");
static_assert(static_cast<int>(mp::PortalPhase::kReplaced) == MP_PHASE_REPLACED, "phase mapping");

struct mp_engine {
    std::unique_ptr<mp::Engine> impl;
};

namespace {

mp_status to_c(mp::Status status) {
    return static_cast<mp_status>(static_cast<int>(status));
}

void write_text(const std::string &text, char *buffer, size_t capacity) {
    if (buffer == nullptr || capacity == 0U) {
        return;
    }
    const size_t count = std::min(text.size(), capacity - 1U);
    std::memcpy(buffer, text.data(), count);
    buffer[count] = '\0';
}

// A struct is accepted when the caller's copy is at least as large as the part we read.
template <typename T>
bool struct_ok(const T *value, size_t minimum) {
    return value != nullptr && value->struct_size >= minimum;
}

// Copies at most `value->struct_size` bytes so older (smaller) caller structs stay valid.
template <typename T>
void store_output(const T &source, T *destination) {
    const std::uint32_t caller_size = destination->struct_size;
    const size_t count = std::min<size_t>(caller_size, sizeof(T));
    std::memcpy(destination, &source, count);
    destination->struct_size = caller_size;
}

template <typename Body>
mp_status guarded(Body &&body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc &) {
        return MP_ERROR_OUT_OF_MEMORY;
    } catch (const std::invalid_argument &) {
        return MP_ERROR_INVALID_ARGUMENT;
    } catch (...) {
        return MP_ERROR_INTERNAL;
    }
}

mp::Rgba color_from(const float value[4]) {
    const auto channel = [](float v) {
        const float clamped = std::min(1.0F, std::max(0.0F, v));
        return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
    };
    return {channel(value[0]), channel(value[1]), channel(value[2]), channel(value[3])};
}

void color_to(mp::Rgba color, float out[4]) {
    out[0] = static_cast<float>(color.r) / 255.0F;
    out[1] = static_cast<float>(color.g) / 255.0F;
    out[2] = static_cast<float>(color.b) / 255.0F;
    out[3] = static_cast<float>(color.a) / 255.0F;
}

// Fields up to and including person_protection form the v1 layout.
constexpr size_t kMinimumConfigSize = offsetof(mp_engine_config, person_protection) + sizeof(float);

mp_engine_config default_config() {
    const mp::MaskRefinerConfig refiner;
    const mp::PortalTimelineConfig timeline;
    const mp::PortalStyle style;
    const mp::MaskFreshnessConfig freshness;
    const mp::TflitePersonSegmenterConfig model;
    mp_engine_config config;
    std::memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.num_threads = model.num_threads;
    config.output_mode = static_cast<int32_t>(model.output_mode);
    config.person_class_index = model.person_class_index;
    config.background_class_index = model.background_class_index;
    config.output_is_logits = model.output_is_logits ? 1 : 0;
    config.resize_mode = static_cast<int32_t>(model.resize_mode);
    config.input_mean = model.input_mean;
    config.input_std = model.input_std;
    config.stale_after_ms = freshness.stale_after_ms;
    config.transition_duration_ms = timeline.duration_ms;
    config.morphology_radius = refiner.morphology_radius;
    config.bilateral_radius = refiner.bilateral_radius;
    config.spatial_sigma = refiner.spatial_sigma;
    config.color_sigma = refiner.color_sigma;
    config.temporal_attack = refiner.temporal_attack;
    config.temporal_release = refiner.temporal_release;
    config.low_confidence = refiner.low_confidence;
    config.high_confidence = refiner.high_confidence;
    config.exact_background_cutoff = refiner.exact_background_cutoff;
    config.exact_person_cutoff = refiner.exact_person_cutoff;
    config.bounds_threshold = refiner.bounds_threshold;
    config.minimum_person_area_fraction = refiner.minimum_person_area_fraction;
    color_to(style.warm_color, config.warm_color);
    color_to(style.cool_color, config.cool_color);
    config.ring_width = style.ring_width;
    config.ring_strength = style.ring_strength;
    config.particle_strength = style.particle_strength;
    config.silhouette_glow = style.silhouette_glow;
    config.reveal_feather = style.reveal_feather;
    config.person_protection = style.person_protection;
    return config;
}

bool all_finite(const float *values, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

extern "C" {

uint32_t mp_abi_version(void) {
    return MP_ABI_VERSION;
}

const char *mp_version_string(void) {
    return MP_VERSION_STRING;
}

int32_t mp_has_tflite_backend(void) {
#if defined(MAGIC_PORTAL_WITH_TFLITE)
    return 1;
#else
    return 0;
#endif
}

const char *mp_status_string(mp_status status) {
    switch (status) {
        case MP_OK:
            return "ok";
        case MP_NO_PERSON:
            return "no person in frame";
        case MP_ERROR_INVALID_ARGUMENT:
            return "invalid argument";
        case MP_ERROR_INVALID_STATE:
            return "invalid state";
        case MP_ERROR_UNSUPPORTED:
            return "unsupported";
        case MP_ERROR_MODEL:
            return "model error";
        case MP_ERROR_INFERENCE:
            return "inference error";
        case MP_ERROR_OUT_OF_MEMORY:
            return "out of memory";
        case MP_ERROR_INTERNAL:
            return "internal error";
    }
    return "unknown status";
}

void mp_engine_config_init(mp_engine_config *config) {
    if (config == nullptr) {
        return;
    }
    *config = default_config();
}

void mp_engine_config_init_sized(mp_engine_config *config, size_t size) {
    if (config == nullptr || size < kMinimumConfigSize) {
        return;  // not even the v1 layout fits: leave the caller's memory alone
    }
    const mp_engine_config defaults = default_config();
    // Only the bytes the caller owns are written; anything past our layout is zeroed.
    const size_t known = std::min(size, sizeof(defaults));
    std::memcpy(config, &defaults, known);
    if (size > known) {
        std::memset(reinterpret_cast<unsigned char *>(config) + known, 0, size - known);
    }
    config->struct_size = static_cast<uint32_t>(size);
}

mp_status mp_engine_create(const mp_engine_config *config, mp_engine **out_engine, char *error_buffer,
                           size_t error_capacity) {
    if (error_buffer != nullptr && error_capacity > 0U) {
        error_buffer[0] = '\0';
    }
    if (out_engine == nullptr) {
        write_text("out_engine must not be null", error_buffer, error_capacity);
        return MP_ERROR_INVALID_ARGUMENT;
    }
    *out_engine = nullptr;
    if (!struct_ok(config, kMinimumConfigSize)) {
        write_text("config is null or its struct_size is too small; call mp_engine_config_init", error_buffer,
                   error_capacity);
        return MP_ERROR_INVALID_ARGUMENT;
    }
    try {
        if (config->model_path != nullptr && config->model_data != nullptr) {
            write_text("provide model_path or model_data, not both", error_buffer, error_capacity);
            return MP_ERROR_INVALID_ARGUMENT;
        }
        if (config->model_data != nullptr && config->model_size == 0U) {
            write_text("model_data requires a non-zero model_size", error_buffer, error_capacity);
            return MP_ERROR_INVALID_ARGUMENT;
        }
        if (config->output_mode != MP_OUTPUT_SINGLE_CLASS &&
            config->output_mode != MP_OUTPUT_ONE_MINUS_BACKGROUND) {
            write_text("unknown output_mode", error_buffer, error_capacity);
            return MP_ERROR_INVALID_ARGUMENT;
        }
        if (config->resize_mode != MP_RESIZE_BILINEAR && config->resize_mode != MP_RESIZE_AREA) {
            write_text("unknown resize_mode", error_buffer, error_capacity);
            return MP_ERROR_INVALID_ARGUMENT;
        }

        if (!all_finite(config->warm_color, 4U) || !all_finite(config->cool_color, 4U)) {
            write_text("warm_color and cool_color must be finite", error_buffer, error_capacity);
            return MP_ERROR_INVALID_ARGUMENT;
        }

        mp::EngineConfig engine_config;
        engine_config.mask_refiner.morphology_radius = config->morphology_radius;
        engine_config.mask_refiner.bilateral_radius = config->bilateral_radius;
        engine_config.mask_refiner.spatial_sigma = config->spatial_sigma;
        engine_config.mask_refiner.color_sigma = config->color_sigma;
        engine_config.mask_refiner.temporal_attack = config->temporal_attack;
        engine_config.mask_refiner.temporal_release = config->temporal_release;
        engine_config.mask_refiner.low_confidence = config->low_confidence;
        engine_config.mask_refiner.high_confidence = config->high_confidence;
        engine_config.mask_refiner.exact_background_cutoff = config->exact_background_cutoff;
        engine_config.mask_refiner.exact_person_cutoff = config->exact_person_cutoff;
        engine_config.mask_refiner.bounds_threshold = config->bounds_threshold;
        engine_config.mask_refiner.minimum_person_area_fraction = config->minimum_person_area_fraction;
        engine_config.timeline.duration_ms = config->transition_duration_ms;
        engine_config.freshness.stale_after_ms = config->stale_after_ms;
        engine_config.style.warm_color = color_from(config->warm_color);
        engine_config.style.cool_color = color_from(config->cool_color);
        engine_config.style.ring_width = config->ring_width;
        engine_config.style.ring_strength = config->ring_strength;
        engine_config.style.particle_strength = config->particle_strength;
        engine_config.style.silhouette_glow = config->silhouette_glow;
        engine_config.style.reveal_feather = config->reveal_feather;
        engine_config.style.person_protection = config->person_protection;
        if (engine_config.timeline.duration_ms <= 0) {
            write_text("transition_duration_ms must be positive", error_buffer, error_capacity);
            return MP_ERROR_INVALID_ARGUMENT;
        }

        std::unique_ptr<mp::PersonSegmenter> segmenter;
        if (config->model_path != nullptr || config->model_data != nullptr) {
            mp::TflitePersonSegmenterConfig model;
            if (config->model_path != nullptr) {
                model.model_path = config->model_path;
            }
            model.model_data = config->model_data;
            model.model_size = config->model_size;
            model.num_threads = config->num_threads;
            model.person_class_index = config->person_class_index;
            model.background_class_index = config->background_class_index;
            model.output_mode = static_cast<mp::OutputMode>(config->output_mode);
            model.output_is_logits = config->output_is_logits != 0;
            model.resize_mode = static_cast<mp::ResizeMode>(config->resize_mode);
            model.input_mean = config->input_mean;
            model.input_std = config->input_std;
            std::string error;
            segmenter = mp::create_tflite_segmenter(model, &error);
            if (!segmenter) {
                write_text(error, error_buffer, error_capacity);
                return mp_has_tflite_backend() != 0 ? MP_ERROR_MODEL : MP_ERROR_UNSUPPORTED;
            }
        }

        auto handle = std::make_unique<mp_engine>();
        handle->impl = std::make_unique<mp::Engine>(engine_config, std::move(segmenter));
        *out_engine = handle.release();
        return MP_OK;
    } catch (const std::bad_alloc &) {
        write_text("out of memory", error_buffer, error_capacity);
        return MP_ERROR_OUT_OF_MEMORY;
    } catch (const std::invalid_argument &exception) {
        write_text(exception.what(), error_buffer, error_capacity);
        return MP_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception &exception) {
        write_text(exception.what(), error_buffer, error_capacity);
        return MP_ERROR_INTERNAL;
    } catch (...) {
        write_text("unknown failure", error_buffer, error_capacity);
        return MP_ERROR_INTERNAL;
    }
}

void mp_engine_destroy(mp_engine *engine) {
    try {
        delete engine;
    } catch (...) {
        // Best effort: this path cannot report further failures.
    }
}

mp_status mp_engine_process_frame(mp_engine *engine, const mp_frame *frame, const mp_raw_mask *raw_mask) {
    if (engine == nullptr || !engine->impl) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    constexpr size_t kMinimumFrameSize = offsetof(mp_frame, crop) + 4U * sizeof(float);
    if (!struct_ok(frame, kMinimumFrameSize)) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    return guarded([&]() -> mp_status {
        // All size arithmetic is done in 64 bits on values that are range-checked first, so
        // a hostile width/height/stride can neither overflow nor reach an allocation.
        std::int64_t stride = 0;
        if (frame->pixels == nullptr || !mp::frame_size_ok(frame->width, frame->height) ||
            !mp::resolve_rgba_stride(frame->width, frame->stride_bytes, &stride)) {
            return MP_ERROR_INVALID_ARGUMENT;
        }
        mp::FrameInput input;
        input.rgba = frame->pixels;
        input.width = frame->width;
        input.height = frame->height;
        input.stride_bytes = static_cast<int>(stride);
        input.mirror_horizontally = frame->mirror_horizontally != 0;
        input.timestamp_ms = frame->timestamp_ms;
        input.crop_left = frame->crop[0];
        input.crop_top = frame->crop[1];
        input.crop_right = frame->crop[2];
        input.crop_bottom = frame->crop[3];
        constexpr size_t kSessionFieldEnd = offsetof(mp_frame, session_id) + sizeof(uint64_t);
        if (frame->struct_size >= kSessionFieldEnd) {
            input.session_id = frame->session_id;
        }

        if (raw_mask == nullptr) {
            return to_c(engine->impl->process_frame(input, nullptr));
        }
        constexpr size_t kMinimumMaskSize = offsetof(mp_raw_mask, height) + sizeof(int32_t);
        if (!struct_ok(raw_mask, kMinimumMaskSize) || raw_mask->confidence == nullptr ||
            !mp::mask_size_ok(raw_mask->width, raw_mask->height)) {
            return MP_ERROR_INVALID_ARGUMENT;
        }
        const size_t count = static_cast<size_t>(raw_mask->width) * static_cast<size_t>(raw_mask->height);
        mp::ConfidenceMask mask(raw_mask->width, raw_mask->height,
                                std::vector<float>(raw_mask->confidence, raw_mask->confidence + count));
        return to_c(engine->impl->process_frame(input, &mask));
    });
}

uint64_t mp_engine_session_id(const mp_engine *engine) {
    if (engine == nullptr || !engine->impl) {
        return 0U;
    }
    return engine->impl->session_id();
}

mp_status mp_engine_begin_transition(mp_engine *engine, int64_t now_ms, int32_t next_background_id) {
    if (engine == nullptr || !engine->impl) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    return guarded([&]() { return to_c(engine->impl->begin_transition(now_ms, next_background_id)); });
}

mp_status mp_engine_reset(mp_engine *engine) {
    if (engine == nullptr || !engine->impl) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    return guarded([&]() {
        engine->impl->reset();
        return MP_OK;
    });
}

mp_status mp_engine_get_render_state(mp_engine *engine, int64_t now_ms, mp_render_state *out_state) {
    if (engine == nullptr || !engine->impl) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    constexpr size_t kMinimumStateSize = offsetof(mp_render_state, mask_height) + sizeof(int32_t);
    if (!struct_ok(out_state, kMinimumStateSize)) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    return guarded([&]() -> mp_status {
        const mp::RenderState s = engine->impl->render_state(now_ms);
        const mp::PortalStyle &style = engine->impl->style();
        mp_render_state out;
        std::memset(&out, 0, sizeof(out));
        out.struct_size = sizeof(out);
        out.effect_enable = s.effect_enable ? 1 : 0;
        out.phase = static_cast<int32_t>(s.phase);
        out.current_background = s.current_background;
        out.next_background = s.next_background;
        out.camera_is_current = s.camera_is_current ? 1 : 0;
        out.has_person = s.has_person ? 1 : 0;
        out.mask_updated = s.mask_updated ? 1 : 0;
        out.mask_version = s.mask_version;
        out.progress = s.visual.progress;
        out.reveal = s.visual.complete ? 1.0F : s.visual.reveal;
        out.ring_scale = s.visual.ring_scale;
        out.ring_alpha = s.visual.ring_alpha;
        out.spin_turns = s.visual.spin_turns;
        out.person_visibility = std::min(1.0F, std::max(0.0F, s.visual.person_visibility));
        std::memcpy(out.person_bounds, s.person_bounds, sizeof(out.person_bounds));
        std::memcpy(out.mask_crop, s.mask_crop, sizeof(out.mask_crop));
        out.style_a[0] = style.ring_width;
        out.style_a[1] = style.ring_strength;
        out.style_a[2] = style.particle_strength;
        out.style_a[3] = style.silhouette_glow;
        out.style_b[0] = style.reveal_feather;
        out.style_b[1] = style.person_protection;
        color_to(style.warm_color, out.warm);
        color_to(style.cool_color, out.cool);
        if (s.mask != nullptr && !s.mask->empty()) {
            out.mask = s.mask->data();
            out.mask_width = s.mask->width();
            out.mask_height = s.mask->height();
        }
        out.mask_timestamp_ms = s.mask_timestamp_ms;
        store_output(out, out_state);
        return MP_OK;
    });
}

mp_status mp_engine_get_stats(const mp_engine *engine, mp_stats *out_stats) {
    if (engine == nullptr || !engine->impl) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    constexpr size_t kMinimumStatsSize = offsetof(mp_stats, last_mask_timestamp_ms) + sizeof(int64_t);
    if (!struct_ok(out_stats, kMinimumStatsSize)) {
        return MP_ERROR_INVALID_ARGUMENT;
    }
    return guarded([&]() -> mp_status {
        const mp::EngineStats s = engine->impl->stats();
        mp_stats out;
        std::memset(&out, 0, sizeof(out));
        out.struct_size = sizeof(out);
        out.frames_processed = s.frames_processed;
        out.frames_without_person = s.frames_without_person;
        out.frames_failed = s.frames_failed;
        out.frames_discarded_by_reset = s.frames_discarded_by_reset;
        out.last_copy_ms = s.last_copy_ms;
        out.last_segment_ms = s.last_segment_ms;
        out.last_refine_ms = s.last_refine_ms;
        out.max_total_ms = s.max_total_ms;
        out.last_mask_timestamp_ms = s.last_mask_timestamp_ms;
        out.frames_rejected_out_of_order = s.frames_rejected_out_of_order;
        store_output(out, out_stats);
        return MP_OK;
    });
}

size_t mp_engine_copy_last_error(const mp_engine *engine, char *buffer, size_t capacity) {
    try {
        if (engine == nullptr || !engine->impl) {
            write_text("", buffer, capacity);
            return 0U;
        }
        const std::string text = engine->impl->last_error();
        write_text(text, buffer, capacity);
        return text.size();
    } catch (...) {
        write_text("", buffer, capacity);
        return 0U;
    }
}

size_t mp_shader_source(char *buffer, size_t capacity) {
    try {
        const std::string text = mp::portal_fragment_shader_source();
        write_text(text, buffer, capacity);
        return text.size();
    } catch (...) {
        write_text("", buffer, capacity);
        return 0U;
    }
}

}  // extern "C"
