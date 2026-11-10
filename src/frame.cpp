#include "magic_portal/frame.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace magic_portal {
namespace {

std::size_t checked_pixel_count(int width, int height, std::size_t channels) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("frame dimensions must be positive");
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h ||
        w * h > std::numeric_limits<std::size_t>::max() / channels) {
        throw std::overflow_error("frame dimensions overflow addressable memory");
    }
    return w * h * channels;
}

std::size_t rgba_offset(int width, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4U;
}

void require_coordinate(int width, int height, int x, int y) {
    if (x < 0 || x >= width || y < 0 || y >= height) {
        throw std::out_of_range("pixel coordinate is outside the frame");
    }
}

float clamp_unit(float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

std::uint8_t lerp_channel(std::uint8_t a, std::uint8_t b, float t) {
    const float value = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

}  // namespace

RgbaFrame::RgbaFrame(int width, int height)
    : width_(width), height_(height), pixels_(checked_pixel_count(width, height, 4U), 0U) {
    for (std::size_t i = 3; i < pixels_.size(); i += 4U) {
        pixels_[i] = 255U;
    }
}

RgbaFrame::RgbaFrame(int width, int height, std::vector<std::uint8_t> rgba)
    : width_(width), height_(height), pixels_(std::move(rgba)) {
    const std::size_t expected = checked_pixel_count(width, height, 4U);
    if (pixels_.size() != expected) {
        throw std::invalid_argument("RGBA buffer size does not match dimensions");
    }
}

Rgba RgbaFrame::pixel(int x, int y) const {
    require_coordinate(width_, height_, x, y);
    const std::size_t offset = rgba_offset(width_, x, y);
    return {pixels_[offset], pixels_[offset + 1U], pixels_[offset + 2U], pixels_[offset + 3U]};
}

void RgbaFrame::set_pixel(int x, int y, Rgba value) {
    require_coordinate(width_, height_, x, y);
    const std::size_t offset = rgba_offset(width_, x, y);
    pixels_[offset] = value.r;
    pixels_[offset + 1U] = value.g;
    pixels_[offset + 2U] = value.b;
    pixels_[offset + 3U] = value.a;
}

void RgbaFrame::reshape(int width, int height) {
    const std::size_t count = checked_pixel_count(width, height, 4U);
    width_ = width;
    height_ = height;
    pixels_.resize(count);
}

void ConfidenceMask::reshape(int width, int height) {
    const std::size_t count = checked_pixel_count(width, height, 1U);
    width_ = width;
    height_ = height;
    values_.resize(count);
}

void copy_rgba_strided(const std::uint8_t *source, int width, int height, int stride_bytes,
                       bool mirror_horizontally, RgbaFrame *dst) {
    if (source == nullptr || dst == nullptr) {
        throw std::invalid_argument("copy_rgba_strided requires a source and destination");
    }
    const std::size_t row_bytes = checked_pixel_count(width, 1, 4U);
    if (stride_bytes < 0 || static_cast<std::size_t>(stride_bytes) < row_bytes) {
        throw std::invalid_argument("row stride is smaller than one row of pixels");
    }
    dst->reshape(width, height);
    std::uint8_t *out = dst->data();
    for (int y = 0; y < height; ++y) {
        const std::uint8_t *row =
            source + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes);
        std::uint8_t *target = out + static_cast<std::size_t>(y) * row_bytes;
        if (!mirror_horizontally) {
            std::copy(row, row + row_bytes, target);
        } else {
            for (int x = 0; x < width; ++x) {
                const std::uint8_t *from = row + static_cast<std::size_t>(width - 1 - x) * 4U;
                std::uint8_t *to = target + static_cast<std::size_t>(x) * 4U;
                to[0] = from[0];
                to[1] = from[1];
                to[2] = from[2];
                to[3] = from[3];
            }
        }
    }
}

