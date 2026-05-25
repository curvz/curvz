#include "math/PathSimplify.hpp"
#include "math/BezierPath.hpp"
#include "math/CubicSegment.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <vector>
#include <limits>

namespace Curvz {

// Sample count used to evaluate the post-delete refit's deviation from the
// pre-delete two-segment original. 16 samples gives sub-percent accuracy on
// the max-distance reading without being expensive.
static constexpr int    SIMPLIFY_SAMPLES   = 16;
static constexpr int    SIMPLIFY_MAX_PASSES = 16;

// Compute the max distance between the cubic produced by deleting node `idx`
// from `bp` and the two original cubic segments it replaces. Returns
// std::numeric_limits<double>::infinity() if the delete would degenerate the
// path (fewer than 3 nodes for closed, fewer than 2 for open) or if the
// neighbours don't both exist (open-path endpoint).
//
// Strategy: clone bp, perform the delete on the clone (which runs the
// LSQ refit), then sample the new segment at SIMPLIFY_SAMPLES points and
// measure each sample's distance to the original two-segment curve via
// CubicSegment::dist_to (which uses nearest_t internally — robust to
// parameter mismatch between old and new).
static double trial_delete_deviation(const BezierPath& bp, int idx) {
    const int n = (int)bp.nodes.size();

    // Floor checks: never delete to a degenerate state.
    if (bp.closed) {
        if (n <= 3) return std::numeric_limits<double>::infinity();
    } else {
        if (n <= 2) return std::numeric_limits<double>::infinity();
        // Open-path endpoint guard — endpoints have no flanking pair.
        if (idx == 0 || idx == n - 1)
            return std::numeric_limits<double>::infinity();
    }

    // Capture the two original segments that will be replaced.
    const int prev_idx = (idx - 1 + n) % n;
    CubicSegment seg_in  = bp.segment(prev_idx); // prev → idx
    CubicSegment seg_out = bp.segment(idx);       // idx  → next

    // Clone, delete, get the new combined segment.
    BezierPath trial = bp;
    trial.delete_node(idx);

    // After delete, the segment that previously started at prev_idx now
    // goes from prev (still at prev_idx in the new array if prev_idx < idx,
    // else at prev_idx - 1 because the array shifted) directly to next.
    int new_prev_idx = (prev_idx < idx) ? prev_idx : prev_idx - 1;
    // Wrap defensively for closed paths if delete shifted everything.
    if (new_prev_idx < 0) new_prev_idx = (int)trial.nodes.size() - 1;
    if (new_prev_idx >= (int)trial.nodes.size())
        new_prev_idx = (int)trial.nodes.size() - 1;

    CubicSegment seg_new = trial.segment(new_prev_idx);

    // Sample the new segment, measure each sample's distance to the closer
    // of the two original segments.
    double max_err = 0.0;
    for (int i = 1; i < SIMPLIFY_SAMPLES; ++i) {
        double t = (double)i / SIMPLIFY_SAMPLES;
        Vec2 q = seg_new.at(t);
        double d_in  = seg_in.dist_to(q);
        double d_out = seg_out.dist_to(q);
        double d = (d_in < d_out) ? d_in : d_out;
        if (d > max_err) max_err = d;
    }
    return max_err;
}

int simplify_cubic_path(PathData& path, double tolerance) {
    if (!path.node_sets.empty()) return 0;
    if (path.nodes.size() < 4)   return 0;
    if (tolerance <= 0.0)        return 0;

    BezierPath bp = BezierPath::from_path_data(path);
    int total_deleted = 0;
    int pass = 0;

    // Greedy pass: walk all interior nodes, compute trial-delete deviation,
    // delete the most-removable one (smallest deviation under tolerance).
    // Repeat until no node passes the tolerance gate.
    //
    // "Smallest deviation first" produces the highest-fidelity result:
    // each deletion is the one that disturbs the shape least. Faster
    // alternatives (delete every passing node in one sweep) accumulate
    // error because each LSQ refit shifts neighbouring handles, changing
    // what "the original shape" was for the next decision.
    while (pass < SIMPLIFY_MAX_PASSES) {
        int    best_idx = -1;
        double best_err = tolerance; // strictly less than tolerance to qualify

        int n = (int)bp.nodes.size();
        for (int i = 0; i < n; ++i) {
            double err = trial_delete_deviation(bp, i);
            if (err < best_err) {
                best_err = err;
                best_idx = i;
            }
        }

        if (best_idx < 0) break; // no node passes the gate

        bp.delete_node(best_idx);
        ++total_deleted;
        ++pass;

        // Floor — never reduce below 3 (closed) or 2 (open).
        int new_n = (int)bp.nodes.size();
        if (bp.closed && new_n <= 3) break;
        if (!bp.closed && new_n <= 2) break;
    }

    if (total_deleted > 0) {
        path = bp.to_path_data();
        LOG_DEBUG("PathSimplify: deleted {} node(s) in {} pass(es) "
                  "(tolerance={}, final node count={})",
                  total_deleted, pass, tolerance, (int)path.nodes.size());
    }
    return total_deleted;
}

} // namespace Curvz
