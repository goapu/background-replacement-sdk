// Command-line front end of magic_portal_eval. It lives in a header so the unit tests drive
// exactly the code the tool runs.
//
//   magic_portal_eval                       synthetic sequences (deterministic, runs in CI)
//   magic_portal_eval --gate                same, but exits non-zero if a quality gate fails
//   magic_portal_eval --dataset <dir> [thresholds]
//                                           your own labelled footage (see below)
//
// Dataset layout. <dir> is either one clip or a folder of clips (one sub-folder per clip, so
// unrelated recordings are never smoothed into each other):
//   <name>.ppm        the camera frame   (binary PPM, P6)
//   <name>_pred.pgm   the model's person probability, 0..255 (binary PGM, P5)
//   <name>_gt.pgm     ground-truth matte, 0..255            (binary PGM, P5)
// Frames of a clip are processed in name order. Within a clip every prediction and ground
// truth must share one size; the camera frame may be any size.
//
// Dataset thresholds (all evaluated on the refined mask, averaged over frames):
//   --min-iou <0..1>           mean IoU must be at least this
//   --min-boundary-f1 <0..1>   mean boundary F1 must be at least this
//   --max-flicker <value>      mean frame-to-frame change must be at most this
//   --max-iou-loss <0..1>      refinement may lose at most this much IoU against the raw mask
// Any threshold turns gating on, and `--gate` with a dataset requires at least one threshold:
// the right values depend on the model and the footage, so none are assumed. Exit codes:
// 0 all checks passed, 1 a check failed or the data could not be used, 2 bad usage.
//
// The synthetic mode shows that the refiner behaves as intended on controlled noise. It says
// nothing about accuracy on real people.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "eval_metrics.h"
#include "golden_support.h"
#include "magic_portal/mask_refiner.h"

namespace mp_eval {

using magic_portal::MaskRefiner;
using magic_portal::RgbaFrame;

struct Series {
    std::vector<RgbaFrame> frames;
    std::vector<ConfidenceMask> predictions;
    std::vector<ConfidenceMask> truth;
};

struct Scores {
    double iou = 0.0;
    double boundary_f1 = 0.0;
    double flicker = 0.0;
};

struct Comparison {
    Scores raw;
    Scores refined;
    std::size_t frames = 0;
};

inline Scores score(const std::vector<ConfidenceMask> &prediction, const std::vector<ConfidenceMask> &truth) {
    Scores s;
    for (std::size_t i = 0; i < prediction.size(); ++i) {
        s.iou += iou(prediction[i], truth[i]);
        s.boundary_f1 += boundary_f1(prediction[i], truth[i], 0.5F, 1);
    }
    s.iou /= static_cast<double>(prediction.size());
    s.boundary_f1 /= static_cast<double>(prediction.size());
    s.flicker = temporal_flicker(prediction);
    return s;
}

inline Comparison evaluate(const Series &series) {
    MaskRefiner refiner;
    std::vector<ConfidenceMask> raw;
    std::vector<ConfidenceMask> refined;
    for (std::size_t i = 0; i < series.frames.size(); ++i) {
        raw.push_back(series.predictions[i]);
        refined.push_back(
            refiner.refine(series.frames[i], series.predictions[i], static_cast<std::int64_t>(i) * 66).alpha);
    }
    return {score(raw, series.truth), score(refined, series.truth), series.frames.size()};
}

// ---- synthetic data ----------------------------------------------------------------------

inline ConfidenceMask truth_mask(int w, int h, float cx, float cy, float rx, float ry) {
    ConfidenceMask mask(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float du = ((static_cast<float>(x) + 0.5F) / static_cast<float>(w) - cx) / rx;
            const float dv = ((static_cast<float>(y) + 0.5F) / static_cast<float>(h) - cy) / ry;
            mask.set(x, y, du * du + dv * dv <= 1.0F ? 1.0F : 0.0F);
        }
    }
    return mask;
}

