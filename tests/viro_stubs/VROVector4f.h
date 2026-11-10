#pragma once
struct VROVector4f {
    float x = 0, y = 0, z = 0, w = 0;
    VROVector4f() = default;
    VROVector4f(float ix, float iy, float iz, float iw) : x(ix), y(iy), z(iz), w(iw) {}
};
