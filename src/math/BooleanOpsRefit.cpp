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
// Three-phase pipeline (s140 final shape):
//
//   1. ENRICH      — enrich(): insert smooth on-curve guard anchors
//                    around every original anchor in the input geometry,
//                    via BezierPath::insert_node_at (same primitive used
//                    interactively for click-to-add-node). Two-pass
//                    per-side: out-side first, then in-side. Geometry
//                    is preserved exactly; guards exist only to carry
//                    tangent information through Clipper2's polyline
//                    phase.
//   2. CLIP        — boolean_op_clipper (external): Clipper2 boolean op
//                    on the enriched geometry, emits polyline result.
//                    BooleanOpsClipper::refit_path_straight_cubics turns
//                    each polyline edge into one Corner cubic.
//   3. CLEANUP     — cleanup_loop: per-keeper claim-and-restore walk on
//                    the refitted output. Tags each keeper's closest
//                    match in the polyline; deletes everything untagged;
//                    restores OriginalAnchor keepers byte-for-byte from
//                    KeeperPoint::source, retracts Intersection keepers
//                    to Corner. Synthetic guards are NOT keepers — they
//                    get deleted along with the rest of the polyline
//                    noise.
//
// Wired into Canvas::boolean_op behind AppPreferences::boolean_cleanup_enabled.
// Off → behaviour identical to pre-s139 (raw Clipper2 polyline output via
// BooleanOpsClipper's straight-cubic refit, no enrichment, no cleanup).
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
// Public entry point — enrich  (s140 — phase 1 of 3)
// ══════════════════════════════════════════════════════════════════════════════
//
// Insert two smooth on-curve guard anchors around every original anchor
// in every input subpath. The guards sit at small distance along each
// adjacent segment, exactly on the curve with tangent handles by
// construction (via BezierPath::insert_node_at). Result: every original
// anchor is positionally flanked by two on-curve guards in the enriched
// path, with the segments between them carrying tangent information
// through to Clipper2's polyline phase.
//
// Why pre-Clipper2 enrichment beats post-Clipper2 reconstruction:
// without guards, sharp corners and intersection cusps in the polyline
// output drag the curve away from the original shape after delete_node
// renegotiation. With smooth on-curve guards in the input, the polyline
// preserves enough tangent samples that the cleanup walk's per-keeper
// match step finds the right neighbourhood for restoration.
//
// Synthetic guards:
//   - Type Smooth, on-curve, tangent handles by construction.
//   - Sit at offset of ENRICH_OFFSET doc units along each adjacent
//     segment, capped at ENRICH_OFFSET_FRAC_CAP (25%) for short edges
//     and floored at ENRICH_OFFSET_MIN (1e-3) for degenerate inputs.
//   - Closed paths use modular wrap; open paths get one-sided guards
//     on endpoints (only the side facing into the path).
//   - NOT keepers — synthetic guards are transport, not destination,
//     and get deleted by the cleanup walk along with the rest of the
//     polyline noise.
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
void enrich_subpath(const BezierPath& in, BezierPath& out, int diag_subpath_idx)
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

    // ── Summary log ─────────────────────────────────────────────────────────
    // After both passes, each position p in out where is_original[p] is
    // false is a synthetic guard. We log the per-node detail at INFO level
    // for grep-based validation; positions are tagged ORIGINAL or GUARD
    // by their is_original flag, no further classification needed.
    const int n_out = (int)out.nodes.size();
    int guard_count = 0;
    for (int p = 0; p < n_out; ++p) {
        const BezierNode& node = out.nodes[p];
        if (is_original[p]) {
            log_node("ORIGINAL", p, node);
        } else {
            log_node("GUARD   ", p, node);
            ++guard_count;
        }
    }

    LOG_INFO("BooleanOpsRefit::enrich_subpath: sp={} END — {} originals, "
             "{} guards inserted → {} output nodes",
             diag_subpath_idx, n_in, guard_count, n_out);
}

} // anonymous namespace

