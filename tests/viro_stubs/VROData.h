#pragma once
#include <cstdint>
#include <vector>
class VROData {
public:
    VROData(const void *data, int size)
        : bytes(static_cast<const std::uint8_t *>(data), static_cast<const std::uint8_t *>(data) + size) {}
    std::vector<std::uint8_t> bytes;
};
