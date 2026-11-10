// Deterministic fixtures and byte-level comparison helpers shared by the
// regression tests. Fixtures use integer arithmetic only so that every
// platform builds identical inputs.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "magic_portal/frame.h"

namespace mp_test {

inline std::uint32_t lcg(std::uint32_t &state) {
    state = state * 1664525U + 1013904223U;
    return state;
}

inline magic_portal::RgbaFrame make_camera(int width, int height, std::uint32_t seed) {
    magic_portal::RgbaFrame frame(width, height);
    std::uint32_t state = seed;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int base = ((x * 5 + y * 3) & 127) + 64;
            const int noise = static_cast<int>(lcg(state) >> 28);
            const bool checker = (((x / 8) + (y / 8)) & 1) != 0;
            frame.set_pixel(x, y,
                            {static_cast<std::uint8_t>(base + noise),
                             static_cast<std::uint8_t>(checker ? 190 : 60 + noise),
                             static_cast<std::uint8_t>(255 - base / 2 + noise), 255});
        }
    }
    return frame;
}

inline magic_portal::RgbaFrame make_background(int width, int height, int variant) {
    magic_portal::RgbaFrame frame(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            frame.set_pixel(x, y,
                            {static_cast<std::uint8_t>(x * 255 / (width - 1)),
                             static_cast<std::uint8_t>(y * 255 / (height - 1)),
                             static_cast<std::uint8_t>(variant != 0 ? 200 : 40), 255});
        }
    }
    return frame;
}

inline magic_portal::ConfidenceMask make_ellipse_mask(int width, int height, float cx, float cy, float rx,
                                                      float ry, std::uint32_t seed) {
    magic_portal::ConfidenceMask mask(width, height);
    std::uint32_t state = seed;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
            const float du = (u - cx) / rx;
            const float dv = (v - cy) / ry;
            const float distance = std::sqrt(du * du + dv * dv);
            const float noise = (static_cast<float>(lcg(state) >> 28) - 8.0F) * 0.005F;
            mask.set(x, y, (1.15F - distance) / 0.3F + noise);
        }
    }
    return mask;
}

inline bool read_file(const std::string &path, std::vector<std::uint8_t> *bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    bytes->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

inline bool write_file(const std::string &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

inline void append_float(std::vector<std::uint8_t> *bytes, float value) {
    const auto *raw = reinterpret_cast<const std::uint8_t *>(&value);
    bytes->insert(bytes->end(), raw, raw + sizeof(float));
}

inline float read_float(const std::vector<std::uint8_t> &bytes, std::size_t index) {
    float value = 0.0F;
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(index * sizeof(float)),
              bytes.begin() + static_cast<std::ptrdiff_t>((index + 1U) * sizeof(float)),
              reinterpret_cast<std::uint8_t *>(&value));
    return value;
}

}  // namespace mp_test
