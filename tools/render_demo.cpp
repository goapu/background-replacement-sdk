#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "magic_portal/pipeline.h"

namespace {

constexpr float kPi = 3.14159265358979323846F;
using magic_portal::ConfidenceMask;
using magic_portal::MagicPortalPipeline;
using magic_portal::Rgba;
using magic_portal::RgbaFrame;

std::uint8_t byte(float value) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

RgbaFrame make_camera(int width, int height) {
    RgbaFrame frame(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width - 1);
            const float v = static_cast<float>(y) / static_cast<float>(height - 1);
            Rgba color{byte(67.0F + 62.0F * u), byte(48.0F + 50.0F * v), byte(35.0F + 38.0F * v), 255};
            if ((x / 35 + y / 35) % 2 == 0) {
                color.r = byte(static_cast<float>(color.r) + 12.0F);
            }
            const float head_x = (u - 0.5F) / 0.19F;
            const float head_y = (v - 0.33F) / 0.17F;
            const bool head = head_x * head_x + head_y * head_y <= 1.0F;
            const float body_x = (u - 0.5F) / 0.30F;
            const float body_y = (v - 0.72F) / 0.43F;
            const bool body = body_x * body_x + body_y * body_y <= 1.0F;
            const float hair_x = (u - 0.5F) / 0.235F;
            const float hair_y = (v - 0.30F) / 0.23F;
            const bool hair = hair_x * hair_x + hair_y * hair_y <= 1.0F && !head;
            if (body)
                color = {166, 62, 31, 255};
            if (hair)
                color = {72, 35, 22, 255};
            if (head) {
                color = {214, 157, 118, 255};
                const bool eye = std::abs(v - 0.32F) < 0.009F &&
                                 (std::abs(u - 0.45F) < 0.016F || std::abs(u - 0.55F) < 0.016F);
                const bool smile = v > 0.37F && v < 0.385F && std::abs(u - 0.5F) < 0.055F;
                if (eye)
                    color = {35, 25, 22, 255};
                else if (smile)
                    color = {125, 42, 45, 255};
            }
            frame.set_pixel(x, y, color);
        }
    }
    return frame;
}

ConfidenceMask make_noisy_mask(int width, int height) {
    ConfidenceMask mask(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
            const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
            const float head = 1.0F - std::sqrt(((u - 0.5F) / 0.22F) * ((u - 0.5F) / 0.22F) +
                                                ((v - 0.32F) / 0.22F) * ((v - 0.32F) / 0.22F));
            const float body = 1.0F - std::sqrt(((u - 0.5F) / 0.33F) * ((u - 0.5F) / 0.33F) +
                                                ((v - 0.72F) / 0.46F) * ((v - 0.72F) / 0.46F));
            const float edge = std::max(head, body);
            const float noise = 0.035F * std::sin(u * 71.0F + v * 113.0F);
            mask.set(x, y, std::clamp(0.5F + edge * 4.2F + noise, 0.0F, 1.0F));
        }
    }
    return mask;
}

RgbaFrame make_future_background(int width, int height) {
    RgbaFrame frame(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width - 1);
            const float v = static_cast<float>(y) / static_cast<float>(height - 1);
            const float grid = 0.5F + 0.5F * std::sin(u * 16.0F * kPi) * std::sin(v * 12.0F * kPi);
            frame.set_pixel(x, y,
                            {byte(21.0F + 28.0F * grid), byte(42.0F + 54.0F * (1.0F - v)),
                             byte(104.0F + 105.0F * u), 255});
        }
    }
    return frame;
}

void write_ppm(const RgbaFrame &frame, const std::filesystem::path &path) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("failed to open demo output: " + path.string());
    stream << "P6\n" << frame.width() << ' ' << frame.height() << "\n255\n";
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            const Rgba pixel = frame.pixel(x, y);
            const char rgb[3]{static_cast<char>(pixel.r), static_cast<char>(pixel.g),
                              static_cast<char>(pixel.b)};
            stream.write(rgb, 3);
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    try {
        const std::filesystem::path output_dir = argc > 1 ? argv[1] : "demo_output";
        std::filesystem::create_directories(output_dir);
        constexpr int width = 360;
        constexpr int height = 540;
        const RgbaFrame camera = make_camera(width, height);
        const ConfidenceMask mask = make_noisy_mask(96, 144);
        const RgbaFrame future = make_future_background(width, height);
        MagicPortalPipeline pipeline;
        pipeline.start_transition(0);
        for (int frame_index = 0; frame_index < 7; ++frame_index) {
            const std::int64_t timestamp = static_cast<std::int64_t>(frame_index) * 300;
            const auto result = pipeline.process(camera, mask, nullptr, future, timestamp);
            const std::filesystem::path path =
                output_dir / ("portal_" + std::to_string(frame_index) + ".ppm");
            write_ppm(result.frame, path);
            std::cout << path.string() << '\n';
        }
    } catch (const std::exception &exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    return 0;
}