void enrich(
    const std::vector<std::vector<BezierPath>>& operands_in,
    std::vector<std::vector<BezierPath>>&       operands_out)
{
    operands_out.clear();
    operands_out.resize(operands_in.size());

    int total_originals = 0;
    int diag_sp = 0;  // flat counter across all (operand, subpath) pairs for log
    for (size_t oi = 0; oi < operands_in.size(); ++oi) {
        const auto& subs_in = operands_in[oi];
        auto&       subs_out = operands_out[oi];
        subs_out.resize(subs_in.size());
        for (size_t si = 0; si < subs_in.size(); ++si) {
            total_originals += (int)subs_in[si].nodes.size();
            enrich_subpath(subs_in[si], subs_out[si], diag_sp);
            ++diag_sp;
        }
    }

    LOG_INFO("BooleanOpsRefit::enrich: {} originals across {} operands "
             "({} subpaths total)",
             total_originals,
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
// Public entry point — compute_keeper_set_with_guards  (s142 m2)
// ══════════════════════════════════════════════════════════════════════════════
//
// Same as compute_keeper_set, plus emits a third class of keeper:
// SyntheticGuard, one per anchor in `enriched` that doesn't appear in
// `originals` at the same position.
//
// We rely on enrich_subpath's contract: it copies originals into the
// output verbatim and inserts guards via insert_node_at, so an enriched
// path's anchors include all the original positions exactly, plus the
// guard positions.
//
// Identifying guards: walk each enriched subpath; for each anchor,
// check whether any original anchor in the corresponding original
// subpath lies within KEEPER_MATCH_TOL_SQ of it. If yes → original.
// If no → guard.
//
// (We could rely on stride-3 indexing from enrich's deterministic
// 3N-output rule, but position-based detection is robust to enrich
// implementation changes and degenerate-segment edge cases.)
// ══════════════════════════════════════════════════════════════════════════════
KeeperSet compute_keeper_set_with_guards(
    const std::vector<std::vector<BezierPath>>& originals,
    const std::vector<std::vector<BezierPath>>& enriched,
    BooleanOpType op)
{
    // Start with the regular keeper set: originals + intersections.
    KeeperSet keepers = compute_keeper_set(originals, op);

    // Walk enriched. Any anchor not in originals (by position) is a guard.
    std::size_t guard_count = 0;
    for (std::size_t oi = 0; oi < enriched.size(); ++oi) {
        if (oi >= originals.size()) break;  // shape mismatch defensive
        for (std::size_t si = 0; si < enriched[oi].size(); ++si) {
            if (si >= originals[oi].size()) break;
            const auto& orig_subpath = originals[oi][si];
            const auto& enr_subpath  = enriched[oi][si];
            for (const auto& enr_node : enr_subpath.nodes) {
                // Is this position one of the originals?
                bool is_original = false;
                for (const auto& orig_node : orig_subpath.nodes) {
                    const double dx = enr_node.x - orig_node.x;
                    const double dy = enr_node.y - orig_node.y;
                    if (dx*dx + dy*dy <= KEEPER_MATCH_TOL_SQ) {
                        is_original = true;
                        break;
                    }
                }
                if (is_original) continue;
                // Otherwise it's a guard (synthetic on-curve smooth anchor).
                KeeperPoint kp;
                kp.pos    = Vec2{enr_node.x, enr_node.y};
                kp.origin = KeeperPoint::Origin::SyntheticGuard;
                kp.source = nullptr;
                keepers.push_back(kp);
                ++guard_count;
            }
        }
    }

    LOG_INFO("BooleanOpsRefit: keeper set extended with {} synthetic guards "
             "(total keepers now {})",
             guard_count, keepers.size());

    return keepers;
}

// ══════════════════════════════════════════════════════════════════════════════
// s142 m3 — targeted enrichment at intersections
// ══════════════════════════════════════════════════════════════════════════════

namespace {

// Parametric-info-preserving sibling of collect_pair_intersections.
// Same filter logic (endpoint gap + transversality), same edge-of-
// segment dedup. Emits IntersectionHit with (seg_a, t_a, seg_b, t_b)
// preserved so the caller can inject anchors back onto either parent.
struct IntersectionHitInternal {
    Vec2  pos;
    int   seg_a;
    double t_a;
    int   seg_b;
    double t_b;
};

void collect_pair_hits(
    const BezierPath& A,
    const BezierPath& B,
    std::vector<IntersectionHitInternal>& out)
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

                if ((pa - pb).length() > ISECT_GAP_DOC) continue;

                const Vec2 tga = sa.tangent(ta);
                const Vec2 tgb = sb.tangent(tb);
                const double la = tga.length();
                const double lb = tgb.length();
                if (la > 1e-12 && lb > 1e-12) {
                    const double cross = std::abs(tga.x * tgb.y - tga.y * tgb.x);
                    const double sin_angle = cross / (la * lb);
                    if (sin_angle < ISECT_CROSS_EPS) continue;
                }

                IntersectionHitInternal hit;
                hit.pos   = (pa + pb) * 0.5;
                hit.seg_a = ia;
                hit.t_a   = ta;
                hit.seg_b = ib;
                hit.t_b   = tb;
                out.push_back(hit);
            }
        }
    }
}

// Inject one triplet (pre-guard, intersection-anchor, post-guard) on
// the subject subpath at (seg_idx, t). Walks insertions in descending
// parametric order so the seg_idx remains valid across all three
// insertions.
//
// Records the resulting indices in out_idx_pre / out_idx_isect / out_idx_post.
// These indices are valid IMMEDIATELY after this call but will shift
// if subsequent inject calls operate on lower seg_idx values. Caller
// is responsible for calling inject in descending (seg, t) order so
// earlier-injection indices don't shift.
//
// Note: insert_node_at inserts at index (seg_idx + 1), and the new
// node sits between the original endpoints of the segment. So three
// successive insertions on the SAME seg_idx, walking descending t,
// land in the order:
//   1st inject at t+eps → new node at seg_idx+1; segment seg_idx now
//      runs from (anchor_seg_idx) to (post_guard); segment seg_idx+1
//      runs from (post_guard) to (anchor_seg_idx+1).
//   2nd inject at t on segment seg_idx → splits the LEFT half. New
//      node lands at seg_idx+1, pushing the post_guard to seg_idx+2.
//   3rd inject at t-eps on segment seg_idx → splits the (now) left-
//      most quarter. New node at seg_idx+1, pushing intersection to
//      seg_idx+2 and post_guard to seg_idx+3.
//
// HOWEVER — the t values are in the local parametric frame of the
// CURRENT (split) segment, not the original. After inserting at t+eps,
// the segment seg_idx now spans the original t∈[0, t+eps], so a new
// inject at the original "t" would correspond to local t' = t / (t+eps).
// Same for t-eps becoming local t'' = (t-eps) / (t' * (t+eps)) = ...
// this gets messy.
//
// Cleaner approach: do NOT split-then-split. Each insert_node_at
// reparameterises. Simpler: insert all three at independent (seg_idx, t)
// pairs against the *current state*, picking the *highest-remaining* t
// each time.
//
// Even cleaner: do it in TWO passes. Pass 1: insert intersection anchor
// (1 split per hit) at (seg_idx, t_a). Pass 2: walk the now-modified
// path, find each intersection anchor (we recorded its index), and
// insert a guard on each side via insert_node_at on the segments
// adjacent to the intersection. This avoids the reparameterisation
// problem entirely — guard t is small (eps in absolute terms relative
// to the new segment's chord, which compute_t handles).
//
// We use the two-pass approach.

// Same compute_t pattern as enrich_subpath. Returns a small t value
// for placing a guard close to one end of a segment. s296 m14: uses
// ISECT_TRIPLET_OFFSET (smaller than ENRICH_OFFSET) so triplet guards
// hug the intersection rather than spreading along the curve.
inline double compute_guard_t(double chord_len) {
    if (chord_len <= 0.0) return ENRICH_OFFSET_FRAC_CAP;
    double off = ISECT_TRIPLET_OFFSET;
    if (off > chord_len * ENRICH_OFFSET_FRAC_CAP) off = chord_len * ENRICH_OFFSET_FRAC_CAP;
    if (off < ENRICH_OFFSET_MIN) off = ENRICH_OFFSET_MIN;
    const double t = off / chord_len;
    if (t >= 0.5) return 0.49;
    return t;
}

inline double chord_len_between(const BezierNode& a, const BezierNode& b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    return std::sqrt(dx*dx + dy*dy);
}

