// Adapter logic tests over stub ViroCore/GLES headers (see tests/viro_stubs/README.md).
#include <GLES3/gl3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "VROARScene.h"
#include "VROMaterial.h"
#include "VROSceneRendererARCore.h"
#include "ViroMagicPortalFrameListener.h"
#include "ViroMagicPortalMaterial.h"
#include "arcore/VROARCameraARCore.h"
#include "golden_support.h"
#include "magic_portal/portal_shader.h"
#include "test_harness.h"

const char *g_stub_gl_extensions = "GL_OES_texture_float_linear GL_EXT_other";

using namespace magic_portal;
using namespace magic_portal::viro;

namespace {

std::shared_ptr<VROTexture> make_texture() {
    return std::make_shared<VROTexture>(
        VROTextureType::Texture2D, VROTextureFormat::R32F, VROTextureInternalFormat::R32F, false,
        VROMipmapMode::None, std::vector<std::shared_ptr<VROData>>{}, 4, 4, std::vector<std::uint32_t>{});
}

ConfidenceMask mask_of(int w, int h, float value) {
    return ConfidenceMask(
        w, h, std::vector<float>(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), value));
}

RenderState transitioning_state(const ConfidenceMask *mask, bool updated) {
    RenderState state;
    state.effect_enable = true;
    state.phase = PortalPhase::kTransitioning;
    state.camera_is_current = true;
    state.current_background = kNoBackground;
    state.next_background = 5;
    state.visual.reveal = 0.4F;
    state.visual.ring_alpha = 0.7F;
    state.visual.person_visibility = 1.7F;  // out of range on purpose
    state.mask = mask;
    state.mask_updated = updated;
    return state;
}

struct Fixture {
    std::shared_ptr<VROMaterial> material = std::make_shared<VROMaterial>();
    std::shared_ptr<VRODriver> driver = std::make_shared<VRODriver>();
};

class EllipseSegmenter final : public PersonSegmenter {
public:
    bool segment(const RgbaFrame &, std::int64_t, ConfidenceMask *out, std::string *) override {
        ++calls;
        *out = mp_test::make_ellipse_mask(32, 32, 0.5F, 0.5F, 0.25F, 0.4F, 7);
        return true;
    }
    std::atomic<int> calls{0};
};

template <typename Pred>
bool wait_for(Pred pred) {
    for (int i = 0; i < 4000; ++i) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

MT_TEST("viro material: attaches the generated shader and starts as a pass-through") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    MT_CHECK(f.material->modifiers.size() == 1);
    MT_CHECK(f.material->modifiers[0]->lines == portal_fragment_shader_lines());
    MT_CHECK(f.material->modifiers[0]->requires_camera);
    MT_CHECK(f.material->prewarms == 1);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    for (const char *name :
         {uniforms::kPersonMask, uniforms::kCurrentBackground, uniforms::kNextBackground}) {
        MT_CHECK(f.material->get_texture(name) != nullptr);  // no unbound samplers
    }
    MT_CHECK(f.material->get_vec4(uniforms::kStyleA).x == PortalStyle{}.ring_width);
    MT_CHECK(f.material->get_vec4(uniforms::kWarm).x > 0.9F);
}

MT_TEST("viro material: rejects missing dependencies") {
    Fixture f;
    MT_CHECK_THROWS(ViroMagicPortalMaterial::create(nullptr, f.driver), std::invalid_argument);
    MT_CHECK_THROWS(ViroMagicPortalMaterial::create(f.material, nullptr), std::invalid_argument);
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    MT_CHECK_THROWS(portal->set_background(-1, make_texture()), std::invalid_argument);
    MT_CHECK_THROWS(portal->set_background(1, nullptr), std::invalid_argument);
}

MT_TEST("viro material: invalid styles are rejected before uniforms are changed") {
    Fixture f;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    const auto before = f.material->get_vec4(uniforms::kStyleB);
    PortalStyle invalid;
    invalid.person_protection = 0.5F;
    invalid.reveal_feather = 0.25F;
    MT_CHECK_THROWS(portal->set_style(invalid), std::invalid_argument);
    const auto after = f.material->get_vec4(uniforms::kStyleB);
    MT_CHECK(after.x == before.x && after.y == before.y);

    PortalStyle valid;
    valid.person_protection = 0.8F;
    portal->set_style(valid);
    MT_CHECK(f.material->get_vec4(uniforms::kStyleB).y == 0.8F);
}

