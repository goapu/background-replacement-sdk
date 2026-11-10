#pragma once

#include <cstdint>
#include <stdexcept>

#include "magic_portal/limits.h"

namespace magic_portal {

struct MaskFreshnessConfig {
    // A mask older than this no longer describes the scene; the effect must fail safe
    // (show the unmodified camera) instead of cutting out a ghost of where the person was.
    std::int64_t stale_after_ms = 500;
    // A mask stamped further ahead of "now" than this was taken on a different clock (or the
    // clock jumped). It cannot be trusted either, so it is treated like a stale one.
    std::int64_t future_tolerance_ms = 100;
};

// Whether a mask taken at `mask_ms` may be used at `now_ms`. This is the single freshness
// rule: the engine applies it to the mask that is actually on screen.
class MaskFreshness {
public:
    explicit MaskFreshness(MaskFreshnessConfig config = {}) : config_(config) {
        if (config_.stale_after_ms <= 0) {
            throw std::invalid_argument("stale_after_ms must be positive");
        }
        if (config_.future_tolerance_ms < 0) {
            throw std::invalid_argument("future_tolerance_ms must not be negative");
        }
    }

    // `now_ms - mask_ms`, saturating at the int64 limits instead of overflowing.
    [[nodiscard]] static std::int64_t age_between(std::int64_t now_ms, std::int64_t mask_ms) noexcept {
        return saturating_sub(now_ms, mask_ms);
    }

    // True when the mask is neither older than `stale_after_ms` nor stamped in the future by
    // more than `future_tolerance_ms`.
    [[nodiscard]] bool fresh_at(std::int64_t now_ms, std::int64_t mask_ms) const noexcept {
        const std::int64_t age = age_between(now_ms, mask_ms);
        return age <= config_.stale_after_ms && age >= -config_.future_tolerance_ms;
    }

    // Records the newest valid mask. Out-of-order timestamps never make it look older.
    void on_valid_mask(std::int64_t timestamp_ms) noexcept {
        last_valid_ms_ = has_mask_ && timestamp_ms < last_valid_ms_ ? last_valid_ms_ : timestamp_ms;
        has_mask_ = true;
    }

    void reset() noexcept {
        has_mask_ = false;
        last_valid_ms_ = 0;
    }

    [[nodiscard]] bool has_mask() const noexcept { return has_mask_; }

    // False before the first valid mask and whenever the newest one fails `fresh_at`.
    [[nodiscard]] bool is_fresh(std::int64_t now_ms) const noexcept {
        return has_mask_ && fresh_at(now_ms, last_valid_ms_);
    }

    // Age of the newest mask (negative when it is stamped in the future), or -1 before the
    // first one.
    [[nodiscard]] std::int64_t age_ms(std::int64_t now_ms) const noexcept {
        return has_mask_ ? age_between(now_ms, last_valid_ms_) : -1;
    }

private:
    MaskFreshnessConfig config_;
    std::int64_t last_valid_ms_ = 0;
    bool has_mask_ = false;
};

}  // namespace magic_portal
