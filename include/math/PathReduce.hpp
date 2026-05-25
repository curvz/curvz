#pragma once
#include "../SceneNode.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// PathReduce — keeper-anchored redundant-curve-node deletion
//
// Strips uniformity from a path while preserving shape anchors. The reducer
// walks the path looking at three-windows (N-1, N, N+1): if all three are
// curve-typed AND the middle node N is not a keeper, N is redundant and
// gets shape-preservingly deleted. Iterate until a sweep deletes zero.
//
// Keepers (pre-pass):
//   Walk the path identifying maximal runs of consecutive curve-typed nodes
//   bracketed by pivot nodes (Corner / Cusp) or path endpoints. Within each
//   run pin the run's first node, last node, and midpoint. These pinned
//   anchors carry the shape's overall arc — without them, long curve runs
//   collapse all the way down to 3 nodes via the back-up logic, distorting
//   the shape badly.
//
//   For fully-curve closed paths (no pivots, whole loop is one run that
//   wraps), pin three evenly-spaced nodes as canonical "anchors."
//
//   Keepers are stored as anchor coordinates (Vec2) rather than indices,
//   so they remain valid as `delete_node` shifts indices during reduction.
//   Same key-by-position pattern as the boolean side's KeeperSet.
//
// Why this works (Huffman-style strip dominant redundancy with anchors):
//   - A "curve node" here means Smooth or Symmetric — the producer (or the
//     normalize pre-step) has told us this node has tangent continuity.
//   - Corner and Cusp are protected because they are pivot types: nodes
//     where the user (or producer) explicitly marked a shape feature.
//   - Nodes adjacent to Corner/Cusp are protected automatically — the
//     window centred on them would include the pivot, which fails the
//     all-curve test.
//   - Open-path endpoints are protected automatically — they cannot be
//     the middle of any three-window.
//   - Keeper anchors protect long-run midpoints from being deleted by the
//     back-up cascade; the shape's macro-arc survives.
//
// Shape preservation comes from BezierPath::delete_node, which runs a
// least-squares refit of the handles on the flanking nodes to minimize
// distance from the original two-segment curve.
//
// Prerequisite: types must be honest. Run normalize_node_types first on
// any input from a producer that lies about types (Clipper2 offset/refit
// output is the canonical case — every node is type-tagged Corner even
// when geometrically smooth).
//
// Guards:
//   - Paths with non-empty node_sets are skipped (parametric primitives
//     — Rect, Ellipse, Star, Polygon — must keep all nodes intact).
//   - MAX_PASSES safety bound prevents runaway iteration if the back-up
//     logic ever loops on a pathological case.
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// Reduce `path` in place by deleting redundant curve nodes, with run-end
// and run-middle anchors pinned. Returns the number of nodes deleted
// across all passes.
//
// No-op (returns 0) if path has fewer than 4 nodes, or has non-empty
// node_sets (primitive shape).
int reduce_redundant_curve_nodes(PathData& path);

} // namespace Curvz
