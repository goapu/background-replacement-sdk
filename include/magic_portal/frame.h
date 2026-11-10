#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace magic_portal {

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

class RgbaFrame {
public:
    RgbaFrame() = default;
    RgbaFrame(int width, int height);
    RgbaFrame(int width, int height, std::vector<std::uint8_t> rgba);

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] bool empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] std::size_t byte_size() const noexcept { return pixels_.size(); }

    [[nodiscard]] const std::uint8_t *data() const noexcept { return pixels_.data(); }
    [[nodiscard]] std::uint8_t *data() noexcept { return pixels_.data(); }
    [[nodiscard]] const std::vector<std::uint8_t> &pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::vector<std::uint8_t> &pixels() noexcept { return pixels_; }

    [[nodiscard]] Rgba pixel(int x, int y) const;
    void set_pixel(int x, int y, Rgba value);

    // Changes the dimensions while reusing the existing allocation when it is large
    // enough. Pixel contents are unspecified afterwards; the caller overwrites them.
    void reshape(int width, int height);

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

class ConfidenceMask {
public:
    ConfidenceMask() = default;
    ConfidenceMask(int width, int height);
    ConfidenceMask(int width, int height, std::vector<float> confidence);

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

    [[nodiscard]] const float *data() const noexcept { return values_.data(); }
    [[nodiscard]] float *data() noexcept { return values_.data(); }
    [[nodiscard]] const std::vector<float> &values() const noexcept { return values_; }
    [[nodiscard]] std::vector<float> &values() noexcept { return values_; }

    [[nodiscard]] float at(int x, int y) const;
    void set(int x, int y, float value);

    // Changes the dimensions while reusing the existing allocation when it is large
    // enough. Values are unspecified afterwards; the caller overwrites them.
    void reshape(int width, int height);

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<float> values_;
};

struct PersonBounds {
    float min_x = 0.0F;
    float min_y = 0.0F;
    float max_x = 1.0F;
    float max_y = 1.0F;
    bool valid = false;

    [[nodiscard]] float center_x() const noexcept { return 0.5F * (min_x + max_x); }
    [[nodiscard]] float center_y() const noexcept { return 0.5F * (min_y + max_y); }
    [[nodiscard]] float width() const noexcept { return max_x - min_x; }
    [[nodiscard]] float height() const noexcept { return max_y - min_y; }
};

// Copies a tightly or loosely packed RGBA8 image (row stride in bytes) into `dst`,
// optionally mirroring each row. `dst` is reshaped to width x height.
void copy_rgba_strided(const std::uint8_t *source, int width, int height, int stride_bytes,
                       bool mirror_horizontally, RgbaFrame *dst);

// Mirrors the frame left-to-right in place.
void mirror_horizontal(RgbaFrame *frame) noexcept;

[[nodiscard]] Rgba sample_bilinear(const RgbaFrame &frame, float u, float v);
[[nodiscard]] float sample_bilinear(const ConfidenceMask &mask, float u, float v);

}  // namespace magic_portal