MT_TEST("viro material: every shader uniform is written by the adapter") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    portal->set_background(5, make_texture());
    const ConfidenceMask mask = mask_of(8, 8, 0.5F);
    portal->apply(transitioning_state(&mask, true));
    for (const std::string &name : portal_custom_uniform_names()) {
        const bool written = f.material->floats.count(name) != 0 || f.material->vec4s.count(name) != 0 ||
                             f.material->textures.count(name) != 0;
        MT_CHECK(written);
    }
}

MT_TEST("viro material: missing float-linear support falls back to nearest and says so") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    {
        Fixture f;
        auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
        MT_CHECK(portal->mask_filtering_is_linear());
        MT_CHECK(f.material->get_texture(uniforms::kPersonMask)->mag_filter == VROFilterMode::Linear);
        MT_CHECK(portal->last_error().empty());
    }
    g_stub_gl_extensions = "GL_EXT_something_else";
    {
        Fixture f;
        auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
        MT_CHECK(!portal->mask_filtering_is_linear());
        MT_CHECK(f.material->get_texture(uniforms::kPersonMask)->mag_filter == VROFilterMode::Nearest);
        MT_CHECK(portal->last_error().find("GL_OES_texture_float_linear") != std::string::npos);
        const ConfidenceMask mask = mask_of(8, 8, 1.0F);
        RenderState state = transitioning_state(&mask, true);
        auto portal2 = ViroMagicPortalMaterial::create(f.material, f.driver);
        portal2->apply(state);
        MT_CHECK(f.material->get_texture(uniforms::kPersonMask)->mag_filter == VROFilterMode::Nearest);
    }
    g_stub_gl_extensions = nullptr;  // a driver that reports nothing at all
    {
        Fixture f;
        auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
        MT_CHECK(!portal->mask_filtering_is_linear());
    }
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
}

MT_TEST("viro material: mask texture is created at the mask size, updated in place, rebuilt on resize") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    portal->set_background(5, make_texture());
    const auto placeholder = f.material->get_texture(uniforms::kPersonMask);

    const ConfidenceMask a = mask_of(16, 8, 0.25F);
    portal->apply(transitioning_state(&a, true));
    const auto first = f.material->get_texture(uniforms::kPersonMask);
    MT_CHECK(first != placeholder);
    MT_CHECK(first->width == 16 && first->height == 8);

    const ConfidenceMask b = mask_of(16, 8, 0.75F);
    portal->apply(transitioning_state(&b, true));
    MT_CHECK(f.material->get_texture(uniforms::kPersonMask) == first);  // same texture, new data
    MT_CHECK(first->substrate.uploads == 1);
    MT_CHECK(first->substrate.last_data[0] == 0.75F);

    portal->apply(transitioning_state(&b, false));  // not updated: no upload
    MT_CHECK(first->substrate.uploads == 1);

    const ConfidenceMask c = mask_of(24, 24, 0.5F);
    portal->apply(transitioning_state(&c, true));
    const auto second = f.material->get_texture(uniforms::kPersonMask);
    MT_CHECK(second != first && second->width == 24 && second->height == 24);
}

MT_TEST("viro material: backgrounds are resolved by id and the effect fails safe without them") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    const ConfidenceMask mask = mask_of(8, 8, 0.5F);

    portal->apply(transitioning_state(&mask, true));  // background 5 was never registered
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    MT_CHECK(!portal->last_error().empty());

    auto texture = make_texture();
    portal->set_background(5, texture);
    portal->apply(transitioning_state(&mask, false));
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);
    MT_CHECK(f.material->get_texture(uniforms::kNextBackground) == texture);
    MT_CHECK(f.material->get_float(uniforms::kUseCameraOld) == 1.0F);
    MT_CHECK_NEAR(f.material->get_float(uniforms::kReveal), 0.4, 1e-6);
    MT_CHECK(f.material->get_float(uniforms::kPersonVisibility) == 1.0F);  // clamped

    // Replaced phase: both sides are registered backgrounds, camera is no longer the "old" side.
    auto previous = make_texture();
    portal->set_background(2, previous);
    RenderState replaced = transitioning_state(&mask, false);
    replaced.camera_is_current = false;
    replaced.current_background = 2;
    replaced.next_background = 5;
    portal->apply(replaced);
    MT_CHECK(f.material->get_texture(uniforms::kCurrentBackground) == previous);
    MT_CHECK(f.material->get_float(uniforms::kUseCameraOld) == 0.0F);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);

    portal->remove_background(2);
    portal->apply(replaced);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);

    RenderState live;  // engine says: camera only
    portal->apply(live);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
}

