// test_boolean_ops_refit.cpp ─────────────────────────────────────────────
//
// Standalone test for compute_keeper_set on the canonical rect+circle
// Union test case (test-case/01-original-8-nodes.png).
//
// Build (matches the Curvz build env — cairomm, glib, spdlog/fmt, gtkmm
// are all on the include path; only spdlog/fmt actually need linking):
//
//   g++ -std=c++17 -Iinclude \
//       $(pkg-config --cflags cairomm-1.16 glib-2.0) \
//       -o tests/test_boolean_ops_refit \
//       tests/test_boolean_ops_refit.cpp \
//       src/math/BooleanOpsRefit.cpp \
//       src/math/BezierPath.cpp \
//       src/math/CubicSegment.cpp \
//       src/math/Vec2.cpp \
//       -lspdlog -lfmt
//
// (Adjust cairomm-1.16 to whatever pkg-config name your local Fedora
// install uses if different.)
//
// Run:
//   ./tests/test_boolean_ops_refit
//
// Expected: "All N tests passed." Otherwise prints failing case and exits 1.
//
// What this validates:
//   - 8 OriginalAnchor keepers are recorded for rect (4 corners) + circle
//     (4 symmetrics)
//   - 2 Intersection keepers are found at the two rect-circle crossings
//   - Each Intersection keeper has source == nullptr
//   - Each OriginalAnchor keeper has a non-null source pointer
//
// What this does NOT validate (deferred to the wiring + cleanup ship):
//   - cleanup_loop's behavior (still a stub in s139)
//   - End-to-end Clipper2 → 9-node output
//
// The test geometry is a 200×200 axis-aligned rect at (200, 200) and an
// r=80 circle centred at (390, 290), chosen so the circle crosses the
// rect's right edge at two points that don't coincide with any input
// anchor — keeping the OriginalAnchor and Intersection keeper categories
// clearly separated.

#include "math/BooleanOpsRefit.hpp"

#include "math/BezierPath.hpp"
#include "math/Vec2.hpp"
#include "SceneNode.hpp"

#include <cmath>
#include <iostream>
#include <string>

using Curvz::BezierNode;
using Curvz::BezierPath;
using Curvz::PathData;
using Curvz::Vec2;
using Curvz::BooleanOpType;
using Curvz::refit::compute_keeper_set;
using Curvz::refit::KeeperPoint;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { ++g_pass; }
    else      { ++g_fail; std::cerr << "FAIL: " << what << "\n"; }
}

// ── Geometry builders ───────────────────────────────────────────────────────
// Axis-aligned rectangle as 4 Cusp anchors with collinear handles. Handles
// are positioned 1/3 of the way along each edge — geometrically equivalent
// to a straight-line segment.
static PathData make_rect(double x, double y, double w, double h) {
    PathData pd;
    pd.closed = true;

    auto edge_node = [](double px, double py,
                        double prev_x, double prev_y,
                        double next_x, double next_y) {
        BezierNode n;
        n.x = px; n.y = py;
        // in-handle: 1/3 toward prev
        n.cx1 = px + (prev_x - px) / 3.0;
        n.cy1 = py + (prev_y - py) / 3.0;
        // out-handle: 1/3 toward next
        n.cx2 = px + (next_x - px) / 3.0;
        n.cy2 = py + (next_y - py) / 3.0;
        n.type = BezierNode::Type::Cusp;
        return n;
    };

    const double x0 = x,         y0 = y;
    const double x1 = x + w,     y1 = y;
    const double x2 = x + w,     y2 = y + h;
    const double x3 = x,         y3 = y + h;

    pd.nodes.push_back(edge_node(x0, y0, x3, y3, x1, y1));
    pd.nodes.push_back(edge_node(x1, y1, x0, y0, x2, y2));
    pd.nodes.push_back(edge_node(x2, y2, x1, y1, x3, y3));
    pd.nodes.push_back(edge_node(x3, y3, x2, y2, x0, y0));
    return pd;
}

// Circle as 4 symmetric anchors. Standard kappa = 0.5522847498 for the
// cubic Bézier circle approximation.
static PathData make_circle(double cx, double cy, double r) {
    constexpr double K = 0.5522847498307933;
    PathData pd;
    pd.closed = true;

    auto sym_node = [r, K](double px, double py, double dir_in_x, double dir_in_y,
                           double dir_out_x, double dir_out_y) {
        BezierNode n;
        n.x = px; n.y = py;
        const double hl = r * K;
        n.cx1 = px + dir_in_x  * hl;
        n.cy1 = py + dir_in_y  * hl;
        n.cx2 = px + dir_out_x * hl;
        n.cy2 = py + dir_out_y * hl;
        n.type = BezierNode::Type::Symmetric;
        return n;
    };

    // Anchors at right (E), bottom (S), left (W), top (N) — CW winding
    // (positive in Y-down coordinate space, which is what BezierPath uses).
    // E: tangent vertical, in-handle pointing up, out-handle pointing down.
    pd.nodes.push_back(sym_node(cx + r, cy,       0, -1,  0,  1));
    pd.nodes.push_back(sym_node(cx,     cy + r,   1,  0, -1,  0));
    pd.nodes.push_back(sym_node(cx - r, cy,       0,  1,  0, -1));
    pd.nodes.push_back(sym_node(cx,     cy - r,  -1,  0,  1,  0));
    return pd;
}

