#include "math/BooleanOpsRefit.hpp"
#include "math/CubicSegment.hpp"
#include "CurvzLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

// ══════════════════════════════════════════════════════════════════════════════
// BooleanOpsRefit — Clipper2 boolean-op output reconstruction.
//
// Four-stage pipeline (s139 m5):
//
//   1. ENRICH     — enrich_operands: insert synthetic guard anchors
//                   around every original anchor in the input geometry,
//                   so each original sits as the middle of a uniform
//                   Corner-Corner-Corner triple after Clipper2.
//   2. CLIP       — boolean_op_clipper (external): Clipper2 boolean op
//                   on the enriched geometry, emits polyline result.
//   3. REMOVE     — cleanup_loop (pass 1, "remove constant unnecessary"):
//      CONSTANT     walks the polyline, keeps tagged keepers (originals,
//                   intersections, synthetic guards), deletes the rest
//                   by guard-band rule. Retracts handles on middles
//                   (originals + intersections); leaves siblings
//                   (synthetic guards) alone with their renegotiated
//                   handles. Output is uniform C-C-C triples.
//   4. REMOVE     — reduce_loop (pass 2, "remove refined unnecessary"):
//      REFINED      geometric classifier on each triple. Currently STUB
//                   in s139 m5; full classifier ships in s140.
//
// Each stage's contract is the input to the next. enrich_operands
// guarantees uniform triple structure regardless of input geometry
// peculiarities (adjacent originals, lone keepers on straight runs,
// arbitrary Clipper2 seam alignment).
//
// All four stages are wired into Canvas::boolean_op behind app-tier
// toggles (AppPreferences::boolean_cleanup_enabled controls stages 1+3;
// boolean_reduce_enabled controls stage 4 conditional on stage 3).
// Both off → behaviour identical to pre-s139 (raw Clipper2 polyline output).
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {
namespace refit {

// ── Subpath-pair intersection collector ──────────────────────────────────────
// Same algorithm as the retired hand-rolled engine's find_intersections in
// src/math/BooleanOps.cpp (lines 151-226), distilled to "emit world-space
// points only". Tolerances and filter logic are reproduced verbatim — they
// were earned over multiple sessions of empirical testing on the cases
// that mattered.
//
// Two filters applied to each (ta, tb) hit reported by Bézier clipping:
//   1. ENDPOINT GAP — if the two curves' evaluations at (ta, tb) differ by
//      more than ISECT_GAP_DOC, the convex-hull rejection failed on a
//      near-miss; reject as false positive.
//   2. TRANSVERSALITY — the unit tangents at the contact point must have
//      |sin(angle)| > ISECT_CROSS_EPS. Tangent / kissing contacts touch
//      without crossing; the boundary doesn't swap sides, so they're not
//      real boolean-op intersections.
//
// Edge-of-segment dedup: a hit at t≈1 on segment ia will be reported again
// at t≈0 on segment ia+1. Skip the t≈1 report unless we're on the last
// segment of the path.
static void collect_pair_intersections(
    const BezierPath& A,
    const BezierPath& B,
    std::vector<Vec2>& out)
{
    const int na = A.segment_count();
    const int nb = B.segment_count();
    if (na <= 0 || nb <= 0) return;

    for (int ia = 0; ia < na; ++ia) {
        CubicSegment sa = A.segment(ia);
        for (int ib = 0; ib < nb; ++ib) {
            CubicSegment sb = B.segment(ib);
            auto pairs = sa.intersect(sb, ISECT_EPS, 48);
            for (auto& [ta, tb] : pairs) {
                if (ta > 1.0 - ISECT_EPS * 10 && ia < na - 1) continue;
                if (tb > 1.0 - ISECT_EPS * 10 && ib < nb - 1) continue;

                const Vec2 pa = sa.at(ta);
                const Vec2 pb = sb.at(tb);

                // Filter 1: endpoint gap
                if ((pa - pb).length() > ISECT_GAP_DOC) continue;

                // Filter 2: transversality
                const Vec2 tga = sa.tangent(ta);
                const Vec2 tgb = sb.tangent(tb);
                const double la = tga.length();
                const double lb = tgb.length();
                if (la > 1e-12 && lb > 1e-12) {
                    const double cross = std::abs(tga.x * tgb.y - tga.y * tgb.x);
                    const double sin_angle = cross / (la * lb);
                    if (sin_angle < ISECT_CROSS_EPS) continue;
                }

                // Canonical world position: average of both evaluations
                out.push_back((pa + pb) * 0.5);
            }
        }
    }
}

// ── Position-distance dedup of intersection points ───────────────────────────
// Two intersections within ISECT_DEDUP_TOL_SQ of each other are the same
// crossing for cleanup purposes. Collapse runs of near-duplicates into the
// first occurrence. O(n²) but n is tiny (handful of intersections per op).
static void dedup_intersections_by_position(std::vector<Vec2>& pts) {
    std::vector<Vec2> kept;
    kept.reserve(pts.size());
    for (const auto& p : pts) {
        bool is_dup = false;
        for (const auto& k : kept) {
            if (p.dist_sq(k) <= ISECT_DEDUP_TOL_SQ) { is_dup = true; break; }
        }
        if (!is_dup) kept.push_back(p);
    }
    pts = std::move(kept);
}

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — enrich_operands  (s139 m5 — stage 1 of 4)
// ══════════════════════════════════════════════════════════════════════════════
//
// Insert two synthetic guard anchors around every original anchor in
// every input subpath. The guards sit a small distance along the chord
// toward each neighbour. Result: every original anchor is positionally
// flanked by two synthetic guards in the enriched path.
//
// Why pre-Clipper2 enrichment beats post-Clipper2 reconstruction:
//   - Adjacent originals (rect-corner pairs joining on a shared edge)
//     would otherwise produce keeper-keeper adjacencies in the cleanup
//     output, breaking the C-C-C triplet assumption.
//   - Lone keepers on long straight runs (rect edges that flatten to a
//     single Clipper2 segment) would otherwise produce 1-node motifs
//     where a triple is expected.
//   - Seam alignment: by pre-placing the guards in the input geometry,
//     each original anchor's triple structure is robust to whatever
//     position Clipper2 chooses for the polyline start.
//
// Synthetic guards:
//   - Have type Corner with handles meeting at the anchor (no curvature
//     contribution; they are positional anchors only).
//   - Sit at chord-direction offset of ENRICH_OFFSET (default 0.5 doc
//     units), capped at ENRICH_OFFSET_FRAC_CAP (25%) of segment length
//     for short edges, floored at ENRICH_OFFSET_MIN (1e-3) for
//     degenerate inputs.
//   - Closed paths use modular wrap for prev/next neighbours; open
//     paths get one-sided guards on the endpoints (only the side
//     facing into the path).
// ══════════════════════════════════════════════════════════════════════════════
namespace {

// Enrich a single subpath, accumulating synthetic-guard keepers.
//
// Two-pass per-side insertion (s140 m1):
//
//   Pass 1 — out-side. For each original anchor, split the segment
//            going OUT of it at small parameter t_post. The split places
//            a post-guard on the curve, tangent by construction. The
//            anchor's in-handle stays exactly as authored (untouched);
//            its out-handle gets renegotiated by insert_node_at to
//            preserve the curve shape together with the new post-guard.
//
//   Pass 2 — in-side. For each ORIGINAL anchor (now at every-other
//            position in out, because pass 1 inserted post-guards
//            between them), split the segment coming IN to it at
//            parameter t_pre near 1. Same on-curve, tangent-by-split
//            placement. The anchor's out-handle is already settled
//            from pass 1 (the post-guard sits on it cleanly) and stays
//            untouched in pass 2; only the in-handle gets renegotiated.
//
// Each side of every anchor is touched in isolation. No segment is
// ever split twice. Pre-guards split a segment whose left endpoint is
// a post-guard from pass 1 — that segment is a piece of the original
// curve, geometrically clean.
//
// Walks anchors in DESCENDING order in each pass so that earlier
// indices stay valid as later insertions push their tail indices
// upward. Standard insert-in-descending-order pattern.
//
// Closed paths:  N anchors → 2N output nodes (anchor, post-guard, anchor,
//                post-guard, ...; pre-guards inserted between every
//                post-guard and the following anchor in pass 2 → 3N).
// Open paths:    last anchor has no out-segment → no post-guard. First
//                anchor has no in-segment → no pre-guard. Output is
//                3N - 2 nodes (one-sided guards on both endpoints).
//
// Logs every emitted node (anchor + handles + type) at INFO level so
// the user can grep the log and visually validate the enrichment
// result. Tag: "BooleanOpsRefit::enrich_subpath".
void enrich_subpath(const BezierPath& in, BezierPath& out, KeeperSet& guards_out,
                    int diag_subpath_idx)
{
    const int n_in = (int)in.nodes.size();
    out.closed = in.closed;
    out.nodes.clear();
    out.node_sets.clear();

    if (n_in == 0) return;
    if (n_in == 1) { out.nodes = in.nodes; return; }

    // ── Diagnostic logger ───────────────────────────────────────────────────
    auto type_char = [](BezierNode::Type t) -> char {
        switch (t) {
            case BezierNode::Type::Smooth:    return 'S';
            case BezierNode::Type::Corner:    return 'C';
            case BezierNode::Type::Cusp:      return 'U';
            case BezierNode::Type::Symmetric: return 'Y';
        }
        return '?';
    };
    auto log_node = [&](const char* role, int out_idx, const BezierNode& n) {
        LOG_INFO("BooleanOpsRefit::enrich_subpath: sp={} out_idx={:>3} {} "
                 "type={} anchor=({:.3f},{:.3f}) "
                 "in_h=({:.3f},{:.3f}) out_h=({:.3f},{:.3f})",
                 diag_subpath_idx, out_idx, role, type_char(n.type),
                 n.x, n.y, n.cx1, n.cy1, n.cx2, n.cy2);
    };

    LOG_INFO("BooleanOpsRefit::enrich_subpath: sp={} BEGIN — input has {} "
             "nodes (closed={})",
             diag_subpath_idx, n_in, in.closed ? "yes" : "no");

    // ── Compute t value for a guard split, given the segment's chord length.
    // Chord length is a reasonable proxy for arc length at small t; exact
    // arc-length via CubicSegment::length is overkill since the t value is
    // small either way. Capped at ENRICH_OFFSET_FRAC_CAP fraction of length
    // so very short segments don't get t > 0.25; floored at ENRICH_OFFSET_MIN
    // to avoid degenerate-zero. Returned t is for the post-guard side
    // (close to 0); pre-guard t is 1 - t.
    auto compute_t = [](double chord_len) -> double {
        if (chord_len <= 0.0) return ENRICH_OFFSET_FRAC_CAP;  // safe fallback
        double off = ENRICH_OFFSET;
        if (off > chord_len * ENRICH_OFFSET_FRAC_CAP) off = chord_len * ENRICH_OFFSET_FRAC_CAP;
        if (off < ENRICH_OFFSET_MIN) off = ENRICH_OFFSET_MIN;
        const double t = off / chord_len;
        // Clamp into [0, 0.5) so post-guard and pre-guard never collide.
        if (t >= 0.5) return 0.49;
        return t;
    };

    auto chord_length_between = [](const BezierNode& a, const BezierNode& b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        return std::sqrt(dx*dx + dy*dy);
    };

    // ── Seed: copy input wholesale into out ─────────────────────────────────
    // We will mutate `out` via insert_node_at, walking in descending order
    // so that earlier indices stay valid as later insertions extend the
    // tail. After both passes, originals will sit at known stride-3
    // positions (closed) or near-stride-3 positions (open endpoints).
    out.nodes = in.nodes;

    // Track which positions in out are originals. The is_original vector
    // grows alongside out.nodes — set false for every inserted guard.
    std::vector<bool> is_original(out.nodes.size(), true);

    // ── Pass 1 — out-side (post-guards) ─────────────────────────────────────
    // For closed:  walk i = n_in-1 down to 0. Each i has segment i (going
    //              from anchor i to anchor (i+1) mod n) — split it at t_post.
    // For open:    walk i = n_in-2 down to 0. Last anchor has no out-segment.
    //
    // insert_node_at inserts the new node at index seg_idx + 1 = i + 1.
    // Descending order ensures later i's insertion has already happened
    // before we process earlier i's, so out's earlier-than-i indices are
    // unchanged when we operate on i.
    {
        const int last_i = in.closed ? (n_in - 1) : (n_in - 2);
        for (int i = last_i; i >= 0; --i) {
            const int j = in.closed ? ((i + 1) % n_in) : (i + 1);
            const double clen = chord_length_between(in.nodes[i], in.nodes[j]);
            const double t = compute_t(clen);
            // Capture the anchor i's pre-split in-handle and out-anchor's
            // post-split in-handle for the log; insert_node_at mutates them.
            out.insert_node_at(i, t);
            // The new guard landed at index i+1. Mark it non-original.
            is_original.insert(is_original.begin() + i + 1, false);
        }
    }

    // ── Pass 2 — in-side (pre-guards) ───────────────────────────────────────
    // After pass 1, originals sit at positions p in out where is_original[p].
    // For each such position (descending), split the segment going INTO p:
    // segment index p-1 (closed wrap: if p == 0, that's segment last = out.nodes.size()-1).
    // Use t_pre = 1 - compute_t(chord_length_of_that_segment).
    //
    // Guard insertion goes at (p-1)+1 = p, pushing the original anchor to
    // p+1. Update is_original accordingly.
    //
    // Open-path edge: skip p == 0 (no in-segment for the first anchor).
    {
        // Collect original positions in descending order. After pass 1 they
        // sit at deterministic positions, but compute them from is_original
        // for robustness against edge cases.
        std::vector<int> original_positions;
        for (int p = 0; p < (int)is_original.size(); ++p) {
            if (is_original[p]) original_positions.push_back(p);
        }
        // Descending walk.
        for (auto it = original_positions.rbegin(); it != original_positions.rend(); ++it) {
            const int p = *it;
            // Open-path: no in-segment for the first anchor.
            if (!out.closed && p == 0) continue;
            // Segment going INTO p: index p-1 if p>0, else last (closed wrap).
            const int seg_idx = (p == 0) ? ((int)out.nodes.size() - 1) : (p - 1);
            // Chord length of that segment (current state of out).
            const int seg_lo = seg_idx;
            const int seg_hi = (seg_idx + 1) % (int)out.nodes.size();
            const double clen = chord_length_between(out.nodes[seg_lo], out.nodes[seg_hi]);
            const double t_offset = compute_t(clen);
            const double t_pre = 1.0 - t_offset;
            out.insert_node_at(seg_idx, t_pre);
            // The new guard landed at index seg_idx + 1 (which equals p for
            // p > 0; equals out.nodes.size() - 1 for the wrap case after
            // insertion, but we don't need is_original updates after this
            // walk since we've already collected the targets).
            const int insert_idx = seg_idx + 1;
            is_original.insert(is_original.begin() + insert_idx, false);
        }
    }

    // ── Emit keeper points for guards + log every node ──────────────────────
    // After both passes, every position p in out where is_original[p] is
    // false is a synthetic guard. For each guard, we need to know which
    // original anchor it flanks (parent_pos for the KeeperPoint). The
    // simple rule: a guard's parent is the nearest original anchor in
    // out — for closed loops, walk neighbours in both directions and
    // pick the closer original. In practice every guard is immediately
    // adjacent to its parent (post-guards sit at index parent+1, pre-
    // guards at index parent-1 in the closed-loop case after both
    // passes), so the nearest-by-index rule is equivalent.
    auto find_nearest_original = [&](int p) -> int {
        const int n = (int)out.nodes.size();
        // Search outward by index distance.
        for (int d = 1; d < n; ++d) {
            const int forward  = out.closed ? ((p + d) % n) : (p + d);
            const int backward = out.closed ? ((p - d + n) % n) : (p - d);
            if (forward >= 0 && forward < n && is_original[forward])  return forward;
            if (backward >= 0 && backward < n && is_original[backward]) return backward;
        }
        return -1;
    };

    const int n_out = (int)out.nodes.size();
    int guard_count = 0;
    for (int p = 0; p < n_out; ++p) {
        const BezierNode& node = out.nodes[p];
        if (is_original[p]) {
            log_node("ORIGINAL  ", p, node);
        } else {
            const int parent_idx = find_nearest_original(p);
            const Vec2 parent_pos = (parent_idx >= 0)
                ? Vec2{out.nodes[parent_idx].x, out.nodes[parent_idx].y}
                : Vec2{node.x, node.y};
            // Distinguish post-guard (forward neighbour-of-parent) from
            // pre-guard (backward neighbour-of-parent) for log clarity.
            const int n = n_out;
            const int next_p = out.closed ? ((p + 1) % n) : (p + 1);
            const bool is_post = (parent_idx >= 0 && parent_idx == ((p - 1 + n) % n));
            (void)next_p;
            log_node(is_post ? "POST_GUARD" : "PRE_GUARD ", p, node);

            KeeperPoint kp;
            kp.pos = Vec2{node.x, node.y};
            kp.origin = KeeperPoint::Origin::SyntheticGuard;
            kp.parent_pos = parent_pos;
            kp.source = nullptr;
            guards_out.push_back(kp);
            ++guard_count;
        }
    }

    LOG_INFO("BooleanOpsRefit::enrich_subpath: sp={} END — {} originals, "
             "{} guards inserted → {} output nodes",
             diag_subpath_idx, n_in, guard_count, n_out);
}

} // anonymous namespace

void enrich_operands(
    const std::vector<std::vector<BezierPath>>& operands_in,
    std::vector<std::vector<BezierPath>>&       operands_out,
    KeeperSet&                                  guards_out)
{
    operands_out.clear();
    operands_out.resize(operands_in.size());
    guards_out.clear();

    int total_originals = 0;
    int diag_sp = 0;  // flat counter across all (operand, subpath) pairs for log
    for (size_t oi = 0; oi < operands_in.size(); ++oi) {
        const auto& subs_in = operands_in[oi];
        auto&       subs_out = operands_out[oi];
        subs_out.resize(subs_in.size());
        for (size_t si = 0; si < subs_in.size(); ++si) {
            total_originals += (int)subs_in[si].nodes.size();
            enrich_subpath(subs_in[si], subs_out[si], guards_out, diag_sp);
            ++diag_sp;
        }
    }

    LOG_INFO("BooleanOpsRefit::enrich_operands: {} originals → {} synthetic guards "
             "across {} operands ({} subpaths total)",
             total_originals, (int)guards_out.size(),
             (int)operands_in.size(), diag_sp);
}

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — compute_keeper_set
// ══════════════════════════════════════════════════════════════════════════════
KeeperSet compute_keeper_set(
    const std::vector<std::vector<BezierPath>>& operands,
    BooleanOpType /*op*/)
{
    KeeperSet keepers;

    // ── Phase 1: every input anchor is a candidate OriginalAnchor keeper ─────
    // Filtering happens later in cleanup_loop by position match against the
    // actual Clipper2 output: anchors that survived the boolean op will land
    // within KEEPER_MATCH_TOL of an output polyline vertex; anchors that
    // didn't survive simply never match anything.
    //
    // We hold raw pointers back into operands. The Canvas integration in
    // boolean_op_clipper keeps operands alive across the call, so the
    // KeeperSet lifetime is bounded to the same scope.
    std::size_t anchor_count = 0;
    for (const auto& operand : operands) {
        for (const auto& subpath : operand) {
            for (const auto& node : subpath.nodes) {
                KeeperPoint kp;
                kp.pos    = Vec2{node.x, node.y};
                kp.origin = KeeperPoint::Origin::OriginalAnchor;
                kp.source = &node;
                keepers.push_back(kp);
                ++anchor_count;
            }
        }
    }

    // ── Phase 2: analytic intersections between distinct subpath pairs ───────
    // Build a flat (BezierPath*, owner-index) list of all subpaths, then
    // iterate unordered pairs. Cross-operand and within-operand intersections
    // are both collected — Clipper2's topology handling decides which
    // actually appear in the output, the keeper set just lists candidates.
    //
    // Self-intersections within a single subpath are NOT collected. Those
    // are an input-quality issue; Clipper2 handles them as a topology
    // problem; they aren't new corners created by the boolean op.
    struct SubpathRef { const BezierPath* bp; std::size_t op_idx; std::size_t sp_idx; };
    std::vector<SubpathRef> all_subpaths;
    for (std::size_t oi = 0; oi < operands.size(); ++oi) {
        for (std::size_t si = 0; si < operands[oi].size(); ++si) {
            all_subpaths.push_back({&operands[oi][si], oi, si});
        }
    }

    std::vector<Vec2> raw_intersections;
    for (std::size_t i = 0; i < all_subpaths.size(); ++i) {
        for (std::size_t j = i + 1; j < all_subpaths.size(); ++j) {
            collect_pair_intersections(*all_subpaths[i].bp,
                                       *all_subpaths[j].bp,
                                       raw_intersections);
        }
    }

    const std::size_t pre_dedup = raw_intersections.size();
    dedup_intersections_by_position(raw_intersections);

    for (const auto& pos : raw_intersections) {
        KeeperPoint kp;
        kp.pos    = pos;
        kp.origin = KeeperPoint::Origin::Intersection;
        kp.source = nullptr;
        keepers.push_back(kp);
    }

    LOG_INFO("BooleanOpsRefit: keeper set built — {} original anchors, "
             "{} intersections ({} before dedup), {} subpaths across {} operands",
             anchor_count, raw_intersections.size(), pre_dedup,
             all_subpaths.size(), operands.size());

    return keepers;
}

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — cleanup_loop  (s139 m2 → s140 m3)
// ══════════════════════════════════════════════════════════════════════════════
//
// Walks the all-Corner refit PathData produced by refit_path_straight_cubics
// and reduces it to the minimum node set: the originals (with their
// originally-authored handles + types) and the intersections (as plain
// Corner anchors).
//
// s140 m3 algorithm — invert the walk. Per keeper, find its slot in the
// output; mark it kept. Everything not so marked gets deleted. No
// guard-band rule, no triplet structure assumptions, no pass 2:
//
//   1. Convert refit_pd → BezierPath bp.
//   2. For each keeper k in the keeper set (originals + intersections —
//      synthetic guards are NOT in this set in the m3 wiring; they served
//      their purpose at Clipper2 time and are now interpolant-equivalent):
//
//        Find best_idx in bp.nodes minimising dist² to k.pos, capped at
//        KEEPER_MATCH_TOL_SQ. If no node is within tolerance the keeper
//        didn't survive Clipper2 (e.g. cut away by a Subtract); skip it.
//
//        Bind keeper → best_idx in a per-output tag map. If two keepers
//        compete for the same best_idx the second one loses its slot
//        (this is rare and almost always means the keepers are
//        coincident in the input — cleanup is doing the right thing
//        either way).
//
//   3. Delete every untagged index in descending order, with a hard
//      floor at 3 nodes to keep the path closed.
//
//   4. For each surviving (post-delete) tagged node:
//        - OriginalAnchor → restore *KeeperPoint::source byte-for-byte.
//          The user's authored handles + type come back exactly as drawn,
//          regardless of what the polyline refit had given them. This is
//          why the smooth-on-curve guards in m1+m2 don't need to survive
//          to here: the original record is the ground truth.
//        - Intersection   → retract handles to anchor, type Corner.
//          Intersections are direction changes by definition; corner is
//          the right type, no handles to record.
//
//   5. Return bp.to_path_data().
//
// Defensive cases:
//   - Loops with fewer than 3 refit nodes: return unchanged (caller
//     should already have filtered, but cheap to repeat).
//   - Loops with no matched keepers: return unchanged. This is the
//     "boolean op produced a wholly new boundary with no original
//     anchors and no analytic intersections" pathological case;
//     deleting everything would open the loop.
//   - Hard floor of 3 nodes during the delete walk. delete_node on a
//     2-node path opens it (closed → false), which is wrong for a
//     cleaned boolean result.
// ══════════════════════════════════════════════════════════════════════════════
namespace {

// Find the index in `bp` of the node closest to `pos`, within
// KEEPER_MATCH_TOL_SQ. Returns -1 if none. Linear is fine — node counts
// are at most a few hundred and the call is per-keeper, not per-node.
int nearest_node_index(const BezierPath& bp, const Vec2& pos) {
    int best = -1;
    double best_d2 = KEEPER_MATCH_TOL_SQ;
    const int n = (int)bp.nodes.size();
    for (int i = 0; i < n; ++i) {
        const double dx = bp.nodes[i].x - pos.x;
        const double dy = bp.nodes[i].y - pos.y;
        const double d2 = dx*dx + dy*dy;
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

} // anonymous namespace

PathData cleanup_loop(PathData refit_pd, const KeeperSet& keepers) {
    if (refit_pd.nodes.size() < 3) return refit_pd;

    BezierPath bp = BezierPath::from_path_data(refit_pd);
    const int n_initial = (int)bp.nodes.size();

    // ── Step 2: per-keeper claim walk ────────────────────────────────────────
    // Each keeper finds its closest output node; that node is tagged with
    // a pointer to the keeper for later restoration. If a node is already
    // tagged (two keepers in tolerance of the same node — rare), the
    // second keeper loses; skip silently.
    std::vector<const KeeperPoint*> tag(n_initial, nullptr);
    int matched = 0, missed = 0;
    for (const auto& k : keepers) {
        // Skip synthetic guards if any slipped into the keeper set —
        // m3's contract is "originals + intersections only," but be
        // defensive in case Canvas merges them.
        if (k.origin == KeeperPoint::Origin::SyntheticGuard) continue;

        const int idx = nearest_node_index(bp, k.pos);
        if (idx < 0) { ++missed; continue; }
        if (tag[idx]) continue;  // already claimed by another keeper
        tag[idx] = &k;
        ++matched;
    }

    if (matched == 0) {
        LOG_WARN("BooleanOpsRefit::cleanup_loop: no keepers matched the {} "
                 "refit nodes — returning loop unchanged",
                 n_initial);
        return refit_pd;
    }

    // ── Step 3: delete every untagged node, descending ───────────────────────
    // Hard floor of 3 nodes so delete_node never opens the path.
    std::vector<int> deletable;
    deletable.reserve(n_initial);
    for (int i = 0; i < n_initial; ++i) {
        if (!tag[i]) deletable.push_back(i);
    }
    std::sort(deletable.rbegin(), deletable.rend());

    int kept = n_initial;
    for (int idx : deletable) {
        if (kept <= 3) break;
        bp.delete_node(idx);
        // Mirror the deletion in tag[] so post-delete index → keeper
        // mapping stays consistent. Erasing from tag at the same index
        // shifts the later entries to match bp.nodes.
        tag.erase(tag.begin() + idx);
        --kept;
    }

    // ── Step 4: restore originals / cornerize intersections ──────────────────
    const int n_final = (int)bp.nodes.size();
    int restored_originals = 0;
    int cornerized_intersections = 0;
    for (int i = 0; i < n_final; ++i) {
        const KeeperPoint* k = tag[i];
        if (!k) continue;  // shouldn't happen post-delete, but defensive
        if (k->origin == KeeperPoint::Origin::OriginalAnchor && k->source) {
            // Byte-for-byte restore. The user's authored anchor record is
            // the ground truth for handles + type.
            bp.nodes[i] = *k->source;
            ++restored_originals;
        } else if (k->origin == KeeperPoint::Origin::Intersection) {
            // Genuine direction change. Corner with retracted handles.
            bp.nodes[i].cx1 = bp.nodes[i].x;
            bp.nodes[i].cy1 = bp.nodes[i].y;
            bp.nodes[i].cx2 = bp.nodes[i].x;
            bp.nodes[i].cy2 = bp.nodes[i].y;
            bp.nodes[i].type = BezierNode::Type::Corner;
            ++cornerized_intersections;
        }
    }

    LOG_INFO("BooleanOpsRefit::cleanup_loop: {} → {} nodes "
             "({} keepers matched, {} missed, {} originals restored, "
             "{} intersections cornerized)",
             n_initial, n_final, matched, missed,
             restored_originals, cornerized_intersections);

    bp.node_sets.clear();
    return bp.to_path_data();
}

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — reduce_loop  (s139 m4 → m5 — pass 2)
// ══════════════════════════════════════════════════════════════════════════════
//
// STATUS s139 m5: STUB — returns input unchanged.
//
// Background. m4 shipped a stride-3 triplet collapse based on middle's
// handle topology (retracted vs extended). It worked on the rect+circle
// case where Clipper2 happened to produce uniform triple structure with
// the seam at a triplet boundary. The rect+ellipse case (Scott's m4
// stress test) revealed two failure modes:
//   - Adjacent originals (rect-corner pair on a shared edge) produce
//     2-node motifs, not 3.
//   - Lone keepers on long straight runs produce 1-node motifs.
//   - Seam alignment varies — Clipper2 picks the start position based
//     on its own internal logic, not on triplet boundaries.
//
// The fix is structural, not algorithmic: pre-Clipper2 enrichment
// (s139 m5 stage 1) inserts synthetic guards around every original
// anchor in the input, guaranteeing uniform triple structure in the
// cleanup output. With enrichment in place, pass 1 produces
// Corner-Corner-Corner triples reliably.
//
// The m4 stride-3 rule no longer fits the new pass-1 contract. Under
// the new contract, every triple's middle is retracted (originals AND
// intersections both retract). The "always delete middle + prev"
// degenerate rule would drop real corner anchors at sharp turns, which
// is the opposite of what we want.
//
// The replacement is the geometric classifier (s140 — Ship B):
//   - Triple is collinear (siblings + middle on a line within ε):
//     middle is on a straight run, redundant. Drop middle only.
//   - Bend at middle is sharp (angle threshold): middle is a real
//     corner anchor (or intersection cusp). Drop both siblings.
//   - Smooth bend: middle is a smooth-curve position pin. Drop
//     middle + one sibling.
//
// Until that lands, reduce_loop is a no-op so the user sees pass-1
// output directly (uniform triples, ~3× the load-bearing minimum).
// The toggle stays exposed so the milestone path is visible in the
// UI; the user can flip it on, observe no change, confirm Ship A's
// pass-1 contract is correct, and report back.
// ══════════════════════════════════════════════════════════════════════════════
PathData reduce_loop(PathData cleaned_pd) {
    LOG_INFO("BooleanOpsRefit::reduce_loop: STUB (s139 m5) — returning {} nodes "
             "unchanged. Geometric classifier ships in s140.",
             (int)cleaned_pd.nodes.size());
    return cleaned_pd;
}

} // namespace refit
} // namespace Curvz
