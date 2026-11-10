#include "magic_portal/portal_shader.h"

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "magic_portal/portal_constants.h"
#include "magic_portal/portal_hash.h"

namespace magic_portal {
namespace {

// Tokens of the form @kName@ are replaced from the constants table.
const char *const kTemplate = R"GLSL(
uniform sampler2D camera_texture;
uniform sampler2D mp_person_mask;
uniform sampler2D mp_current_background;
uniform sampler2D mp_next_background;
uniform highp mat4 camera_image_transform;
uniform highp vec3 ar_viewport_size;
uniform highp vec4 mp_person_bounds;
uniform highp vec4 mp_mask_crop;
uniform highp vec4 mp_style_a;
uniform highp vec4 mp_style_b;
uniform highp vec4 mp_warm;
uniform highp vec4 mp_cool;
uniform highp float mp_progress;
uniform highp float mp_reveal;
uniform highp float mp_ring_scale;
uniform highp float mp_ring_alpha;
uniform highp float mp_spin_turns;
uniform highp float mp_person_visibility;
uniform highp float mp_use_camera_old;
uniform highp float mp_effect_enable;
highp vec2 mpScreenUV = gl_FragCoord.xy / ar_viewport_size.xy;
highp vec2 mpCameraUV = (camera_image_transform * vec4(mpScreenUV, 0.0, 1.0)).xy;
highp vec4 mpCamera = texture(camera_texture, mpCameraUV);
highp vec2 mpMaskUV = vec2(mix(mp_mask_crop.x, mp_mask_crop.z, mpScreenUV.x), mix(mp_mask_crop.w, mp_mask_crop.y, mpScreenUV.y));
highp float mpPerson = clamp(texture(mp_person_mask, mpMaskUV).r, 0.0, 1.0);
highp vec2 mpDissolveCell = floor(mpScreenUV * ar_viewport_size.xy / @kDissolveCellPixels@);
@DISSOLVE_HASH@
highp float mpDissolveNoise = @kNoiseHashWeight@ * mpDissolveHash
    + @kNoiseWaveWeight@ * (0.5 + 0.5 * sin(mpScreenUV.x * @kNoiseWaveAX@ + mpScreenUV.y * @kNoiseWaveAY@))
    + @kNoiseWaveWeight@ * (0.5 + 0.5 * sin(mpScreenUV.x * @kNoiseWaveBX@ - mpScreenUV.y * @kNoiseWaveBY@));
highp float mpPersonGate = mp_person_visibility <= @kVisibilityEpsLow@ ? 0.0 : (mp_person_visibility >= @kVisibilityEpsHigh@ ? 1.0 : smoothstep(1.0 - mp_person_visibility - @kDissolveBand@, 1.0 - mp_person_visibility + @kDissolveBand@, mpDissolveNoise));
highp float mpVisiblePerson = mpPerson * mpPersonGate;
if (mp_effect_enable < 0.5) {
    _output_color = mpCamera;
} else if (mp_person_visibility >= @kVisibilityEpsHigh@ && mpPerson >= mp_style_b.y) {
    _output_color = mpCamera;
} else {
    highp float mpFeather = mp_style_b.x;
    highp float mpViewportAspect = ar_viewport_size.x / ar_viewport_size.y;
    highp vec2 mpOldSize = vec2(textureSize(mp_current_background, 0));
    highp vec2 mpNextSize = vec2(textureSize(mp_next_background, 0));
    highp float mpOldAspect = mpOldSize.x / max(mpOldSize.y, 1.0);
    highp float mpNextAspect = mpNextSize.x / max(mpNextSize.y, 1.0);
    highp vec2 mpOldUV = mpScreenUV;
    highp vec2 mpNextUV = mpScreenUV;
    if (mpOldAspect > mpViewportAspect) mpOldUV.x = 0.5 + (mpOldUV.x - 0.5) * mpViewportAspect / mpOldAspect;
    else mpOldUV.y = 0.5 + (mpOldUV.y - 0.5) * mpOldAspect / mpViewportAspect;
    if (mpNextAspect > mpViewportAspect) mpNextUV.x = 0.5 + (mpNextUV.x - 0.5) * mpViewportAspect / mpNextAspect;
    else mpNextUV.y = 0.5 + (mpNextUV.y - 0.5) * mpNextAspect / mpViewportAspect;
    highp vec4 mpOldTexture = texture(mp_current_background, mpOldUV);
    highp vec4 mpOld = mix(mpOldTexture, mpCamera, step(0.5, mp_use_camera_old));
    highp vec4 mpNext = texture(mp_next_background, mpNextUV);
    highp vec2 mpCenter = vec2(0.5 * (mp_person_bounds.x + mp_person_bounds.z), 0.5 * (mp_person_bounds.y + mp_person_bounds.w));
    highp vec2 mpExtent = max(mp_person_bounds.zw - mp_person_bounds.xy, vec2(@kMinBodyWidth@, @kMinBodyHeight@));
    highp float mpAspect = mpViewportAspect;
    highp vec2 mpDelta = mpScreenUV - mpCenter;
    mpDelta.x *= mpAspect;
    highp float mpDistance = length(mpDelta);
    highp float mpBodyRadius = clamp(@kBodyRadiusScale@ * length(vec2(mpExtent.x * mpAspect, mpExtent.y)), @kBodyRadiusMin@, @kBodyRadiusMax@);
    highp vec2 mpFar = max(mpCenter, 1.0 - mpCenter);
    mpFar.x *= mpAspect;
    highp float mpMaxRadius = length(mpFar) + 2.0 * mpFeather;
    highp float mpRevealRadius = mix(@kRevealStartRadiusScale@ * mpBodyRadius, mpMaxRadius, mp_reveal);
    highp float mpBackgroundMix = (1.0 - smoothstep(mpRevealRadius - mpFeather, mpRevealRadius + mpFeather, mpDistance)) * smoothstep(0.0, @kRevealGateEnd@, mp_reveal);
    mpBackgroundMix = max(mpBackgroundMix, mpPerson * (1.0 - mpPersonGate));
    highp vec4 mpBackground = mix(mpOld, mpNext, mpBackgroundMix);
    highp vec4 mpComposed = mix(mpBackground, mpCamera, mpVisiblePerson);
    if (mp_ring_alpha > @kRingAlphaEpsilon@) {
        highp float mpAngle = atan(mpDelta.y, mpDelta.x);
        highp float mpRingRadius = mpBodyRadius * mix(@kRingStartScale@, 1.0, mp_ring_scale);
        highp float mpSpin = mp_spin_turns * 6.28318530718;
        highp float mpW = mp_style_a.x;
        highp float mpWobble = mpW * (@kWobbleAmpA@ * sin(mpAngle * @kWobbleFreqA@ - mpSpin * @kWobbleSpinA@) + @kWobbleAmpB@ * sin(mpAngle * @kWobbleFreqB@ + mpSpin * @kWobbleSpinB@));
        highp float mpRingDelta = mpDistance - (mpRingRadius + mpWobble);
        highp float mpBand = exp(-(mpRingDelta * mpRingDelta) / (2.0 * mpW * mpW));
        highp float mpArc = @kArcBase@ + @kArcAmp@ * pow(0.5 + 0.5 * sin(mpAngle * @kArcFreq@ - mpSpin * @kArcSpin@), @kArcPower@);
        highp float mpCell = max(floor((mpAngle + 3.14159265359) * @kSparkCells@), 0.0);
        highp float mpSparkStep = max(floor(mp_progress * @kSparkTimeSteps@), 0.0);
@SPARK_HASH@
        highp float mpSparkOffset = (mpSparkNoise - 0.5) * mpW * @kSparkOffsetScale@;
        highp float mpSparkDistance = abs(mpRingDelta - mpSparkOffset);
        highp float mpSpark = step(@kSparkThreshold@, mpSparkNoise) * exp(-(mpSparkDistance * mpSparkDistance) / (mpW * mpW * @kSparkWidthFactor@));
        highp vec2 mpMaskTexel = 1.0 / vec2(textureSize(mp_person_mask, 0));
        highp float mpLeft = texture(mp_person_mask, mpMaskUV - vec2(mpMaskTexel.x, 0.0)).r;
        highp float mpRight = texture(mp_person_mask, mpMaskUV + vec2(mpMaskTexel.x, 0.0)).r;
        highp float mpDown = texture(mp_person_mask, mpMaskUV - vec2(0.0, mpMaskTexel.y)).r;
        highp float mpUp = texture(mp_person_mask, mpMaskUV + vec2(0.0, mpMaskTexel.y)).r;
        highp float mpEdge = clamp((abs(mpRight - mpLeft) + abs(mpUp - mpDown)) * @kSilhouetteGain@, 0.0, 1.0);
        highp float mpGuard = 1.0 - smoothstep(@kPersonGuardStart@, mp_style_b.y, mpVisiblePerson);
        highp float mpDissolveDelta = mpDissolveNoise - (1.0 - mp_person_visibility);
        highp float mpDissolveEdge = step(@kVisibilityEpsLow@, mp_person_visibility) * (1.0 - step(@kVisibilityEpsHigh@, mp_person_visibility)) * mpPerson * exp(-(mpDissolveDelta * mpDissolveDelta) / @kDissolveEdgeVariance@);
        highp float mpEnergy = mp_ring_alpha * mpGuard * clamp(mpBand * mpArc * mp_style_a.y + mpSpark * mp_style_a.z + mpEdge * mp_style_a.w + mpDissolveEdge * @kDissolveEdgeGain@, 0.0, 1.0);
        highp vec3 mpEnergyColor = mix(mp_warm.rgb, mp_cool.rgb, smoothstep(mpCenter.x - @kWarmCoolHalfWidth@, mpCenter.x + @kWarmCoolHalfWidth@, mpScreenUV.x));
        mpComposed.rgb = 1.0 - (1.0 - mpComposed.rgb) * (1.0 - mpEnergyColor * mpEnergy);
    }
    _output_color = vec4(mpComposed.rgb, 1.0);
}
)GLSL";

std::string glsl_uint(std::uint32_t value) {
    return std::to_string(value) + "u";
}

// One PCG round written out as statements on `var` (modifier bodies cannot define functions).
std::string pcg_round_statements(const std::string &var, const std::string &indent) {
    namespace hc = hash_constants;
    std::string out;
    out += indent + var + " = " + var + " * " + glsl_uint(hc::kMultiplier) + " + " +
           glsl_uint(hc::kIncrement) + ";\n";
    out += indent + var + " = ((" + var + " >> ((" + var + " >> 28u) + 4u)) ^ " + var + ") * " +
           glsl_uint(hc::kMix) + ";\n";
    out += indent + var + " = (" + var + " >> 22u) ^ " + var + ";\n";
    return out;
}

// Statements that declare `name` as hash_unit(x, y) from portal_hash.h.
std::string hash_statements(const std::string &name, const std::string &x_expr, const std::string &y_expr,
                            const std::string &indent) {
    const std::string x = name + "X";
    const std::string y = name + "Y";
    std::string out;
    out += indent + "highp uint " + y + " = uint(" + y_expr + ");\n";
    out += pcg_round_statements(y, indent);
    out += indent + "highp uint " + x + " = uint(" + x_expr + ") ^ " + y + ";\n";
    out += pcg_round_statements(x, indent);
    out += indent + "highp float " + name + " = float(" + x + " >> 8u) * (1.0 / 16777216.0);";
    return out;
}

std::string format_glsl_float(float value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    std::string text = buffer;
    if (text.find_first_of(".eEnN") == std::string::npos) {
        text += ".0";  // GLSL requires a decimal point or exponent on float literals
    }
    return text;
}

void replace_all(std::string *text, const std::string &token, const std::string &value) {
    std::size_t at = 0;
    while ((at = text->find(token, at)) != std::string::npos) {
        text->replace(at, token.size(), value);
        at += value.size();
    }
}

}  // namespace

