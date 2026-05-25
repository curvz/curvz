#pragma once
#include "BooleanOps.hpp"
#include "BezierPath.hpp"
#include "Vec2.hpp"
#include "../SceneNode.hpp"
#include <vector>

namespace Curvz {

// ══════════════════════════════════════════════════════════════════════════════
// BooleanOpsRefit — Clipper2 output reconstruction (s140 final shape)
//
// Clipper2 produces topologically-correct boolean results, but the
// geometric output is a polyline with ~100 vertices per circle-equivalent
// curve. The first-cut refit in BooleanOpsClipper.cpp turns each polyline
// edge into a straight cubic, preserving the polyline shape exactly but
// inflating node count by ~30×.
//
// This module is the post-pass that walks the refitted PathData and
// reduces it to the minimum node set that reproduces the original cubic
// curves: original-anchor survivors with their original handles, and
// curve-curve intersections as Corner anchors with retracted handles.
//
// Three-phase pipeline (s140):
//
//   1. ENRICH (pre-Clipper2):
//        Insert smooth on-curve guard anchors around every original
//        anchor via BezierPath::insert_node_at — same primitive used
//        interactively when the user clicks on a curve to add a node.
//        Guards carry curve-shape information through Clipper2's
//        polyline phase. Geometry is preserved exactly; re-running the
//        enriched path through Clipper2 produces the same polyline as
//        the un-enriched input would have.
//
//   2. KEEPER SET (post-Clipper2):
//        Build the keeper set from the ORIGINAL operands (not enriched).
//        Two categories: OriginalAnchor (with back-pointer to source
//        BezierNode) and Intersection (analytic curve-curve crossing,
//        same transversality filters as the retired Sederberg-Nishita
//        engine). Synthetic guards are NOT keepers — they're transport,
//        not destination, and get deleted with the rest of the polyline
//        noise.
//
//   3. CLEANUP WALK:
//        For each keeper, find its closest match in the polyline output
//        and tag that node. Delete every untagged node descending. For
//        each surviving tag: OriginalAnchor restores byte-for-byte from
//        KeeperPoint::source (corner stays Corner, smooth stays Smooth,
//        original handles preserved); Intersection retracts handles and
//        becomes Corner.
//
// Validation: rect+circle Union → 9 nodes (matches hand-clean target);
// complex freeform-plus-ellipse → 20 nodes with all authored types
// preserved, clean intersections, correct path direction.
//
// All three phases are wired into Canvas::boolean_op behind
// AppPreferences::boolean_cleanup_enabled. Off → behaviour identical
// to pre-s139 (raw Clipper2 polyline output via BooleanOpsClipper's
// straight-cubic refit).
// ══════════════════════════════════════════════════════════════════════════════

namespace refit {

// A single point on the keeper-set wishlist. Two categories (s140 m3):
//
//   - OriginalAnchor: a user-authored anchor in the input geometry.
//     `source` points back into the operand's BezierPath::nodes so the
//     cleanup pass can restore the authored handles + type byte-for-
//     byte (corner stays Corner, smooth stays Smooth — originals are
//     already what they are).
//
//   - Intersection: an analytic curve-curve intersection found by
//     compute_keeper_set walking distinct subpath pairs. No source.
//     Cleanup retracts handles and types the survivor as Corner —
//     intersections are direction changes by definition.
//
// Synthetic guards inserted by the enrich phase are NOT keepers. They
// exist purely to carry curve-shape information through Clipper2's
// polyline phase; once the boolean is done they're transport noise
// and get deleted with everything else untagged.
struct KeeperPoint {
    Vec2 pos;
    // Five categories (s142 m6):
    //   - OriginalAnchor: a user-authored anchor in the input geometry.
    //   - Intersection:   analytic curve-curve crossing.
    //   - SyntheticGuard: smooth on-curve anchor inserted by enrich() to
    //                     scaffold the curve shape near originals through
    //                     Clipper2's polyline phase. In v3+ these survive
    //                     into the output as Smooth anchors, guaranteeing
    //                     curve shape near intersections without depending
    //                     on byte-for-byte handle preservation.
    //   - CurvatureApex:  promoted-from-untagged node at a local maximum
    //                     of polyline turning angle. Pins shape at rapid-
    //                     curvature features that delete_node's least-
    //                     squares fit cannot reconstruct from sparse
    //                     surviving keepers. Runtime-detected in
    //                     cleanup_loop_v4. Added in s142 m5.
    //   - SpanFiller:     promoted midpoint of a too-long untagged run
    //                     between consecutive keepers. Long gentle arcs
    //                     produce no apexes (no local turning-angle
    //                     maximum), but delete_node's 2-handle fit can't
    //                     honestly express many doc-units of curve in
    //                     one cubic — it over-extends handles. Span
    //                     fillers break long runs into shorter ones so
    //                     each delete_node span is fit-able. NEW IN s142 m6.
    enum class Origin {
        OriginalAnchor, Intersection, SyntheticGuard, CurvatureApex, SpanFiller
    } origin;
    // Raw pointer back into the operand input. Lifetime: callers must
    // keep the operands alive across compute_keeper_set ... cleanup_loop
    // calls. The Canvas integration keeps both alive for the duration
    // of boolean_op_clipper (operands are stored by value into a local
    // vector that outlives the call).
    // Set for OriginalAnchor; nullptr for Intersection and SyntheticGuard.
    const BezierNode* source = nullptr;
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

// ── Enrichment tunables ──────────────────────────────────────────────────────
// enrich() inserts smooth on-curve guard anchors immediately before and
// after every original anchor via De Casteljau split (insert_node_at).
// The guards sit a small distance along each adjacent segment, exactly
// on the curve with tangent handles by construction.
//
// ENRICH_OFFSET is the default per-side offset in doc units. 5.0 was
// settled in s140 m1 visual inspection — large enough to survive
// Clipper2's Int64 quantization, small enough not to perturb the
// boolean output.
constexpr double ENRICH_OFFSET = 20.0;
// Cap the offset at this fraction of segment length so very short
// segments do not produce overlapping guards.
constexpr double ENRICH_OFFSET_FRAC_CAP = 0.25;
// Floor the offset at this absolute value so degenerate inputs do not
// produce zero-length guards.
constexpr double ENRICH_OFFSET_MIN = 1e-3;

// s296 m14 — separate, smaller offset for intersection-triplet guards.
// Triplet guards (pre, intersection, post) should hug the crossing so
// the cleanup walk reads them as a tight cluster rather than three
// spread-out keepers. ENRICH_OFFSET (20) was too far for this role —
// guards drifted from the intersection and the cleanup couldn't pin
// the curve shape at crossings. Same cap + floor semantics as
// ENRICH_OFFSET. Used by inject_intersection_triplets_on_subpath via
// the file-static compute_guard_t.
constexpr double ISECT_TRIPLET_OFFSET = 4.0;

// ── API ──────────────────────────────────────────────────────────────────────

// Enrich operand subpaths for boolean processing (pre-Clipper2).
//
// For every anchor in every input subpath, insert two smooth on-curve
// guard anchors via BezierPath::insert_node_at — one on the segment
// going OUT of the anchor, one on the segment coming IN. Same primitive
// the user invokes interactively when clicking on a curve to add a
// node: De Casteljau split at small parameter t, on-curve position,
// tangent handles by construction, type Smooth.
//
// Two-pass per-side insertion (s140 m1):
//   - Pass 1 splits each segment going OUT of every anchor in
//     descending order. The anchor's in-handle stays untouched; the
//     out-handle gets renegotiated by insert_node_at to preserve
//     the curve shape together with the new post-guard.
//   - Pass 2 splits each segment coming IN to every original anchor.
//     The out-side is now already settled from pass 1; pass 2 only
//     affects the in-side.
//
// Each side of every anchor is touched in isolation. No segment is
// ever split twice. Guards exist purely to carry tangent information
// through Clipper2's polyline phase — they're transport, not
// destination, and are deleted by the cleanup walk along with the
// rest of the polyline noise. The keeper set passed to cleanup is
// originals + intersections only.
void enrich(
    const std::vector<std::vector<BezierPath>>& operands_in,
    std::vector<std::vector<BezierPath>>&       operands_out);

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

// s142 m2 — extended keeper-set builder. Same as compute_keeper_set
// but also emits SyntheticGuard keepers from the enriched operands.
//
// The enriched operands carry the smooth on-curve guard anchors that
// enrich() injected around every original. We compare enriched against
// pre-enrich to identify which anchors are guards (positions that are
// in `enriched` but not in `originals`). Each guard becomes a keeper
// with origin=SyntheticGuard. Guards survive cleanup_loop_v3 and emit
// as Smooth anchors in the output, scaffolding the curve shape near
// intersections without needing byte-for-byte handle preservation.
//
// Originals + intersections are handled identically to compute_keeper_set.
KeeperSet compute_keeper_set_with_guards(
    const std::vector<std::vector<BezierPath>>& originals,
    const std::vector<std::vector<BezierPath>>& enriched,
    BooleanOpType op);

// ══════════════════════════════════════════════════════════════════════════════
// s142 m3/m4 — targeted enrichment: triplets only at intersections
// ══════════════════════════════════════════════════════════════════════════════
//
// m1 + m2 enriched broadly — every original anchor on every operand got
// flanking guards. This was overkill: original-anchor regions away from
// intersections were already clean (m1 confirmed), and the extra guards
// inflated the output node count without earning their keep.
//
// m3 was targeted but injected on operand[0] only (subject). Symptom:
// at intersections where Clipper2 routed the boundary through the OTHER
// side, that side had no scaffolding → keeper match failed → drift /
// notch / missing intersection.
//
// m4 — inject on BOTH crossing curves at every intersection. Each
// physical intersection appears as a triplet on each parent. Whichever
// side the boolean output traverses, surviving smooth anchors anchor
// the local curve shape.
//
// Algorithm:
//   1. Compute analytic intersections between every cross-operand pair.
//   2. For each hit, inject a triplet (pre-guard + intersection-anchor
//      + post-guard) on BOTH parent subpaths at their respective
//      (seg, t) parameters. Per-subpath dedup against ISECT_DEDUP_TOL_SQ
//      so shared-endpoint cases don't double-inject.
//   3. Single Clipper2 pass on the both-sides-modified operands.
//   4. Cleanup walk (cleanup_loop_v3): keepers = every operand's
//      anchors (originals tagged OriginalAnchor; injected intersections
//      tagged Intersection; injected guards tagged SyntheticGuard).
//      Strip everything else; delete_node renegotiates handles.

// Run targeted enrichment on every operand (m4 — both sides of every
// intersection). Mutates `operands_out` in place with the same shape
// as `operands_in`, where each operand subpath has triplets injected
// at its analytic intersection points with every other operand's
// subpaths.
//
// Returns the keeper set ready for cleanup_loop_v3.
//
// Caller must keep `operands_in` alive across the cleanup walk because
// the keeper set holds raw pointers into it for OriginalAnchor source.
KeeperSet enrich_at_intersections_and_build_keepers(
    const std::vector<std::vector<BezierPath>>& operands_in,
    std::vector<std::vector<BezierPath>>&       operands_out,
    BooleanOpType op);
// ── End m3/m4 API ────────────────────────────────────────────────────────────

// s296 m11 — snapshot every BezierNode from every subpath across every
// operand as an OriginalAnchor keeper. Used by the chunked-fold path
// (boolean_op chain) to seed the master keeper set with originals up
// front, before per-step intersection triplets accumulate on top.
// Caller MUST keep `operands` alive across the eventual cleanup_loop
// call — source pointers refer into operands' storage.
KeeperSet original_anchors_keepers(
    const std::vector<std::vector<BezierPath>>& operands);

// Apply the per-keeper claim-and-restore walk to a single refitted
// result loop.
//
// `refit_pd` is the output of refit_path_straight_cubics: a closed
// PathData with one Corner cubic per Clipper2 polyline edge. Caller
// should pass loops with >=3 nodes; smaller inputs are returned
// unchanged.
//
// Walk:
//   - For each keeper, find its closest match in refit_pd within
//     KEEPER_MATCH_TOL_SQ and tag that node.
//   - Delete every untagged node descending (subject to a 3-node
//     floor so delete_node never opens the path).
//   - For each surviving tag: OriginalAnchor restores byte-for-byte
//     from KeeperPoint::source (authored type and handles preserved);
//     Intersection retracts handles and types as Corner.
PathData cleanup_loop(PathData refit_pd, const KeeperSet& keepers);

// s142 m1 — probe variant of cleanup_loop. Same delete walk, NO
// byte-for-byte handle restoration. Hypothesis: the s140 byte-for-byte
// restore step writes the originally-authored handles back over the
// shape-preserving handles BezierPath::delete_node had renegotiated,
// causing the spike/over-extension symptoms on complex shapes.
//
// v2 algorithm:
//   1. Tag keepers (same as v1).
//   2. Delete every untagged node descending — delete_node's
//      shape-preserving fit (least-squares 2-handle) renegotiates
//      surviving neighbours' handles correctly through each removal.
//   3. For surviving keepers:
//        - OriginalAnchor: restore *type only* from KeeperPoint::source.
//          Handles stay exactly where delete_node put them.
//        - Intersection: retract handles, type Corner (same as v1).
//
// Wired temporarily as the cleanup function called by Canvas. v1 is
// preserved adjacent for rollback. If the probe succeeds we keep v2
// and retire v1; if it fails we revert by flipping the call site.
PathData cleanup_loop_v2(PathData refit_pd, const KeeperSet& keepers);

// s142 m2 — guards-as-keepers cleanup. Same delete walk as v2, but
// SyntheticGuard keepers survive into the output as Smooth on-curve
// anchors. The guard's *position* is what's matched (from
// KeeperPoint::pos); after the delete walk, the surviving guard's
// type is set to Smooth. delete_node has already renegotiated handles
// during the walk; v3 leaves them as-is for guards (no source byte to
// restore), preserving the smooth-on-curve scaffolding the enrich
// phase set up.
//
// Hypothesis (s142 m1 result + Scott's "save the triplets" insight):
// v2's improvement was real but incomplete. v2 leaves originals as
// type-only restored — handles via delete_node fit. Where v2 still
// drifts is at intersections where originals' renegotiated handles
// have lost their local curve constraint (no nearby scaffolding to
// pin shape). v3 keeps the enrich-phase guards through cleanup so
// the local curve shape on either side of every intersection has
// surviving smooth anchors holding it; originals' handles renegotiate
// against neighbours that include guards, not just other originals
// or retracted intersection-corners.
//
// Output node count: O(N_originals + N_intersections + 2*N_originals)
// per loop, vs v2's O(N_originals + N_intersections). Higher node
// count, but type-honest (Smooth is Smooth by construction, not by
// "Corner with smooth-looking handles").
PathData cleanup_loop_v3(PathData refit_pd, const KeeperSet& keepers);

// s142 m5/m6 — apex-pinning + span-filler cleanup. Builds on v3.
// Two promotion passes run between the keeper claim and the delete walk:
//
//   1. Apex promotion (m5): scan untagged polyline nodes for local
//      maxima of turning angle; promote each apex above
//      `apex_min_turn_deg` to a CurvatureApex keeper. Pins shape at
//      rapid-curvature features.
//
//   2. Span filler promotion (m6): walk between consecutive tagged
//      nodes (keepers + just-promoted apexes); if the untagged run
//      between two tagged nodes exceeds `max_untagged_run` length,
//      promote the midpoint of that run to a SpanFiller keeper.
//      Single-pass — recurse not yet implemented (added later if
//      visual evidence demands). Breaks long gentle arcs so each
//      delete_node span is short enough for least-squares 2-handle
//      fit to express honestly.
//
// Both promotions tag as Smooth in finalisation (handles via
// delete_node fit, identical to SyntheticGuard finalisation).
//
// Quality tunables (one slider may drive both in s142 future work):
//   - apex_min_turn_deg: lower → more apexes saved → faithful (more nodes)
//   - max_untagged_run:  lower → more span fillers → faithful (more nodes)
//
// Pass `max_untagged_run = 0` (or any value <= 1) to disable span
// filling and run pure m5 apex-only cleanup.
PathData cleanup_loop_v4(PathData refit_pd, const KeeperSet& keepers,
                         double apex_min_turn_deg,
                         int max_untagged_run);

} // namespace refit

} // namespace Curvz
