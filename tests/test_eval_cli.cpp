// The evaluation tool's dataset mode: it must gate on real thresholds and refuse to pass
// silently. Fixtures are written to a scratch folder and removed afterwards.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "eval_cli.h"
#include "test_harness.h"

namespace {

namespace fs = std::filesystem;

struct ScratchDir {
    ScratchDir() {
        path = fs::temp_directory_path() /
               ("mp_eval_cli_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

void write_pnm(const fs::path &file, const char *magic, int w, int h, const std::vector<std::uint8_t> &data) {
    std::ofstream out(file, std::ios::binary);
    out << magic << "\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

// One frame whose prediction and ground truth are the given rectangles (x0, y0, x1, y1).
void write_frame(const fs::path &dir, const std::string &name, int x0, int y0, int x1, int y1, int px0,
                 int py0, int px1, int py1, int mask_size = 16) {
    fs::create_directories(dir);
    write_pnm(dir / (name + ".ppm"), "P6", 8, 8, std::vector<std::uint8_t>(8U * 8U * 3U, 90));
    const auto side = static_cast<std::size_t>(mask_size);
    std::vector<std::uint8_t> truth(side * side, 0);
    std::vector<std::uint8_t> prediction(truth.size(), 0);
    for (int y = 0; y < mask_size; ++y) {
        for (int x = 0; x < mask_size; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * side + static_cast<std::size_t>(x);
            truth[i] = (x >= x0 && x < x1 && y >= y0 && y < y1) ? 255 : 0;
            prediction[i] = (x >= px0 && x < px1 && y >= py0 && y < py1) ? 255 : 0;
        }
    }
    write_pnm(dir / (name + "_gt.pgm"), "P5", mask_size, mask_size, truth);
    write_pnm(dir / (name + "_pred.pgm"), "P5", mask_size, mask_size, prediction);
}

void write_good_clip(const fs::path &dir) {
    for (int i = 0; i < 4; ++i) {
        write_frame(dir, "f" + std::to_string(i), 4, 3, 12, 14, 4, 3, 12, 14);  // prediction == truth
    }
}

void write_opposite_clip(const fs::path &dir) {
    for (int i = 0; i < 4; ++i) {
        write_frame(dir, "f" + std::to_string(i), 0, 0, 0, 0, 0, 0, 16, 16);  // truth empty, prediction full
    }
}

int run(const std::vector<std::string> &args) {
    return mp_eval::run_cli(args, nullptr, nullptr);
}

}  // namespace

MT_TEST("eval cli: a good dataset passes its thresholds") {
    ScratchDir scratch;
    write_good_clip(scratch.path);
    MT_CHECK(run({"--dataset", scratch.path.string()}) == 0);  // report only
    MT_CHECK(run({"--dataset", scratch.path.string(), "--gate", "--min-iou", "0.8", "--min-boundary-f1",
                  "0.8"}) == 0);
}

MT_TEST("eval cli: a bad dataset fails the gate instead of exiting 0") {
    ScratchDir scratch;
    write_opposite_clip(scratch.path);  // the model says "everyone" where the truth says "nobody"
    MT_CHECK(run({"--dataset", scratch.path.string()}) == 0);  // still just a report
    MT_CHECK(run({"--dataset", scratch.path.string(), "--gate", "--min-iou", "0.5"}) == 1);
    MT_CHECK(run({"--dataset", scratch.path.string(), "--min-boundary-f1", "0.5"}) ==
             1);  // threshold implies gating
    MT_CHECK(run({"--dataset", scratch.path.string(), "--max-flicker", "0.0001", "--min-iou", "0.5"}) == 1);
}

MT_TEST("eval cli: --gate on a dataset needs a threshold and bad usage is rejected") {
    ScratchDir scratch;
    write_good_clip(scratch.path);
    MT_CHECK(run({"--dataset", scratch.path.string(), "--gate"}) == 2);
    MT_CHECK(run({"--gate", "--min-iou", "0.5"}) == 2);  // thresholds need a dataset
    MT_CHECK(run({"--dataset", scratch.path.string(), "--min-iou", "1.5"}) == 2);
    MT_CHECK(run({"--dataset", scratch.path.string(), "--min-iou", "nan"}) == 2);
    MT_CHECK(run({"--dataset", scratch.path.string(), "--min-iou"}) == 2);
    MT_CHECK(run({"--unknown"}) == 2);
}

MT_TEST("eval cli: clips are evaluated separately and one bad clip fails the dataset") {
    ScratchDir scratch;
    write_good_clip(scratch.path / "a_good");
    write_good_clip(scratch.path / "b_good");
    MT_CHECK(run({"--dataset", scratch.path.string(), "--gate", "--min-iou", "0.9"}) == 0);
    write_opposite_clip(scratch.path / "c_bad");
    MT_CHECK(run({"--dataset", scratch.path.string(), "--gate", "--min-iou", "0.9"}) == 1);
}

MT_TEST("eval cli: unusable data is an error, not a pass") {
    ScratchDir missing;
    MT_CHECK(run({"--dataset", (missing.path / "does_not_exist").string(), "--min-iou", "0.1"}) == 1);
    ScratchDir empty;
    MT_CHECK(run({"--dataset", empty.path.string(), "--min-iou", "0.1"}) == 1);

    ScratchDir broken;
    write_good_clip(broken.path);
    fs::remove(broken.path / "f2_pred.pgm");
    MT_CHECK(run({"--dataset", broken.path.string(), "--min-iou", "0.1"}) == 1);

    ScratchDir mixed;  // masks of different sizes inside one clip
    write_frame(mixed.path, "f0", 4, 3, 12, 14, 4, 3, 12, 14, 16);
    write_frame(mixed.path, "f1", 2, 2, 6, 6, 2, 2, 6, 6, 8);
    MT_CHECK(run({"--dataset", mixed.path.string(), "--min-iou", "0.1"}) == 1);
}
