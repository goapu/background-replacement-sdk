#include "ViroMagicPortalMaterial.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

#include "VROData.h"
#include "VRODriver.h"
#include "VROMaterial.h"
#include "VROShaderModifier.h"
#include "VROTexture.h"
#include "VROTextureSubstrate.h"
#include "VROVector4f.h"
#include "magic_portal/portal_shader.h"

namespace magic_portal::viro {
namespace {

bool float_linear_filtering_supported() {
    const auto *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    return extensions != nullptr && std::strstr(extensions, "GL_OES_texture_float_linear") != nullptr;
}

std::shared_ptr<VROTexture> make_float_texture(int width, int height, const float *data, VROFilterMode filter,
                                               const char *name) {
    if (width <= 0 || height <= 0) {
        throw std::invalid_argument("texture dimensions must be positive");
    }
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> zeros;
    if (data == nullptr) {
        zeros.assign(count, 0.0F);
        data = zeros.data();
    }
    auto bytes = std::make_shared<VROData>(data, static_cast<int>(count * sizeof(float)));
    std::vector<std::shared_ptr<VROData>> levels{std::move(bytes)};
    std::vector<std::uint32_t> mip_sizes;
    auto texture = std::make_shared<VROTexture>(VROTextureType::Texture2D, VROTextureFormat::R32F,
                                                VROTextureInternalFormat::R32F, false, VROMipmapMode::None,
                                                levels, width, height, mip_sizes);
    texture->setName(name);
    texture->setMinificationFilter(filter);
    texture->setMagnificationFilter(filter);
    texture->setWrapS(VROWrapMode::Clamp);
    texture->setWrapT(VROWrapMode::Clamp);
    return texture;
}

std::shared_ptr<VROShaderModifier> make_modifier() {
    // Viro injects these lines at the fragment entry point of the camera-background
    // material and supplies camera_texture / camera_image_transform / ar_viewport_size.
    auto modifier =
        std::make_shared<VROShaderModifier>(VROShaderEntryPoint::Fragment, portal_fragment_shader_lines());
    modifier->setName("magic_portal_background_replacement");
    modifier->setPriority(10);
    modifier->setRequiresCameraTexture(true);
    return modifier;
}

void set_color(VROMaterial *material, const char *name, Rgba color) {
    material->setShaderUniform(
        name, VROVector4f(static_cast<float>(color.r) / 255.0F, static_cast<float>(color.g) / 255.0F,
                          static_cast<float>(color.b) / 255.0F, static_cast<float>(color.a) / 255.0F));
}

}  // namespace

std::shared_ptr<ViroMagicPortalMaterial> ViroMagicPortalMaterial::create(
    std::shared_ptr<VROMaterial> camera_background_material, std::shared_ptr<VRODriver> driver) {
    if (!camera_background_material || !driver) {
        throw std::invalid_argument("Viro portal needs the camera-background material and driver");
    }
    auto portal = std::shared_ptr<ViroMagicPortalMaterial>(new ViroMagicPortalMaterial());
    portal->driver_ = std::move(driver);
    portal->material_ = std::move(camera_background_material);
    portal->linear_filtering_ = float_linear_filtering_supported();
    const VROFilterMode mask_filter =
        portal->linear_filtering_ ? VROFilterMode::Linear : VROFilterMode::Nearest;
    if (!portal->linear_filtering_) {
        portal->record_error(
            "GL_OES_texture_float_linear is unavailable; using nearest filtering for the person mask");
    }
    // Unbound samplers would read as incomplete textures, so every sampler gets a 1x1
    // placeholder until a real mask or background arrives.
    portal->placeholder_ =
        make_float_texture(1, 1, nullptr, VROFilterMode::Nearest, "magic_portal_placeholder");
    portal->mask_texture_ = make_float_texture(1, 1, nullptr, mask_filter, "magic_portal_person_mask");
    portal->mask_width_ = 1;
    portal->mask_height_ = 1;
    portal->modifier_ = make_modifier();

    VROMaterial &m = *portal->material_;
    m.addShaderModifier(portal->modifier_);
    portal->attached_ = true;
    m.setShaderUniform(uniforms::kPersonMask, portal->mask_texture_);
    m.setShaderUniform(uniforms::kCurrentBackground, portal->placeholder_);
    m.setShaderUniform(uniforms::kNextBackground, portal->placeholder_);
    portal->bound_current_ = portal->placeholder_;
    portal->bound_next_ = portal->placeholder_;
    m.setShaderUniform(uniforms::kPersonBounds, VROVector4f(0.29F, 0.14F, 0.71F, 0.86F));
    m.setShaderUniform(uniforms::kMaskCrop, VROVector4f(0.0F, 0.0F, 1.0F, 1.0F));
    m.setShaderUniform(uniforms::kEffectEnable, 0.0F);  // camera passes through until told otherwise
    m.setShaderUniform(uniforms::kProgress, 0.0F);
    m.setShaderUniform(uniforms::kReveal, 0.0F);
    m.setShaderUniform(uniforms::kRingScale, 0.0F);
    m.setShaderUniform(uniforms::kRingAlpha, 0.0F);
    m.setShaderUniform(uniforms::kSpinTurns, 0.0F);
    m.setShaderUniform(uniforms::kPersonVisibility, 1.0F);
    m.setShaderUniform(uniforms::kUseCameraOld, 1.0F);
    portal->set_style(PortalStyle{});
    m.prewarm(portal->driver_);
    return portal;
}

ViroMagicPortalMaterial::~ViroMagicPortalMaterial() {
    detach();
}

void ViroMagicPortalMaterial::detach() noexcept {
    try {
        if (attached_ && material_ && modifier_) {
            material_->setShaderUniform(uniforms::kEffectEnable, 0.0F);
            material_->removeShaderModifier(modifier_);
        }
    } catch (...) {
        // Best effort: this path cannot report further failures.
    }
    attached_ = false;
}

void ViroMagicPortalMaterial::set_style(const PortalStyle &style) {
    validate_style(style);  // reject before changing any uniforms
    if (!attached_) {
        return;
    }
    VROMaterial &m = *material_;
    m.setShaderUniform(uniforms::kStyleA, VROVector4f(style.ring_width, style.ring_strength,
                                                      style.particle_strength, style.silhouette_glow));
    m.setShaderUniform(uniforms::kStyleB,
                       VROVector4f(style.reveal_feather, style.person_protection, 0.0F, 0.0F));
    set_color(&m, uniforms::kWarm, style.warm_color);
    set_color(&m, uniforms::kCool, style.cool_color);
}

void ViroMagicPortalMaterial::set_background(int id, std::shared_ptr<VROTexture> texture) {
    if (id < 0 || !texture) {
        throw std::invalid_argument("background id must be >= 0 and the texture non-null");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    backgrounds_[id] = std::move(texture);
}

void ViroMagicPortalMaterial::remove_background(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    backgrounds_.erase(id);
}

std::string ViroMagicPortalMaterial::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void ViroMagicPortalMaterial::record_error(const std::string &message) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = message;
}

void ViroMagicPortalMaterial::upload_mask(const ConfidenceMask &mask) {
    const VROFilterMode filter = linear_filtering_ ? VROFilterMode::Linear : VROFilterMode::Nearest;
    if (!mask_texture_ || mask.width() != mask_width_ || mask.height() != mask_height_) {
        // First real mask, or the model's output size changed: build a texture of that size.
        // Nothing is committed until the new texture is bound, so a failure leaves the
        // adapter describing the texture that is really in use.
        auto texture =
            make_float_texture(mask.width(), mask.height(), mask.data(), filter, "magic_portal_person_mask");
        material_->setShaderUniform(uniforms::kPersonMask, texture);
        mask_texture_ = std::move(texture);
        mask_width_ = mask.width();
        mask_height_ = mask.height();
        return;
    }
    VROTextureSubstrate *substrate = mask_texture_->getSubstrate(0, driver_, true);
    if (substrate == nullptr) {
        throw std::runtime_error("Viro failed to hydrate the person-mask texture");
    }
    substrate->updateR32FData(mask.data(), mask_width_, mask_height_);
}

// Uploads the mask the state refers to when the GPU copy is missing, outdated or invalid.
// Returns whether the GPU now holds exactly that mask.
bool ViroMagicPortalMaterial::sync_mask(const RenderState &state) noexcept {
    if (state.mask == nullptr || state.mask->empty()) {
        gpu_mask_valid_ = false;  // nothing to draw with; the next mask must be uploaded afresh
        return false;
    }
    if (gpu_mask_valid_ && !state.mask_updated && uploaded_version_ == state.mask_version) {
        return true;
    }
    try {
        upload_mask(*state.mask);
    } catch (const std::exception &exception) {
        note_upload_failure(exception.what());
        return false;
    } catch (...) {
        note_upload_failure("unknown failure while uploading the person mask");
        return false;
    }
    gpu_mask_valid_ = true;
    uploaded_version_ = state.mask_version;
    upload_failures_consecutive_.store(0, std::memory_order_relaxed);
    if (upload_error_active_) {
        upload_error_active_ = false;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_.clear();
        } catch (...) {
            // Best effort: a stale message is harmless.
        }
    }
    return true;
}

