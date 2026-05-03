#pragma once
#include "../SceneNode.hpp"
#include <unordered_set>
#include <vector>

namespace Curvz {

// ── Corner treatment types ────────────────────────────────────────────────────
enum class CornerType {
    Round,        // Cubic arc approximation (tangent-continuous)
    Chamfer,      // Straight cut across the corner
    InverseRound, // Concave arc (handles flipped inward)
};

// ── Per-node result of a corner treatment attempt ────────────────────────────
enum class CornerSkipReason {
    Ok,
    AngleTooSharp,    // < 15 degrees
    AngleTooFlat,     // > 170 degrees
    CurvedIncoming,   // incoming segment has non-degenerate handle
    CurvedOutgoing,   // outgoing segment has non-degenerate handle
    TooFewNeighbours, // open path endpoint — no incoming or outgoing segment
};

struct CornerResult {
    int              node_index  = -1;
    CornerSkipReason skip_reason = CornerSkipReason::Ok;
    bool             skipped()   const { return skip_reason != CornerSkipReason::Ok; }
};

// ── Apply corner treatment to a PathData ─────────────────────────────────────
//
// Operates on nodes whose indices are in `node_indices` AND whose
// BezierNode::Type is Corner or Cusp. Other node types are skipped silently.
//
// `radius`       — desired distance from corner to the start/end of the treatment.
//                  Clamped per node so neither P1 nor P2 overshoots the midpoint
//                  of its respective adjacent segment.
// `out_results`  — optional; filled with one CornerResult per requested index,
//                  in the same order as node_indices. Skipped nodes are noted.
//
// Returns the modified PathData (original is unchanged).
// node_sets are cleared (geometry is no longer parametric after treatment).
//
PathData apply_corner_treatment(
    const PathData&               path,
    const std::unordered_set<int>& node_indices,
    CornerType                    type,
    double                        radius,
    std::vector<CornerResult>*    out_results = nullptr);

// ── Helpers (exposed for live-preview use) ───────────────────────────────────

// Angle (degrees) at node idx between incoming and outgoing straight segments.
// Returns -1.0 if the node is an open endpoint or has curved neighbours.
double corner_angle_deg(const PathData& path, int idx);

// True if the handle is within epsilon of its anchor (degenerate / retracted).
bool handle_is_degenerate(double ax, double ay, double hx, double hy,
                           double epsilon = 1e-4);

} // namespace Curvz
