#include "math/BooleanOps.hpp"
#include "math/CubicSegment.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <optional>

// ══════════════════════════════════════════════════════════════════════════════
//                         RETIRED — NOT IN USE — S93 m7
//
// This translation unit implements the hand-rolled Sederberg-Nishita
// Bézier-clipping engine for boolean path operations. As of S93 m7 it is
// NO LONGER CALLED from any live code path: Curvz routes all union /
// subtract / intersect operations through Clipper2 (see
// BooleanOpsClipper.cpp).
//
// The file is RETAINED on disk for two reasons:
//
//   1. The math is non-trivial and well-commented. Sederberg-Nishita fat-
//      line clipping, the convex-hull pair-test (testing all six edge
//      pairs of the four Bernstein control points, not just adjacent
//      pairs — a subtle correctness bug that took multiple sessions to
//      pin down), and the union-fold-of-N approach all live here with
//      paper references and worked examples.
//
//   2. Future Bézier-curve work (self-intersection detection, offset
//      trimming, advanced corner handling) may want to lift primitives
//      or commentary from this file rather than re-derive them.
//
// Do NOT add new callers. The Curvz::boolean_op symbol exported here is
// kept ABI-stable for any out-of-tree consumers who linked against
// previous Curvz versions, but in-tree it is dead.
//
// To revive the engine: re-add the dispatch branch in Canvas::boolean_op
// (removed S93 m7) and the action/menu/hotkey trio in MainWindow.cpp. The
// removal points are tagged `S93 m7` in those files.
//
// Original implementation notes follow below.
// ══════════════════════════════════════════════════════════════════════════════

// ══════════════════════════════════════════════════════════════════════════════
// Boolean Operations — Implementation Notes
//
// Curve intersection uses Sederberg-Nishita Bézier clipping.
// See CubicSegment.cpp for the full paper reference and key formulas.
//
// ── Pipeline ─────────────────────────────────────────────────────────────────
// Stage 2: find_intersections(A, B)
//   Iterates all N×M segment pairs, calls CubicSegment::intersect() on each.
//   Stores (seg_a, t_a, seg_b, t_b, world_pt) per crossing.
//   world_pt = average of sa.at(ta) and sb.at(tb) — canonical snap position.
//   Deduplicates hits closer than ISECT_EPS in global parameter space.
//
// Stage 3: subdivide_path(path, is_a, hits)
//   Splits each path segment at its intersection t values using de Casteljau.
//   Each resulting Piece stores:
//     seg         — CubicSegment geometry (endpoints snapped to world_pt)
//     snap_start  — canonical world position at piece start
//     snap_end    — canonical world position at piece end
//     midpoint    — seg.at(0.5), used for winding number classification
//   For straight-line segments (p1==p0, p2==p3 — e.g. rect edges):
//     After snapping p0/p3, also force p1=p0 and p2=p3 to maintain straight lines.
//
// Stage 4: winding_number(pt, path)
//   Horizontal ray cast (+X direction) counting signed crossings with each
//   cubic segment. Finds t values where B(t).y == pt.y by solving the cubic
//   in Bernstein form (converted to power basis via Cardano + Newton refinement).
//   Non-zero winding = inside (non-zero fill rule).
//   keep_piece() uses the result to filter pieces by operation type:
//     Union:     keep if NOT inside other path
//     Subtract:  A pieces: not inside B; B pieces: inside A (and reversed)
//     Intersect: keep if inside other path
//
// Stage 5a: B-piece reversal for Subtract
//   For subtract, kept B-pieces are traversed in reverse direction so their
//   endpoints align with A-piece endpoints at intersection junctions.
//   reverse_piece() swaps p0↔p3, p1↔p2, snap_start↔snap_end.
//
// Stage 5b: assemble_loops(pieces)
//   Chains pieces by matching snap_end → snap_start (within CHAIN_EPS = 0.5).
//   Snap points are canonical world positions so matching is exact regardless
//   of floating-point drift between independently computed A and B subdivisions.
//   Each closed chain → one PathData (closed=true, nodes use Cusp type).
//   Node assembly: cx1 = prev_seg.p2 (in-handle), cx2 = cur_seg.p1 (out-handle).
//
// ── Disjoint Fallback ─────────────────────────────────────────────────────────
// When no intersections are found, tests containment via point_inside() on
// each path's first node against the other path:
//   Union:     A inside B → B; B inside A → A; disjoint → both (compound)
//   Subtract:  A inside B → empty; B inside A → A+B (compound, hole);
//              disjoint → A unchanged
//   Intersect: A inside B → A; B inside A → B; disjoint → empty
// Multi-loop returns are wrapped by the caller into a SceneNode::Compound.
//
// ── Known Limitations ────────────────────────────────────────────────────────
// - Both input paths must be closed
// - Winding normalisation is NOT applied — point_inside() non-zero rule is
//   direction-agnostic, so CW/CCW winding doesn't matter
// - Coincident segments (paths sharing a boundary edge) not handled
// - Self-intersecting paths may produce unexpected results
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// ═══════════════════════════════════════════════════════════════════════════════
// Stage 2 — Path-level intersection collector
// ═══════════════════════════════════════════════════════════════════════════════

