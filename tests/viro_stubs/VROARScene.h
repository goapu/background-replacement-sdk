#pragma once
#include <memory>
#include "VROARSession.h"
#include "VROScene.h"
class VROARScene : public VROScene {
public:
    std::shared_ptr<VROARSession> getARSession() const { return session; }
    std::shared_ptr<VROARSession> session;
};