// Inject intersection triplets into a single subpath. Returns three
// vectors of indices (into the FINAL subpath state): one per role.
// Indices in the same array position index correspond to the same hit;
// e.g. out_pre_idx[k], out_isect_idx[k], out_post_idx[k] all relate
// to hit k.
//
// Walk strategy (two-pass, see comment above for why):
//   Pass A: insert each hit's intersection anchor at (seg, t).
//           Walk hits in descending (seg, t) order so prior insertions
//           don't shift not-yet-processed hits.
//   Pass B: for each intersection anchor (now at known indices in the
//           modified path), split the IN-segment at t_pre to make the
//           pre-guard, and the OUT-segment at t_post to make the
//           post-guard. Walk in descending intersection-index order so
//           guards on later intersections don't shift earlier ones.
void inject_intersection_triplets_on_subpath(
    BezierPath& subpath,
    const std::vector<std::pair<int, double>>& hits_seg_and_t,
    std::vector<int>& out_pre_idx,
    std::vector<int>& out_isect_idx,
    std::vector<int>& out_post_idx)
{
    out_pre_idx.clear();
    out_isect_idx.clear();
    out_post_idx.clear();
    if (hits_seg_and_t.empty()) return;

    // Pass A — insert intersection anchors. Sort hits descending by
    // (seg, t) so prior insertions' index shifts don't invalidate
    // remaining hits. Each insertion places an anchor at index
    // (seg + 1) in the current state of subpath.
    //
    // Track the (final) index of each hit's intersection anchor.
    // Because we walk descending, hits processed first land at higher
    // indices; subsequent insertions push them up by 1 each. We
    // compute the final index by replaying the inserts.
    const int n_hits = (int)hits_seg_and_t.size();
    std::vector<std::pair<int, double>> hits_sorted = hits_seg_and_t;
    // We need to remember the *original* hit ordering so out_*
    // arrays line up with caller's intent. We do that by carrying
    // the original index alongside.
    std::vector<int> hit_orig_idx(n_hits);
    for (int k = 0; k < n_hits; ++k) hit_orig_idx[k] = k;

    // Sort the (hit_orig_idx) array by descending (seg, t) of hits_sorted.
    std::sort(hit_orig_idx.begin(), hit_orig_idx.end(),
              [&](int a, int b) {
                  const auto& ha = hits_sorted[a];
                  const auto& hb = hits_sorted[b];
                  if (ha.first != hb.first) return ha.first > hb.first;
                  return ha.second > hb.second;
              });

    out_isect_idx.assign(n_hits, -1);
    for (int rank = 0; rank < n_hits; ++rank) {
        const int orig = hit_orig_idx[rank];
        const auto& [seg, t] = hits_sorted[orig];
        // insert at (seg, t) → new anchor lands at index seg+1
        subpath.insert_node_at(seg, t);
        const int landed_at = seg + 1;
        // All previously-inserted intersections (at higher rank, i.e.
        // earlier in the sorted walk) had higher seg, so they sit at
        // landed_at >= seg+2 currently → their index needs +1 because
        // we just inserted before them.
        //
        // Wait — they sit at higher seg, which means higher landed_at.
        // Inserting at lower seg pushes them up.
        for (int prev = 0; prev < rank; ++prev) {
            const int prev_orig = hit_orig_idx[prev];
            // If their landed index is >= landed_at, bump.
            if (out_isect_idx[prev_orig] >= landed_at) {
                ++out_isect_idx[prev_orig];
            }
        }
        out_isect_idx[orig] = landed_at;
    }

    // Pass B — inject pre and post guards around each intersection.
    // Walk intersections in descending current-index order so earlier
    // (lower-index) insertions don't shift later ones we still need.
    //
    // For a closed subpath with N nodes after pass A:
    //   pre-guard: split segment (isect_idx - 1 mod N) at t = 1 - small
    //              → new node lands at (isect_idx - 1 + 1) = isect_idx,
    //                pushing intersection to isect_idx + 1.
    //   post-guard: split segment isect_idx (which goes from isect to
    //               next) at t = small → new node lands at isect_idx + 1,
    //               pushing the original "next" further.
    //
    // For a NON-closed subpath: same logic but no wrap. If isect is the
    // first or last anchor (shouldn't happen for a real intersection
    // mid-curve, but defensive), we skip the unavailable side.
    //
    // We walk in descending isect_idx order to keep prior-step indices
    // stable. For each step we update all OTHER hits' indices that
    // need shifting, mirroring pass A's bookkeeping.
    out_pre_idx.assign(n_hits, -1);
    out_post_idx.assign(n_hits, -1);

    // Sort hit indices by descending current intersection-index.
    std::vector<int> orig_by_isect_desc(n_hits);
    for (int k = 0; k < n_hits; ++k) orig_by_isect_desc[k] = k;
    std::sort(orig_by_isect_desc.begin(), orig_by_isect_desc.end(),
              [&](int a, int b) {
                  return out_isect_idx[a] > out_isect_idx[b];
              });

    for (int rank = 0; rank < n_hits; ++rank) {
        const int orig = orig_by_isect_desc[rank];
        const int isect_idx = out_isect_idx[orig];
        const int n_now = (int)subpath.nodes.size();

        // ── post-guard side: segment going OUT of intersection ──────────
        // Segment index = isect_idx (closed wrap if isect_idx == n_now-1).
        bool post_ok = subpath.closed || (isect_idx < n_now - 1);
        if (post_ok) {
            const int post_seg = isect_idx;
            const int post_seg_lo = post_seg;
            const int post_seg_hi = (post_seg + 1) % n_now;
            const double clen = chord_len_between(subpath.nodes[post_seg_lo],
                                                  subpath.nodes[post_seg_hi]);
            const double t_post = compute_guard_t(clen);
            subpath.insert_node_at(post_seg, t_post);
            const int post_landed = post_seg + 1;
            // Update other hits' isect indices that we've already
            // recorded but haven't yet processed: any whose isect_idx
            // is >= post_landed must be bumped.
            for (int other = 0; other < n_hits; ++other) {
                if (other == orig) continue;
                if (out_isect_idx[other] >= post_landed) ++out_isect_idx[other];
                if (out_pre_idx[other]   >= post_landed) ++out_pre_idx[other];
                if (out_post_idx[other]  >= post_landed) ++out_post_idx[other];
            }
            // Our own intersection idx must also bump if post_landed
            // landed before/at it; here post_landed = isect_idx + 1
            // > isect_idx, so it doesn't.
            out_post_idx[orig] = post_landed;
        }

        // ── pre-guard side: segment coming IN to intersection ───────────
        // Now intersection sits at out_isect_idx[orig] (unchanged so far
        // for this hit). Segment going INTO it = (isect - 1 mod n).
        const int n_now2 = (int)subpath.nodes.size();
        const int isect_idx_now = out_isect_idx[orig];
        bool pre_ok = subpath.closed || (isect_idx_now > 0);
        if (pre_ok) {
            const int pre_seg = (isect_idx_now == 0)
                                ? (n_now2 - 1)
                                : (isect_idx_now - 1);
            const int pre_seg_lo = pre_seg;
            const int pre_seg_hi = (pre_seg + 1) % n_now2;
            const double clen = chord_len_between(subpath.nodes[pre_seg_lo],
                                                  subpath.nodes[pre_seg_hi]);
            const double t_pre = 1.0 - compute_guard_t(clen);
            subpath.insert_node_at(pre_seg, t_pre);
            const int pre_landed = pre_seg + 1;
            // Bump all other recorded indices that sit at >= pre_landed,
            // including this hit's own intersection and post-guard if
            // applicable.
            for (int other = 0; other < n_hits; ++other) {
                if (other != orig) {
                    if (out_isect_idx[other] >= pre_landed) ++out_isect_idx[other];
                    if (out_pre_idx[other]   >= pre_landed) ++out_pre_idx[other];
                    if (out_post_idx[other]  >= pre_landed) ++out_post_idx[other];
                }
            }
            // Our own indices: pre_landed lands at (isect-1)+1 = isect,
            // so the intersection bumps to isect+1, and post-guard
            // (which was at isect+1) bumps to isect+2.
            if (out_isect_idx[orig] >= pre_landed) ++out_isect_idx[orig];
            if (out_post_idx[orig]  >= pre_landed) ++out_post_idx[orig];
            out_pre_idx[orig] = pre_landed;
        }
    }

    // node_sets cleared after structural changes
    subpath.node_sets.clear();
}

} // anonymous namespace

