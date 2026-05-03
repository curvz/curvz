#pragma once
#include "../SceneNode.hpp"
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// PathOffset — Bézier curve offsetting
//
// Algorithm: Tiller-Hanson (1984) "Offsets of Two-Dimensional Profiles"
//   For each cubic segment, displace all four control points along their local
//   normals by distance d:
//     - p0, p3 (anchors): normalised tangent-perpendicular at t=0 and t=1
//     - p1, p2 (handles): normalised chord-perpendicular (p1-p0) and (p3-p2)
//   Check midpoint error: compare offset_seg.at(0.5) against true offset
//   point (seg.at(0.5) + d * normalised(seg.normal(0.5))).
//   If error > OFFSET_TOL, split and recurse (max depth = OFFSET_MAX_DEPTH).
//
// Join strategy (closed paths): at each segment junction, the two adjacent
//   offset segments' endpoints are averaged — a simple G0 join that works
//   well for small offsets. Sharp corners produce a slight rounding.
//
// Both sides: outer offset path (winding reversed) concatenated with inner
//   offset path, joined at endpoints → one closed filled shape.
//
// Limitations:
//   - Open path offsets not yet implemented (returns empty)
//   - Self-intersecting offset curves not resolved (large offsets on tight curves)
//   - Join type is fixed (average); miter/round/bevel deferred
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

enum class OffsetSide {
    Outside, // offset outward by d
    Inside,  // offset inward by d  (equivalent to Outside with -d)
    Both,    // produce filled stroke shape (outer + inner joined)
};

// Offset a single closed path by distance d on the specified side.
// Returns one PathData for Outside/Inside, one PathData for Both
// (the assembled stroke shape as a single closed path).
// Returns empty vector if path is open or has fewer than 2 nodes.
std::vector<PathData> offset_path(const PathData& path,
                                  double           distance,
                                  OffsetSide       side);

} // namespace Curvz