MT_TEST("viro material: a failing texture upload cannot throw out of apply() and disables the effect") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    portal->set_background(5, make_texture());
    const ConfidenceMask mask = mask_of(8, 8, 0.5F);
    portal->apply(transitioning_state(&mask, true));
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);
    f.material->get_texture(uniforms::kPersonMask)->substrate.throw_on_upload = true;
    portal->apply(transitioning_state(&mask, true));
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    MT_CHECK(portal->last_error().find("stub upload failure") != std::string::npos);
    f.material->get_texture(uniforms::kPersonMask)->substrate.throw_on_upload = false;
    f.material->get_texture(uniforms::kPersonMask)->fail_substrate = true;
    portal->apply(transitioning_state(&mask, true));
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
}

MT_TEST("viro material: after a failed mask upload the effect stays off until an upload succeeds") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    portal->set_background(5, make_texture());
    const ConfidenceMask old_mask = mask_of(8, 8, 0.25F);
    RenderState first = transitioning_state(&old_mask, true);
    first.mask_version = 1;
    portal->apply(first);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);

    const auto texture = f.material->get_texture(uniforms::kPersonMask);
    texture->substrate.throw_on_upload = true;
    const ConfidenceMask new_mask = mask_of(8, 8, 0.9F);
    RenderState failed = transitioning_state(&new_mask, true);
    failed.mask_version = 2;
    portal->apply(failed);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    MT_CHECK(portal->consecutive_mask_upload_failures() == 1);
    MT_CHECK(portal->last_error().find("stub upload failure") != std::string::npos);

    // The engine reports mask_updated only once, so the frames that follow carry
    // mask_updated == false. They must not switch the previous GPU mask back on.
    RenderState later = transitioning_state(&new_mask, false);
    later.mask_version = 2;
    for (int i = 0; i < 5; ++i) {
        portal->apply(later);
        MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    }
    MT_CHECK(portal->consecutive_mask_upload_failures() == 6);  // every frame retried

    texture->substrate.throw_on_upload = false;
    portal->apply(later);  // the adapter retries the mask it never managed to upload
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);
    MT_CHECK(texture->substrate.last_data.size() == 64U && texture->substrate.last_data[0] == 0.9F);
    MT_CHECK(portal->consecutive_mask_upload_failures() == 0);
    MT_CHECK(portal->mask_upload_failures() == 6);
    MT_CHECK(portal->last_error().empty());  // the error clears once the upload recovers

    // Once uploaded, a mask is not uploaded again while its version is unchanged.
    const int uploads = texture->substrate.uploads;
    portal->apply(later);
    MT_CHECK(texture->substrate.uploads == uploads);

    // A mask that is missing altogether keeps the effect off, and the next one is uploaded.
    RenderState none = transitioning_state(nullptr, false);
    none.mask_version = 2;
    portal->apply(none);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    portal->apply(later);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);
}

MT_TEST("viro material: destruction and detach() remove the modifier exactly once") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    {
        auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
        MT_CHECK(f.material->modifiers.size() == 1);
        portal->detach();
        MT_CHECK(f.material->modifiers.empty());
        portal->detach();  // idempotent
        const ConfidenceMask mask = mask_of(4, 4, 1.0F);
        portal->set_background(5, make_texture());
        portal->apply(transitioning_state(&mask, true));  // ignored after detach
        MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    }
    MT_CHECK(f.material->modifiers.empty());
    { auto portal = ViroMagicPortalMaterial::create(f.material, f.driver); }
    MT_CHECK(f.material->modifiers.empty());  // destructor detaches as well
}

namespace {

struct Scene {
    std::shared_ptr<VROSceneRendererARCore> renderer = std::make_shared<VROSceneRendererARCore>();
    std::shared_ptr<VROARCameraARCore> camera = std::make_shared<VROARCameraARCore>();
    std::shared_ptr<VROARSession> session = std::make_shared<VROARSession>();
    std::shared_ptr<VROARScene> scene = std::make_shared<VROARScene>();
    Scene() {
        auto frame = std::make_unique<VROARFrame>();
        frame->camera = camera;
        session->last_frame = std::move(frame);
        scene->session = session;
        renderer->controller = std::make_shared<VROSceneController>();
        renderer->controller->scene = scene;
        camera->width = 40;
        camera->height = 30;
        camera->crop_width = 40;
        camera->crop_height = 30;
    }
};

}  // namespace

