#pragma once

#include <cstdint>
#include <limits>

namespace magic_portal {

// Largest inputs the SDK accepts. Anything bigger is a caller mistake (or hostile input) and
// is refused before memory is allocated for it.
inline constexpr int kMaxFrameDimension = 16384;             // pixels per side
inline constexpr std::int64_t kMaxFramePixels = 64'000'000;  // 8000 x 8000
inline constexpr int kMaxMaskDimension = 4096;               // pixels per side
inline constexpr std::int64_t kMaxMaskPixels = 16LL * 1024 * 1024;

// `a - b` clamped to the int64 range instead of overflowing, for time arithmetic on values
// that come from callers.
[[nodiscard]] constexpr std::int64_t saturating_sub(std::int64_t a, std::int64_t b) noexcept {
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (b < 0 && a > kMax + b) {
        return kMax;
    }
    if (b > 0 && a < kMin + b) {
        return kMin;
    }
    return a - b;
}

// Dimensions are given as 64-bit values so callers can pass untrusted ints without any
// intermediate arithmetic in a narrower type.
[[nodiscard]] constexpr bool frame_size_ok(std::int64_t width, std::int64_t height) noexcept {
    return width > 0 && height > 0 && width <= kMaxFrameDimension && height <= kMaxFrameDimension &&
           width * height <= kMaxFramePixels;
}

[[nodiscard]] constexpr bool mask_size_ok(std::int64_t width, std::int64_t height) noexcept {
    return width > 0 && height > 0 && width <= kMaxMaskDimension && height <= kMaxMaskDimension &&
           width * height <= kMaxMaskPixels;
}

// Row stride of an RGBA8 frame. 0 means tightly packed. Returns false when the stride is
// negative, smaller than one row, or too large to address.
[[nodiscard]] constexpr bool resolve_rgba_stride(std::int64_t width, std::int64_t stride_bytes,
                                                 std::int64_t *resolved) noexcept {
    if (width <= 0 || width > kMaxFrameDimension || stride_bytes < 0) {
        return false;
    }
    const std::int64_t row = width * 4;
    const std::int64_t stride = stride_bytes == 0 ? row : stride_bytes;
    if (stride < row || stride > 0x7fffffffLL) {
        return false;
    }
    *resolved = stride;
    return true;
}

}  // namespace magic_portal