std::vector<std::string> portal_custom_uniform_names() {
    return {"mp_person_mask",
            "mp_current_background",
            "mp_next_background",
            "mp_person_bounds",
            "mp_mask_crop",
            "mp_style_a",
            "mp_style_b",
            "mp_warm",
            "mp_cool",
            "mp_progress",
            "mp_reveal",
            "mp_ring_scale",
            "mp_ring_alpha",
            "mp_spin_turns",
            "mp_person_visibility",
            "mp_use_camera_old",
            "mp_effect_enable"};
}

std::string portal_fragment_shader_source() {
    std::string text = kTemplate;
    replace_all(&text, "@DISSOLVE_HASH@",
                hash_statements("mpDissolveHash", "mpDissolveCell.x", "mpDissolveCell.y", ""));
    replace_all(&text, "@SPARK_HASH@", hash_statements("mpSparkNoise", "mpCell", "mpSparkStep", "        "));
#define MP_SUBSTITUTE(name, value) replace_all(&text, "@" #name "@", format_glsl_float(value));
    MP_PORTAL_CONSTANTS(MP_SUBSTITUTE)
#undef MP_SUBSTITUTE
    if (text.find('@') != std::string::npos) {
        throw std::logic_error("portal shader template references an unknown constant");
    }
    return text;
}

std::string portal_hash_probe_fragment_source() {
    std::string text =
        "#version 300 es\nprecision highp float;\nprecision highp int;\nout vec4 mpProbeColor;\nvoid main() "
        "{\n";
    text += "highp vec2 mpDissolveCell = floor(gl_FragCoord.xy);\n";
    text += hash_statements("mpProbeHash", "mpDissolveCell.x", "mpDissolveCell.y", "") + "\n";
    text += "highp uint mpProbeBits = mpProbeHashX >> 8u;\n";
    text +=
        "mpProbeColor = vec4(float((mpProbeBits >> 16u) & 255u), float((mpProbeBits >> 8u) & 255u), "
        "float(mpProbeBits & 255u), 255.0) / 255.0;\n}\n";
    return text;
}

std::vector<std::string> portal_fragment_shader_lines() {
    std::istringstream stream(portal_fragment_shader_source());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

}  // namespace magic_portal
