#include "magic_portal/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <new>
#include <string>
#include <utility>

#include "magic_portal/portal_constants.h"

namespace magic_portal {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

bool finite_in_unit_range(float value) {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

}  // namespace

Engine::Engine(EngineConfig config, std::unique_ptr<PersonSegmenter> segmenter)
    : config_(config),
      segmenter_(std::move(segmenter)),
      refiner_(config.mask_refiner),
      controller_(config.timeline),
      freshness_(config.freshness) {
    validate_style(config_.style);
}

Engine::~Engine() = default;

void Engine::set_error(std::string message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = std::move(message);
}

std::string Engine::last_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

std::uint64_t Engine::session_id() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return reset_generation_ + 1U;  // never kAnySession (0)
}

EngineStats Engine::stats() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return stats_;
}

Status Engine::process_frame(const FrameInput &input, const ConfidenceMask *external_raw_mask) noexcept {
    try {
        if (input.rgba == nullptr || !frame_size_ok(input.width, input.height)) {
            set_error("frame must have pixels and dimensions within 1.." +
                      std::to_string(kMaxFrameDimension) + " per side and 64 megapixels");
            return Status::kInvalidArgument;
        }
        if (static_cast<std::int64_t>(input.stride_bytes) < 4 * static_cast<std::int64_t>(input.width)) {
            set_error("frame stride is smaller than one row of RGBA pixels");
            return Status::kInvalidArgument;
        }
        const float crop_values[4] = {input.crop_left, input.crop_top, input.crop_right, input.crop_bottom};
        for (float value : crop_values) {
            if (!finite_in_unit_range(value)) {
                set_error("crop rectangle must lie inside [0, 1]");
                return Status::kInvalidArgument;
            }
        }
        if (input.crop_right - input.crop_left < 1.0e-5F || input.crop_bottom - input.crop_top < 1.0e-5F) {
            set_error("crop rectangle is empty");
            return Status::kInvalidArgument;
        }
        if (external_raw_mask != nullptr) {
            if (external_raw_mask->empty() ||
                external_raw_mask->size() != static_cast<std::size_t>(external_raw_mask->width()) *
                                                 static_cast<std::size_t>(external_raw_mask->height()) ||
                !mask_size_ok(external_raw_mask->width(), external_raw_mask->height())) {
                set_error("external raw mask is empty, inconsistent or larger than " +
                          std::to_string(kMaxMaskDimension) + " pixels per side");
                return Status::kInvalidArgument;
            }
        } else if (!segmenter_) {
            set_error("no segmenter is configured; provide a model or pass a raw mask");
            return Status::kInvalidState;
        }

        std::lock_guard<std::mutex> worker_lock(worker_mutex_);
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            generation = reset_generation_;
        }
        if (input.session_id != kAnySession && input.session_id != generation + 1U) {
            // Captured before the latest reset(): it describes a scene that no longer applies.
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.frames_discarded_by_reset;
            return Status::kOk;
        }
        if (generation != worker_generation_) {
            refiner_.reset();
            worker_generation_ = generation;
            has_accepted_ = false;  // a new session may use a new clock
        }
        if (has_accepted_ && input.timestamp_ms < last_accepted_ms_) {
            // A late or reordered frame must never replace a newer mask: it would put an
            // older scene on screen while freshness believes the newer one is current.
            set_error("frame timestamp is older than the newest frame already accepted");
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.frames_rejected_out_of_order;
            return Status::kInvalidArgument;
        }

        const auto t0 = Clock::now();
        copy_rgba_strided(input.rgba, input.width, input.height, input.stride_bytes,
                          input.mirror_horizontally, &frame_scratch_);
        const auto t1 = Clock::now();

        const ConfidenceMask *raw = external_raw_mask;
        if (raw == nullptr) {
            std::string error;
            if (!segmenter_->segment(frame_scratch_, input.timestamp_ms, &raw_scratch_, &error)) {
                set_error(error.empty() ? "person segmentation failed" : std::move(error));
                std::lock_guard<std::mutex> lock(state_mutex_);
                ++stats_.frames_failed;
                return Status::kInferenceError;
            }
            raw = &raw_scratch_;
        }
        const auto t2 = Clock::now();

        RefinedPersonMask refined = refiner_.refine(frame_scratch_, *raw, input.timestamp_ms);
        last_accepted_ms_ = input.timestamp_ms;
        has_accepted_ = true;
        const auto t3 = Clock::now();

        Published next;
        next.valid = true;
        next.timestamp_ms = input.timestamp_ms;
        next.has_person = refined.bounds.valid;
        float left = input.crop_left;
        float right = input.crop_right;
        if (input.mirror_horizontally) {
            left = 1.0F - input.crop_right;
            right = 1.0F - input.crop_left;
        }
        next.mask_crop[0] = left;
        next.mask_crop[1] = input.crop_top;
        next.mask_crop[2] = right;
        next.mask_crop[3] = input.crop_bottom;
        if (refined.bounds.valid) {
            next.alpha = std::move(refined.alpha);
            const float inverse_width = 1.0F / (right - left);
            const float inverse_height = 1.0F / (input.crop_bottom - input.crop_top);
            const PersonBounds &b = refined.bounds;
            const float min_x = std::clamp((b.min_x - left) * inverse_width, 0.0F, 1.0F);
            const float max_x = std::clamp((b.max_x - left) * inverse_width, 0.0F, 1.0F);
            const float min_y_top = std::clamp((b.min_y - input.crop_top) * inverse_height, 0.0F, 1.0F);
            const float max_y_top = std::clamp((b.max_y - input.crop_top) * inverse_height, 0.0F, 1.0F);
            next.person_bounds[0] = min_x;
            next.person_bounds[1] = 1.0F - max_y_top;  // top-left origin -> bottom-left origin
            next.person_bounds[2] = max_x;
            next.person_bounds[3] = 1.0F - min_y_top;
        } else {
            // Nobody is in view: publish an explicit empty mask instead of residual noise, so
            // an implausible blob can never be cut out of the camera.
            next.alpha = ConfidenceMask(refined.alpha.width(), refined.alpha.height());
            const float half_width = 0.5F * constants::kFallbackBodyWidth;
            const float half_height = 0.5F * constants::kFallbackBodyHeight;
            next.person_bounds[0] = 0.5F - half_width;
            next.person_bounds[1] = 0.5F - half_height;
            next.person_bounds[2] = 0.5F + half_width;
            next.person_bounds[3] = 0.5F + half_height;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        if (generation != reset_generation_) {
            ++stats_.frames_discarded_by_reset;  // started before reset(); must not resurrect it
            return Status::kOk;
        }
        published_ = std::move(next);
        ++published_version_;
        freshness_.on_valid_mask(input.timestamp_ms);
        last_error_.clear();
        ++stats_.frames_processed;
        stats_.frames_without_person += refined.bounds.valid ? 0U : 1U;
        stats_.last_copy_ms = elapsed_ms(t0, t1);
        stats_.last_segment_ms = elapsed_ms(t1, t2);
        stats_.last_refine_ms = elapsed_ms(t2, t3);
        stats_.max_total_ms = std::max(stats_.max_total_ms, elapsed_ms(t0, t3));
        stats_.last_mask_timestamp_ms = input.timestamp_ms;
        return published_.has_person ? Status::kOk : Status::kNoPerson;
    } catch (const std::bad_alloc &) {
        try {
            set_error("out of memory while processing a frame");
        } catch (...) {
            // Best effort: this path cannot report further failures.
        }
        return Status::kOutOfMemory;
    } catch (const std::exception &exception) {
        try {
            set_error(exception.what());
            std::lock_guard<std::mutex> lock(state_mutex_);
            ++stats_.frames_failed;
        } catch (...) {
            // Best effort: this path cannot report further failures.
        }
        return Status::kInternal;
    } catch (...) {
        return Status::kInternal;
    }
}