inline Series synthetic(bool moving, std::uint32_t seed) {
    constexpr int kFrames = 40;
    constexpr int kModelW = 48;
    constexpr int kModelH = 64;
    Series series;
    std::uint32_t state = seed;
    for (int f = 0; f < kFrames; ++f) {
        const float cx = moving ? 0.35F + 0.30F * static_cast<float>(f) / (kFrames - 1) : 0.5F;
        const ConfidenceMask gt = truth_mask(kModelW, kModelH, cx, 0.55F, 0.22F, 0.36F);
        RgbaFrame camera(96, 128);
        for (int y = 0; y < 128; ++y) {
            for (int x = 0; x < 96; ++x) {
                const bool inside = gt.at(x / 2, y / 2) >= 0.5F;
                const int noise = static_cast<int>(mp_test::lcg(state) >> 29);
                camera.set_pixel(x, y,
                                 {static_cast<std::uint8_t>((inside ? 200 : 40) + noise),
                                  static_cast<std::uint8_t>((inside ? 150 : 60) + noise),
                                  static_cast<std::uint8_t>((inside ? 120 : 90) + noise), 255});
            }
        }
        // A weak model: soft edge, per-frame noise, and an occasional spurious corner blob.
        ConfidenceMask pred(kModelW, kModelH);
        for (int y = 0; y < kModelH; ++y) {
            for (int x = 0; x < kModelW; ++x) {
                const float du = ((static_cast<float>(x) + 0.5F) / kModelW - cx) / 0.22F;
                const float dv = ((static_cast<float>(y) + 0.5F) / kModelH - 0.55F) / 0.36F;
                const float distance = std::sqrt(du * du + dv * dv);
                const float noise = (static_cast<float>(mp_test::lcg(state) >> 24) / 255.0F - 0.5F) * 0.5F;
                float value = (1.25F - distance) / 0.5F + noise;
                if (f % 5 == 2 && x < 6 && y < 6) {
                    value = 0.9F;
                }
                pred.set(x, y, value);
            }
        }
        series.frames.push_back(std::move(camera));
        series.predictions.push_back(std::move(pred));
        series.truth.push_back(gt);
    }
    return series;
}

// ---- datasets ----------------------------------------------------------------------------

inline bool read_pnm(const std::filesystem::path &path, const char *magic, int *w, int *h,
                     std::vector<std::uint8_t> *pixels) {
    std::ifstream in(path, std::ios::binary);
    std::string m;
    int maxval = 0;
    if (!(in >> m >> *w >> *h >> maxval) || m != magic || maxval != 255 || *w <= 0 || *h <= 0 || *w > 16384 ||
        *h > 16384) {
        return false;
    }
    in.get();
    const std::size_t channels = std::strcmp(magic, "P6") == 0 ? 3U : 1U;
    pixels->resize(static_cast<std::size_t>(*w) * static_cast<std::size_t>(*h) * channels);
    in.read(reinterpret_cast<char *>(pixels->data()), static_cast<std::streamsize>(pixels->size()));
    return static_cast<std::size_t>(in.gcount()) == pixels->size();
}

inline bool load_clip(const std::vector<std::filesystem::path> &frames, Series *series, std::string *error) {
    for (const auto &frame_path : frames) {
        const std::string stem = frame_path.stem().string();
        int fw = 0, fh = 0, pw = 0, ph = 0, gw = 0, gh = 0;
        std::vector<std::uint8_t> rgb, pred, gt;
        if (!read_pnm(frame_path, "P6", &fw, &fh, &rgb) ||
            !read_pnm(frame_path.parent_path() / (stem + "_pred.pgm"), "P5", &pw, &ph, &pred) ||
            !read_pnm(frame_path.parent_path() / (stem + "_gt.pgm"), "P5", &gw, &gh, &gt)) {
            *error = "could not read the frame/prediction/ground-truth files for " + stem;
            return false;
        }
        if (pw != gw || ph != gh) {
            *error = "prediction and ground truth differ in size for " + stem;
            return false;
        }
        if (!series->predictions.empty() &&
            (series->predictions.front().width() != pw || series->predictions.front().height() != ph)) {
            *error = "masks within one clip must share a size; " + stem + " differs";
            return false;
        }
        RgbaFrame camera(fw, fh);
        for (int y = 0; y < fh; ++y) {
            for (int x = 0; x < fw; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(fw) +
                                       static_cast<std::size_t>(x)) *
                                      3U;
                camera.set_pixel(x, y, {rgb[i], rgb[i + 1], rgb[i + 2], 255});
            }
        }
        ConfidenceMask p(pw, ph);
        ConfidenceMask g(gw, gh);
        for (std::size_t i = 0; i < pred.size(); ++i) {
            p.values()[i] = static_cast<float>(pred[i]) / 255.0F;
            g.values()[i] = static_cast<float>(gt[i]) / 255.0F;
        }
        series->frames.push_back(std::move(camera));
        series->predictions.push_back(std::move(p));
        series->truth.push_back(std::move(g));
    }
    return true;
}

struct Clip {
    std::string name;
    Series series;
};

inline std::vector<std::filesystem::path> ppm_files(const std::filesystem::path &dir) {
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".ppm") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

