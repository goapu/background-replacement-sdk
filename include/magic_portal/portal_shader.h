#pragma once

#include <string>
#include <vector>

namespace magic_portal {

// Names of the custom uniforms the generated fragment code reads. Adapters must set them
// through these constants so a rename cannot silently desynchronize shader and host code.
namespace uniforms {
inline constexpr const char *kPersonMask = "mp_person_mask";
inline constexpr const char *kCurrentBackground = "mp_current_background";
inline constexpr const char *kNextBackground = "mp_next_background";
inline constexpr const char *kPersonBounds =
    "mp_person_bounds";                                   // vec4 minx,miny,maxx,maxy (bottom-left origin)
inline constexpr const char *kMaskCrop = "mp_mask_crop";  // vec4 left,top,right,bottom
inline constexpr const char *kStyleA =
    "mp_style_a";  // ring_width, ring_strength, particle_strength, silhouette_glow
inline constexpr const char *kStyleB = "mp_style_b";  // reveal_feather, person_protection, 0, 0
inline constexpr const char *kWarm = "mp_warm";
inline constexpr const char *kCool = "mp_cool";
inline constexpr const char *kProgress = "mp_progress";
inline constexpr const char *kReveal = "mp_reveal";
inline constexpr const char *kRingScale = "mp_ring_scale";
inline constexpr const char *kRingAlpha = "mp_ring_alpha";
inline constexpr const char *kSpinTurns = "mp_spin_turns";
inline constexpr const char *kPersonVisibility = "mp_person_visibility";
inline constexpr const char *kUseCameraOld = "mp_use_camera_old";
inline constexpr const char *kEffectEnable = "mp_effect_enable";  // 0 = bypass, show the camera
}  // namespace uniforms

// All custom uniform names above, for validation.
[[nodiscard]] std::vector<std::string> portal_custom_uniform_names();

// GLSL ES 3.00 fragment-modifier lines (one statement or declaration per element) for a
// full-screen camera-background material. Numeric constants come from portal_constants.h.
// Provided by the host renderer: camera_texture, camera_image_transform, ar_viewport_size,
// and the output variable _output_color.
[[nodiscard]] std::vector<std::string> portal_fragment_shader_lines();

// The same lines joined with newlines (handy for tests and offline validation).
[[nodiscard]] std::string portal_fragment_shader_source();

// A complete GLSL ES 3.00 fragment shader for validation only. It evaluates the effect's noise
// hash (the same generated statements as the effect shader) for the cell floor(gl_FragCoord.xy)
// and writes the 24-bit result into the red, green and blue bytes (most significant first).
// Rendering it on a device and comparing with hash_unit() proves bit-exact integer behaviour.
[[nodiscard]] std::string portal_hash_probe_fragment_source();

}  // namespace magic_portal
