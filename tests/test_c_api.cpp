// The C boundary: configuration validation and arithmetic on caller-supplied sizes.
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "magic_portal/magic_portal.h"
#include "magic_portal/portal_constants.h"
#include "test_harness.h"

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

mp_engine_config default_config() {
    mp_engine_config config{};
    MP_ENGINE_CONFIG_INIT(&config);
    return config;
}

mp_status create_with(const mp_engine_config &config, mp_engine **engine, char *error, size_t capacity) {
    return mp_engine_create(&config, engine, error, capacity);
}

// Creating must fail with a message and leave no engine behind.
bool rejected(const mp_engine_config &config) {
    mp_engine *engine = nullptr;
    char error[256] = {0};
    const mp_status status = create_with(config, &engine, error, sizeof error);
    const bool ok = status == MP_ERROR_INVALID_ARGUMENT && engine == nullptr && error[0] != '\0';
    mp_engine_destroy(engine);
    return ok;
}

struct EngineHandle {
    EngineHandle() {
        const mp_engine_config config = default_config();
        char error[256] = {0};
        (void)create_with(config, &engine, error, sizeof error);
    }
    ~EngineHandle() { mp_engine_destroy(engine); }
    mp_engine *engine = nullptr;
};

mp_frame frame_of(const std::uint8_t *pixels, int width, int height, int stride) {
    mp_frame frame{};
    frame.struct_size = sizeof(frame);
    frame.pixels = pixels;
    frame.width = width;
    frame.height = height;
    frame.stride_bytes = stride;
    frame.mirror_horizontally = 0;
    frame.timestamp_ms = 100;
    frame.crop[0] = 0.0F;
    frame.crop[1] = 0.0F;
    frame.crop[2] = 1.0F;
    frame.crop[3] = 1.0F;
    return frame;
}

}  // namespace

MT_TEST("c api: defaults create an engine") {
    mp_engine *engine = nullptr;
    char error[64] = {0};
    const mp_engine_config config = default_config();
    MT_CHECK(create_with(config, &engine, error, sizeof error) == MP_OK);
    MT_CHECK(engine != nullptr);
    mp_engine_destroy(engine);
}

MT_TEST("c api: a zero or non-finite look parameter is rejected at create time") {
    {
        mp_engine_config c = default_config();
        c.ring_width = 0.0F;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.ring_width = kNaN;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.reveal_feather = 0.0F;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.ring_strength = kInf;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.particle_strength = kNaN;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.person_protection = kNaN;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.warm_color[1] = kNaN;
        MT_CHECK(rejected(c));
    }
}

MT_TEST("c api: person protection must exceed the shader guard threshold") {
    for (float threshold : {0.5F, magic_portal::constants::kPersonGuardStart}) {
        auto config = default_config();
        config.person_protection = threshold;
        MT_CHECK(rejected(config));
    }
    auto config = default_config();
    config.person_protection = 0.8F;
    mp_engine *engine = nullptr;
    char error[128] = {0};
    MT_CHECK(create_with(config, &engine, error, sizeof error) == MP_OK);
    mp_engine_destroy(engine);
}

MT_TEST("c api: non-finite or out-of-range refinement parameters are rejected") {
    {
        mp_engine_config c = default_config();
        c.spatial_sigma = kNaN;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.color_sigma = kInf;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.temporal_attack = kNaN;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.low_confidence = kNaN;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.bounds_threshold = kNaN;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.morphology_radius = 100000;  // would make every frame take minutes
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.bilateral_radius = INT_MAX;
        MT_CHECK(rejected(c));
    }
    {
        mp_engine_config c = default_config();
        c.stale_after_ms = 0;
        MT_CHECK(rejected(c));
    }
}

MT_TEST("c api: frame sizes are checked in 64-bit arithmetic") {
    EngineHandle handle;
    MT_CHECK(handle.engine != nullptr);
    const std::vector<std::uint8_t> pixels(16U * 16U * 4U, 0x40);

    // width * 4 does not fit in int: this used to be signed overflow before any check ran.
    mp_frame huge = frame_of(pixels.data(), INT_MAX, 2, 0);
    MT_CHECK(mp_engine_process_frame(handle.engine, &huge, nullptr) == MP_ERROR_INVALID_ARGUMENT);
    mp_frame huge_stride = frame_of(pixels.data(), INT_MAX, 2, INT_MAX);
    MT_CHECK(mp_engine_process_frame(handle.engine, &huge_stride, nullptr) == MP_ERROR_INVALID_ARGUMENT);
    mp_frame tall = frame_of(pixels.data(), 16, INT_MAX, 0);
    MT_CHECK(mp_engine_process_frame(handle.engine, &tall, nullptr) == MP_ERROR_INVALID_ARGUMENT);
    mp_frame negative = frame_of(pixels.data(), -4, 16, 0);
    MT_CHECK(mp_engine_process_frame(handle.engine, &negative, nullptr) == MP_ERROR_INVALID_ARGUMENT);
    mp_frame short_stride = frame_of(pixels.data(), 16, 16, 63);
    MT_CHECK(mp_engine_process_frame(handle.engine, &short_stride, nullptr) == MP_ERROR_INVALID_ARGUMENT);
    mp_frame negative_stride = frame_of(pixels.data(), 16, 16, -64);
    MT_CHECK(mp_engine_process_frame(handle.engine, &negative_stride, nullptr) == MP_ERROR_INVALID_ARGUMENT);
}

MT_TEST("c api: raw mask sizes are bounded before anything is read or allocated") {
    EngineHandle handle;
    MT_CHECK(handle.engine != nullptr);
    const std::vector<std::uint8_t> pixels(16U * 16U * 4U, 0x40);
    const std::vector<float> values(16U * 16U, 0.9F);
    const mp_frame frame = frame_of(pixels.data(), 16, 16, 0);

    mp_raw_mask mask{};
    mask.struct_size = sizeof(mask);
    mask.confidence = values.data();

    mask.width = INT_MAX;
    mask.height = INT_MAX;
    MT_CHECK(mp_engine_process_frame(handle.engine, &frame, &mask) == MP_ERROR_INVALID_ARGUMENT);
    mask.width = 1 << 20;
    mask.height = 1 << 20;
    MT_CHECK(mp_engine_process_frame(handle.engine, &frame, &mask) == MP_ERROR_INVALID_ARGUMENT);
    mask.width = 0;
    mask.height = 16;
    MT_CHECK(mp_engine_process_frame(handle.engine, &frame, &mask) == MP_ERROR_INVALID_ARGUMENT);

    mask.width = 16;
    mask.height = 16;
    MT_CHECK(mp_engine_process_frame(handle.engine, &frame, &mask) == MP_OK);  // a sane one still works
}
