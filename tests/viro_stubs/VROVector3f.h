#pragma once
struct VROVector3f {
    float x = 0, y = 0, z = 0;
    VROVector3f() = default;
    VROVector3f(float ix, float iy, float iz) : x(ix), y(iy), z(iz) {}
};
