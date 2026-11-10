#pragma once
#include "VROVector3f.h"
enum class VROARTrackingState { Unavailable, Limited, Normal };
class VROARCamera {
public:
    virtual ~VROARCamera() = default;
    virtual VROARTrackingState getTrackingState() const = 0;
};
