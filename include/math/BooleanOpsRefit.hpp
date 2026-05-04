#pragma once
#include "BooleanOps.hpp"
#include "BezierPath.hpp"
#include "Vec2.hpp"
#include "../SceneNode.hpp"
#include <vector>

namespace Curvz {

// ══════════════════════════════════════════════════════════════════════════════
// BooleanOpsRefit — Clipper2 output reconstruction (s139, multi-session)
//
// The Clipper2 boolean engine produces topologically-correct results, but
// the geometric output is a polyline with ~100 vertices per circle-equivalent
// curve. The first-cut refit in BooleanOpsClipper.cpp turns each polyline
// edge into a straight cubic, preserving the polyline shape exactly but
// inflating node count by ~30×.
//
// This module is the post-pass that walks the refitted PathData and
// reduces it to the minimum node set that reproduces the original cubic
// curves: original-anchor survivors with their original handles, and
// curve-curve intersections as Cusp anchors with renegotiated handles.
//
// Design (s137 + s139 refinement; preserved in
// docs/clipper-reconstruction-integration.md):
//
//   1. Pre-pass — compute keeper set:
//        * Every input anchor's position becomes a candidate keeper
//          (filtered later by Clipper2-survival check).
//        * Every analytical curve-curve intersection between distinct
//          subpaths becomes a Cusp keeper.
//
//   2. Walk-and-sweep — for each cleanup target loop:
//        * Tag refit nodes as keeper / deletable by position match
//          against the keeper set.
//        * Apply GUARD BAND: a node is deletable only if it AND both
//          its neighbours are non-keepers. This protects keeper handles
//          from being renegotiated by delete_node when adjacent nodes
//          are removed — without the guard, sharp corners and
//          intersection cusps drag the curve away from the original
//          shape (s139, surfaced from warp-cleanup workflow).
//        * Delete deletable nodes high-index-first via the existing
//          BezierPath::delete_node — its built-in least-squares refit
//          handles handle renegotiation between the surviving guards.
//        * Restore the original BezierNode for surviving anchors with
//          a known source; retype Intersection keepers to Cusp.
//
// Validation criterion: rect (4 cusps) + circle (4 symmetrics) Union
// produces 9 nodes (test-case/03-hand-cleaned-9-nodes.png). The guard
// band doesn't break this — keepers are spread far enough around the
// loop that their guards don't double-protect any shared interior, and
// most "deletable" runs between keepers are short enough to be covered
// by guards in either case.
//
// As of s139 this header is published but NOT WIRED into
// boolean_op_clipper. The wiring + cleanup_loop body will land in a
// subsequent session, behind a feature flag for at least one round of
// diagnostic comparison against the current 139-node behavior.
// ══════════════════════════════════════════════════════════════════════════════

namespace refit {

// A single point on the keeper-set wishlist. Three categories (s139 m5):
//
//   - OriginalAnchor: a user-authored anchor in the input geometry.
//     `source` points back into the operand's BezierPath::nodes.
//
//   - Intersection: an analytic curve-curve intersection found by
//     compute_keeper_set walking distinct subpath pairs. No source.
//
//   - SyntheticGuard: a synthetic anchor inserted by enrich_operands
//     immediately before/after every original anchor, to normalize the
//     cleanup output into uniform Corner-Corner-Corner triples regardless
//     of how Clipper2 packs the result. Synthetic guards survive Clipper2
//     as polyline samples and survive cleanup as keepers; they are NOT
//     retracted by cleanup_loop because their renegotiated handles after
//     delete_node carry the curve shape into the triple's middle (which
//     IS retracted).
//
//     Type is Smooth (s140 m1, was Corner in s139 m5). Each guard is
//     placed by exact De Casteljau split of the original input curve
//     (CubicSegment::split) at a small parameter t — so the guard sits
//     ON the curve and its handles are tangent to it by construction.
//     Same primitive that insert_node_at uses interactively when the
//     user clicks on a curve to add a node. Placement is per-side: the
//     post-guard splits the segment going OUT of an anchor (in-side
//     untouched at that moment), the pre-guard splits the segment
//     coming IN to an anchor (out-side already settled). Each split
//     touches one side of an anchor in isolation.
//
//     `source` is null; `parent_pos` records the position of the
//     original anchor this guard flanks (informational; not currently
//     used in matching).
struct KeeperPoint {
    Vec2 pos;
    enum class Origin { OriginalAnchor, Intersection, SyntheticGuard } origin;
    // Raw pointer back into the operand input. Lifetime: callers must
    // keep the operands alive across compute_keeper_set ... cleanup_loop
    // calls. The Canvas integration keeps both alive for the duration
    // of boolean_op_clipper (operands are stored by value into a local
    // vector that outlives the call).
    const BezierNode* source = nullptr;
    // For SyntheticGuard: the original anchor this guard was inserted
    // adjacent to, by position. Recorded for diagnostic/structural use;
    // not consulted by the cleanup match logic (which is purely
    // position-based via KEEPER_MATCH_TOL_SQ).
    Vec2 parent_pos{0.0, 0.0};
};

using KeeperSet = std::vector<KeeperPoint>;

// ── Tunables ─────────────────────────────────────────────────────────────────
// Match tolerance used by cleanup_loop to decide whether a refit node
// corresponds to a keeper. 0.5 doc units squared = 0.707 doc units.
// Well above the ~0.001-unit Int64 quantization in Clipper2 and the
// 0.1-unit flatten tolerance; well below plausible inter-anchor
// distance on real icons. Public so the test can reuse the same value.
constexpr double KEEPER_MATCH_TOL_SQ = 0.5 * 0.5;

// Curve-curve intersection numerical tolerance. Same constant as the
// retired hand-rolled engine's ISECT_EPS.
constexpr double ISECT_EPS = 1e-5;

// Maximum gap between the two curves' evaluations at a reported
// intersection. Hits with a wider gap are false positives from the
// fat-line rejection failing on near-misses. Generous on purpose;
// real intersections converge tightly.
constexpr double ISECT_GAP_DOC = 1.0;

// |sin(angle)| floor for transversality. Tangent / kissing contacts
// register a hit but don't actually cross — boundary doesn't swap
// sides, so they're not real intersections for boolean-op purposes.
constexpr double ISECT_CROSS_EPS = 1e-3;

// Position-distance dedup tolerance for collected intersections. When
// two distinct segment-pair tests resolve to the same crossing (e.g.
// at a shared subpath endpoint), keep one. Same scale as the keeper
// match tolerance.
constexpr double ISECT_DEDUP_TOL_SQ = 0.25 * 0.25;

// ── Enrichment tunables (s139 m5) ────────────────────────────────────────────
// enrich_operands inserts synthetic guard anchors immediately before
// and after every original anchor. The guards sit a small distance
// along the chord toward each neighbour, so they survive Clipper2
// quantization but do not visibly perturb the boolean result.
//
// ENRICH_OFFSET is the default chord-fraction offset. Set to 20.0 doc
// units during s140 m1 visual inspection so guards are clearly
// distinguishable from each original anchor at normal zoom. Will likely
// drop back down once enrichment is validated end-to-end and we want
// guards invisible at typical canvas scales — but the math is offset-
// agnostic, so this constant is purely a UX knob for the diagnostic
// phase.
constexpr double ENRICH_OFFSET = 20.0;
// Cap the offset at this fraction of segment length so very short
// segments do not produce overlapping guards.
constexpr double ENRICH_OFFSET_FRAC_CAP = 0.25;
// Floor the offset at this absolute value so degenerate inputs do not
// produce zero-length guards.
constexpr double ENRICH_OFFSET_MIN = 1e-3;

// ── API ──────────────────────────────────────────────────────────────────────

// Enrich operand subpaths for boolean processing (s139 m5 — pre-Clipper2).
//
// For every anchor in every input subpath, insert two synthetic guard
// anchors: one at offset along the chord toward the previous anchor,
// one at offset along the chord toward the next anchor. The synthetic
// guards have Corner type and handles meeting at their anchor (no
// curvature contribution). They survive Clipper2 as polyline vertices
// and the cleanup walk picks them up via the keeper-set position match.
//
// Returns the enriched operands paired with the synthetic-guard keeper
// points. The caller hands the enriched operands to Clipper2 and the
// guard list to compute_keeper_set (which augments the original
// keeper-set with the synthetic guards).
//
// Architectural role: this is stage 1 of the four-stage boolean
// pipeline (enrich, clip, remove-constant, remove-refined). Its job
// is to guarantee that every original anchor sits at the middle of
// a Corner-Corner-Corner triple in the cleanup output, regardless of
// where Clipper2 places its polyline samples or its seam.
//
// `operands_out` is filled with the enriched subpaths. `guards_out` is
// filled with the synthetic guards' KeeperPoints (Origin::SyntheticGuard,
// source=nullptr, parent_pos=the original anchor's position).
void enrich_operands(
    const std::vector<std::vector<BezierPath>>& operands_in,
    std::vector<std::vector<BezierPath>>&       operands_out,
    KeeperSet&                                  guards_out);

// Build the keeper set from the input operands of a boolean op.
//
// `operands` is the same shape as boolean_op_clipper takes:
// outer = list of operands; inner = list of subpaths per operand.
//
// Pointers in the returned KeeperPoints reference BezierNodes inside
// `operands`. Callers must not mutate or destroy operands until they
// are done consuming the KeeperSet.
//
// The op type is currently informational — the keeper-set contents are
// the same for all three ops (the Clipper2 phase decides which keepers
// actually appear in the output, the cleanup phase filters by position
// match against the actual output). Kept as a parameter for future-
// proofing in case op-specific filtering proves useful.
KeeperSet compute_keeper_set(
    const std::vector<std::vector<BezierPath>>& operands,
    BooleanOpType op);

// Apply the cleanup walk to a single refitted result loop. (Pass 1.)
//
// `refit_pd` is the output of refit_path_straight_cubics: a closed
// PathData with one Corner cubic per Clipper2 polyline edge. Caller
// should pass loops with >=3 nodes; smaller inputs are returned
// unchanged.
//
// Returns a new PathData with deletable nodes removed (subject to the
// guard-band rule), OriginalAnchor keepers retracted to handles-at-anchor
// with type Corner (s139 m2 fix1+fix2), and Intersection keepers left
// untouched. The output has a recognizable C-C-C structure: every
// keeper sits at the middle of a Corner-Corner-Corner triple with one
// guard on each side. The middle's handle topology encodes the keeper
// origin: retracted handles → OriginalAnchor (a position pin), extended
// handles toward siblings → Intersection (a curve transition cusp).
//
// STATUS s139 m2 fix2: shipped and tested. Wired in Canvas::boolean_op
// behind AppPreferences::boolean_cleanup_enabled.
PathData cleanup_loop(PathData refit_pd, const KeeperSet& keepers);

// Apply the geometric reduction pass to a cleaned loop. (Pass 2.)
//
// STATUS s139 m5: STUB — returns input unchanged. The replacement
// geometric classifier (collinear / sharp / smooth detection) ships
// in s140 (Ship B). See implementation comment for the rationale —
// the m4 stride-3 rule no longer fits the new pass-1 contract under
// enrichment, and the geometric classifier is the right replacement
// rather than a quick-fix to m4's rule.
PathData reduce_loop(PathData cleaned_pd);

} // namespace refit

} // namespace Curvz
