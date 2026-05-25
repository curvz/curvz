#include "math/PathReduce.hpp"
#include "math/BezierPath.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <utility>
#include <vector>

namespace Curvz {

// ── is_curve_node ─────────────────────────────────────────────────────────────
// A node "passes the curve test" iff its type tag asserts tangent continuity
// AND the producer/user hasn't pinned it as a feature point.
//
// Smooth/Symmetric → curve. Corner → pivot (tangent discontinuity).
// Cusp → also pivot (the user explicitly tagged it as a feature, even
// though it's tangent-continuous in the codebase's definition — its
// presence in the type tag means "this matters").
static bool is_curve_node(const BezierNode& n) {
    return n.type == BezierNode::Type::Smooth
        || n.type == BezierNode::Type::Symmetric;
}

// ── KeeperSet — by anchor position ────────────────────────────────────────────
// Lightweight position-keyed pin set. Indices shift as delete_node mutates
// the array, but anchor coordinates do not — they survive the whole reduce
// pass. Lookup is linear (small N: a typical run has 5-30 keepers); the
// epsilon match tolerates the floating-point noise that delete_node's
// least-squares refit introduces into NEIGHBOURING anchors (the pinned
// node's own coordinates are never touched, but a more permissive epsilon
// is cheap insurance).
using PinPoint = std::pair<double, double>;

static bool is_pinned(const std::vector<PinPoint>& pins, double x, double y) {
    constexpr double EPS = 1e-6;
    for (const auto& p : pins) {
        if (std::abs(p.first  - x) < EPS &&
            std::abs(p.second - y) < EPS) return true;
    }
    return false;
}

// ── build_keeper_pins ─────────────────────────────────────────────────────────
// Walk the path, identify maximal runs of consecutive curve-typed nodes,
// and emit pins at each run's first node, last node, and midpoint.
//
// "Maximal run" definition:
//   - Open path: bracketed by (path start | pivot) on the left and
//                            (path end   | pivot) on the right.
//   - Closed path: bracketed by pivots on both sides; runs can wrap the
//                  array boundary if no pivot sits between the last and
//                  first nodes. Fully-curve closed loop (no pivots) is
//                  one wrapping run covering the whole array — we emit
//                  three evenly-spaced pins as canonical anchors since
//                  there are no real "ends."
//
// A "run of length 1 or 2" gets all its nodes pinned (every node is an
// end or the midpoint). A run of length 3 pins all three (start, middle,
// end coincide as the three nodes). Runs of length 4+ pin three distinct
// nodes (start, midpoint, end).
static std::vector<PinPoint> build_keeper_pins(const BezierPath& bp) {
    std::vector<PinPoint> pins;
    int n = (int)bp.nodes.size();
    if (n == 0) return pins;

    auto pin_idx = [&](int i) {
        const auto& nd = bp.nodes[i];
        pins.emplace_back(nd.x, nd.y);
    };

    // Helper: pin first, middle, last of an index range [lo..hi] (inclusive,
    // linear — no wrap; caller arranges indices). Length = hi - lo + 1.
    auto pin_run = [&](int lo, int hi) {
        int len = hi - lo + 1;
        if (len <= 0) return;
        pin_idx(lo);
        if (len >= 2) pin_idx(hi);
        if (len >= 3) pin_idx(lo + len / 2);
    };

    // For closed paths, detect the "fully-curve wrapping loop" special case
    // first — no pivot anywhere → one run covering the entire array.
    if (bp.closed) {
        bool any_pivot = false;
        for (int i = 0; i < n; ++i) {
            if (!is_curve_node(bp.nodes[i])) { any_pivot = true; break; }
        }
        if (!any_pivot) {
            // Whole loop is one run. No real "ends" — pin three evenly-
            // spaced anchors as canonical fixed points so the shape's
            // overall extent survives reduction.
            pin_idx(0);
            pin_idx(n / 3);
            pin_idx((2 * n) / 3);
            return pins;
        }
    }

    // General case: walk linearly, accumulating runs delimited by pivots
    // (or by path endpoints for open paths).
    int i = 0;
    while (i < n) {
        if (!is_curve_node(bp.nodes[i])) { ++i; continue; }
        int run_lo = i;
        while (i < n && is_curve_node(bp.nodes[i])) ++i;
        int run_hi = i - 1;
        pin_run(run_lo, run_hi);
    }

    // ── Pivot-halo pins ───────────────────────────────────────────────────
    // Pin a halo of ±2 around every pivot. The two-step halo protects the
    // tangent fidelity near every authored anchor:
    //
    //   pivot       — already pivot-protected by the curve-test, but pin
    //                 anyway for completeness.
    //   pivot ± 1   — first subdivision anchor on each side. Already
    //                 pinned indirectly by pin_run (as run_lo / run_hi of
    //                 the adjacent run). Re-pinning is harmless.
    //   pivot ± 2   — second subdivision anchor. THIS is what the halo
    //                 actually adds. Without it, reduce can delete the
    //                 first subdivision's neighbour, forcing the refit at
    //                 the first subdivision to span a longer arc and
    //                 distort the tangent leaving / arriving at the pivot.
    //
    // For closed paths, neighbour indices wrap. For open paths, missing
    // sides (idx near 0 / n-1) are skipped.
    auto pin_halo_step = [&](int p, int step) {
        bool valid_prev = (p - step >= 0) || bp.closed;
        bool valid_next = (p + step <= n - 1) || bp.closed;
        if (valid_prev) pin_idx((p - step + 2 * n) % n);
        if (valid_next) pin_idx((p + step) % n);
    };
    for (int p = 0; p < n; ++p) {
        if (is_curve_node(bp.nodes[p])) continue; // pivot test
        pin_idx(p);
        pin_halo_step(p, 1);
        pin_halo_step(p, 2);
    }

    // Closed path with at least one pivot: also handle the wrap-around run
    // (curves at end of array → pivot → curves at start of array, which the
    // linear walk above sees as two separate runs but is really one). The
    // linear walk pinned each half independently, which actually does the
    // right thing for the "ends" of the conceptual single run: the last
    // node of the trailing run and the first node of the leading run are
    // the two true ends. The only thing missing is the conceptual midpoint
    // of the combined run. Adding it: if the trailing-run's last node and
    // the leading-run's first node both border the same pivot-free wrap,
    // we'd pin the combined midpoint here. Skipped for now — the per-half
    // midpoints already provide reasonable anchor density. Document as a
    // known refinement.

    return pins;
}

// ── window_eligible ───────────────────────────────────────────────────────────
// Returns true iff the three-window centred on `idx` is delete-eligible:
//   - both neighbours exist (closed path → always; open path → not endpoint)
//   - all three nodes are curve-typed
//   - the middle node is NOT pinned (keeper protection)
static bool window_eligible(const BezierPath& bp, int idx,
                            const std::vector<PinPoint>& pins) {
    int n = (int)bp.nodes.size();
    if (n < 4) return false; // need at least 4 nodes — never reduce below 3

    bool has_prev = (idx > 0) || bp.closed;
    bool has_next = (idx < n - 1) || bp.closed;
    if (!has_prev || !has_next) return false;

    int prev_i = (idx - 1 + n) % n;
    int next_i = (idx + 1) % n;

    if (!is_curve_node(bp.nodes[prev_i])) return false;
    if (!is_curve_node(bp.nodes[idx]))    return false;
    if (!is_curve_node(bp.nodes[next_i])) return false;

    // Keeper check — the middle is the one being deleted, so only it
    // needs the pin check. Endpoints of the window are not being deleted.
    if (is_pinned(pins, bp.nodes[idx].x, bp.nodes[idx].y)) return false;

    return true;
}

// ── reduce_redundant_curve_nodes ──────────────────────────────────────────────
int reduce_redundant_curve_nodes(PathData& path) {
    // Guard: primitive shapes (Rect/Ellipse/Star/Polygon) must keep their
    // node count intact — the NodeSet params index into the node array.
    if (!path.node_sets.empty()) return 0;
    if (path.nodes.size() < 4)   return 0;

    BezierPath bp = BezierPath::from_path_data(path);

    // ── Pre-pass: build keeper pins ──────────────────────────────────────────
    // Identified once against the pre-reduce array. Pinned anchors survive
    // every deletion because they're keyed by (x, y) — delete_node never
    // moves a node's own coordinates (it refits the FLANKING anchors'
    // handles only).
    std::vector<PinPoint> pins = build_keeper_pins(bp);
    int pin_count = (int)pins.size();

    // Per the algorithm: outer "until a sweep deletes zero" loop with an
    // inner forward sweep that backs up one step after each deletion. The
    // back-up handles immediate reconvergence (the new neighbour at i-1
    // becomes eligible because its right neighbour has changed). The outer
    // loop catches anything the back-up missed. MAX_PASSES is a safety
    // bound, never expected to fire.
    constexpr int MAX_PASSES = 16;
    int total_deleted = 0;
    int pass = 0;

    while (pass < MAX_PASSES) {
        int deleted_this_pass = 0;
        int i = 0;
        int n = (int)bp.nodes.size();
        while (i < n) {
            if (window_eligible(bp, i, pins)) {
                bp.delete_node(i);
                ++deleted_this_pass;
                n = (int)bp.nodes.size();
                if (n < 4) break; // can't reduce further
                // Back up one step so the new window centred on what was
                // (i-1) gets re-tested. For closed paths the back-up wraps;
                // for open paths clamp to 0.
                if (i > 0) --i;
                else if (bp.closed && n > 0) i = n - 1;
                // else: i stays at 0 (open path, no back-up possible)
            } else {
                ++i;
            }
        }
        total_deleted += deleted_this_pass;
        if (deleted_this_pass == 0) break;
        ++pass;
    }

    if (total_deleted > 0) {
        path = bp.to_path_data();
        LOG_DEBUG("PathReduce: deleted {} curve node(s) in {} pass(es) "
                  "(pinned {} keeper anchor(s))",
                  total_deleted, pass + 1, pin_count);
    }
    return total_deleted;
}

} // namespace Curvz