inline bool load_dataset(const std::string &dir, std::vector<Clip> *clips, std::string *error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        *error = "not a directory: " + dir;
        return false;
    }
    const std::filesystem::path root(dir);
    std::vector<std::filesystem::path> folders;
    for (const auto &entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_directory()) {
            folders.push_back(entry.path());
        }
    }
    std::sort(folders.begin(), folders.end());
    const std::vector<std::filesystem::path> root_frames = ppm_files(root);
    if (!root_frames.empty()) {
        Clip clip;
        clip.name = ".";
        if (!load_clip(root_frames, &clip.series, error)) {
            return false;
        }
        clips->push_back(std::move(clip));
    }
    for (const auto &folder : folders) {
        const std::vector<std::filesystem::path> frames = ppm_files(folder);
        if (frames.empty()) {
            continue;
        }
        Clip clip;
        clip.name = folder.filename().string();
        if (!load_clip(frames, &clip.series, error)) {
            return false;
        }
        clips->push_back(std::move(clip));
    }
    if (clips->empty()) {
        *error = "no .ppm frames found in " + dir;
        return false;
    }
    return true;
}

// ---- reporting ---------------------------------------------------------------------------

inline void print(std::FILE *out, const char *label, const Comparison &c) {
    if (out == nullptr) {
        return;
    }
    std::fprintf(
        out,
        "%-22s | raw: IoU %.3f  boundaryF1 %.3f  flicker %.4f | refined: IoU %.3f  boundaryF1 %.3f  flicker "
        "%.4f\n",
        label, c.raw.iou, c.raw.boundary_f1, c.raw.flicker, c.refined.iou, c.refined.boundary_f1,
        c.refined.flicker);
}

// Frame-weighted mean of the per-clip results (flicker is weighted by the number of
// frame pairs, which is what it averages over).
inline Comparison aggregate(const std::vector<Comparison> &parts) {
    Comparison total;
    double pair_weight = 0.0;
    for (const Comparison &part : parts) {
        const double w = static_cast<double>(part.frames);
        const double pairs = part.frames > 1 ? static_cast<double>(part.frames - 1) : 0.0;
        total.raw.iou += part.raw.iou * w;
        total.raw.boundary_f1 += part.raw.boundary_f1 * w;
        total.refined.iou += part.refined.iou * w;
        total.refined.boundary_f1 += part.refined.boundary_f1 * w;
        total.raw.flicker += part.raw.flicker * pairs;
        total.refined.flicker += part.refined.flicker * pairs;
        pair_weight += pairs;
        total.frames += part.frames;
    }
    const double frames = static_cast<double>(total.frames);
    if (frames > 0.0) {
        total.raw.iou /= frames;
        total.raw.boundary_f1 /= frames;
        total.refined.iou /= frames;
        total.refined.boundary_f1 /= frames;
    }
    if (pair_weight > 0.0) {
        total.raw.flicker /= pair_weight;
        total.refined.flicker /= pair_weight;
    }
    return total;
}

// ---- command line ------------------------------------------------------------------------

struct Thresholds {
    bool has_min_iou = false;
    bool has_min_boundary_f1 = false;
    bool has_max_flicker = false;
    bool has_max_iou_loss = false;
    double min_iou = 0.0;
    double min_boundary_f1 = 0.0;
    double max_flicker = 0.0;
    double max_iou_loss = 0.0;
    [[nodiscard]] bool any() const {
        return has_min_iou || has_min_boundary_f1 || has_max_flicker || has_max_iou_loss;
    }
};

inline bool parse_number(const char *text, double lo, double hi, double *value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char *end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed) || parsed < lo || parsed > hi) {
        return false;
    }
    *value = parsed;
    return true;
}

