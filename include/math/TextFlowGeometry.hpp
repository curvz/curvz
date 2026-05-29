#pragma once
#include "../SceneNode.hpp"
#include "Vec2.hpp"     // BaselineSpan stores Vec2; SceneNode.hpp doesn't pull it
#include <vector>

// ══════════════════════════════════════════════════════════════════════════════
// TextFlowGeometry — the pure-geometry layer of form-fit text reflow.
//
// This file holds the small composable pumps that turn "a shaped boundary +
// a baseline" into "the x-spans where text is allowed on that baseline."
// None of it knows about Pango, the renderer, or the SceneNode tree beyond
// the PathData it is handed — it is the same no-GTK math layer PathOffset and
// BooleanOps live in, so it can be unit-tested in isolation.
//
// The settled model (s320 design, recovered and locked s322):
//
//   intervals_for_baseline(eroded_outline, base_baseline, k*leading)
//       -> list of (x,y) start/end spans
//
//   built from three independent steps, each its own pump:
//
//     1. erode_outline(shape, inset)      — THIS milestone.
//          The text margin is a UNIFORM inward inset of the whole outline,
//          not four per-edge subtractions (per-edge has no meaning on a
//          blob). Leans on offset_path(Inside); the rect insets to a
//          smaller rect (degenerate case), a pinching shape can erode into
//          two disjoint pieces.
//
//     2. baseline_ribbon(base_baseline, leading*k, thickness)   — next.
//          The base baseline (a path wider than the bbox, straight by
//          default, per-tb) translated down by k*leading, offset to a thin
//          closed ribbon whose TOP edge is the baseline itself. Straight or
//          curvy is the same op — straight is the degenerate curve.
//
//     3. Clipper2 Intersect(eroded_outline, ribbon) -> disjoint runs;       — next.
//        each run's TOP-edge vertices give absolute (x,y) start/end.
//          Concave shape -> more runs (no crossing-parity bookkeeping);
//          curvy baseline -> endpoints come back as points on the curve.
//          Same code for both because Clipper only ever sees two polygons.
//
// "Solve 10% = solve 100%": there is one algorithm, the shapes are inputs.
// Rect, ellipse, vase, crescent all flow through the same three pumps.
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// ── 1. Margin generator ───────────────────────────────────────────────────────
//
// Inset a closed shape inward by `inset` document units, producing the region
// where text is allowed once the margin is honored. This is the form-fit
// margin: a single uniform erosion of the boundary outline, replacing the
// rect-only per-edge margin subtraction.
//
// Returns:
//   - one PathData for the common case (rect/convex shrinks to one region),
//   - MULTIPLE PathData when an inward inset pinches the shape into separate
//     pieces (e.g. an hourglass eroded past its waist),
//   - an EMPTY vector when nothing survives the erosion (shape smaller than
//     2*inset in every direction) OR the input is open / has < 3 nodes. The
//     caller treats empty as "no room for any baseline" and emits no text.
//
// inset <= 0 returns the shape unchanged (a single-element vector): no margin
// means the whole interior is available.
//
// Built on offset_path(OffsetSide::Inside). The s321 fix made the inward
// square-corner path trustworthy, so a rect boundary erodes to a sharp-
// cornered interior rect rather than a spuriously rounded one. Compound
// boundaries (text with a cutout) are the caller's concern: pass each closed
// subpath through separately and intersect the results downstream — this
// pump operates on one closed path at a time.
std::vector<PathData> erode_outline(const PathData& shape, double inset);

// ── 2. Ribbon builder ─────────────────────────────────────────────────────────
//
// Build the thin closed ribbon for one baseline. The base baseline (a per-tb
// path, wider than the boundary bbox, straight by default) is translated DOWN
// by `dy` (= k * leading for line k), then given a small downward `thickness`
// to make a closed sliver. The returned path's TOP edge IS the translated
// baseline — the downstream Clipper2 intersect reads each run's endpoints off
// that top edge, so the (possibly slanted) bottom edge only has to keep the
// ribbon thin, not exact.
//
// Straight or curvy baseline is the same operation: a straight baseline yields
// a rectangle, a curvy one a curved ribbon following the curve (straight is the
// degenerate curve). Frame-agnostic: the baseline is consumed in whatever frame
// it is given and the ribbon comes back in that same frame — the caller erodes
// the boundary in the SAME frame so the two polygons line up for the intersect.
//
// The base baseline is expected to overhang the bbox on both ends (it is
// DEFINED wider than the bbox); the builder does not add overhang. That keeps
// every run boundary in the intersect a true margin crossing rather than a
// ribbon terminus.
//
// `flatten_step` is the approximate spacing (doc units) between samples on a
// curved segment; straight segments emit only their endpoints regardless.
// Returns a closed PathData (top edge in baseline order, bottom edge reversed).
// Empty if the baseline has < 2 distinct points.
PathData baseline_ribbon(const PathData& base_baseline,
                         double dy,
                         double thickness    = 2.0,
                         double flatten_step = 2.0);

// ── 3. Span read ──────────────────────────────────────────────────────────────
//
// One allowed run of text on a baseline: where it begins and ends, as points
// on the (possibly curved) baseline. For a straight baseline start.y == end.y.
struct BaselineSpan {
    Vec2 start;   // (x,y) on the baseline where this run begins
    Vec2 end;     // (x,y) on the baseline where this run ends
};

// Compute the allowed text runs on baseline line k (dy = k*leading below the
// base baseline) for a shaped, margin-eroded boundary. Builds the thin ribbon
// for this baseline, intersects it against the eroded margin region(s) via the
// raw Clipper2 entry, and reads each disjoint run's endpoints off the TOP edge
// (the on-baseline crossings). Scott's ribbon method: the crossing-pairing is
// structural — one returned loop per run, gaps fall out as the space between
// loops — and the code is identical for straight and curvy baselines because
// Clipper only ever sees two polygons.
//
// Returns runs ordered left-to-right along the baseline (wrap order). Empty
// when the baseline misses the eroded shape entirely (above the top, below the
// bottom, or through a pinched-out gap). `thickness`/`flatten_step` forward to
// the ribbon builder; `thickness` also sets the on-baseline classification
// band (a vertex counts as on the baseline when far closer to it than the
// ribbon is thick, so bottom-edge and margin-wall vertices never read as
// endpoints).
//
// M1 reads endpoints as min/max x of each run's on-baseline vertices, which is
// exact for a straight (and any monotonic-x) baseline. A baseline that doubles
// back in x would need arc-length ordering instead — deferred with curvy
// baselines, which are not in M1.
std::vector<BaselineSpan> intervals_for_baseline(
    const std::vector<PathData>& eroded_margin,
    const PathData&              base_baseline,
    double                       dy,
    double                       thickness    = 2.0,
    double                       flatten_step = 2.0);

} // namespace Curvz
