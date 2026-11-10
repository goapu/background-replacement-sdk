#include "eval_metrics.h"
#include "test_harness.h"

using magic_portal::ConfidenceMask;

namespace {

ConfidenceMask rect(int w, int h, int x0, int y0, int x1, int y1) {
    ConfidenceMask mask(w, h);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            mask.set(x, y, 1.0F);
        }
    }
    return mask;
}

}  // namespace

MT_TEST("eval: iou of identical, disjoint and half-overlapping masks") {
    const ConfidenceMask a = rect(10, 10, 0, 0, 5, 10);
    MT_CHECK_NEAR(mp_eval::iou(a, a), 1.0, 1e-12);
    MT_CHECK_NEAR(mp_eval::iou(a, rect(10, 10, 5, 0, 10, 10)), 0.0, 1e-12);
    MT_CHECK_NEAR(mp_eval::iou(rect(10, 10, 0, 0, 6, 10), rect(10, 10, 0, 0, 3, 10)), 0.5, 1e-12);
    MT_CHECK_NEAR(mp_eval::iou(ConfidenceMask(4, 4), ConfidenceMask(4, 4)), 1.0, 1e-12);
    MT_CHECK_THROWS(mp_eval::iou(ConfidenceMask(4, 4), ConfidenceMask(5, 4)), std::invalid_argument);
}

MT_TEST("eval: boundary F1 rewards displaced-by-one edges and punishes far ones") {
    const ConfidenceMask truth = rect(40, 40, 10, 10, 30, 30);
    MT_CHECK_NEAR(mp_eval::boundary_f1(truth, truth, 0.5F, 1), 1.0, 1e-12);
    MT_CHECK(mp_eval::boundary_f1(rect(40, 40, 11, 10, 31, 30), truth, 0.5F, 1) > 0.9);
    MT_CHECK(mp_eval::boundary_f1(rect(40, 40, 16, 16, 36, 36), truth, 0.5F, 1) < 0.5);
    MT_CHECK_NEAR(
        mp_eval::boundary_f1(ConfidenceMask(8, 8), truth.width() == 40 ? ConfidenceMask(8, 8) : truth), 1.0,
        1e-12);
}

MT_TEST("eval: boundary F1 never rewards opposite masks") {
    const ConfidenceMask full = rect(16, 16, 0, 0, 16, 16);
    const ConfidenceMask none(16, 16);
    MT_CHECK_NEAR(mp_eval::boundary_f1(full, none), 0.0, 1e-12);  // everything vs nothing
    MT_CHECK_NEAR(mp_eval::boundary_f1(none, full), 0.0, 1e-12);
    MT_CHECK_NEAR(mp_eval::boundary_f1(full, full), 1.0, 1e-12);
    MT_CHECK_NEAR(mp_eval::boundary_f1(none, none), 1.0, 1e-12);
    // A mask that touches the frame border still has to match where its edge is.
    const ConfidenceMask left = rect(16, 16, 0, 0, 8, 16);
    const ConfidenceMask right = rect(16, 16, 8, 0, 16, 16);
    MT_CHECK(mp_eval::boundary_f1(left, right, 0.5F, 0) < 0.6);
}

MT_TEST("eval: flicker is zero for a static sequence and grows with change") {
    std::vector<ConfidenceMask> still(5, rect(8, 8, 2, 2, 6, 6));
    MT_CHECK_NEAR(mp_eval::temporal_flicker(still), 0.0, 1e-12);
    std::vector<ConfidenceMask> blinking;
    for (int i = 0; i < 4; ++i) {
        blinking.push_back(i % 2 == 0 ? rect(8, 8, 0, 0, 8, 8) : ConfidenceMask(8, 8));
    }
    MT_CHECK_NEAR(mp_eval::temporal_flicker(blinking), 1.0, 1e-12);
    MT_CHECK_NEAR(mp_eval::temporal_flicker({}), 0.0, 1e-12);
}