// Returns the process exit code.
inline int run_cli(const std::vector<std::string> &args, std::FILE *out, std::FILE *err) {
    const auto say = [err](const char *format, const std::string &text) {
        if (err != nullptr) {
            std::fprintf(err, format, text.c_str());
        }
    };
    const char *usage =
        "usage: magic_portal_eval [--gate]\n"
        "       magic_portal_eval --dataset <dir> [--gate] [--min-iou X] [--min-boundary-f1 X]\n"
        "                         [--max-flicker X] [--max-iou-loss X]\n";
    bool gate = false;
    std::string dataset;
    Thresholds limits;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string &arg = args[i];
        const bool has_value = i + 1 < args.size();
        double value = 0.0;
        if (arg == "--gate") {
            gate = true;
        } else if (arg == "--dataset" && has_value) {
            dataset = args[++i];
        } else if (arg == "--min-iou" && has_value && parse_number(args[i + 1].c_str(), 0.0, 1.0, &value)) {
            limits.has_min_iou = true;
            limits.min_iou = value;
            ++i;
        } else if (arg == "--min-boundary-f1" && has_value &&
                   parse_number(args[i + 1].c_str(), 0.0, 1.0, &value)) {
            limits.has_min_boundary_f1 = true;
            limits.min_boundary_f1 = value;
            ++i;
        } else if (arg == "--max-flicker" && has_value &&
                   parse_number(args[i + 1].c_str(), 0.0, 1.0, &value)) {
            limits.has_max_flicker = true;
            limits.max_flicker = value;
            ++i;
        } else if (arg == "--max-iou-loss" && has_value &&
                   parse_number(args[i + 1].c_str(), 0.0, 1.0, &value)) {
            limits.has_max_iou_loss = true;
            limits.max_iou_loss = value;
            ++i;
        } else {
            say("%s", usage);
            return 2;
        }
    }
    if (dataset.empty() && limits.any()) {
        say("%s", std::string("threshold options need --dataset\n") + usage);
        return 2;
    }
    if (!dataset.empty() && gate && !limits.any()) {
        say("%s", std::string("--gate with --dataset needs at least one threshold (--min-iou, "
                              "--min-boundary-f1, --max-flicker, --max-iou-loss)\n"));
        return 2;
    }

    try {
        if (!dataset.empty()) {
            std::vector<Clip> clips;
            std::string error;
            if (!load_dataset(dataset, &clips, &error)) {
                say("error: %s\n", error);
                return 1;
            }
            std::vector<Comparison> parts;
            for (const Clip &clip : clips) {
                const Comparison result = evaluate(clip.series);
                print(out, clip.name.c_str(), result);
                parts.push_back(result);
            }
            const Comparison total = aggregate(parts);
            print(out, clips.size() == 1 ? "dataset" : "dataset (all clips)", total);
            if (!limits.any()) {
                return 0;
            }
            bool ok = true;
            const auto require = [&ok, err](bool condition, const char *message, double actual) {
                if (!condition) {
                    if (err != nullptr) {
                        std::fprintf(err, "GATE FAILED: %s (measured %.4f)\n", message, actual);
                    }
                    ok = false;
                }
            };
            if (limits.has_min_iou) {
                require(total.refined.iou >= limits.min_iou, "refined IoU is below --min-iou",
                        total.refined.iou);
            }
            if (limits.has_min_boundary_f1) {
                require(total.refined.boundary_f1 >= limits.min_boundary_f1,
                        "refined boundary F1 is below --min-boundary-f1", total.refined.boundary_f1);
            }
            if (limits.has_max_flicker) {
                require(total.refined.flicker <= limits.max_flicker, "refined flicker is above --max-flicker",
                        total.refined.flicker);
            }
            if (limits.has_max_iou_loss) {
                require(total.refined.iou >= total.raw.iou - limits.max_iou_loss,
                        "refinement lost more IoU than --max-iou-loss", total.raw.iou - total.refined.iou);
            }
            if (out != nullptr) {
                std::fprintf(out, "%s\n", ok ? "quality gates passed" : "quality gates FAILED");
            }
            return ok ? 0 : 1;
        }

        const Comparison still = evaluate(synthetic(false, 5));
        const Comparison moving = evaluate(synthetic(true, 9));
        print(out, "synthetic, static", still);
        print(out, "synthetic, moving", moving);
        if (out != nullptr) {
            std::fprintf(out, "(synthetic data only; it does not measure accuracy on real people)\n");
        }
        if (!gate) {
            return 0;
        }
        bool ok = true;
        const auto require = [&ok, err](bool condition, const char *message) {
            if (!condition) {
                if (err != nullptr) {
                    std::fprintf(err, "GATE FAILED: %s\n", message);
                }
                ok = false;
            }
        };
        require(still.refined.flicker <= 0.6 * still.raw.flicker,
                "refinement must cut static-subject flicker to at most 60% of the raw mask");
        require(still.refined.iou >= still.raw.iou - 0.02, "refinement must not lose IoU (static)");
        require(moving.refined.iou >= moving.raw.iou - 0.03, "refinement must not lose IoU (moving)");
        require(moving.refined.boundary_f1 >= moving.raw.boundary_f1 - 0.03,
                "refinement must not lose boundary accuracy (moving)");
        require(still.refined.iou >= 0.80, "absolute floor: refined IoU on the static clip >= 0.80");
        if (out != nullptr) {
            std::fprintf(out, "%s\n", ok ? "quality gates passed" : "quality gates FAILED");
        }
        return ok ? 0 : 1;
    } catch (const std::exception &exception) {
        say("error: %s\n", std::string(exception.what()));
        return 1;
    }
}

}  // namespace mp_eval
