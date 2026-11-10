// Structural checks on the generated GLSL. These do not replace running it on a GPU;
// they stop the generator and the host-side uniform names from drifting apart.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <set>
#include <string>

#include "magic_portal/portal_constants.h"
#include "magic_portal/portal_hash.h"
#include "magic_portal/portal_shader.h"
#include "test_harness.h"

using namespace magic_portal;

namespace {

bool contains(const std::string &text, const std::string &needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

MT_TEST("shader: no unresolved template tokens") {
    const std::string source = portal_fragment_shader_source();
    MT_CHECK(!source.empty());
    MT_CHECK(!contains(source, "@"));
}

MT_TEST("shader: braces and parentheses are balanced") {
    const std::string source = portal_fragment_shader_source();
    int braces = 0;
    int parens = 0;
    bool never_negative = true;
    for (char c : source) {
        braces += c == '{' ? 1 : c == '}' ? -1 : 0;
        parens += c == '(' ? 1 : c == ')' ? -1 : 0;
        never_negative = never_negative && braces >= 0 && parens >= 0;
    }
    MT_CHECK(braces == 0);
    MT_CHECK(parens == 0);
    MT_CHECK(never_negative);
}

MT_TEST("shader: every custom uniform is declared exactly once and used") {
    const std::string source = portal_fragment_shader_source();
    for (const std::string &name : portal_custom_uniform_names()) {
        std::size_t declarations = 0;
        std::size_t position = 0;
        const std::regex declaration("uniform\\s+(highp\\s+)?\\w+\\s+" + name + "\\s*;");
        for (auto it = std::sregex_iterator(source.begin(), source.end(), declaration);
             it != std::sregex_iterator(); ++it) {
            ++declarations;
        }
        (void)position;
        MT_CHECK(declarations == 1);
        // Used somewhere other than its declaration.
        std::size_t hits = 0;
        for (std::size_t at = source.find(name); at != std::string::npos;
             at = source.find(name, at + name.size())) {
            ++hits;
        }
        MT_CHECK(hits >= 2);
    }
}

MT_TEST("shader: host uniform constants match the generated declarations") {
    const std::set<std::string> expected = {uniforms::kPersonMask,
                                            uniforms::kCurrentBackground,
                                            uniforms::kNextBackground,
                                            uniforms::kPersonBounds,
                                            uniforms::kMaskCrop,
                                            uniforms::kStyleA,
                                            uniforms::kStyleB,
                                            uniforms::kWarm,
                                            uniforms::kCool,
                                            uniforms::kProgress,
                                            uniforms::kReveal,
                                            uniforms::kRingScale,
                                            uniforms::kRingAlpha,
                                            uniforms::kSpinTurns,
                                            uniforms::kPersonVisibility,
                                            uniforms::kUseCameraOld,
                                            uniforms::kEffectEnable};
    const auto names = portal_custom_uniform_names();
    const std::set<std::string> actual(names.begin(), names.end());
    MT_CHECK(actual == expected);
    MT_CHECK(names.size() == expected.size());
}

MT_TEST("shader: max radius is computed, not the legacy hard-coded literal") {
    const std::string source = portal_fragment_shader_source();
    MT_CHECK(contains(source, "mpMaxRadius"));
    MT_CHECK(!contains(source, "1.55"));
    MT_CHECK(contains(source, "mp_effect_enable"));
}

MT_TEST("shader: embeds the CPU constants exactly (float round trip)") {
    const std::string source = portal_fragment_shader_source();
    const float values[] = {constants::kNoiseHashWeight, constants::kDissolveCellPixels,
                            constants::kDissolveBand, constants::kSilhouetteGain};
    for (float value : values) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
        MT_CHECK(contains(source, text));
        MT_CHECK(std::strtof(text, nullptr) == value);  // the literal denotes the same float
    }
}

MT_TEST("shader: lines never define functions (modifier bodies are statements only)") {
    for (const std::string &line : portal_fragment_shader_lines()) {
        const bool function_like = std::regex_search(
            line, std::regex("^\\s*(void|float|vec[234]|highp\\s+\\w+)\\s+\\w+\\s*\\([^;]*\\)\\s*\\{"));
        MT_CHECK(!function_like);
    }
}

MT_TEST("shader: generation is deterministic") {
    MT_CHECK(portal_fragment_shader_source() == portal_fragment_shader_source());
}

MT_TEST("shader: the noise hash is the shared integer hash, not a sine hash") {
    const std::string source = portal_fragment_shader_source();
    const std::string multiplier = std::to_string(hash_constants::kMultiplier) + "u";
    const std::string increment = std::to_string(hash_constants::kIncrement) + "u";
    const std::string mix = std::to_string(hash_constants::kMix) + "u";
    for (const std::string &needle : {multiplier, increment, mix}) {
        std::size_t hits = 0;
        for (std::size_t at = source.find(needle); at != std::string::npos;
             at = source.find(needle, at + 1)) {
            ++hits;
        }
        // Two hash evaluations (dissolve, spark), two rounds each.
        MT_CHECK(hits == 4);
    }
    MT_CHECK(!contains(source, "43758"));
    MT_CHECK(!contains(source, "fract(sin("));
    MT_CHECK(contains(source, "highp uint"));
    MT_CHECK(contains(source, "1.0 / 16777216.0"));
}

MT_TEST("shader: noise cells are clamped to non-negative before the unsigned conversion") {
    const std::string source = portal_fragment_shader_source();
    MT_CHECK(contains(source, "mpCell = max(floor("));
    MT_CHECK(contains(source, "mpSparkStep = max(floor("));
}

MT_TEST("shader: the hash probe uses the same statements as the effect shader") {
    const std::string probe = portal_hash_probe_fragment_source();
    MT_CHECK(contains(probe, "#version 300 es"));
    MT_CHECK(!contains(probe, "@"));
    MT_CHECK(contains(probe, std::to_string(hash_constants::kMultiplier) + "u"));
    MT_CHECK(contains(probe, std::to_string(hash_constants::kMix) + "u"));
    MT_CHECK(!contains(probe, "sin("));
    MT_CHECK(probe == portal_hash_probe_fragment_source());
}
