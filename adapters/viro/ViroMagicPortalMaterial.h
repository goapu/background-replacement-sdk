#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "magic_portal/engine.h"
#include "magic_portal/portal_compositor.h"

class VRODriver;
class VROMaterial;
class VROShaderModifier;
class VROTexture;

namespace magic_portal::viro {

// Connects the engine's per-frame RenderState to ViroCore's camera-background material.
//
// Threading: create(), apply(), detach() and the destructor belong to Viro's render thread
// (they touch GL objects). set_background()/remove_background() may be called from any
// thread.
class ViroMagicPortalMaterial {
public:
    // Needs a current GL context. Throws std::invalid_argument for missing dependencies.
    static std::shared_ptr<ViroMagicPortalMaterial> create(
        std::shared_ptr<VROMaterial> camera_background_material, std::shared_ptr<VRODriver> driver);

    ~ViroMagicPortalMaterial();
    ViroMagicPortalMaterial(const ViroMagicPortalMaterial &) = delete;
    ViroMagicPortalMaterial &operator=(const ViroMagicPortalMaterial &) = delete;

    // Registers the texture that background `id` (>= 0) refers to in engine transitions.
    // The caller keeps ownership of its own handle; the adapter shares it.
    void set_background(int id, std::shared_ptr<VROTexture> texture);
    void remove_background(int id);

    // Pushes the look (colors, widths) once; call again if the style changes. Throws
    // std::invalid_argument for an invalid style before changing any uniforms.
    void set_style(const PortalStyle &style);

    // Applies one frame. Never throws: on any problem the effect is switched off, which
    // shows the unmodified camera, and last_error() describes the cause.
    //
    // The effect is enabled only while the mask on the GPU is the mask the state refers to
    // (RenderState::mask_version). After a failed upload the adapter keeps retrying the
    // current mask on every frame and keeps the effect off until one upload succeeds, even
    // though the engine reports `mask_updated` only once per mask.
    void apply(const RenderState &state) noexcept;

    // Removes the shader modifier so the material renders normally again. Idempotent.
    void detach() noexcept;

    // False when the device cannot linearly filter float textures (no
    // GL_OES_texture_float_linear). Masks then use nearest filtering: the effect still
    // works but mask edges are blockier.
    [[nodiscard]] bool mask_filtering_is_linear() const noexcept { return linear_filtering_; }
    [[nodiscard]] std::string last_error() const;

    // Upload diagnostics for field telemetry. The consecutive count returns to zero after the
    // first successful upload.
    [[nodiscard]] std::uint64_t mask_upload_failures() const noexcept {
        return upload_failures_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t consecutive_mask_upload_failures() const noexcept {
        return upload_failures_consecutive_.load(std::memory_order_relaxed);
    }

private:
    ViroMagicPortalMaterial() = default;

    void upload_mask(const ConfidenceMask &mask);
    [[nodiscard]] bool sync_mask(const RenderState &state) noexcept;
    void note_upload_failure(const char *message) noexcept;
    void record_error(const std::string &message);

    std::shared_ptr<VRODriver> driver_;
    std::shared_ptr<VROMaterial> material_;
    std::shared_ptr<VROShaderModifier> modifier_;
    std::shared_ptr<VROTexture> placeholder_;
    std::shared_ptr<VROTexture> mask_texture_;
    std::shared_ptr<VROTexture> bound_current_;
    std::shared_ptr<VROTexture> bound_next_;
    int mask_width_ = 0;
    int mask_height_ = 0;
    bool linear_filtering_ = true;
    bool attached_ = false;
    // Render thread only. True when the texture holds the mask of `uploaded_version_`.
    bool gpu_mask_valid_ = false;
    std::uint64_t uploaded_version_ = 0;
    bool upload_error_active_ = false;
    std::atomic<std::uint64_t> upload_failures_total_{0};
    std::atomic<std::uint64_t> upload_failures_consecutive_{0};

    mutable std::mutex mutex_;  // guards backgrounds_ and last_error_
    std::map<int, std::shared_ptr<VROTexture>> backgrounds_;
    std::string last_error_;
};

}  // namespace magic_portal::viro