KeeperSet enrich_at_intersections_and_build_keepers(
    const std::vector<std::vector<BezierPath>>& operands_in,
    std::vector<std::vector<BezierPath>>&       operands_out,
    BooleanOpType /*op*/)
{
    // ─────────────────────────────────────────────────────────────────────────
    // s142 m4 — inject intersection triplets onto BOTH curves at each hit.
    //
    // m3 injected only on operand[0] (the subject). Symptom: at intersections
    // where Clipper2's output topology routed the boundary through the
    // OTHER side, that side had no scaffolding → keeper match failed →
    // intersection got stripped → curve drifted / notched. Cure: scaffold
    // every intersection on both crossing curves so whichever side the
    // output traverses, the cleanup walk has triplets to anchor.
    //
    // Two passes:
    //   1. Gather hits per (operand, subpath) bucket. Each hit appears in
    //      both buckets — once with (seg_a, t_a) for operand A's subpath,
    //      once with (seg_b, t_b) for operand B's subpath.
    //   2. For each non-empty bucket, inject triplets and build keepers
    //      for that subpath in walk order.
    // ─────────────────────────────────────────────────────────────────────────
    operands_out.clear();
    operands_out.resize(operands_in.size());
    for (std::size_t oi = 0; oi < operands_in.size(); ++oi) {
        operands_out[oi] = operands_in[oi];  // copy by value
    }

    KeeperSet keepers;
    int total_intersections = 0;
    int total_guards = 0;

    if (operands_in.empty()) return keepers;

    // ── Pass 1 — gather hits into per-(operand, subpath) buckets ─────────────
    // hits_per_subpath[oi][si] is a vector<(seg, t)> of intersection
    // hits on that subpath. A single physical intersection between
    // (oi=0, si=0) and (oi=1, si=0) appears in both buckets, with the
    // (seg, t) coordinates appropriate to each.
    std::vector<std::vector<std::vector<std::pair<int, double>>>> hits_per_subpath;
    hits_per_subpath.resize(operands_in.size());
    for (std::size_t oi = 0; oi < operands_in.size(); ++oi) {
        hits_per_subpath[oi].resize(operands_in[oi].size());
    }

    // For every unordered pair of subpaths across distinct operands, collect
    // hits and push into both sides.
    //
    // Within-operand intersections (two subpaths in the same operand) are
    // not collected — same convention as compute_keeper_set / m3.
    for (std::size_t oa = 0; oa < operands_in.size(); ++oa) {
        for (std::size_t sa = 0; sa < operands_in[oa].size(); ++sa) {
            for (std::size_t ob = oa + 1; ob < operands_in.size(); ++ob) {
                for (std::size_t sb = 0; sb < operands_in[ob].size(); ++sb) {
                    std::vector<IntersectionHitInternal> raw;
                    collect_pair_hits(operands_in[oa][sa], operands_in[ob][sb], raw);
                    for (const auto& h : raw) {
                        hits_per_subpath[oa][sa].emplace_back(h.seg_a, h.t_a);
                        hits_per_subpath[ob][sb].emplace_back(h.seg_b, h.t_b);
                    }
                }
            }
        }
    }

    // ── Pass 2 — inject triplets into each subpath, build keepers ────────────
    // For each (oi, si) bucket: dedup by world position, inject triplets,
    // walk modified subpath in order to emit keepers tagged by role.
    int per_subpath_isect_total = 0;
    for (std::size_t oi = 0; oi < operands_in.size(); ++oi) {
        for (std::size_t si = 0; si < operands_in[oi].size(); ++si) {
            auto& hits = hits_per_subpath[oi][si];
            const auto& orig_subpath = operands_in[oi][si];
            auto& mod_subpath = operands_out[oi][si];

            // Position-distance dedup (when two hit reports share a position,
            // collapse to one — happens when shared-endpoint intersections
            // get reported across multiple subpath pairs).
            if (!hits.empty()) {
                std::vector<std::pair<int, double>> deduped;
                std::vector<Vec2> deduped_pos;
                for (const auto& h : hits) {
                    const Vec2 p = orig_subpath.segment(h.first).at(h.second);
                    bool dup = false;
                    for (const auto& kp : deduped_pos) {
                        if (p.dist_sq(kp) <= ISECT_DEDUP_TOL_SQ) { dup = true; break; }
                    }
                    if (!dup) {
                        deduped.push_back(h);
                        deduped_pos.push_back(p);
                    }
                }
                hits = std::move(deduped);
            }

            std::vector<int> pre_idx, isect_idx, post_idx;
            if (!hits.empty()) {
                inject_intersection_triplets_on_subpath(
                    mod_subpath, hits,
                    pre_idx, isect_idx, post_idx);
                per_subpath_isect_total += (int)isect_idx.size();
                for (int x : pre_idx)  if (x >= 0) ++total_guards;
                for (int x : post_idx) if (x >= 0) ++total_guards;
            }

            // Build keepers for this subpath.
            //   - injected intersection anchors → Intersection
            //   - injected pre/post guards      → SyntheticGuard
            //   - originals (everything else)   → OriginalAnchor with
            //                                     source pointer into orig
            //                                     in walk order
            std::vector<bool> is_pre(mod_subpath.nodes.size(), false);
            std::vector<bool> is_isect(mod_subpath.nodes.size(), false);
            std::vector<bool> is_post(mod_subpath.nodes.size(), false);
            for (int x : pre_idx)   if (x >= 0 && x < (int)is_pre.size())   is_pre[x] = true;
            for (int x : isect_idx) if (x >= 0 && x < (int)is_isect.size()) is_isect[x] = true;
            for (int x : post_idx)  if (x >= 0 && x < (int)is_post.size())  is_post[x] = true;

            std::size_t orig_walk_k = 0;
            for (std::size_t i = 0; i < mod_subpath.nodes.size(); ++i) {
                const auto& n = mod_subpath.nodes[i];
                KeeperPoint kp;
                kp.pos = Vec2{n.x, n.y};
                if (is_isect[i]) {
                    kp.origin = KeeperPoint::Origin::Intersection;
                    kp.source = nullptr;
                } else if (is_pre[i] || is_post[i]) {
                    kp.origin = KeeperPoint::Origin::SyntheticGuard;
                    kp.source = nullptr;
                } else {
                    // Original anchor — match to orig_subpath in walk order.
                    if (orig_walk_k < orig_subpath.nodes.size()) {
                        kp.origin = KeeperPoint::Origin::OriginalAnchor;
                        kp.source = &orig_subpath.nodes[orig_walk_k];
                        ++orig_walk_k;
                    } else {
                        // Shouldn't happen — defensive fallback.
                        kp.origin = KeeperPoint::Origin::SyntheticGuard;
                        kp.source = nullptr;
                    }
                }
                keepers.push_back(kp);
            }
        }
    }

    // total_intersections counts physical intersections (each appears
    // once per side, so per_subpath_isect_total is roughly 2× the
    // physical count). Report both for diagnostic clarity.
    total_intersections = per_subpath_isect_total / 2;
    LOG_INFO("BooleanOpsRefit::enrich_at_intersections_and_build_keepers (m4): "
             "{} physical intersections injected on BOTH sides "
             "({} per-side anchor insertions, {} flanking guards); "
             "{} keepers total across {} operands",
             total_intersections, per_subpath_isect_total, total_guards,
             keepers.size(), (int)operands_in.size());

    return keepers;
}

