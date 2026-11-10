#pragma once
#include <string>
#include <vector>
enum class VROShaderEntryPoint { Fragment };
class VROShaderModifier {
public:
    VROShaderModifier(VROShaderEntryPoint, std::vector<std::string> l) : lines(std::move(l)) {}
    void setName(const std::string &n) { name = n; }
    void setPriority(int p) { priority = p; }
    void setRequiresCameraTexture(bool b) { requires_camera = b; }
    std::vector<std::string> lines;
    std::string name;
    int priority = 0;
    bool requires_camera = false;
};