void mirror_horizontal(RgbaFrame *frame) noexcept {
    if (frame == nullptr || frame->empty()) {
        return;
    }
    const int width = frame->width();
    std::uint8_t *base = frame->data();
    for (int y = 0; y < frame->height(); ++y) {
        std::uint8_t *row = base + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4U;
        for (int x = 0; x < width / 2; ++x) {
            std::uint8_t *left = row + static_cast<std::size_t>(x) * 4U;
            std::uint8_t *right = row + static_cast<std::size_t>(width - 1 - x) * 4U;
            for (int c = 0; c < 4; ++c) {
                std::swap(left[c], right[c]);
            }
        }
    }
}

ConfidenceMask::ConfidenceMask(int width, int height)
    : width_(width), height_(height), values_(checked_pixel_count(width, height, 1U), 0.0F) {}

ConfidenceMask::ConfidenceMask(int width, int height, std::vector<float> confidence)
    : width_(width), height_(height), values_(std::move(confidence)) {
    const std::size_t expected = checked_pixel_count(width, height, 1U);
    if (values_.size() != expected) {
        throw std::invalid_argument("mask buffer size does not match dimensions");
    }
    for (float &value : values_) {
        value = std::isfinite(value) ? clamp_unit(value) : 0.0F;
    }
}

float ConfidenceMask::at(int x, int y) const {
    require_coordinate(width_, height_, x, y);
    const std::size_t offset =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    return values_[offset];
}

void ConfidenceMask::set(int x, int y, float value) {
    require_coordinate(width_, height_, x, y);
    const std::size_t offset =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x);
    values_[offset] = std::isfinite(value) ? clamp_unit(value) : 0.0F;
}

Rgba sample_bilinear(const RgbaFrame &frame, float u, float v) {
    if (frame.empty()) {
        throw std::invalid_argument("cannot sample an empty RGBA frame");
    }
    const float x = clamp_unit(u) * static_cast<float>(frame.width() - 1);
    const float y = clamp_unit(v) * static_cast<float>(frame.height() - 1);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, frame.width() - 1);
    const int y1 = std::min(y0 + 1, frame.height() - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    // Coordinates are clamped above, so direct reads are in range.
    const std::uint8_t *base = frame.data();
    const auto read = [base, &frame](int px, int py) {
        const std::uint8_t *p =
            base + (static_cast<std::size_t>(py) * static_cast<std::size_t>(frame.width()) +
                    static_cast<std::size_t>(px)) *
                       4U;
        return Rgba{p[0], p[1], p[2], p[3]};
    };
    const Rgba p00 = read(x0, y0);
    const Rgba p10 = read(x1, y0);
    const Rgba p01 = read(x0, y1);
    const Rgba p11 = read(x1, y1);
    const Rgba top{lerp_channel(p00.r, p10.r, tx), lerp_channel(p00.g, p10.g, tx),
                   lerp_channel(p00.b, p10.b, tx), lerp_channel(p00.a, p10.a, tx)};
    const Rgba bottom{lerp_channel(p01.r, p11.r, tx), lerp_channel(p01.g, p11.g, tx),
                      lerp_channel(p01.b, p11.b, tx), lerp_channel(p01.a, p11.a, tx)};
    return {lerp_channel(top.r, bottom.r, ty), lerp_channel(top.g, bottom.g, ty),
            lerp_channel(top.b, bottom.b, ty), lerp_channel(top.a, bottom.a, ty)};
}

float sample_bilinear(const ConfidenceMask &mask, float u, float v) {
    if (mask.empty()) {
        throw std::invalid_argument("cannot sample an empty confidence mask");
    }
    const float x = clamp_unit(u) * static_cast<float>(mask.width() - 1);
    const float y = clamp_unit(v) * static_cast<float>(mask.height() - 1);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, mask.width() - 1);
    const int y1 = std::min(y0 + 1, mask.height() - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const float *values = mask.data();
    const auto read = [values, &mask](int px, int py) {
        return values[static_cast<std::size_t>(py) * static_cast<std::size_t>(mask.width()) +
                      static_cast<std::size_t>(px)];
    };
    const float top = read(x0, y0) + (read(x1, y0) - read(x0, y0)) * tx;
    const float bottom = read(x0, y1) + (read(x1, y1) - read(x0, y1)) * tx;
    return top + (bottom - top) * ty;
}

}  // namespace magic_portal
