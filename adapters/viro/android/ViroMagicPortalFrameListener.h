#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "VROFrameListener.h"
#include "ViroMagicPortalMaterial.h"
#include "magic_portal/engine.h"
#include "magic_portal/person_segmenter.h"
#include "magic_portal/portal_runner.h"

class VRODriver;
class VROSceneRendererARCore;

namespace magic_portal::viro {

struct ViroMagicPortalConfig {
    EngineConfig engine;
    PortalRunnerConfig runner;
    // Mirror the camera image before segmentation (front camera shown mirrored).
    bool mirror_segmentation_input = false;
    // Millisecond clock shared by the render loop and the engine. Defaults to steady_clock.
    std::function<std::int64_t()> clock;
};

// Android ViroCore frame listener. Camera readback happens on Viro's render thread into a
// recycled buffer; segmentation and refinement run on the SDK's worker thread.
class ViroMagicPortalFrameListener final : public VROFrameListener, private FrameSource {
public:
    // Throws std::invalid_argument when a dependency is missing.
    ViroMagicPortalFrameListener(std::shared_ptr<VROSceneRendererARCore> renderer,
                                 std::shared_ptr<VRODriver> driver,
                                 std::shared_ptr<ViroMagicPortalMaterial> material,
                                 std::unique_ptr<PersonSegmenter> segmenter,
                                 ViroMagicPortalConfig config = {});
    ~ViroMagicPortalFrameListener() override;

    ViroMagicPortalFrameListener(const ViroMagicPortalFrameListener &) = delete;
    ViroMagicPortalFrameListener &operator=(const ViroMagicPortalFrameListener &) = delete;

    // Starts the portal towards the background registered under `background_id`. Returns
    // kInvalidState until a fresh person mask exists or while a transition is running.
    [[nodiscard]] Status trigger_transition(int background_id) noexcept;

    // Back to the live camera. Callable from any thread. A frame captured before the call is
    // never published after it, whether it is waiting for the worker or already inside it.
    void reset() noexcept;

    [[nodiscard]] EngineStats engine_stats() const noexcept { return engine_->stats(); }
    [[nodiscard]] PortalRunnerStats runner_stats() const noexcept { return runner_->stats(); }
    [[nodiscard]] std::string last_error() const;

    void onFrameWillRender(const VRORenderContext &context) override;
    void onFrameDidRender(const VRORenderContext &context) override;

private:
    bool capture(CapturedFrame *frame) override;  // FrameSource
    [[nodiscard]] std::int64_t now_ms() const;

    std::weak_ptr<VROSceneRendererARCore> renderer_;
    std::shared_ptr<VRODriver> driver_;
    std::shared_ptr<ViroMagicPortalMaterial> material_;
    ViroMagicPortalConfig config_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<PortalRunner> runner_;  // declared last: stops its worker first
};

}  // namespace magic_portal::viro
