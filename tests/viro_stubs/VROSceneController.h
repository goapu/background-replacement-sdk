#pragma once
#include <memory>
#include "VROScene.h"
class VROSceneController {
public:
    std::shared_ptr<VROScene> getScene() const { return scene; }
    std::shared_ptr<VROScene> scene;
};