struct PathHit {
    int    seg;
    double t;
};

struct PathIntersection {
    PathHit on_a;
    PathHit on_b;
    Vec2    world_pt;   // canonical world position (average of both sides)
};

static constexpr double ISECT_EPS  = 1e-5;
static constexpr double CHAIN_EPS  = 0.5;   // generous — snap handles precision

// S58 Issue 1: false-positive rejection tolerances.
//
// CubicSegment::intersect() uses Bézier clipping with convex-hull tests. For
// two curves that pass very close to each other without actually crossing
// (e.g. a circle fully contained inside an amorphous outer path), the fat-line
// rejection can fail to prune all the near-miss segment pairs and converge to
// a "hit" where the two curves are geometrically 1..10+ units apart. Taking
// such a hit as real triggers the subdivide / piece-keep / assemble pipeline
// instead of the disjoint fallback, producing corrupted geometry.
//
// Two filters are applied to each reported (ta, tb) pair:
//   1. ENDPOINT GAP — if |A(ta) - B(tb)| exceeds ISECT_GAP_DOC, reject. The
//      hit is claiming the curves meet there but they don't.
//   2. TRANSVERSALITY — the 2D cross product of unit tangents at the contact
//      point must exceed ISECT_CROSS_EPS. Tangent/kissing contacts that
//      produce a single hit (rather than a crossing pair) are not a real
//      intersection for boolean-op purposes — the curves touch but don't
//      swap sides, so no boundary piece gets split.
//
// The gap tolerance is deliberately generous (1.0 doc unit) — real
// intersections converge tightly, false positives sit much further apart.
// Tighten later if legitimate edge cases are missed.
static constexpr double ISECT_GAP_DOC    = 1.0;   // doc units
static constexpr double ISECT_CROSS_EPS  = 1e-3;  // |sin(angle)| between tangents

static double global_t(int seg, double t) { return seg + t; }

