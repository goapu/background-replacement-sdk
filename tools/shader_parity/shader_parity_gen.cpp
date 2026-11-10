// Writes the fixtures for the shader parity check (see README.md in this directory).
//
// Pipeline for every scenario:
//   Engine -> RenderState -> Viro adapter (a recording material captures the uniforms it sets)
//     -> uniforms for the generated GLSL ES 3.00 shader
//   Engine -> RenderState -> PortalCompositor -> CPU reference image
// The page in this directory then runs the shader in a WebGL2 browser and compares the two.
#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "VRODriver.h"
#include "VROMaterial.h"
#include "VROTexture.h"
#include "ViroMagicPortalMaterial.h"
#include "golden_support.h"
#include "magic_portal/engine.h"
#include "magic_portal/magic_portal.h"
#include "magic_portal/portal_hash.h"
#include "magic_portal/portal_shader.h"

// The adapter checks this string for the float-texture filtering extension.
const char *g_stub_gl_extensions = "GL_OES_texture_float_linear";

using namespace magic_portal;

namespace {

constexpr int kWidth = 192;
constexpr int kHeight = 256;
constexpr int kProbeWidth = 64;
constexpr int kProbeHeight = 86;

std::string base64(const std::uint8_t *data, std::size_t size) {
    static const char *const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2U) / 3U * 4U);
    for (std::size_t i = 0; i < size; i += 3U) {
        std::uint32_t value = static_cast<std::uint32_t>(data[i]) << 16U;
        if (i + 1U < size) {
            value |= static_cast<std::uint32_t>(data[i + 1U]) << 8U;
        }
        if (i + 2U < size) {
            value |= data[i + 2U];
        }
        out += kAlphabet[(value >> 18U) & 63U];
        out += kAlphabet[(value >> 12U) & 63U];
        out += (i + 1U < size) ? kAlphabet[(value >> 6U) & 63U] : '=';
        out += (i + 2U < size) ? kAlphabet[value & 63U] : '=';
    }
    return out;
}

std::string json_string(const std::string &text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string number(float value) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return buffer;
}

std::string vec4_json(const VROVector4f &v) {
    return "[" + number(v.x) + "," + number(v.y) + "," + number(v.z) + "," + number(v.w) + "]";
}

std::shared_ptr<VROTexture> placeholder_texture() {
    return std::make_shared<VROTexture>(
        VROTextureType::Texture2D, VROTextureFormat::R32F, VROTextureInternalFormat::R32F, false,
        VROMipmapMode::None, std::vector<std::shared_ptr<VROData>>{}, 4, 4, std::vector<std::uint32_t>{});
}

const char *phase_name(PortalPhase phase) {
    switch (phase) {
        case PortalPhase::kLive:
            return "live";
        case PortalPhase::kTransitioning:
            return "transitioning";
        case PortalPhase::kReplaced:
            return "replaced";
    }
    return "unknown";
}

