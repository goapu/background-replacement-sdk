#pragma once
#include <memory>
#include "VROARCamera.h"
class VROARFrame {
public:
    std::shared_ptr<VROARCamera> getCamera() const { return camera; }
    std::shared_ptr<VROARCamera> camera;
};
