#pragma once
struct VRORenderContext {};
class VROFrameListener {
public:
    virtual ~VROFrameListener() = default;
    virtual void onFrameWillRender(const VRORenderContext &context) = 0;
    virtual void onFrameDidRender(const VRORenderContext &context) = 0;
};
