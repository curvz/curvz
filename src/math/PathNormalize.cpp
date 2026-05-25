#include "math/PathNormalize.hpp"
#include "math/BezierPath.hpp"
#include "math/Vec2.hpp"
#include "CurvzLog.hpp"
#include <cmath>

namespace Curvz {

// ── Helper: tangent discontinuity angle at node `idx` ─────────────────────────
//
// Computes the angle (degrees) between the in-tangent and out-tangent
// directions at node `idx`. The tangent directions come from the node's
// own handles when they are non-degenerate:
//
//   in-tangent  = (anchor - in_handle)   — direction curve arrives at anchor
//   out-tangent = (out_handle - anchor)  — direction curve leaves anchor
//
// When a handle is colocated with the anchor (as in Clipper2's straight-cubic
// refit output — every node has cx1=cx2=anchor), the tangent is undefined
// from that handle alone. We fall back to chord directions to/from the
// neighbour anchor on that side.
//
// This dual approach is critical because different producers emit different
// "smooth" representations:
//   - Clipper2 offset/refit: colocated handles → must use chord fallback
//   - Warp per-slice fit:     real fitted handles → use them directly
//
// Using chord-only (the prior s299 m1 behaviour) gave false-positive
// "smoothness" on warp output: a node in a tight curve has a chord angle
// of 30-60° even though its handles agree on the tangent. The handle-based
// reading sees those nodes correctly as smooth.
//
// Returns -1.0 if either tangent is unrecoverable (no neighbour AND no
// non-degenerate handle on that side).
static double tangent_angle_deg(const PathData& path, int idx) {
    int n = (int)path.nodes.size();
    if (idx < 0 || idx >= n) return -1.0;

    bool has_prev = (idx > 0) || path.closed;
    bool has_next = (idx < n - 1) || path.closed;

    const BezierNode& c = path.nodes[idx];

    // In-tangent: prefer handle-based, fall back to chord-from-prev.
    Vec2 in_dir{0, 0};
    Vec2 from_in_handle{ c.x - c.cx1, c.y - c.cy1 };
    if (from_in_handle.length() > 1e-9) {
        in_dir = from_in_handle.normalised();
    } else if (has_prev) {
        int prev_i = (idx - 1 + n) % n;
        const BezierNode& p = path.nodes[prev_i];
        Vec2 chord{ c.x - p.x, c.y - p.y };
        if (chord.length() > 1e-9) in_dir = chord.normalised();
    }
    if (in_dir.length_sq() < 1e-12) return -1.0;

    // Out-tangent: prefer handle-based, fall back to chord-to-next.
    Vec2 out_dir{0, 0};
    Vec2 to_out_handle{ c.cx2 - c.x, c.cy2 - c.y };
    if (to_out_handle.length() > 1e-9) {
        out_dir = to_out_handle.normalised();
    } else if (has_next) {
        int next_i = (idx + 1) % n;
        const BezierNode& q = path.nodes[next_i];
        Vec2 chord{ q.x - c.x, q.y - c.y };
        if (chord.length() > 1e-9) out_dir = chord.normalised();
    }
    if (out_dir.length_sq() < 1e-12) return -1.0;

    // atan2(|cross|, dot) — robust over the whole [0, 180°] range,
    // unlike acos(dot) which loses precision near 0° and near 180°.
    double d = in_dir.dot(out_dir);
    double c2 = in_dir.cross(out_dir);
    double rad = std::atan2(std::abs(c2), d);
    return rad * (180.0 / M_PI);
}

// ── normalize_node_types ──────────────────────────────────────────────────────
//
// Repair dishonest node types in-place. Walks every node, computes its
// tangent discontinuity angle, and retypes based on the angle:
//
//   angle < SMOOTH_THRESHOLD (default 5°)   → Corner becomes Smooth
//                                             (false Corner — geometry passes
//                                             through smoothly, producer lied)
//   angle > CORNER_PROMOTE (default 60°)    → Smooth becomes Corner
//                                             (false Smooth — geometry has a
//                                             real corner, producer also lied)
//   between thresholds                      → keep existing type
//
// The two thresholds are far apart so the "promote" rule is conservative
// — only sharp turns get re-tagged Corner. Letterform corners (typically
// 80-120°) are above the promote line; smooth-arc samples (typically 5-30°)
// are well below it.
//
// Returns the count of type changes (both demotes and promotes combined).
int normalize_node_types(PathData& path, double threshold_deg) {
    if (path.nodes.size() < 3) return 0;
    if (threshold_deg <= 0.0)   return 0;

    // Promote threshold: anything turning more than this gets re-tagged as
    // a true Corner regardless of producer's claim. 60° is well above any
    // honest smooth-curve sample density yet below typical letterform
    // corner sharpness, so it slices safely between the two cases.
    constexpr double CORNER_PROMOTE_DEG = 60.0;

    BezierPath bp = BezierPath::from_path_data(path);

    int demoted = 0;
    int promoted = 0;
    int n = (int)bp.nodes.size();
    for (int i = 0; i < n; ++i) {
        const auto& nd = bp.nodes[i];
        double angle = tangent_angle_deg(path, i);
        if (angle < 0.0) continue;  // degenerate / endpoint with no fallback

        if (nd.type == BezierNode::Type::Corner) {
            // Demote false Corners — geometry is actually smooth.
            if (angle < threshold_deg) {
                bp.set_node_type(i, BezierNode::Type::Smooth);
                ++demoted;
            }
        } else if (nd.type == BezierNode::Type::Smooth
                || nd.type == BezierNode::Type::Symmetric) {
            // Promote false Smooth — geometry has a real corner.
            if (angle >= CORNER_PROMOTE_DEG) {
                bp.set_node_type(i, BezierNode::Type::Corner);
                ++promoted;
            }
        }
        // Cusp: leave alone (user-tagged pivot, even though tangent
        // continuous — see PathReduce.cpp's is_curve_node).
    }

    if (demoted > 0 || promoted > 0) {
        path = bp.to_path_data();
        LOG_DEBUG("PathNormalize: demoted {} Corner→Smooth, "
                  "promoted {} Smooth→Corner "
                  "(of {} nodes, thresholds {:.1f}° / {:.1f}°)",
                  demoted, promoted, n, threshold_deg, CORNER_PROMOTE_DEG);
    }
    return demoted + promoted;
}

} // namespace Curvz
