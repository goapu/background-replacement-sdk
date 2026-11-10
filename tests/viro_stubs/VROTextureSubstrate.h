#pragma once
#include <stdexcept>
#include <vector>
class VROTextureSubstrate {
public:
    void updateR32FData(const float *data, int width, int height) {
        if (throw_on_upload) {
            throw std::runtime_error("stub upload failure");
        }
        ++uploads;
        last_width = width;
        last_height = height;
        last_data.assign(data, data + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    }
    int uploads = 0;
    int last_width = 0;
    int last_height = 0;
    std::vector<float> last_data;
    bool throw_on_upload = false;
};
