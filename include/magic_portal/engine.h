#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "magic_portal/frame.h"
#include "magic_portal/limits.h"
#include "magic_portal/mask_freshness.h"
#include "magic_portal/mask_refiner.h"
#include "magic_portal/person_segmenter.h"
#include "magic_portal/portal_compositor.h"
#include "magic_portal/portal_controller.h"
#include "magic_portal/portal_timeline.h"

namespace magic_portal {

// Numeric values are part of the C ABI (see magic_portal.h): positive = informational,
// negative = error.
enum class Status : int {
    kOk = 0,
    kNoPerson = 1,  // the frame was processed; nobody is in it
    kInvalidArgument = -1,
    kInvalidState = -2,
    kUnsupported = -3,
    kModelError = -4,
    kInferenceError = -5,
    kOutOfMemory = -6,
    kInternal = -7,
};

struct EngineConfig {
    MaskRefinerConfig mask_refiner;
    PortalTimelineConfig timeline;
    PortalStyle style;
    MaskFreshnessConfig freshness;
};

// `FrameInput::session_id` value that skips the session check (the default).
inline constexpr std::uint64_t kAnySession = 0;

struct FrameInput {
    const std::uint8_t *rgba = nullptr;  // display-orientation RGBA8, rows `stride_bytes` apart
    int width = 0;
    int height = 0;
    int stride_bytes = 0;
    bool mirror_horizontally = false;  // mirror before segmentation (e.g. front camera)
    // Capture time on the same clock used for render_state(). Frames must arrive in
    // non-decreasing order: an older one is rejected rather than replacing a newer mask.
    std::int64_t timestamp_ms = 0;
    // The Engine::session_id() that was current when the pixels were captured, or kAnySession.
    // A frame from before a reset() is dropped instead of bringing the old scene back.
    std::uint64_t session_id = kAnySession;
    // Visible part of the frame, normalized, top-left origin, in the *unmirrored* frame.
    float crop_left = 0.0F;
    float crop_top = 0.0F;
    float crop_right = 1.0F;
    float crop_bottom = 1.0F;
};

// Everything the renderer needs for one frame. Screen-space values already account for the
// viewport crop and use a bottom-left origin (the convention of gl_FragCoord).
struct RenderState {
    bool effect_enable = false;  // false: draw the camera untouched
    PortalPhase phase = PortalPhase::kLive;
    int current_background = kNoBackground;
    int next_background = kNoBackground;
    bool camera_is_current = true;
    PortalVisualState visual;
    float person_bounds[4] = {0.29F, 0.14F, 0.71F, 0.86F};  // min_x, min_y, max_x, max_y
    float mask_crop[4] = {0.0F, 0.0F, 1.0F, 1.0F};          // left, top, right, bottom
    bool has_person = false;
    bool mask_updated = false;  // true when `mask` changed since the previous call
    std::uint64_t mask_version = 0;
    std::int64_t mask_timestamp_ms = -1;  // capture time of `mask`; -1 when there is none
    // Render-thread-owned. Valid until the next render_state() call on this engine: neither
    // process_frame() nor reset() on another thread touches it.
    const ConfidenceMask *mask = nullptr;
};

struct EngineStats {
    std::uint64_t frames_processed = 0;  // masks published (with or without a person)
    std::uint64_t frames_without_person = 0;
    std::uint64_t frames_failed = 0;
    std::uint64_t frames_discarded_by_reset = 0;  // from before a reset(); never published
    std::uint64_t frames_rejected_out_of_order = 0;
    double last_copy_ms = 0.0;
    double last_segment_ms = 0.0;
    double last_refine_ms = 0.0;
    double max_total_ms = 0.0;
    std::int64_t last_mask_timestamp_ms = -1;
};

// Platform-independent core of the SDK.
//
// Threading: process_frame() is meant for one worker thread (concurrent calls are
// serialized). begin_transition(), reset(), render_state() and stats() may be called from
// any other thread, typically the render thread. All time arguments use one caller-chosen
// monotonic clock in milliseconds.
class Engine {
public:
    // `segmenter` may be null when callers supply raw masks themselves.
    // Throws std::invalid_argument for an invalid configuration.
    explicit Engine(EngineConfig config, std::unique_ptr<PersonSegmenter> segmenter = nullptr);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Segments (or accepts `external_raw_mask`), refines and publishes a mask. Never throws;
    // on failure the previous mask stays in place and ages out via the freshness limit.
    [[nodiscard]] Status process_frame(const FrameInput &frame,
                                       const ConfidenceMask *external_raw_mask = nullptr) noexcept;

    // Requires a fresh mask so the effect never plays over a camera feed it cannot cut out.
    [[nodiscard]] Status begin_transition(std::int64_t now_ms, int next_background_id) noexcept;

    // Back to the live camera; discards the mask and temporal filter state. Starts a new
    // session: frames captured earlier (see FrameInput::session_id) are dropped, including
    // one that is being processed right now. Safe from any thread, and it does not
    // invalidate a mask pointer the render thread is still using.
    void reset() noexcept;

    // Identifies the current session. It changes on every reset() and is never kAnySession.
    // Read it just before capturing pixels and put it in FrameInput::session_id.
    [[nodiscard]] std::uint64_t session_id() const noexcept;

    [[nodiscard]] RenderState render_state(std::int64_t now_ms) noexcept;

    [[nodiscard]] EngineStats stats() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] const PortalStyle &style() const noexcept { return config_.style; }

private:
    struct Published {
        ConfidenceMask alpha;
        float person_bounds[4] = {0.29F, 0.14F, 0.71F, 0.86F};
        float mask_crop[4] = {0.0F, 0.0F, 1.0F, 1.0F};
        bool has_person = false;
        bool valid = false;  // false: this slot holds no mask (initial state or after reset())
        std::int64_t timestamp_ms = -1;
    };

    void set_error(std::string message);

    EngineConfig config_;
    std::unique_ptr<PersonSegmenter> segmenter_;

    // Worker-only state (guarded by worker_mutex_).
    std::mutex worker_mutex_;
    MaskRefiner refiner_;
    RgbaFrame frame_scratch_;
    ConfidenceMask raw_scratch_;
    std::uint64_t worker_generation_ = 0;
    std::int64_t last_accepted_ms_ = 0;
    bool has_accepted_ = false;

    // Shared state (guarded by state_mutex_).
    mutable std::mutex state_mutex_;
    PortalController controller_;
    MaskFreshness freshness_;
    Published published_;
    Published render_;
    std::uint64_t published_version_ = 0;
    std::uint64_t render_version_ = 0;
    std::uint64_t reset_generation_ = 0;
    bool has_render_mask_ = false;
    EngineStats stats_;
    std::string last_error_;
};

}  // namespace magic_portal