MT_TEST("viro listener: camera -> worker -> mask -> transition -> reset, end to end on stubs") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    Scene s;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    portal->set_background(1, make_texture());

    std::atomic<std::int64_t> clock{1000};
    ViroMagicPortalConfig config;
    config.clock = [&clock] { return clock.load(); };
    config.engine.timeline.duration_ms = 600;
    config.engine.freshness.stale_after_ms = 5000;
    auto owned = std::make_unique<EllipseSegmenter>();
    EllipseSegmenter *segmenter = owned.get();
    ViroMagicPortalFrameListener listener(s.renderer, f.driver, portal, std::move(owned), config);
    VRORenderContext context;

    MT_CHECK(listener.trigger_transition(1) == Status::kInvalidState);  // nothing segmented yet
    listener.onFrameWillRender(context);                                // captures the first frame
    MT_CHECK(wait_for([&] { return listener.engine_stats().frames_processed == 1; }));
    MT_CHECK(segmenter->calls == 1);
    MT_CHECK(s.camera->reads == 1);

    clock = 1100;
    listener.onFrameWillRender(context);  // uploads the mask; live camera is still untouched
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    MT_CHECK(f.material->get_texture(uniforms::kPersonMask)->width == 32);

    MT_CHECK(listener.trigger_transition(1) == Status::kOk);
    clock = 1300;
    listener.onFrameWillRender(context);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);
    MT_CHECK(f.material->get_float(uniforms::kReveal) > 0.0F);

    for (int i = 0; i < 12; ++i) {  // run past the end of the 600 ms transition
        clock += 66;
        listener.onFrameWillRender(context);
        wait_for([&] {
            return listener.runner_stats().frames_submitted <= listener.engine_stats().frames_processed + 1;
        });
    }
    clock = 2200;
    listener.onFrameWillRender(context);
    MT_CHECK(f.material->get_float(uniforms::kUseCameraOld) == 0.0F);  // replaced background is now "old"
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 1.0F);

    listener.reset();
    MT_CHECK(listener.trigger_transition(1) == Status::kInvalidState);  // the old mask is gone
    listener.onFrameWillRender(context);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
}

MT_TEST("viro listener: an unusable camera is skipped without errors or effect") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    Scene s;
    s.camera->tracking = VROARTrackingState::Limited;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    std::atomic<std::int64_t> clock{0};
    ViroMagicPortalConfig config;
    config.clock = [&clock] { return clock.load(); };
    ViroMagicPortalFrameListener listener(s.renderer, f.driver, portal, std::make_unique<EllipseSegmenter>(),
                                          config);
    VRORenderContext context;
    for (int i = 0; i < 5; ++i) {
        clock += 100;
        listener.onFrameWillRender(context);
    }
    MT_CHECK(listener.runner_stats().captures_failed == 5);
    MT_CHECK(listener.engine_stats().frames_processed == 0);
    MT_CHECK(f.material->get_float(uniforms::kEffectEnable) == 0.0F);
    s.camera->tracking = VROARTrackingState::Normal;
    s.camera->available = false;
    clock += 100;
    listener.onFrameWillRender(context);
    MT_CHECK(listener.runner_stats().captures_failed == 6);
}

MT_TEST("viro listener: construction validates dependencies and teardown restores the material") {
    g_stub_gl_extensions = "GL_OES_texture_float_linear";
    Fixture f;
    Scene s;
    auto portal = ViroMagicPortalMaterial::create(f.material, f.driver);
    MT_CHECK_THROWS(
        ViroMagicPortalFrameListener(nullptr, f.driver, portal, std::make_unique<EllipseSegmenter>()),
        std::invalid_argument);
    MT_CHECK_THROWS(ViroMagicPortalFrameListener(s.renderer, f.driver, portal, nullptr),
                    std::invalid_argument);
    {
        ViroMagicPortalFrameListener listener(s.renderer, f.driver, portal,
                                              std::make_unique<EllipseSegmenter>());
        MT_CHECK(f.material->modifiers.size() == 1);
    }
    MT_CHECK(f.material->modifiers.empty());  // the shader modifier does not outlive the listener
}