void ViroMagicPortalMaterial::note_upload_failure(const char *message) noexcept {
    gpu_mask_valid_ = false;
    upload_error_active_ = true;
    upload_failures_total_.fetch_add(1, std::memory_order_relaxed);
    upload_failures_consecutive_.fetch_add(1, std::memory_order_relaxed);
    try {
        record_error(message);
    } catch (...) {
        // Best effort: this path cannot report further failures.
    }
}

void ViroMagicPortalMaterial::apply(const RenderState &state) noexcept {
    if (!attached_) {
        return;
    }
    try {
        VROMaterial &m = *material_;
        const bool mask_ready = sync_mask(state);

        std::shared_ptr<VROTexture> current;
        std::shared_ptr<VROTexture> next;
        bool backgrounds_ready = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto next_it = backgrounds_.find(state.next_background);
            const auto current_it = backgrounds_.find(state.current_background);
            if (next_it != backgrounds_.end()) {
                next = next_it->second;
            }
            if (current_it != backgrounds_.end()) {
                current = current_it->second;
            }
        }
        if (state.camera_is_current) {
            // The shader mixes in the live camera for the "old" side; any valid sampler will do.
            current = next ? next : placeholder_;
            backgrounds_ready = next != nullptr;
        } else {
            backgrounds_ready = next != nullptr && current != nullptr;
        }
        if (!next) {
            next = placeholder_;
        }
        if (!current) {
            current = placeholder_;
        }
        if (current != bound_current_) {
            m.setShaderUniform(uniforms::kCurrentBackground, current);
            bound_current_ = current;
        }
        if (next != bound_next_) {
            m.setShaderUniform(uniforms::kNextBackground, next);
            bound_next_ = next;
        }

        if (state.effect_enable && !backgrounds_ready) {
            record_error("a transition references a background id that was never registered");
        }
        // An upload failure has already been recorded by sync_mask(); the effect just stays off.
        const bool enable = state.effect_enable && backgrounds_ready && mask_ready;
        m.setShaderUniform(uniforms::kEffectEnable, enable ? 1.0F : 0.0F);
        m.setShaderUniform(uniforms::kProgress, state.visual.progress);
        m.setShaderUniform(uniforms::kReveal, state.visual.complete ? 1.0F : state.visual.reveal);
        m.setShaderUniform(uniforms::kRingScale, state.visual.ring_scale);
        m.setShaderUniform(uniforms::kRingAlpha, state.visual.ring_alpha);
        m.setShaderUniform(uniforms::kSpinTurns, state.visual.spin_turns);
        m.setShaderUniform(uniforms::kPersonVisibility,
                           std::clamp(state.visual.person_visibility, 0.0F, 1.0F));
        m.setShaderUniform(uniforms::kUseCameraOld, state.camera_is_current ? 1.0F : 0.0F);
        m.setShaderUniform(uniforms::kPersonBounds,
                           VROVector4f(state.person_bounds[0], state.person_bounds[1], state.person_bounds[2],
                                       state.person_bounds[3]));
        m.setShaderUniform(uniforms::kMaskCrop, VROVector4f(state.mask_crop[0], state.mask_crop[1],
                                                            state.mask_crop[2], state.mask_crop[3]));
    } catch (const std::exception &exception) {
        try {
            record_error(exception.what());
            material_->setShaderUniform(uniforms::kEffectEnable, 0.0F);
        } catch (...) {
            // Best effort: this path cannot report further failures.
        }
    } catch (...) {
        try {
            material_->setShaderUniform(uniforms::kEffectEnable, 0.0F);
        } catch (...) {
            // Best effort: this path cannot report further failures.
        }
    }
}

}  // namespace magic_portal::viro
