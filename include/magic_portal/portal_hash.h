#pragma once

#include <cstdint>

namespace magic_portal {

// Integer hash behind the dissolve and spark noise.
//
// The effect needs a stable pseudo-random value per screen cell. A sine-based hash such as
// fract(sin(dot(cell, k)) * 43758.5) cannot provide that across devices: the product turns a
// one-step difference in the sine argument (a fused multiply-add, a different sine routine)
// into a completely different value. This hash uses only 32-bit unsigned integer arithmetic,
// which wraps identically on every CPU and in GLSL ES 3.00 (`highp uint`), so the CPU
// reference and every GPU produce the same bits. The shader text is generated from the same
// constants (see portal_shader.cpp), and the shader parity tool checks both sides on a GPU.
namespace hash_constants {
inline constexpr std::uint32_t kMultiplier = 747796405U;
inline constexpr std::uint32_t kIncrement = 2891336453U;
inline constexpr std::uint32_t kMix = 277803737U;
}  // namespace hash_constants

// One round of a PCG output permutation.
[[nodiscard]] constexpr std::uint32_t pcg_round(std::uint32_t input) noexcept {
    const std::uint32_t state = input * hash_constants::kMultiplier + hash_constants::kIncrement;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * hash_constants::kMix;
    return (word >> 22U) ^ word;
}

// Uniform value in [0, 1) with 24 significant bits, so the float conversion is exact.
[[nodiscard]] constexpr float hash_unit(std::uint32_t x, std::uint32_t y) noexcept {
    return static_cast<float>(pcg_round(x ^ pcg_round(y)) >> 8U) * (1.0F / 16777216.0F);
}

// Converts a non-negative floored coordinate to a hash input; negative values (which the
// callers cannot produce except through rounding at a boundary) map to zero.
[[nodiscard]] constexpr std::uint32_t hash_cell(float floored) noexcept {
    return floored <= 0.0F ? 0U : static_cast<std::uint32_t>(floored);
}

}  // namespace magic_portal
