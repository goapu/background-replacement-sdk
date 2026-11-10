#pragma once
#include <cstdint>
#include <vector>
#include "../VROARCamera.h"
class VROARCameraARCore : public VROARCamera {
public:
    bool isImageDataAvailable() const { return available; }
    VROARTrackingState getTrackingState() const override { return tracking; }
    VROVector3f getRotatedImageSize() const {
        return VROVector3f(static_cast<float>(width), static_cast<float>(height), 0);
    }
    void getViewportCropRectangle(int *left, int *top, int *w, int *h) const {
        *left = crop_left;
        *top = crop_top;
        *w = crop_width;
        *h = crop_height;
    }
    void getRotatedImageData(std::uint8_t *destination) const {
        ++reads;
        for (int i = 0; i < width * height * 4; ++i) {
            destination[i] = static_cast<std::uint8_t>(i + fill);
        }
    }
    bool available = true;
    VROARTrackingState tracking = VROARTrackingState::Normal;
    int width = 32, height = 24, crop_left = 0, crop_top = 0, crop_width = 32, crop_height = 24;
    std::uint8_t fill = 0;
    mutable int reads = 0;
};
