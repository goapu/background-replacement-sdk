#pragma once

// Single source of truth for every numeric design constant of the portal effect.
// The CPU reference compositor uses these directly, and the GPU shader text is generated
// from the same table (see portal_shader.h), so the two paths cannot drift apart.

#define MP_PORTAL_CONSTANTS(X)        \
    X(kMinBodyWidth, 0.18F)           \
    X(kMinBodyHeight, 0.30F)          \
    X(kFallbackBodyWidth, 0.42F)      \
    X(kFallbackBodyHeight, 0.72F)     \
    X(kBodyRadiusScale, 0.54F)        \
    X(kBodyRadiusMin, 0.20F)          \
    X(kBodyRadiusMax, 0.82F)          \
    X(kRingStartScale, 0.32F)         \
    X(kRingAlphaEpsilon, 0.0001F)     \
    X(kRevealStartRadiusScale, 0.10F) \
    X(kRevealGateEnd, 0.035F)         \
    X(kDissolveCellPixels, 3.0F)      \
    X(kNoiseHashWeight, 0.68F)        \
    X(kNoiseWaveWeight, 0.16F)        \
    X(kNoiseWaveAX, 91.0F)            \
    X(kNoiseWaveAY, 37.0F)            \
    X(kNoiseWaveBX, 29.0F)            \
    X(kNoiseWaveBY, 73.0F)            \
    X(kVisibilityEpsLow, 0.0001F)     \
    X(kVisibilityEpsHigh, 0.9999F)    \
    X(kDissolveBand, 0.055F)          \
    X(kDissolveEdgeVariance, 0.0032F) \
    X(kDissolveEdgeGain, 0.88F)       \
    X(kSilhouetteGain, 1.35F)         \
    X(kPersonGuardStart, 0.72F)       \
    X(kWarmCoolHalfWidth, 0.18F)      \
    X(kWobbleAmpA, 0.72F)             \
    X(kWobbleFreqA, 7.0F)             \
    X(kWobbleSpinA, 1.7F)             \
    X(kWobbleAmpB, 0.38F)             \
    X(kWobbleFreqB, 13.0F)            \
    X(kWobbleSpinB, 2.3F)             \
    X(kArcBase, 0.58F)                \
    X(kArcAmp, 0.42F)                 \
    X(kArcFreq, 11.0F)                \
    X(kArcSpin, 3.2F)                 \
    X(kArcPower, 3.0F)                \
    X(kSparkCells, 23.0F)             \
    X(kSparkTimeSteps, 90.0F)         \
    X(kSparkThreshold, 0.78F)         \
    X(kSparkOffsetScale, 9.0F)        \
    X(kSparkWidthFactor, 0.28F)

namespace magic_portal::constants {

#define MP_DEFINE_CONSTANT(name, value) inline constexpr float name = value;
MP_PORTAL_CONSTANTS(MP_DEFINE_CONSTANT)
#undef MP_DEFINE_CONSTANT

inline constexpr float kPi = 3.14159265358979323846F;

}  // namespace magic_portal::constants
