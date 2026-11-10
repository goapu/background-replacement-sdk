#pragma once
#include <memory>
#include "VROARFrame.h"
class VROARSession {
public:
    bool isReady() const { return ready; }
    std::unique_ptr<VROARFrame> &getLastFrame() { return last_frame; }
    bool ready = true;
    std::unique_ptr<VROARFrame> last_frame;
};
