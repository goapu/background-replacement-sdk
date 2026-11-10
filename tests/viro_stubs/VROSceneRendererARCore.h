#pragma once
#include <memory>
#include "VROSceneController.h"
class VROSceneRendererARCore {
public:
    std::shared_ptr<VROSceneController> getSceneController() const { return controller; }
    std::shared_ptr<VROSceneController> controller;
};