std::string b64_floats(const float *data, std::size_t count) {
    return base64(reinterpret_cast<const std::uint8_t *>(data), count * sizeof(float));
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: magic_portal_shader_parity_gen <output fixtures.json>\n";
        return 2;
    }

    const RgbaFrame camera = mp_test::make_camera(kWidth, kHeight, 3U);
    const RgbaFrame background1 = mp_test::make_background(320, 200, 1);  // wider than the viewport
    const RgbaFrame background2 = mp_test::make_background(160, 280, 2);  // narrower than the viewport
    const ConfidenceMask raw_mask = mp_test::make_ellipse_mask(64, 80, 0.42F, 0.52F, 0.22F, 0.40F, 11U);
    const ConfidenceMask nobody(64, 80);  // an empty scene: the model finds no one
    struct Crop {
        float left, top, right, bottom;
    };
    const Crop cropped_view{0.12F, 0.06F, 0.88F, 0.82F};  // the camera image is larger than the view

    Engine engine{EngineConfig{}};
    auto material = std::make_shared<VROMaterial>();
    auto driver = std::make_shared<VRODriver>();
    auto portal = viro::ViroMagicPortalMaterial::create(material, driver);
    portal->set_background(1, placeholder_texture());
    portal->set_background(2, placeholder_texture());
    const PortalCompositor compositor(engine.style());
    const RgbaFrame *backgrounds[3] = {nullptr, &background1, &background2};

    std::ostringstream scenarios;
    int count = 0;

    const auto frame_input = [&](std::int64_t t, const Crop &crop = Crop{0.0F, 0.0F, 1.0F, 1.0F}) {
        FrameInput in;
        in.rgba = camera.data();
        in.width = kWidth;
        in.height = kHeight;
        in.stride_bytes = kWidth * 4;
        in.timestamp_ms = t;
        in.crop_left = crop.left;
        in.crop_top = crop.top;
        in.crop_right = crop.right;
        in.crop_bottom = crop.bottom;
        return in;
    };

    // `mask` null: do not process a new frame (the previous mask is left to age).
    const auto emit = [&](const std::string &name, std::int64_t t, const ConfidenceMask *mask,
                          const Crop &crop = Crop{0.0F, 0.0F, 1.0F, 1.0F}) {
        if (mask != nullptr) {
            const Status status = engine.process_frame(frame_input(t, crop), mask);
            if (status != Status::kOk && status != Status::kNoPerson) {
                std::cerr << "process_frame failed at " << t << "\n";
                std::exit(1);
            }
        }
        const RenderState state = engine.render_state(t);
        portal->apply(state);  // the real adapter code decides which uniforms the shader sees
        if (state.mask == nullptr || state.mask->empty()) {
            std::cerr << "no mask for scenario " << name << "\n";
            std::exit(1);
        }

        RgbaFrame reference = camera;
        if (state.effect_enable) {
            RefinedPersonMask person;
            person.alpha = *state.mask;
            person.bounds.valid = state.has_person;
            person.bounds.min_x = state.person_bounds[0];
            person.bounds.max_x = state.person_bounds[2];
            person.bounds.min_y = 1.0F - state.person_bounds[3];  // bottom-left -> top-left origin
            person.bounds.max_y = 1.0F - state.person_bounds[1];
            CompositeInput in;
            in.camera = &camera;
            in.current_background = state.camera_is_current ? nullptr : backgrounds[state.current_background];
            in.next_background = backgrounds[state.next_background];
            in.person = &person;
            in.transition = state.visual;
            std::copy(std::begin(state.mask_crop), std::end(state.mask_crop), std::begin(in.mask_crop));
            reference = compositor.compose(in);
        }

        std::ostringstream s;
        s << (count == 0 ? "" : ",\n") << "{\"name\":" << json_string(name) << ",\"t\":" << t
          << ",\"phase\":" << json_string(phase_name(state.phase))
          << ",\"effect_enable\":" << (state.effect_enable ? "true" : "false") << ",\"uniforms\":{"
          << "\"effect_enable\":" << number(material->get_float("mp_effect_enable"))
          << ",\"progress\":" << number(material->get_float("mp_progress"))
          << ",\"reveal\":" << number(material->get_float("mp_reveal"))
          << ",\"ring_scale\":" << number(material->get_float("mp_ring_scale"))
          << ",\"ring_alpha\":" << number(material->get_float("mp_ring_alpha"))
          << ",\"spin_turns\":" << number(material->get_float("mp_spin_turns"))
          << ",\"person_visibility\":" << number(material->get_float("mp_person_visibility"))
          << ",\"use_camera_old\":" << number(material->get_float("mp_use_camera_old"))
          << ",\"person_bounds\":" << vec4_json(material->get_vec4("mp_person_bounds"))
          << ",\"mask_crop\":" << vec4_json(material->get_vec4("mp_mask_crop"))
          << ",\"style_a\":" << vec4_json(material->get_vec4("mp_style_a"))
          << ",\"style_b\":" << vec4_json(material->get_vec4("mp_style_b"))
          << ",\"warm\":" << vec4_json(material->get_vec4("mp_warm"))
          << ",\"cool\":" << vec4_json(material->get_vec4("mp_cool")) << "}"
          << ",\"current_bg\":" << (state.camera_is_current ? 0 : state.current_background)
          << ",\"next_bg\":" << state.next_background << ",\"mask_w\":" << state.mask->width()
          << ",\"mask_h\":" << state.mask->height() << ",\"mask\":\""
          << b64_floats(state.mask->data(), state.mask->size()) << "\",\"cpu\":\""
          << base64(reference.data(), reference.byte_size()) << "\"}";
        scenarios << s.str();
        ++count;
    };

    const std::int64_t t0 = 1000;
    emit("live camera (no transition)", t0, &raw_mask);
    if (engine.begin_transition(t0, 1) != Status::kOk) {
        std::cerr << "begin_transition refused\n";
        return 1;
    }
    for (const std::int64_t dt : {0, 200, 400, 650, 900, 1150, 1400, 1650, 1790}) {
        emit("camera -> bg1 @" + std::to_string(dt) + "ms", t0 + dt, &raw_mask);
    }
    emit("camera -> bg1 complete", t0 + 1900, &raw_mask);
    const std::int64_t t1 = t0 + 2200;
    (void)engine.process_frame(frame_input(t1), &raw_mask);
    if (engine.begin_transition(t1, 2) != Status::kOk) {
        std::cerr << "second transition refused\n";
        return 1;
    }
    for (const std::int64_t dt : {300, 700, 1100, 1500}) {
        emit("bg1 -> bg2 @" + std::to_string(dt) + "ms", t1 + dt, &raw_mask);
    }
    emit("stale mask falls back to camera", t1 + 1500 + 900, nullptr);

    // Nobody in view: the engine publishes an empty mask and the destination fills the view
    // (the CPU reference and the shader must agree on this, not only on the person case).
    const std::int64_t t2 = t1 + 4000;
    (void)engine.process_frame(frame_input(t2), &raw_mask);
    if (engine.begin_transition(t2, 1) != Status::kOk) {
        std::cerr << "third transition refused\n";
        return 1;
    }
    for (const std::int64_t dt : {300, 800, 1300}) {
        emit("empty scene, bg2 -> bg1 @" + std::to_string(dt) + "ms", t2 + dt, &nobody);
    }
    emit("empty scene, transition complete", t2 + 1900, &nobody);
    emit("person walks back in", t2 + 2000, &raw_mask);

    // The camera image is larger than the view: the mask is addressed through mask_crop.
    const std::int64_t t3 = t2 + 4000;
    (void)engine.process_frame(frame_input(t3, cropped_view), &raw_mask);
    if (engine.begin_transition(t3, 2) != Status::kOk) {
        std::cerr << "fourth transition refused\n";
        return 1;
    }
    for (const std::int64_t dt : {500, 1000, 1500}) {
        emit("cropped view, bg1 -> bg2 @" + std::to_string(dt) + "ms", t3 + dt, &raw_mask, cropped_view);
    }

    // Hash probe table: the 24-bit hash of every cell, as the three bytes the probe shader writes.
    std::vector<std::uint8_t> probe;
    probe.reserve(static_cast<std::size_t>(kProbeWidth) * static_cast<std::size_t>(kProbeHeight) * 3U);
    for (int y = 0; y < kProbeHeight; ++y) {
        for (int x = 0; x < kProbeWidth; ++x) {
            const std::uint32_t bits =
                pcg_round(static_cast<std::uint32_t>(x) ^ pcg_round(static_cast<std::uint32_t>(y))) >> 8U;
            probe.push_back(static_cast<std::uint8_t>((bits >> 16U) & 255U));
            probe.push_back(static_cast<std::uint8_t>((bits >> 8U) & 255U));
            probe.push_back(static_cast<std::uint8_t>(bits & 255U));
        }
    }

    std::ofstream out(argv[1], std::ios::binary);
    if (!out) {
        std::cerr << "cannot write " << argv[1] << "\n";
        return 1;
    }
    out << "{\"sdk_version\":" << json_string(MP_VERSION_STRING) << ",\"width\":" << kWidth
        << ",\"height\":" << kHeight << ",\n\"shader\":[";
    const auto lines = portal_fragment_shader_lines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << (i == 0 ? "" : ",\n") << json_string(lines[i]);
    }
    out << "],\n\"camera\":\"" << base64(camera.data(), camera.byte_size())
        << "\",\n\"bg\":{\"1\":{\"w\":" << background1.width() << ",\"h\":" << background1.height()
        << ",\"rgba\":\"" << base64(background1.data(), background1.byte_size())
        << "\"},\"2\":{\"w\":" << background2.width() << ",\"h\":" << background2.height() << ",\"rgba\":\""
        << base64(background2.data(), background2.byte_size())
        << "\"}},\n\"hash_probe\":{\"w\":" << kProbeWidth << ",\"h\":" << kProbeHeight
        << ",\"source\":" << json_string(portal_hash_probe_fragment_source()) << ",\"rgb\":\""
        << base64(probe.data(), probe.size()) << "\"},\n\"scenarios\":[\n"
        << scenarios.str() << "\n]}\n";
    std::cerr << "wrote " << count << " scenarios to " << argv[1] << "\n";
    return out.good() ? 0 : 1;
}
