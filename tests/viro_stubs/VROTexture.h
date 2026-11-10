#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "VROData.h"
#include "VRODriver.h"
#include "VROTextureSubstrate.h"
enum class VROTextureType { Texture2D };
enum class VROTextureFormat { R32F };
enum class VROTextureInternalFormat { R32F };
enum class VROMipmapMode { None };
enum class VROFilterMode { Nearest, Linear };
enum class VROWrapMode { Clamp };
class VROTexture {
public:
    VROTexture(VROTextureType, VROTextureFormat, VROTextureInternalFormat, bool, VROMipmapMode,
               std::vector<std::shared_ptr<VROData>> levels, int w, int h, std::vector<std::uint32_t>)
        : width(w), height(h), initial_levels(std::move(levels)) {}
    void setName(const std::string &n) { name = n; }
    void setMinificationFilter(VROFilterMode m) { min_filter = m; }
    void setMagnificationFilter(VROFilterMode m) { mag_filter = m; }
    void setWrapS(VROWrapMode) {}
    void setWrapT(VROWrapMode) {}
    VROTextureSubstrate *getSubstrate(int, std::shared_ptr<VRODriver> &, bool) {
        return fail_substrate ? nullptr : &substrate;
    }
    int width, height;
    std::vector<std::shared_ptr<VROData>> initial_levels;
    std::string name;
    VROFilterMode min_filter = VROFilterMode::Nearest;
    VROFilterMode mag_filter = VROFilterMode::Nearest;
    VROTextureSubstrate substrate;
    bool fail_substrate = false;
};