// ── Tests ───────────────────────────────────────────────────────────────────
int main() {
    // Build the canonical rect+circle scene. Geometry chosen so the circle
    // intersects the rect's right edge in exactly two points, and the
    // intersection coordinates do NOT coincide with any input anchor —
    // so the keeper-set categories don't overlap and the test can check
    // each independently.
    //
    // Rect: x ∈ [200, 400], y ∈ [200, 400] (200×200, top-left at (200,200))
    // Circle: centre (390, 290), radius 80
    //   Circle anchors: (470, 290), (390, 370), (310, 290), (390, 210)
    //   Right edge of rect (x = 400, y ∈ [200, 400]):
    //     (400-390)² + (y-290)² = 80² → (y-290)² = 6300
    //     → y ≈ 290 ± 79.3725 → (400, 210.6275) and (400, 369.3725)
    //     Both are inside the rect's right-edge y-range.
    //   Top edge (y = 200): (x-390)² = 6400 - (200-290)² = 6400 - 8100 < 0
    //     → no intersection
    //   Bottom edge (y = 400): (x-390)² = 6400 - (400-290)² = 6400 - 12100 < 0
    //     → no intersection
    //   So exactly 2 intersections, both on the right edge, neither
    //   coincident with any of the 8 input anchors.
    PathData rect_pd   = make_rect(200, 200, 200, 200);
    PathData circle_pd = make_circle(390, 290, 80);

    BezierPath rect   = BezierPath::from_path_data(rect_pd);
    BezierPath circle = BezierPath::from_path_data(circle_pd);

    std::vector<std::vector<BezierPath>> operands;
    operands.push_back({rect});
    operands.push_back({circle});

    auto keepers = compute_keeper_set(operands, BooleanOpType::Union);

    // ── Test 1: total keeper count = 8 anchors + 2 intersections = 10 ────
    int n_anchor = 0, n_intersect = 0;
    for (const auto& k : keepers) {
        if (k.origin == KeeperPoint::Origin::OriginalAnchor) ++n_anchor;
        else                                                  ++n_intersect;
    }
    check(n_anchor == 8,
          "expected 8 OriginalAnchor keepers, got " + std::to_string(n_anchor));
    check(n_intersect == 2,
          "expected 2 Intersection keepers, got " + std::to_string(n_intersect));

    // ── Test 2: every OriginalAnchor has a non-null source pointer ────────
    int null_sources = 0;
    for (const auto& k : keepers) {
        if (k.origin == KeeperPoint::Origin::OriginalAnchor && k.source == nullptr) {
            ++null_sources;
        }
    }
    check(null_sources == 0,
          "expected 0 OriginalAnchor keepers with null source, got " +
              std::to_string(null_sources));

    // ── Test 3: every Intersection has a null source pointer ──────────────
    int nonnull_intersections = 0;
    for (const auto& k : keepers) {
        if (k.origin == KeeperPoint::Origin::Intersection && k.source != nullptr) {
            ++nonnull_intersections;
        }
    }
    check(nonnull_intersections == 0,
          "expected 0 Intersection keepers with non-null source, got " +
              std::to_string(nonnull_intersections));

    // ── Test 4: intersection positions match analytic answer ──────────────
    // Circle (390, 290) r=80, rect right edge x=400.
    // Intersections: (400, 290 ± sqrt(6300)) ≈ (400, 210.6275) and (400, 369.3725).
    // Allow 0.1 tolerance — well above the 1e-5 ISECT_EPS but below any
    // plausible drift.
    constexpr double ISECT_TOP_X = 400.0;
    constexpr double ISECT_TOP_Y = 290.0 - 79.37253933;  // ≈ 210.6275
    constexpr double ISECT_BOT_X = 400.0;
    constexpr double ISECT_BOT_Y = 290.0 + 79.37253933;  // ≈ 369.3725

    bool found_top = false, found_bot = false;
    for (const auto& k : keepers) {
        if (k.origin != KeeperPoint::Origin::Intersection) continue;
        if (std::abs(k.pos.x - ISECT_TOP_X) < 0.1 &&
            std::abs(k.pos.y - ISECT_TOP_Y) < 0.1) {
            found_top = true;
        }
        if (std::abs(k.pos.x - ISECT_BOT_X) < 0.1 &&
            std::abs(k.pos.y - ISECT_BOT_Y) < 0.1) {
            found_bot = true;
        }
    }
    check(found_top, "expected intersection near (400, 210.63)");
    check(found_bot, "expected intersection near (400, 369.37)");

    // ── Test 5: anchor positions cover the original scene ─────────────────
    // Rect corners: (200,200), (400,200), (400,400), (200,400)
    // Circle anchors: (470,290), (390,370), (310,290), (390,210)
    auto has_anchor_at = [&](double x, double y) {
        for (const auto& k : keepers) {
            if (k.origin != KeeperPoint::Origin::OriginalAnchor) continue;
            if (std::abs(k.pos.x - x) < 0.1 && std::abs(k.pos.y - y) < 0.1) return true;
        }
        return false;
    };
    check(has_anchor_at(200, 200), "expected rect TL anchor");
    check(has_anchor_at(400, 200), "expected rect TR anchor");
    check(has_anchor_at(400, 400), "expected rect BR anchor");
    check(has_anchor_at(200, 400), "expected rect BL anchor");
    check(has_anchor_at(470, 290), "expected circle E anchor");
    check(has_anchor_at(390, 370), "expected circle S anchor");
    check(has_anchor_at(310, 290), "expected circle W anchor");
    check(has_anchor_at(390, 210), "expected circle N anchor");

    if (g_fail == 0) {
        std::cout << "All " << g_pass << " tests passed.\n";
        return 0;
    } else {
        std::cerr << g_fail << " of " << (g_pass + g_fail) << " tests failed.\n";
        return 1;
    }
}
