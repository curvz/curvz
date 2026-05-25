#pragma once
#include "../SceneNode.hpp"

// ══════════════════════════════════════════════════════════════════════════════
// PathSimplify — shape-driven minimal-node refit (s300 m3)
//
// Source-agnostic, feature-agnostic, type-agnostic cubic path simplification.
// Takes a PathData with cubics, returns the minimum-node PathData that stays
// within `tolerance` of the input shape.
//
// Algorithm (iterative greedy deletion):
//
//   For each interior node N, ask: if I delete N and refit the handles on
//   N-1 and N+1 to best fit the original two-segment curve (the LSQ refit
//   that BezierPath::delete_node already does), how far does the new
//   combined cubic deviate from the original shape?
//
//   Take the most-removable node (smallest deviation), delete if deviation
//   <= tolerance. Repeat the pass until no node is removable.
//
// Contrast with PathReduce:
//
//   PathReduce uses type-based protection (Smooth/Symmetric only deletable)
//   and run-based keeper pinning (run-first / run-middle / run-last anchors
//   protected). It encodes architectural intent — the producer's type tags
//   carry semantic meaning, runs encode shape segments, midpoints protect
//   macro-arc shape.
//
//   PathSimplify ignores all of that. It looks only at output shape vs.
//   input shape and asks "can this node go without changing the shape?" If
//   yes, delete it. Source-agnostic by design — works on any cubic path
//   regardless of where its anchors came from.
//
//   The two pumps are complementary, not redundant. PathReduce is the
//   right tool when the producer's type tags are meaningful and the
//   path's run structure encodes intent (offset_path output post-
//   normalize, hand-authored work). PathSimplify is the right tool when
//   the producer destroyed source identity and only shape remains
//   (Clipper2-derived stroke band output, where every anchor is
//   indistinguishable polyline-refit noise).
//
// Tolerance is in doc units (same scale as the rest of the codebase —
// roughly pixels at 1:1 zoom). 0.5 = sub-pixel; 1.0 = visually invisible
// at normal zoom; 2.0+ = visible simplification.
//
// Tolerance semantics: greedy-local — each deletion is gated by deviation
// from the CURRENT pre-delete shape, not from the original input. Over many
// deletions cumulative drift can exceed the per-step tolerance, bounded
// roughly by sqrt(passes) × tolerance. With the 16-pass cap that's ~4× the
// configured tolerance in the worst case. For Expand Stroke the practical
// impact is invisible — set tolerance below the visible-drift threshold and
// the cumulative result still passes the eye test. (If a hard "output is
// within T of input" contract is needed later, refactor to track original
// sample points across deletions.)
//
// Guards:
//   - Paths with non-empty node_sets are skipped (parametric primitives —
//     NodeSet params index into the node array).
//   - 3-node floor for closed paths, 2-node floor for open. Never reduce
//     a path to a degenerate state.
//   - MAX_PASSES bound prevents pathological runaway iteration.
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// Simplify `path` in place via shape-driven greedy deletion. Returns the
// number of nodes deleted across all passes.
//
// No-op (returns 0) if path has fewer than 4 nodes, or has non-empty
// node_sets (primitive shape).
int simplify_cubic_path(PathData& path, double tolerance);

} // namespace Curvz
