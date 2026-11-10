// Segmentation quality metrics used by the evaluation tool and its tests.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "magic_portal/frame.h"

namespace mp_eval {

using magic_portal::ConfidenceMask;

inline void require_same_size(const ConfidenceMask &a, const ConfidenceMask &b) {
    if (a.width() != b.width() || a.height() != b.height() || a.empty()) {
        throw std::invalid_argument("masks must be non-empty and the same size");
    }
}

// Intersection over union of the pixels at or above `threshold`. Two empty foregrounds
// count as a perfect match.
inline double iou(const ConfidenceMask &prediction, const ConfidenceMask &truth, float threshold = 0.5F) {
    require_same_size(prediction, truth);
    std::size_t both = 0;
    std::size_t either = 0;
    for (std::size_t i = 0; i < prediction.size(); ++i) {
        const bool p = prediction.values()[i] >= threshold;
        const bool t = truth.values()[i] >= threshold;
        both += (p && t) ? 1U : 0U;
        either += (p || t) ? 1U : 0U;
    }
    return either == 0 ? 1.0 : static_cast<double>(both) / static_cast<double>(either);
}

// Foreground pixels that touch background. The frame border counts as background, so a mask
// that fills the whole frame has a boundary (its outline) and cannot be mistaken for an empty
// one: without this, "everything" and "nothing" would both have no boundary at all and
// compare as identical.
inline std::vector<char> boundary_pixels(const ConfidenceMask &mask, float threshold) {
    const int w = mask.width();
    const int h = mask.height();
    std::vector<char> boundary(mask.size(), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!(mask.at(x, y) >= threshold)) {
                continue;
            }
            const int dx[4] = {1, -1, 0, 0};
            const int dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k];
                const int ny = y + dy[k];
                const bool neighbour_inside =
                    nx >= 0 && ny >= 0 && nx < w && ny < h && mask.at(nx, ny) >= threshold;
                if (!neighbour_inside) {
                    boundary[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x)] = 1;
                    break;
                }
            }
        }
    }
    return boundary;
}

inline bool near_boundary(const std::vector<char> &boundary, int w, int h, int x, int y, int tol) {
    for (int dy = -tol; dy <= tol; ++dy) {
        for (int dx = -tol; dx <= tol; ++dx) {
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < w && ny < h &&
                boundary[static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) +
                         static_cast<std::size_t>(nx)] != 0) {
                return true;
            }
        }
    }
    return false;
}

// F1 of boundary pixels matched within `tolerance_px` (a contour-accuracy measure that is
// sensitive to ragged or displaced edges where IoU barely moves). Two masks with no boundary
// at all (both empty) match perfectly; a mask with a boundary never matches one without.
inline double boundary_f1(const ConfidenceMask &prediction, const ConfidenceMask &truth,
                          float threshold = 0.5F, int tolerance_px = 1) {
    require_same_size(prediction, truth);
    const int w = prediction.width();
    const int h = prediction.height();
    const std::vector<char> pb = boundary_pixels(prediction, threshold);
    const std::vector<char> tb = boundary_pixels(truth, threshold);
    std::size_t pred_total = 0;
    std::size_t pred_hit = 0;
    std::size_t truth_total = 0;
    std::size_t truth_hit = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            if (pb[i] != 0) {
                ++pred_total;
                pred_hit += near_boundary(tb, w, h, x, y, tolerance_px) ? 1U : 0U;
            }
            if (tb[i] != 0) {
                ++truth_total;
                truth_hit += near_boundary(pb, w, h, x, y, tolerance_px) ? 1U : 0U;
            }
        }
    }
    if (pred_total == 0 && truth_total == 0) {
        return 1.0;
    }
    if (pred_total == 0 || truth_total == 0) {
        return 0.0;
    }
    const double precision = static_cast<double>(pred_hit) / static_cast<double>(pred_total);
    const double recall = static_cast<double>(truth_hit) / static_cast<double>(truth_total);
    return precision + recall == 0.0 ? 0.0 : 2.0 * precision * recall / (precision + recall);
}

// Mean absolute change per pixel between consecutive masks. For a static subject, a low
// value means a stable (non-flickering) matte.
inline double temporal_flicker(const std::vector<ConfidenceMask> &sequence) {
    if (sequence.size() < 2) {
        return 0.0;
    }
    double total = 0.0;
    for (std::size_t f = 1; f < sequence.size(); ++f) {
        require_same_size(sequence[f], sequence[f - 1]);
        double sum = 0.0;
        for (std::size_t i = 0; i < sequence[f].size(); ++i) {
            sum += std::fabs(static_cast<double>(sequence[f].values()[i]) -
                             static_cast<double>(sequence[f - 1].values()[i]));
        }
        total += sum / static_cast<double>(sequence[f].size());
    }
    return total / static_cast<double>(sequence.size() - 1);
}

}  // namespace mp_eval