// ── s296 m11 — chunked-fold helper ─────────────────────────────────────
//
// Snapshot every operand's anchors as OriginalAnchor keepers. Used by
// boolean_op's chunked-fold path to seed the master keeper set before
// per-step intersection triplets accumulate.
KeeperSet original_anchors_keepers(
    const std::vector<std::vector<BezierPath>>& operands)
{
    KeeperSet keepers;
    for (const auto& op_subpaths : operands) {
        for (const auto& bp : op_subpaths) {
            for (const auto& node : bp.nodes) {
                KeeperPoint kp;
                kp.pos = Vec2{node.x, node.y};
                kp.origin = KeeperPoint::Origin::OriginalAnchor;
                kp.source = &node;
                keepers.push_back(kp);
            }
        }
    }
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
    // For each keeper, find the nearest refit node within tolerance and
    // tag it with a pointer to the keeper for later restoration. Keepers
    // that find no match within tolerance are silently skipped — the
    // cleanup contract is "best-effort claim," not "every keeper must
    // match." If a node is already tagged (two keepers in tolerance of
    // the same node — rare), the second keeper loses; skip silently.
    std::vector<const KeeperPoint*> tag(n_initial, nullptr);
    int matched = 0, missed = 0;
    for (const auto& k : keepers) {
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
// Public entry point — cleanup_loop_v2  (s142 m1 probe)
// ══════════════════════════════════════════════════════════════════════════════
//
// Hypothesis being tested: the spike / over-extension corruption seen on
// complex shapes is caused by v1's byte-for-byte handle restore at the
// end of the cleanup walk. delete_node renegotiates surviving neighbours'
// handles correctly via least-squares 2-handle fit (BezierPath::delete_node
// at src/math/BezierPath.cpp:70). v1 then overwrites those renegotiated
// handles with the originally-authored handles from KeeperPoint::source.
// On simple shapes the authored handles still point at sensible neighbours
// post-op, so the clobber is invisible. On complex shapes the kept-anchor's
// new neighbour is at a different position (intersection cusp, distant
// surviving anchor across a deleted span) and the authored handles point
// into space → visible spikes.
//
// v2 keeps everything from v1 except the handle-restore step. Type
// fidelity is preserved (Corner stays Corner, Smooth stays Smooth) by
// copying source.type only. Intersection handling is unchanged from v1
// (retract to Corner — intersections are direction changes by definition).
//
// If v2 fixes the corpus, the s140 m3 byte-for-byte rule was the bug
// and v2 becomes the new default; v1 retires.
// If v2 doesn't fix it (or fixes some cases and breaks others), the
// failure was multi-causal and we move to the larger triplets-as-keepers
// redesign discussed in s142 planning.
// ══════════════════════════════════════════════════════════════════════════════
PathData cleanup_loop_v2(PathData refit_pd, const KeeperSet& keepers) {
    if (refit_pd.nodes.size() < 3) return refit_pd;

    BezierPath bp = BezierPath::from_path_data(refit_pd);
    const int n_initial = (int)bp.nodes.size();

    // ── Step 1: per-keeper claim walk ────────────────────────────────────────
    // Identical to v1.
    std::vector<const KeeperPoint*> tag(n_initial, nullptr);
    int matched = 0, missed = 0;
    for (const auto& k : keepers) {
        const int idx = nearest_node_index(bp, k.pos);
        if (idx < 0) { ++missed; continue; }
        if (tag[idx]) continue;
        tag[idx] = &k;
        ++matched;
    }

    if (matched == 0) {
        LOG_WARN("BooleanOpsRefit::cleanup_loop_v2: no keepers matched the {} "
                 "refit nodes — returning loop unchanged",
                 n_initial);
        return refit_pd;
    }

    // ── Step 2: delete every untagged node, descending ───────────────────────
    // delete_node renegotiates surviving neighbours' handles via
    // least-squares 2-handle fit on each call. After this loop, handles
    // around every kept node reflect the post-boolean shape — NOT the
    // pre-boolean authored shape.
    //
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
        tag.erase(tag.begin() + idx);
        --kept;
    }

    // ── Step 3: restore TYPE only / cornerize intersections ──────────────────
    // KEY DIFFERENCE FROM v1: handles are NOT touched for OriginalAnchor.
    // Whatever delete_node left in cx1/cy1/cx2/cy2 is the answer — those
    // handles fit the post-boolean shape. Only the type byte is copied
    // from source (corner stays Corner, smooth stays Smooth).
    //
    // Intersection survivors are still cornerized — same as v1, since
    // intersections are direction changes by definition and have no
    // authored handles to refer to.
    const int n_final = (int)bp.nodes.size();
    int restored_types = 0;
    int cornerized_intersections = 0;
    for (int i = 0; i < n_final; ++i) {
        const KeeperPoint* k = tag[i];
        if (!k) continue;  // shouldn't happen post-delete
        if (k->origin == KeeperPoint::Origin::OriginalAnchor && k->source) {
            // Type only. Handles stay as delete_node renegotiated them.
            bp.nodes[i].type = k->source->type;
            ++restored_types;
        } else if (k->origin == KeeperPoint::Origin::Intersection) {
            bp.nodes[i].cx1 = bp.nodes[i].x;
            bp.nodes[i].cy1 = bp.nodes[i].y;
            bp.nodes[i].cx2 = bp.nodes[i].x;
            bp.nodes[i].cy2 = bp.nodes[i].y;
            bp.nodes[i].type = BezierNode::Type::Corner;
            ++cornerized_intersections;
        }
    }

    LOG_INFO("BooleanOpsRefit::cleanup_loop_v2: {} → {} nodes "
             "({} keepers matched, {} missed, {} types restored, "
             "{} intersections cornerized) — handles via delete_node fit",
             n_initial, n_final, matched, missed,
             restored_types, cornerized_intersections);

    bp.node_sets.clear();
    return bp.to_path_data();
}

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — cleanup_loop_v3  (s142 m2 — guards-as-keepers)
// ══════════════════════════════════════════════════════════════════════════════
//
// Builds on v2's "no byte-for-byte handle restore" win. Adds: synthetic
// guards from the enrich phase survive the cleanup walk as Smooth anchors.
//
// Why guards-as-keepers matters (Scott's s142 insight): a Corner anchor
// surrounded by retracted intersections (which is what v2 produces near
// intersections) renegotiates handles via delete_node, but the result
// anchor is still TYPE Corner with whatever handles delete_node fit.
// Visually it can look smooth-ish because handles exist — but the type
// byte is wrong. Triplets (smooth on-curve guards by construction)
// guarantee Smooth-by-construction anchors with tangent-by-construction
// handles. Type honesty: Smooth is Smooth, not "Corner that looks smooth".
//
// Algorithm:
//   1. Tag keepers (originals + intersections + guards). Same per-keeper
//      claim walk as v1/v2.
//   2. Delete every untagged node descending. delete_node renegotiates
//      surviving neighbours' handles via least-squares 2-handle fit.
//      This step is identical to v2.
//   3. For each surviving keeper:
//        - OriginalAnchor: type only from KeeperPoint::source (v2 win).
//          Handles stay where delete_node put them.
//        - Intersection:   handles retracted, type Corner (v2 unchanged).
//        - SyntheticGuard: type set to Smooth. Handles stay where
//                          delete_node put them — they were on-curve
//                          tangent by enrich-phase construction, and
//                          delete_node's renegotiation preserved that
//                          local shape. NEW IN V3.
//
// Output node count: O(N_originals + N_intersections + ~2*N_originals)
// per loop. Higher than v2, but the additional smooth scaffolding
// near every original anchor stabilises curve shape near intersections.
// ══════════════════════════════════════════════════════════════════════════════
PathData cleanup_loop_v3(PathData refit_pd, const KeeperSet& keepers) {
    if (refit_pd.nodes.size() < 3) return refit_pd;

    BezierPath bp = BezierPath::from_path_data(refit_pd);
    const int n_initial = (int)bp.nodes.size();

    // ── Step 1: per-keeper claim walk ────────────────────────────────────────
    std::vector<const KeeperPoint*> tag(n_initial, nullptr);
    int matched = 0, missed = 0;
    for (const auto& k : keepers) {
        const int idx = nearest_node_index(bp, k.pos);
        if (idx < 0) { ++missed; continue; }
        if (tag[idx]) continue;
        tag[idx] = &k;
        ++matched;
    }

    if (matched == 0) {
        LOG_WARN("BooleanOpsRefit::cleanup_loop_v3: no keepers matched the {} "
                 "refit nodes — returning loop unchanged",
                 n_initial);
        return refit_pd;
    }

    // ── Step 2: delete every untagged node, descending ───────────────────────
    // Identical to v2. delete_node renegotiates handles each call.
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
        tag.erase(tag.begin() + idx);
        --kept;
    }

    // ── Step 3: per-keeper finalisation ──────────────────────────────────────
    // Three branches by origin:
    //   - OriginalAnchor: copy source.type, leave handles.
    //   - Intersection:   retract handles, type Corner.
    //   - SyntheticGuard: type Smooth, leave handles. NEW.
    const int n_final = (int)bp.nodes.size();
    int restored_types = 0;
    int cornerized_intersections = 0;
    int smoothed_guards = 0;
    for (int i = 0; i < n_final; ++i) {
        const KeeperPoint* k = tag[i];
        if (!k) continue;
        switch (k->origin) {
            case KeeperPoint::Origin::OriginalAnchor:
                if (k->source) {
                    bp.nodes[i].type = k->source->type;
                    ++restored_types;
                }
                break;
            case KeeperPoint::Origin::Intersection:
                bp.nodes[i].cx1 = bp.nodes[i].x;
                bp.nodes[i].cy1 = bp.nodes[i].y;
                bp.nodes[i].cx2 = bp.nodes[i].x;
                bp.nodes[i].cy2 = bp.nodes[i].y;
                bp.nodes[i].type = BezierNode::Type::Corner;
                ++cornerized_intersections;
                break;
            case KeeperPoint::Origin::SyntheticGuard:
                // Smooth by construction. Handles stay as delete_node fit
                // them — they were tangent at enrich time and the
                // renegotiation preserved local shape.
                bp.nodes[i].type = BezierNode::Type::Smooth;
                ++smoothed_guards;
                break;
            case KeeperPoint::Origin::CurvatureApex:
                // v3 doesn't promote apexes — this branch is unreachable
                // from a v3 caller, but added defensively so the switch
                // is exhaustive against the enum (silences -Wswitch).
                // If it ever fires, treat as Smooth.
                bp.nodes[i].type = BezierNode::Type::Smooth;
                break;
            case KeeperPoint::Origin::SpanFiller:
                // v3 doesn't promote span fillers — defensive only.
                bp.nodes[i].type = BezierNode::Type::Smooth;
                break;
        }
    }

    LOG_INFO("BooleanOpsRefit::cleanup_loop_v3: {} → {} nodes "
             "({} keepers matched, {} missed; {} originals typed, "
             "{} intersections cornerized, {} guards smoothed) "
             "— handles via delete_node fit",
             n_initial, n_final, matched, missed,
             restored_types, cornerized_intersections, smoothed_guards);

    bp.node_sets.clear();
    return bp.to_path_data();
}

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — cleanup_loop_v4  (s142 m5 — apex pinning)
// ══════════════════════════════════════════════════════════════════════════════
//
// Builds on v3. Before the delete walk, scans every untagged polyline
// node for the LOCAL TURNING ANGLE — the angle between the inbound
// edge (prev→cur) and the outbound edge (cur→next). Polyline samples
// from a gentle curve produce small turning angles per node; samples
// from a rapid-curvature feature produce larger angles, with the apex
// of the feature at the local maximum.
//
// One apex per feature is promoted: a node `i` is an apex iff
//   turn[i] > turn[i-1]  AND  turn[i] > turn[i+1]  AND  turn[i] >= threshold.
//
// The threshold filters out polyline noise on essentially-straight runs
// (where a tiny float wiggle would otherwise produce many trivial
// "apexes"). Apex keepers are tagged Origin::CurvatureApex; finalisation
// treats them as Smooth + handles via delete_node fit (same as guards).
//
// Algorithm flow:
//   1. Per-keeper claim walk (identical to v3).
//   2. NEW: for each untagged node, compute turn angle. Promote local
//      maxima above threshold to fake KeeperPoint(CurvatureApex)
//      keepers. Tag those nodes.
//   3. Delete every still-untagged node descending (identical to v3).
//   4. Per-keeper finalisation. CurvatureApex behaves like
//      SyntheticGuard (Smooth, handles untouched).
// ══════════════════════════════════════════════════════════════════════════════
namespace {

// Compute the turning angle in degrees at node i of bp (closed path).
// Returns 0.0 for degenerate edges (zero-length adjacent chord).
double turn_angle_deg_at(const BezierPath& bp, int i) {
    const int n = (int)bp.nodes.size();
    if (n < 3) return 0.0;
    const int prev = (i - 1 + n) % n;
    const int next = (i + 1) % n;
    const Vec2 P{bp.nodes[prev].x, bp.nodes[prev].y};
    const Vec2 C{bp.nodes[i].x,    bp.nodes[i].y};
    const Vec2 N{bp.nodes[next].x, bp.nodes[next].y};
    Vec2 in_dir  = C - P;
    Vec2 out_dir = N - C;
    const double in_len  = in_dir.length();
    const double out_len = out_dir.length();
    if (in_len < 1e-9 || out_len < 1e-9) return 0.0;
    in_dir  = in_dir  / in_len;
    out_dir = out_dir / out_len;
    // Angle between in_dir and out_dir. Use atan2 of cross/dot for
    // signed angle, take absolute value (we don't care about turn
    // direction — just magnitude).
    const double cross = in_dir.x * out_dir.y - in_dir.y * out_dir.x;
    const double dot   = in_dir.x * out_dir.x + in_dir.y * out_dir.y;
    const double rad   = std::atan2(std::abs(cross), dot);
    return rad * (180.0 / 3.14159265358979323846);
}

} // anonymous namespace

