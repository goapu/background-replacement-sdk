#pragma once
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "VRODriver.h"
#include "VROShaderModifier.h"
#include "VROTexture.h"
#include "VROVector4f.h"
class VROMaterial {
public:
    void addShaderModifier(std::shared_ptr<VROShaderModifier> m) { modifiers.push_back(std::move(m)); }
    void removeShaderModifier(std::shared_ptr<VROShaderModifier> m) {
        modifiers.erase(std::remove(modifiers.begin(), modifiers.end(), m), modifiers.end());
    }
    void setShaderUniform(const std::string &n, float v) { floats[n] = v; }
    void setShaderUniform(const std::string &n, VROVector4f v) { vec4s[n] = v; }
    void setShaderUniform(const std::string &n, std::shared_ptr<VROTexture> t) { textures[n] = std::move(t); }
    void prewarm(std::shared_ptr<VRODriver>) { ++prewarms; }
    // Inspection helpers for tests.
    float get_float(const std::string &n) const { return floats.at(n); }
    VROVector4f get_vec4(const std::string &n) const { return vec4s.at(n); }
    std::shared_ptr<VROTexture> get_texture(const std::string &n) const { return textures.at(n); }
    std::vector<std::shared_ptr<VROShaderModifier>> modifiers;
    std::map<std::string, float> floats;
    std::map<std::string, VROVector4f> vec4s;
    std::map<std::string, std::shared_ptr<VROTexture>> textures;
    int prewarms = 0;
};