Status Engine::begin_transition(std::int64_t now_ms, int next_background_id) noexcept {
    try {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (next_background_id < 0) {
            last_error_ = "background id must be non-negative";
            return Status::kInvalidArgument;
        }
        if (controller_.phase() == PortalPhase::kTransitioning) {
            last_error_ = "a transition is already running";
            return Status::kInvalidState;
        }
        if (!freshness_.is_fresh(now_ms)) {
            last_error_ = "no fresh person mask yet; start the camera and wait for segmentation";
            return Status::kInvalidState;
        }
        return controller_.begin(now_ms, next_background_id) ? Status::kOk : Status::kInvalidState;
    } catch (...) {
        return Status::kInternal;
    }
}

void Engine::reset() noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    controller_.reset();
    freshness_.reset();
    ++reset_generation_;
    // Only the publish slot is cleared here. The render copy may still be read through the
    // pointer from the last render_state(); render_state() drops it on its next call, on the
    // thread that uses it.
    published_ = Published();
    ++published_version_;
}

RenderState Engine::render_state(std::int64_t now_ms) noexcept {
    RenderState state;
    std::lock_guard<std::mutex> lock(state_mutex_);
    const PortalFrameState frame = controller_.update(now_ms);
    if (published_version_ != render_version_) {
        if (published_.valid) {
            std::swap(render_.alpha, published_.alpha);  // O(1): no copy on the render thread
            std::copy(std::begin(published_.person_bounds), std::end(published_.person_bounds),
                      std::begin(render_.person_bounds));
            std::copy(std::begin(published_.mask_crop), std::end(published_.mask_crop),
                      std::begin(render_.mask_crop));
            render_.has_person = published_.has_person;
            render_.timestamp_ms = published_.timestamp_ms;
            has_render_mask_ = true;
            state.mask_updated = true;
        } else {
            render_ = Published();  // a reset() happened: drop the old mask now
            has_render_mask_ = false;
        }
        render_version_ = published_version_;
    }
    state.phase = frame.phase;
    state.current_background = frame.current_background;
    state.next_background = frame.next_background;
    state.camera_is_current = frame.camera_is_current;
    state.visual = frame.visual;
    state.mask_version = render_version_;
    state.mask_timestamp_ms = has_render_mask_ ? render_.timestamp_ms : -1;
    state.has_person = has_render_mask_ && render_.has_person;
    std::copy(std::begin(render_.person_bounds), std::end(render_.person_bounds),
              std::begin(state.person_bounds));
    std::copy(std::begin(render_.mask_crop), std::end(render_.mask_crop), std::begin(state.mask_crop));
    state.mask = has_render_mask_ ? &render_.alpha : nullptr;
    // Fail safe: the effect runs only over a recent mask (judged by the timestamp of the mask
    // that is on screen, not of whatever arrived last), and not at all while live.
    state.effect_enable = frame.phase != PortalPhase::kLive && has_render_mask_ &&
                          freshness_.fresh_at(now_ms, render_.timestamp_ms);
    return state;
}

}  // namespace magic_portal