PathData cleanup_loop_v4(PathData refit_pd, const KeeperSet& keepers,
                         double apex_min_turn_deg,
                         int max_untagged_run) {
    if (refit_pd.nodes.size() < 3) return refit_pd;

    BezierPath bp = BezierPath::from_path_data(refit_pd);
    const int n_initial = (int)bp.nodes.size();

    // ── Step 1: per-keeper claim walk (identical to v3) ──────────────────────
    std::vector<const KeeperPoint*> tag(n_initial, nullptr);
    int matched = 0, missed = 0;
    for (const auto& k : keepers) {
        const int idx = nearest_node_index(bp, k.pos);
        if (idx < 0) { ++missed; continue; }
        if (tag[idx]) continue;
        tag[idx] = &k;
        ++matched;
    }

    if (matched == 0) {
        LOG_WARN("BooleanOpsRefit::cleanup_loop_v4: no keepers matched the {} "
                 "refit nodes — returning loop unchanged",
                 n_initial);
        return refit_pd;
    }

    // ── Step 2 (m5): apex detection ──────────────────────────────────────────
    // Compute turning angle at every untagged node. Find local maxima;
    // promote those above threshold to CurvatureApex keepers.
    //
    // Apex keeper data lives in apex_storage so tag pointers remain
    // stable across the rest of the function.
    std::vector<KeeperPoint> apex_storage;
    apex_storage.reserve(32);
    {
        std::vector<double> turn(n_initial, 0.0);
        for (int i = 0; i < n_initial; ++i) {
            turn[i] = turn_angle_deg_at(bp, i);
        }

        int apex_count = 0;
        for (int i = 0; i < n_initial; ++i) {
            if (tag[i]) continue;            // already claimed by a real keeper
            if (turn[i] < apex_min_turn_deg) continue;
            const int prev = (i - 1 + n_initial) % n_initial;
            const int next = (i + 1) % n_initial;
            if (turn[i] <= turn[prev]) continue;
            if (turn[i] <= turn[next]) continue;
            apex_count++;
        }

        apex_storage.reserve(apex_count);

        for (int i = 0; i < n_initial; ++i) {
            if (tag[i]) continue;
            if (turn[i] < apex_min_turn_deg) continue;
            const int prev = (i - 1 + n_initial) % n_initial;
            const int next = (i + 1) % n_initial;
            if (turn[i] <= turn[prev]) continue;
            if (turn[i] <= turn[next]) continue;

            KeeperPoint kp;
            kp.pos    = Vec2{bp.nodes[i].x, bp.nodes[i].y};
            kp.origin = KeeperPoint::Origin::CurvatureApex;
            kp.source = nullptr;
            apex_storage.push_back(kp);
            tag[i] = &apex_storage.back();
        }

        LOG_INFO("BooleanOpsRefit::cleanup_loop_v4: apex pass — "
                 "{} apexes promoted (threshold={:.2f} deg, "
                 "{} nodes scanned)",
                 (int)apex_storage.size(), apex_min_turn_deg, n_initial);
    }

    // ── Step 2b (NEW in m6): span filler promotion ───────────────────────────
    // Walk between consecutive tagged indices around the closed path.
    // For each run of untagged indices longer than max_untagged_run,
    // promote the midpoint of that run to a SpanFiller keeper.
    //
    // Long gentle arcs that produced no apex (no local turning-angle
    // maximum) get broken into shorter spans here so each delete_node
    // span is short enough for honest least-squares fit.
    //
    // Pass max_untagged_run <= 1 to disable this pass (pure m5 mode).
    //
    // Filler keepers live in filler_storage with the same lifetime
    // contract as apex_storage: reserved up-front so push_backs don't
    // invalidate raw pointers stored in tag[].
    std::vector<KeeperPoint> filler_storage;
    int spans_filled = 0;
    if (max_untagged_run > 1) {
        // First, collect all currently tagged indices in order. The path
        // is closed; we walk it as a circular sequence.
        std::vector<int> tagged_order;
        tagged_order.reserve(n_initial);
        for (int i = 0; i < n_initial; ++i) {
            if (tag[i]) tagged_order.push_back(i);
        }

        const int n_tagged = (int)tagged_order.size();
        if (n_tagged > 0) {
            // For each adjacent (a, b) pair in the circular sequence,
            // the run of untagged indices is (a, b) exclusive — i.e.
            // b - a - 1 untagged nodes (modulo n_initial).
            //
            // Single-pass: find midpoint of each over-long run,
            // promote it to SpanFiller. No recursion in this milestone.
            //
            // Reservation: worst-case one filler per span. Reserve
            // n_tagged so no relocation.
            filler_storage.reserve(n_tagged);

            for (int t = 0; t < n_tagged; ++t) {
                const int a = tagged_order[t];
                const int b = tagged_order[(t + 1) % n_tagged];
                // Length of the untagged run from a's right neighbour
                // to b's left neighbour (inclusive). Modular arithmetic
                // for the closed case.
                int run_len;
                if (b > a) {
                    run_len = b - a - 1;
                } else {
                    // wrap: run goes a+1 .. n_initial-1 .. 0 .. b-1
                    run_len = (n_initial - 1 - a) + b;
                }
                if (run_len <= max_untagged_run) continue;

                // Midpoint index (modular).
                // Walk from a forward by run_len/2 + 1 (skip the tagged
                // a itself). For run_len = 12, midpoint is at offset 6.
                const int offset = (run_len + 1) / 2;
                const int mid = (a + offset) % n_initial;

                // Defensive: if mid happens to already be tagged
                // (shouldn't for a true untagged run), skip.
                if (tag[mid]) continue;

                KeeperPoint kp;
                kp.pos    = Vec2{bp.nodes[mid].x, bp.nodes[mid].y};
                kp.origin = KeeperPoint::Origin::SpanFiller;
                kp.source = nullptr;
                filler_storage.push_back(kp);
                tag[mid] = &filler_storage.back();
                ++spans_filled;
            }
        }

        LOG_INFO("BooleanOpsRefit::cleanup_loop_v4: span filler pass — "
                 "{} spans filled (max_untagged_run={}, "
                 "{} tagged before pass)",
                 spans_filled, max_untagged_run, (int)tagged_order.size());
    }

    // ── Step 3: delete every still-untagged node descending ──────────────────
    // Identical to v3.
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
        tag.erase(tag.begin() + idx);
        --kept;
    }

    // ── Step 4: per-keeper finalisation ──────────────────────────────────────
    // Apex and SpanFiller behave like SyntheticGuard: type Smooth,
    // handles where delete_node fit them.
    const int n_final = (int)bp.nodes.size();
    int restored_types = 0;
    int cornerized_intersections = 0;
    int smoothed_guards = 0;
    int smoothed_apexes = 0;
    int smoothed_fillers = 0;
    for (int i = 0; i < n_final; ++i) {
        const KeeperPoint* k = tag[i];
        if (!k) continue;
        switch (k->origin) {
            case KeeperPoint::Origin::OriginalAnchor:
                if (k->source) {
                    bp.nodes[i].type = k->source->type;
                    ++restored_types;
                }
                break;
            case KeeperPoint::Origin::Intersection:
                bp.nodes[i].cx1 = bp.nodes[i].x;
                bp.nodes[i].cy1 = bp.nodes[i].y;
                bp.nodes[i].cx2 = bp.nodes[i].x;
                bp.nodes[i].cy2 = bp.nodes[i].y;
                bp.nodes[i].type = BezierNode::Type::Corner;
                ++cornerized_intersections;
                break;
            case KeeperPoint::Origin::SyntheticGuard:
                bp.nodes[i].type = BezierNode::Type::Smooth;
                ++smoothed_guards;
                break;
            case KeeperPoint::Origin::CurvatureApex:
                bp.nodes[i].type = BezierNode::Type::Smooth;
                ++smoothed_apexes;
                break;
            case KeeperPoint::Origin::SpanFiller:
                bp.nodes[i].type = BezierNode::Type::Smooth;
                ++smoothed_fillers;
                break;
        }
    }

    LOG_INFO("BooleanOpsRefit::cleanup_loop_v4: {} → {} nodes "
             "({} keepers matched + {} apexes + {} fillers; {} missed; "
             "{} originals typed, {} intersections cornerized, "
             "{} guards smoothed, {} apexes smoothed, {} fillers smoothed) "
             "— handles via delete_node fit",
             n_initial, n_final, matched, (int)apex_storage.size(),
             spans_filled, missed,
             restored_types, cornerized_intersections, smoothed_guards,
             smoothed_apexes, smoothed_fillers);

    bp.node_sets.clear();
    return bp.to_path_data();
}

} // namespace refit
} // namespace Curvz