static std::vector<PathIntersection> find_intersections(
    const BezierPath& A, const BezierPath& B)
{
    std::vector<PathIntersection> hits;
    int na = A.segment_count();
    int nb = B.segment_count();

    for (int ia = 0; ia < na; ++ia) {
        CubicSegment sa = A.segment(ia);
        for (int ib = 0; ib < nb; ++ib) {
            CubicSegment sb = B.segment(ib);
            auto pairs = sa.intersect(sb, ISECT_EPS, 48);
            for (auto& [ta, tb] : pairs) {
                if (ta > 1.0 - ISECT_EPS * 10 && ia < na - 1) continue;
                if (tb > 1.0 - ISECT_EPS * 10 && ib < nb - 1) continue;

                Vec2 pa = sa.at(ta);
                Vec2 pb = sb.at(tb);

                // Filter 1: reject hits where the two curves don't actually
                // meet at the reported parameters.
                double gap = (pa - pb).length();
                if (gap > ISECT_GAP_DOC) {
                    LOG_DEBUG("BooleanOps: rejecting false-positive A[{}] x B[{}] "
                              "ta={:.4f} tb={:.4f} gap={:.3f} (> {:.1f})",
                              ia, ib, ta, tb, gap, ISECT_GAP_DOC);
                    continue;
                }

                // Filter 2: reject tangent/kissing contacts that don't
                // actually cross. A real transversal crossing has tangents
                // that are not parallel; a near-tangent contact has
                // |sin(angle)| near zero.
                Vec2 tga = sa.tangent(ta);
                Vec2 tgb = sb.tangent(tb);
                double la = tga.length();
                double lb = tgb.length();
                if (la > 1e-12 && lb > 1e-12) {
                    double cross = std::abs(tga.x * tgb.y - tga.y * tgb.x);
                    double sin_angle = cross / (la * lb);
                    if (sin_angle < ISECT_CROSS_EPS) {
                        LOG_DEBUG("BooleanOps: rejecting tangent-contact A[{}] x B[{}] "
                                  "ta={:.4f} tb={:.4f} sin_angle={:.6f}",
                                  ia, ib, ta, tb, sin_angle);
                        continue;
                    }
                }

                // Canonical world position: average of both evaluations
                Vec2 world = (pa + pb) * 0.5;
                hits.push_back({{ia, ta}, {ib, tb}, world});
            }
        }
    }

    // Sort and deduplicate by global parameter on A
    std::sort(hits.begin(), hits.end(), [](const PathIntersection& x, const PathIntersection& y) {
        return global_t(x.on_a.seg, x.on_a.t) < global_t(y.on_a.seg, y.on_a.t);
    });

    std::vector<PathIntersection> deduped;
    for (auto& h : hits) {
        if (!deduped.empty()) {
            auto& prev = deduped.back();
            double da = std::abs(global_t(h.on_a.seg, h.on_a.t) -
                                  global_t(prev.on_a.seg, prev.on_a.t));
            double db = std::abs(global_t(h.on_b.seg, h.on_b.t) -
                                  global_t(prev.on_b.seg, prev.on_b.t));
            if (da < ISECT_EPS * 20 && db < ISECT_EPS * 20) continue;
        }
        deduped.push_back(h);
    }

    LOG_DEBUG("BooleanOps: {} intersections ({} before dedup)", deduped.size(), hits.size());
    return deduped;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stage 3 — Subdivide paths into pieces at intersection points
// ═══════════════════════════════════════════════════════════════════════════════

struct Piece {
    CubicSegment seg;
    bool         from_a;
    double       t_start;
    double       t_end;
    Vec2         midpoint;
    Vec2         snap_start;  // canonical world position at piece start
    Vec2         snap_end;    // canonical world position at piece end
};

static std::vector<Piece> subdivide_path(
    const BezierPath& path, bool is_a,
    const std::vector<PathIntersection>& hits)
{
    int nseg = path.segment_count();

    // Per-segment: list of (t, world_snap_pt)
    struct HitOnSeg { double t; Vec2 world; };
    std::vector<std::vector<HitOnSeg>> seg_hits(nseg);

    for (auto& h : hits) {
        const PathHit& ph = is_a ? h.on_a : h.on_b;
        if (ph.seg >= 0 && ph.seg < nseg)
            seg_hits[ph.seg].push_back({ph.t, h.world_pt});
    }

    std::vector<Piece> pieces;

    for (int si = 0; si < nseg; ++si) {
        auto& hs = seg_hits[si];
        std::sort(hs.begin(), hs.end(), [](const HitOnSeg& a, const HitOnSeg& b){ return a.t < b.t; });
        // Deduplicate
        hs.erase(std::unique(hs.begin(), hs.end(),
            [](const HitOnSeg& a, const HitOnSeg& b){ return std::abs(a.t-b.t) < ISECT_EPS; }), hs.end());

        CubicSegment full_seg = path.segment(si);

        // Build split list: always include t=0 (path node) and t=1
        // Snap points at t=0 and t=1 are the actual node positions
        struct SplitPt { double t; Vec2 world; };
        std::vector<SplitPt> splits;
        splits.push_back({0.0, full_seg.p0});
        for (auto& h : hs)
            if (h.t > ISECT_EPS && h.t < 1.0 - ISECT_EPS)
                splits.push_back({h.t, h.world});
        splits.push_back({1.0, full_seg.p3});

        for (size_t k = 0; k + 1 < splits.size(); ++k) {
            double t0 = splits[k].t;
            double t1 = splits[k+1].t;
            if (t1 - t0 < ISECT_EPS * 0.5) continue;

            // Extract sub-segment [t0,t1] from full_seg
            CubicSegment sub = full_seg;
            if (t1 < 1.0 - 1e-9) {
                auto [left, _r] = sub.split(t1);
                sub = left;
            }
            if (t0 > 1e-9) {
                double inner = std::clamp((t1 > 1e-9) ? (t0 / t1) : t0, 0.0, 1.0);
                auto [_l, right] = sub.split(inner);
                sub = right;
            }

            // Snap endpoints to canonical intersection positions
            sub.p0 = splits[k].world;
            sub.p3 = splits[k+1].world;

            // For straight-line segments (rect edges), handles are collocated
            // with their anchors. After subdivision and snapping endpoints, we
            // must also snap handles to maintain straight lines.
            // Detect: if the original segment was straight (p1==p0, p2==p3),
            // then after split the sub-piece has p1 at the split-left anchor
            // and p2 at the split-right anchor. Force them to match the snapped endpoints.
            {
                // Check if this segment is a straight line (degenerate cubic)
                double seg_len = full_seg.p0.dist(full_seg.p3);
                bool straight = (full_seg.p0.dist(full_seg.p1) < 1e-6) &&
                                (full_seg.p2.dist(full_seg.p3) < 1e-6);
                if (straight) {
                    sub.p1 = sub.p0;
                    sub.p2 = sub.p3;
                }
            }

            Piece p;
            p.seg        = sub;
            p.from_a     = is_a;
            p.t_start    = si + t0;
            p.t_end      = si + t1;
            p.midpoint   = sub.at(0.5);
            p.snap_start = splits[k].world;
            p.snap_end   = splits[k+1].world;
            pieces.push_back(p);
        }
    }

    return pieces;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stage 4 — Winding number (non-zero rule) via horizontal ray cast
// ═══════════════════════════════════════════════════════════════════════════════

static void cubic_roots_01(double d0, double d1, double d2, double d3,
                             std::vector<double>& roots)
{
    double a =  -d0 + 3*d1 - 3*d2 + d3;
    double b =   3*d0 - 6*d1 + 3*d2;
    double c =  -3*d0 + 3*d1;
    double d =   d0;

    auto refine = [&](double t0) -> double {
        double t = t0;
        for (int i = 0; i < 10; ++i) {
            double ft  = ((a*t + b)*t + c)*t + d;
            double dft = (3*a*t + 2*b)*t + c;
            if (std::abs(dft) < 1e-14) break;
            t -= ft / dft;
        }
        return std::clamp(t, 0.0, 1.0);
    };

    if (std::abs(a) < 1e-10) {
        if (std::abs(b) < 1e-10) {
            if (std::abs(c) > 1e-10) {
                double t = -d / c;
                if (t >= 0.0 && t <= 1.0) roots.push_back(t);
            }
            return;
        }
        double disc = c*c - 4*b*d;
        if (disc < 0) return;
        double sq = std::sqrt(disc);
        for (double r : {(-c+sq)/(2*b), (-c-sq)/(2*b)})
            if (r >= 0.0 && r <= 1.0) roots.push_back(refine(r));
        return;
    }

    double inv_a = 1.0 / a;
    double b2 = b * inv_a, c2 = c * inv_a, d2n = d * inv_a;
    double p  = c2 - b2*b2/3.0;
    double q  = 2*b2*b2*b2/27.0 - b2*c2/3.0 + d2n;
    double disc = q*q/4.0 + p*p*p/27.0;

    if (disc > 1e-10) {
        double sq = std::sqrt(disc);
        double u  = std::cbrt(-q/2.0 + sq);
        double v  = std::cbrt(-q/2.0 - sq);
        double r  = refine((u + v) - b2/3.0);
        if (r >= 0.0 && r <= 1.0) roots.push_back(r);
    } else if (disc < -1e-10) {
        double m     = 2.0 * std::sqrt(-p/3.0);
        double theta = std::acos(std::clamp(3*q/(p*m), -1.0, 1.0)) / 3.0;
        for (int k = 0; k < 3; ++k) {
            double r = refine(m * std::cos(theta - 2*M_PI*k/3.0) - b2/3.0);
            if (r >= 0.0 && r <= 1.0) roots.push_back(r);
        }
    } else {
        double u = std::cbrt(-q/2.0);
        for (double r0 : {2*u - b2/3.0, -u - b2/3.0}) {
            double r = refine(r0);
            if (r >= 0.0 && r <= 1.0) roots.push_back(r);
        }
    }
}

static int winding_number(Vec2 pt, const BezierPath& path)
{
    int winding = 0;
    int nseg = path.segment_count();

    for (int si = 0; si < nseg; ++si) {
        CubicSegment s = path.segment(si);
        double d0 = s.p0.y - pt.y;
        double d1 = s.p1.y - pt.y;
        double d2 = s.p2.y - pt.y;
        double d3 = s.p3.y - pt.y;

        std::vector<double> roots;
        roots.reserve(3);
        cubic_roots_01(d0, d1, d2, d3, roots);

        for (double t : roots) {
            // Skip roots at segment endpoints to avoid double-counting
            // at path nodes (each node is shared by two segments).
            // Convention: count t=1 endpoint on the current segment,
            // skip t=0 (it will be t=1 on the previous segment).
            // Also skip t very close to 1 on non-final segments to avoid
            // numerical double-counts when the ray grazes a node.
            if (t < ISECT_EPS * 10) continue;
            if (t > 1.0 - ISECT_EPS * 10) continue;
            Vec2 pos  = s.at(t);
            if (pos.x <= pt.x) continue;
            Vec2 tang = s.tangent(t);
            if (std::abs(tang.y) < 1e-10) continue;
            winding += (tang.y > 0) ? +1 : -1;
        }
    }
    return winding;
}

static bool point_inside(Vec2 pt, const BezierPath& path)
{
    int w = winding_number(pt, path);
    LOG_DEBUG("BooleanOps: point_inside ({:.2f},{:.2f}) winding={}", pt.x, pt.y, w);
    return w != 0;
}

static bool keep_piece(const Piece& p,
                        const BezierPath& A, const BezierPath& B,
                        BooleanOpType op)
{
    bool inside_other = p.from_a ? point_inside(p.midpoint, B)
                                 : point_inside(p.midpoint, A);
    bool keep = false;
    switch (op) {
    case BooleanOpType::Union:     keep = !inside_other; break;
    case BooleanOpType::Subtract:  keep = p.from_a ? !inside_other : inside_other; break;
    case BooleanOpType::Intersect: keep = inside_other; break;
    }
    LOG_DEBUG("BooleanOps: piece from_{} t=[{:.3f},{:.3f}] mid=({:.1f},{:.1f}) inside_other={} keep={}",
              p.from_a ? 'A' : 'B', p.t_start, p.t_end,
              p.midpoint.x, p.midpoint.y, inside_other, keep);
    return keep;
}

// Reverse a Piece — flips the segment direction and swaps snap_start/snap_end
static Piece reverse_piece(const Piece& p) {
    Piece r = p;
    // Swap control points: p0↔p3, p1↔p2
    r.seg.p0 = p.seg.p3;
    r.seg.p1 = p.seg.p2;
    r.seg.p2 = p.seg.p1;
    r.seg.p3 = p.seg.p0;
    r.snap_start = p.snap_end;
    r.snap_end   = p.snap_start;
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stage 5b — Assemble kept pieces into closed loops → PathData
// ═══════════════════════════════════════════════════════════════════════════════
// Uses snap_start/snap_end for endpoint matching — these are the canonical
// intersection world positions, so floating-point drift between A and B pieces
// is eliminated.

static std::vector<PathData> assemble_loops(std::vector<Piece>& pieces)
{
    std::vector<bool> used(pieces.size(), false);
    std::vector<PathData> loops;

    for (size_t start = 0; start < pieces.size(); ++start) {
        if (used[start]) continue;

        std::vector<CubicSegment> chain;
        chain.push_back(pieces[start].seg);
        used[start] = true;

        Vec2 tail_snap  = pieces[start].snap_end;
        Vec2 loop_snap  = pieces[start].snap_start;
        bool closed_loop = false;

        for (int iter = 0; iter < (int)pieces.size() * 2; ++iter) {
            if (tail_snap.dist(loop_snap) < CHAIN_EPS) { closed_loop = true; break; }

            bool found = false;
            for (size_t i = 0; i < pieces.size(); ++i) {
                if (used[i]) continue;
                if (pieces[i].snap_start.dist(tail_snap) < CHAIN_EPS) {
                    used[i]    = true;
                    tail_snap  = pieces[i].snap_end;
                    chain.push_back(pieces[i].seg);
                    found = true;
                    break;
                }
            }
            if (!found) {
                LOG_DEBUG("BooleanOps: chain of len {} stuck at ({:.2f},{:.2f})",
                          chain.size(), tail_snap.x, tail_snap.y);
                break;
            }
        }

        if (!closed_loop || chain.empty()) {
            LOG_DEBUG("BooleanOps: chain starting at piece {} failed to close (len={})",
                      start, chain.size());
            continue;
        }

        // Convert chain of CubicSegments to PathData
        PathData pd;
        pd.closed = true;
        int n = (int)chain.size();
        for (int k = 0; k < n; ++k) {
            const CubicSegment& cs   = chain[k];
            const CubicSegment& prev = chain[(k - 1 + n) % n];
            BezierNode node;
            node.x   = cs.p0.x;  node.y   = cs.p0.y;
            node.cx1 = prev.p2.x; node.cy1 = prev.p2.y;
            node.cx2 = cs.p1.x;  node.cy2 = cs.p1.y;
            node.type = BezierNode::Type::Corner;
            pd.nodes.push_back(node);
        }
        loops.push_back(std::move(pd));
    }

    LOG_DEBUG("BooleanOps: assembled {} closed loops", loops.size());
    return loops;
}

// ── Disjoint fallback ─────────────────────────────────────────────────────────
// Handles the cases where the two paths don't cross:
//   - fully disjoint (neither inside the other)
//   - one fully inside the other
// Returns 0, 1, or 2 loops depending on the op and topology. Multi-loop
// returns express "compound" intent — the caller wraps them in a
// SceneNode::Compound so even/odd fill renders correctly.
static std::vector<PathData> disjoint_result(
    const BezierPath& A, const BezierPath& B, BooleanOpType op)
{
    bool a_inside_b = !A.nodes.empty() &&
                       point_inside(Vec2{A.nodes[0].x, A.nodes[0].y}, B);
    bool b_inside_a = !B.nodes.empty() &&
                       point_inside(Vec2{B.nodes[0].x, B.nodes[0].y}, A);

    LOG_DEBUG("BooleanOps: disjoint_result a_in_b={} b_in_a={}",
              a_inside_b, b_inside_a);

    switch (op) {
    case BooleanOpType::Union:
        // A swallows B (or vice versa): return outer alone.
        // Fully disjoint: both paths survive as separate loops (compound).
        if (a_inside_b) return { B.to_path_data() };
        if (b_inside_a) return { A.to_path_data() };
        return { A.to_path_data(), B.to_path_data() };

    case BooleanOpType::Subtract:
        // A inside B: A is entirely removed, result is empty.
        // B inside A: A with B as a hole — return both loops so the caller
        //             can wrap them in a Compound (even/odd winding renders
        //             this as "A minus the B-shaped hole").
        // Disjoint:   A unchanged (B doesn't touch it).
        if (a_inside_b) return {};
        if (b_inside_a) return { A.to_path_data(), B.to_path_data() };
        return { A.to_path_data() };

    case BooleanOpType::Intersect:
        // Disjoint: no overlap, result is empty.
        // One inside the other: intersection is the inner path.
        if (a_inside_b) return { A.to_path_data() };
        if (b_inside_a) return { B.to_path_data() };
        return {};
    }
    return {};
}

// ═══════════════════════════════════════════════════════════════════════════════
// Public entry point
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<PathData> boolean_op(const BezierPath& A_in,
                                  const BezierPath& B_in,
                                  BooleanOpType op)
{
    if (!A_in.closed || !B_in.closed) {
        LOG_WARN("BooleanOps: both paths must be closed");
        return {};
    }
    if (A_in.segment_count() < 1 || B_in.segment_count() < 1) return {};

    LOG_DEBUG("BooleanOps: A signed_area={:.2f} ({} segs), B signed_area={:.2f} ({} segs)",
              A_in.signed_area(), A_in.segment_count(),
              B_in.signed_area(), B_in.segment_count());

    const BezierPath& A = A_in;
    const BezierPath& B = B_in;

    auto hits = find_intersections(A, B);
    if (hits.empty()) {
        LOG_DEBUG("BooleanOps: no intersections — disjoint fallback");
        return disjoint_result(A, B, op);
    }

    auto pieces_a = subdivide_path(A, true,  hits);
    auto pieces_b = subdivide_path(B, false, hits);

    std::vector<Piece> all_pieces;
    all_pieces.reserve(pieces_a.size() + pieces_b.size());
    for (auto& p : pieces_a) all_pieces.push_back(p);
    for (auto& p : pieces_b) all_pieces.push_back(p);

    std::vector<Piece> kept;
    for (auto& p : all_pieces) {
        if (!keep_piece(p, A, B, op)) continue;

        // For subtract: B pieces need to be reversed so their direction is
        // consistent with A pieces at intersection junctions.
        // Both paths in doc space have the same winding convention (CW positive),
        // so B pieces used in subtract must be flipped to form a coherent boundary.
        if (op == BooleanOpType::Subtract && !p.from_a)
            kept.push_back(reverse_piece(p));
        else
            kept.push_back(p);
    }

    LOG_DEBUG("BooleanOps: {} / {} pieces kept", kept.size(), all_pieces.size());

    if (kept.empty()) return {};
    return assemble_loops(kept);
}

} // namespace Curvz
