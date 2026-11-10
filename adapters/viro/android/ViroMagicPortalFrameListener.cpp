#include "ViroMagicPortalFrameListener.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>

#include "VROARCamera.h"
#include "VROARFrame.h"
#include "VROARScene.h"
#include "VROARSession.h"
#include "VRODriver.h"
#include "VROSceneController.h"
#include "VROSceneRendererARCore.h"
#include "arcore/VROARCameraARCore.h"
#include "magic_portal/limits.h"

namespace magic_portal::viro {

ViroMagicPortalFrameListener::ViroMagicPortalFrameListener(std::shared_ptr<VROSceneRendererARCore> renderer,
                                                           std::shared_ptr<VRODriver> driver,
                                                           std::shared_ptr<ViroMagicPortalMaterial> material,
                                                           std::unique_ptr<PersonSegmenter> segmenter,
                                                           ViroMagicPortalConfig config)
    : renderer_(renderer),
      driver_(std::move(driver)),
      material_(std::move(material)),
      config_(std::move(config)) {
    if (!renderer || !driver_ || !material_ || !segmenter) {
        throw std::invalid_argument("magic portal frame listener dependencies are incomplete");
    }
    engine_ = std::make_unique<Engine>(config_.engine, std::move(segmenter));
    material_->set_style(config_.engine.style);
    FrameSource *source = this;  // private base: convert where it is accessible
    runner_ = std::make_unique<PortalRunner>(engine_.get(), source, config_.runner);
}

ViroMagicPortalFrameListener::~ViroMagicPortalFrameListener() {
    runner_.reset();  // join the worker before anything it touches goes away
    material_->detach();
}

std::int64_t ViroMagicPortalFrameListener::now_ms() const {
    if (config_.clock) {
        return config_.clock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

Status ViroMagicPortalFrameListener::trigger_transition(int background_id) noexcept {
    try {
        return engine_->begin_transition(now_ms(), background_id);
    } catch (...) {
        return Status::kInternal;
    }
}

void ViroMagicPortalFrameListener::reset() noexcept {
    try {
        runner_->reset();  // also drops a frame that is waiting for, or inside, the worker
    } catch (...) {
        engine_->reset();
    }
}

std::string ViroMagicPortalFrameListener::last_error() const {
    std::string error = engine_->last_error();
    return error.empty() ? material_->last_error() : error;
}

void ViroMagicPortalFrameListener::onFrameWillRender(const VRORenderContext & /*context*/) {
    try {
        const std::int64_t now = now_ms();
        material_->apply(engine_->render_state(now));
        runner_->tick(now);
    } catch (...) {
        // Never let an exception unwind into Viro's render loop.
    }
}

void ViroMagicPortalFrameListener::onFrameDidRender(const VRORenderContext & /*context*/) {}

bool ViroMagicPortalFrameListener::capture(CapturedFrame *frame) {
    const std::shared_ptr<VROSceneRendererARCore> renderer = renderer_.lock();
    if (!renderer)
        return false;
    const std::shared_ptr<VROSceneController> scene_controller = renderer->getSceneController();
    if (!scene_controller)
        return false;
    const std::shared_ptr<VROARScene> scene =
        std::dynamic_pointer_cast<VROARScene>(scene_controller->getScene());
    if (!scene || !scene->getARSession() || !scene->getARSession()->isReady())
        return false;
    std::unique_ptr<VROARFrame> &ar_frame = scene->getARSession()->getLastFrame();
    if (!ar_frame)
        return false;
    const std::shared_ptr<VROARCameraARCore> camera =
        std::dynamic_pointer_cast<VROARCameraARCore>(ar_frame->getCamera());
    if (!camera || !camera->isImageDataAvailable() ||
        camera->getTrackingState() != VROARTrackingState::Normal)
        return false;

    const VROVector3f size = camera->getRotatedImageSize();
    // Checked as floats first: converting NaN or a huge value to int is undefined behaviour.
    if (!(size.x >= 1.0F && size.y >= 1.0F && size.x <= static_cast<float>(kMaxFrameDimension) &&
          size.y <= static_cast<float>(kMaxFrameDimension)))
        return false;
    const int width = static_cast<int>(size.x);
    const int height = static_cast<int>(size.y);
    if (!frame_size_ok(width, height))
        return false;
    int crop_left = 0;
    int crop_top = 0;
    int crop_width = width;
    int crop_height = height;
    camera->getViewportCropRectangle(&crop_left, &crop_top, &crop_width, &crop_height);

    // resize() is a no-op once the camera resolution is stable, so there is no per-frame
    // allocation; mirroring is deferred to the worker thread.
    frame->rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    camera->getRotatedImageData(frame->rgba.data());
    frame->width = width;
    frame->height = height;
    frame->stride_bytes = width * 4;
    frame->mirror_horizontally = config_.mirror_segmentation_input;
    frame->crop_left = static_cast<float>(crop_left) / static_cast<float>(width);
    frame->crop_top = static_cast<float>(crop_top) / static_cast<float>(height);
    frame->crop_right = static_cast<float>(crop_left + crop_width) / static_cast<float>(width);
    frame->crop_bottom = static_cast<float>(crop_top + crop_height) / static_cast<float>(height);
    return true;
}

}  // namespace magic_portal::viro
