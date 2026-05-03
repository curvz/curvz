#include "math/PathOffset.hpp"
#include "math/BezierPath.hpp"
#include "math/CubicSegment.hpp"
#include "math/Vec2.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

// ══════════════════════════════════════════════════════════════════════════════
// PathOffset — Phase A rewrite (S55)
//
// Strategy (three-layer):
//
//   1. PRE-SPLIT input cubics at inflection points and at offset-cusp
//      parameters. This guarantees that within any single sub-segment the
//      offset does not self-fold, because cusps are exactly the points
//      where the offset's tangent vanishes, and inflection points are where
//      the sign of curvature flips (so the offset switches side).
//
//   2. OFFSET each sub-segment with Tiller-Hanson (the existing algorithm
//      is kept — Elber/Lee/Kim 1997 confirm T-H is competitive and actually
//      the best for quadratics). T-H produces G0-continuous output between
//      same-input-segment sub-pieces; any drift is sub-pixel and averaging
//      is safe there.
//
//   3. JOIN with corner awareness. At joints between sub-pieces from the
//      SAME input segment (produced by pre-splitting) — trust T-H and
//      average. At joints between DIFFERENT input segments — test tangent
//      continuity in the input. Smooth → average. Corner → miter join
//      (ray-ray intersection), with bevel fallback when miter length
//      exceeds the limit.
//
// Math references:
//   - Hoschek 1988, §1 — offset definition and curvature relation
//   - Patrikalakis & Maekawa, §11.2.2 eq. 11.9, 11.11 — cusp condition
//   - Sederberg & Nishita 1990 — used indirectly via existing boolean code
//   - Elber, Lee & Kim 1997 — confirms T-H suitability; cited justifies
//     phase-A scope (join fix + pre-split), not a full LSQ rewrite
//
// What Phase A does NOT do (deferred to Phase B+):
//   - Local loop trimming via Elber & Cohen's ω-sign test
//   - Global self-intersection removal
//   - Hoschek parameter-correction refinement
//   Pre-splitting alone prevents most local loops from being generated in
//   the first place, which is why Phase A is expected to produce large
//   visual improvements without yet needing Phase B/C.
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// ── Tuning constants ─────────────────────────────────────────────────────────
static constexpr double OFFSET_TOL       = 0.25; // px — max midpoint error
static constexpr int    OFFSET_MAX_DEPTH = 8;    // max recursion depth
static constexpr double MITER_LIMIT      = 4.0;  // miter-distance / stroke-half
static constexpr double PARAM_EPS        = 1e-6; // min param separation
static constexpr double RAY_PARALLEL_EPS = 1e-9; // |cross| below → treat parallel

// ──────────────────────────────────────────────────────────────────────────────
// §1 · Polynomial helpers for the pre-split step
// ──────────────────────────────────────────────────────────────────────────────

// Convert a cubic Bezier component (p0,p1,p2,p3) into power-basis coefficients
// for B(t) = a0 + a1 t + a2 t² + a3 t³.
//   a0 = p0
//   a1 = 3(p1 - p0)
//   a2 = 3(p0 - 2 p1 + p2)
//   a3 = p3 - 3 p2 + 3 p1 - p0
static inline void bezier_power_basis(double p0, double p1, double p2, double p3,
                                      double& a0, double& a1, double& a2, double& a3) {
    a0 = p0;
    a1 = 3.0 * (p1 - p0);
    a2 = 3.0 * (p0 - 2.0 * p1 + p2);
    a3 = p3 - 3.0 * p2 + 3.0 * p1 - p0;
}

// Solve quadratic a t² + b t + c = 0, return real roots in (0,1).
static std::vector<double> solve_quadratic_in_unit(double a, double b, double c) {
    std::vector<double> roots;
    if (std::abs(a) < 1e-14) {
        if (std::abs(b) < 1e-14) return roots;
        double t = -c / b;
        if (t > PARAM_EPS && t < 1.0 - PARAM_EPS) roots.push_back(t);
        return roots;
    }
    double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return roots;
    double s = std::sqrt(disc);
    double t1 = (-b - s) / (2.0 * a);
    double t2 = (-b + s) / (2.0 * a);
    if (t1 > PARAM_EPS && t1 < 1.0 - PARAM_EPS) roots.push_back(t1);
    if (t2 > PARAM_EPS && t2 < 1.0 - PARAM_EPS) roots.push_back(t2);
    return roots;
}

// ──────────────────────────────────────────────────────────────────────────────
// §1a · Inflection points of a cubic
//
// Inflection of a parametric cubic is where x' y'' − y' x'' = 0.
// x'(t) is quadratic in t, x''(t) is linear in t, so x' y'' − y' x'' is
// quadratic in t. Compute its coefficients directly.
//
// With Dx(t) = ax1 + 2 ax2 t + 3 ax3 t² and Hx(t) = 2 ax2 + 6 ax3 t (and
// likewise for y), f(t) = Dx Hy − Dy Hx expands to:
//   t⁰: 2(ax1 ay2 − ay1 ax2)
//   t¹: 6(ax1 ay3 − ay1 ax3)
//   t²: 6(ax2 ay3 − ay2 ax3)
// ──────────────────────────────────────────────────────────────────────────────
static std::vector<double> find_inflections(const CubicSegment& s) {
    double ax0, ax1, ax2, ax3, ay0, ay1, ay2, ay3;
    bezier_power_basis(s.p0.x, s.p1.x, s.p2.x, s.p3.x, ax0, ax1, ax2, ax3);
    bezier_power_basis(s.p0.y, s.p1.y, s.p2.y, s.p3.y, ay0, ay1, ay2, ay3);
    double c0 = 2.0 * (ax1 * ay2 - ay1 * ax2);
    double c1 = 6.0 * (ax1 * ay3 - ay1 * ax3);
    double c2 = 6.0 * (ax2 * ay3 - ay2 * ax3);
    return solve_quadratic_in_unit(c2, c1, c0);
}

// ──────────────────────────────────────────────────────────────────────────────
// §1b · Cusp parameters of the offset curve
//
// From P&M §11.2.2 eq. 11.9: the offset cusps where κ(t) = −1/d.
// Equivalently the coordinate form (eq. 11.11):
//   d · (ẍ ẏ − ẋ ÿ) − √(ẋ² + ẏ²) · (ẋ² + ẏ²) = 0
//
// Residual sign changes in [0,1] bracket cusp parameters. Numerical
// bisection is used rather than an analytic degree-12 root solve — this
// only runs at design time, and double precision with 40 bisection steps
// gives >12 digits of accuracy, which is far more than needed.
// ──────────────────────────────────────────────────────────────────────────────
static double eval_cusp_residual(const CubicSegment& s, double d, double t) {
    double ax0, ax1, ax2, ax3, ay0, ay1, ay2, ay3;
    bezier_power_basis(s.p0.x, s.p1.x, s.p2.x, s.p3.x, ax0, ax1, ax2, ax3);
    bezier_power_basis(s.p0.y, s.p1.y, s.p2.y, s.p3.y, ay0, ay1, ay2, ay3);
    double xd  = ax1 + 2.0 * ax2 * t + 3.0 * ax3 * t * t;
    double yd  = ay1 + 2.0 * ay2 * t + 3.0 * ay3 * t * t;
    double xdd = 2.0 * ax2 + 6.0 * ax3 * t;
    double ydd = 2.0 * ay2 + 6.0 * ay3 * t;
    double cross    = xdd * yd - xd * ydd;     // ẍ ẏ − ẋ ÿ
    double speed_sq = xd * xd + yd * yd;
    double speed    = std::sqrt(speed_sq);
    return d * cross - speed * speed_sq;
}

static std::vector<double> find_cusp_params(const CubicSegment& s, double d) {
    std::vector<double> out;
    if (std::abs(d) < 1e-12) return out;
    // Short-circuit straight-line inputs: the offset of a straight line is
    // another straight line, which has no singularities. Computing the cusp
    // residual for such inputs is not just wasted work — it risks spurious
    // sign changes from floating-point noise in the cross-product
    // calculation (which is algebraically exactly 0 for straight lines
    // but computes as ±1e-14 in IEEE-754), producing phantom cusps at
    // tiny t values that then split segments and generate node artifacts.
    //
    // Detect straight input geometrically: both handles retracted to within
    // 0.5% of chord length (matching the threshold used in out_tangent /
    // in_tangent for the degenerate-handle test).
    double chord_len_sq = (s.p3 - s.p0).length_sq();
    if (chord_len_sq > 1e-14) {
        double h0_sq = (s.p1 - s.p0).length_sq();
        double h1_sq = (s.p2 - s.p3).length_sq();
        if (h0_sq < 2.5e-5 * chord_len_sq && h1_sq < 2.5e-5 * chord_len_sq) {
            return out;
        }
    }
    const int N = 16;
    double prev_t = 0.0;
    double prev_f = eval_cusp_residual(s, d, 0.0);
    for (int i = 1; i <= N; ++i) {
        double t = static_cast<double>(i) / static_cast<double>(N);
        double f = eval_cusp_residual(s, d, t);
        // Use STRICT inequalities so that f==0 at scan boundaries is
        // neutral rather than claimed by both directions.
        bool sign_change =
            (prev_f < 0.0 && f > 0.0) || (prev_f > 0.0 && f < 0.0);
        if (sign_change) {
            double lo = prev_t, hi = t, flo = prev_f;
            for (int k = 0; k < 40; ++k) {
                double mid = 0.5 * (lo + hi);
                double fm  = eval_cusp_residual(s, d, mid);
                if ((flo < 0.0 && fm > 0.0) || (flo > 0.0 && fm < 0.0)) {
                    hi = mid;
                } else {
                    lo = mid; flo = fm;
                }
            }
            double root = 0.5 * (lo + hi);
            if (root > PARAM_EPS && root < 1.0 - PARAM_EPS) out.push_back(root);
        }
        prev_t = t;
        prev_f = f;
    }
    return out;
}

// ──────────────────────────────────────────────────────────────────────────────
// §1c · Split a CubicSegment at an ordered list of parameters
// ──────────────────────────────────────────────────────────────────────────────
static std::vector<CubicSegment> split_at_params(const CubicSegment& s,
                                                  std::vector<double> params) {
    std::sort(params.begin(), params.end());
    std::vector<double> filtered;
    filtered.reserve(params.size());
    double last = -1.0;
    for (double t : params) {
        if (t <= PARAM_EPS || t >= 1.0 - PARAM_EPS) continue;
        if (t - last < PARAM_EPS) continue;
        filtered.push_back(t);
        last = t;
    }
    std::vector<CubicSegment> out;
    CubicSegment cur = s;
    double t_start = 0.0;
    for (double t_global : filtered) {
        double local = (t_global - t_start) / (1.0 - t_start);
        if (local <= PARAM_EPS || local >= 1.0 - PARAM_EPS) continue;
        auto [L, R] = cur.split(local);
        out.push_back(L);
        cur = R;
        t_start = t_global;
    }
    out.push_back(cur);
    return out;
}

// ──────────────────────────────────────────────────────────────────────────────
// §2 · Tiller-Hanson segment offsetter (unchanged from prior implementation)
// ──────────────────────────────────────────────────────────────────────────────
static Vec2 chord_normal(Vec2 from, Vec2 to) {
    Vec2 chord = to - from;
    return chord.perp().normalised();
}

static void offset_segment(const CubicSegment& seg, double d, int depth,
                            std::vector<CubicSegment>& out) {
    Vec2 n01 = chord_normal(seg.p0, seg.p1);
    Vec2 n23 = chord_normal(seg.p2, seg.p3);
    if ((seg.p1 - seg.p0).length_sq() < 1e-10) n01 = chord_normal(seg.p0, seg.p3);
    if ((seg.p3 - seg.p2).length_sq() < 1e-10) n23 = chord_normal(seg.p0, seg.p3);

    CubicSegment off;
    off.p0 = seg.p0 + n01 * d;
    off.p1 = seg.p1 + n01 * d;
    off.p2 = seg.p2 + n23 * d;
    off.p3 = seg.p3 + n23 * d;

    if (depth < OFFSET_MAX_DEPTH) {
        Vec2 true_mid   = seg.at(0.5) + seg.normal(0.5).normalised() * d;
        Vec2 approx_mid = off.at(0.5);
        if (true_mid.dist(approx_mid) > OFFSET_TOL) {
            auto [L, R] = seg.split(0.5);
            offset_segment(L, d, depth + 1, out);
            offset_segment(R, d, depth + 1, out);
            return;
        }
    }
    out.push_back(off);
}

// ──────────────────────────────────────────────────────────────────────────────
// §3 · Corner detection and miter/bevel join
// ──────────────────────────────────────────────────────────────────────────────

// Outgoing tangent of a segment at p3 (end). Uses chord p2→p3 if it's a
// meaningful length relative to the whole segment; otherwise falls back to
// the chord p0→p3. The threshold is chord-relative (not a fixed floating-
// point epsilon) because tiny-but-nonzero handles on character outlines
// would otherwise produce unreliable unit tangent vectors.
static Vec2 out_tangent(const CubicSegment& s) {
    double chord_len_sq = (s.p3 - s.p0).length_sq();
    double handle_len_sq = (s.p3 - s.p2).length_sq();
    // "Meaningful" = at least 0.5% of segment chord, squared → 2.5e-5.
    Vec2 v = (handle_len_sq > 2.5e-5 * chord_len_sq)
             ? (s.p3 - s.p2)
             : (s.p3 - s.p0);
    return v.normalised();
}

// Incoming tangent of a segment at p0 (start) — see out_tangent for the
// chord-relative epsilon rationale.
static Vec2 in_tangent(const CubicSegment& s) {
    double chord_len_sq = (s.p3 - s.p0).length_sq();
    double handle_len_sq = (s.p1 - s.p0).length_sq();
    Vec2 v = (handle_len_sq > 2.5e-5 * chord_len_sq)
             ? (s.p1 - s.p0)
             : (s.p3 - s.p0);
    return v.normalised();
}

// True iff the joint at input node `nt` should be mitered (corner) rather
// than averaged (smooth continuation).
//
// Uses the PathData's authoritative node type rather than re-deriving it
// geometrically, for two reasons:
//   1. It's more reliable — tangent comparisons at nodes with very short
//      handles (common in font glyphs: serif-to-stem transition) can
//      produce unit vectors that jitter between "parallel" and "turn"
//      depending on sub-pixel handle positions, causing spurious miters.
//   2. It respects authoring intent — a node marked Smooth/Symmetric is
//      guaranteed tangent-continuous by the data model's invariants, so
//      averaging is correct by construction. A node marked Corner was
//      deliberately left tangent-discontinuous by the user or converter.
//
// Cusp nodes are a tricky case: they have both handles explicitly placed
// to create a tangent reversal. On the offset curve side that reversal
// produces its own geometry (often a cusp of the offset); we treat them
// as Average here — mitering would project intersecting offset rays to
// an arbitrary point behind both segments.
static bool is_input_corner(BezierNode::Type nt) {
    return nt == BezierNode::Type::Corner;
}

// Intersect two lines given as (a0 + u·da) ∩ (b0 + v·db).
// Returns success via bool; writes intersection point to out_pt.
static bool line_line_intersect(Vec2 a0, Vec2 da, Vec2 b0, Vec2 db, Vec2& out_pt) {
    double denom = da.cross(db);
    if (std::abs(denom) < RAY_PARALLEL_EPS) return false;
    Vec2 diff = b0 - a0;
    double u = diff.cross(db) / denom;
    out_pt = a0 + da * u;
    return true;
}

// Join decision — either snap both endpoints to a miter point, or insert a
// bevel bridge segment. "Average" is the third option (smooth joints only).
struct JoinResult {
    enum Kind { Miter, Bevel, Average } kind = Average;
    Vec2 miter_point{};
};

// Decide how to join cur → next given the type of the INPUT node that sits
// at the boundary, and offset magnitude d. same_input_segment is true iff
// both pieces came from the same input cubic (via pre-split) — in which
// case T-H's sub-pixel drift is all that's there to fix, and averaging is
// correct.
//
// This function distinguishes between two geometrically different cases
// at a Corner node:
//
//   Growing side — the offset at this corner extends OUTWARD relative to
//     the progenitor's turn (outer at a convex corner, or inner at a
//     concave corner). The two offset endpoints are spread apart; the
//     natural join is a miter that extends away from both endpoints
//     until they meet. At very acute corners this miter distance can
//     get large, so we apply a miter limit and fall back to bevel.
//
//   Shrinking side — the offset extends INWARD. The two offset endpoints
//     are already close together, and for sharp corners they may
//     actually have CROSSED past each other. The miter intersection is
//     geometrically "behind" the endpoints relative to the direction of
//     travel. We still snap both endpoints to the intersection (closing
//     the tiny acute wedge), but we must NOT bevel — a bevel on the
//     shrinking side would insert a straight bridge between two
//     endpoints that are already on the wrong side of the intersection,
//     producing a visible X crossing.
//
// Detection: compare the vector (miter − cur.p3) against cur's tangent.
// Positive dot product = miter is forward along travel = growing side.
// Negative dot product = miter is behind = shrinking side.
static JoinResult decide_join(const CubicSegment& cur, const CubicSegment& next,
                               BezierNode::Type boundary_node_type,
                               double d,
                               bool same_input_segment) {
    JoinResult r;
    if (same_input_segment) { r.kind = JoinResult::Average; return r; }
    if (!is_input_corner(boundary_node_type)) {
        r.kind = JoinResult::Average;
        return r;
    }
    // Input node is a Corner. Compute miter via the offset-ray intersection.
    Vec2 cur_t  = out_tangent(cur);
    Vec2 next_t = in_tangent(next);
    Vec2 miter;
    if (!line_line_intersect(cur.p3, cur_t, next.p0, next_t, miter)) {
        r.kind = JoinResult::Bevel;
        return r;
    }
    // Direction test: is the miter ahead of (growing) or behind (shrinking)
    // the current offset piece's tangent? A forward miter extends outward
    // into the corner gap; a backward miter indicates the offsets have
    // physically crossed and the "miter point" is the crossing itself.
    double forward_dot = (miter - cur.p3).dot(cur_t);
    bool shrinking = (forward_dot < 0.0);

    if (!shrinking) {
        // Growing side — apply miter limit, bevel if spike is too long.
        Vec2 mid = (cur.p3 + next.p0) * 0.5;
        double miter_dist  = mid.dist(miter);
        double half_stroke = std::abs(d);
        if (half_stroke > 1e-12 && miter_dist / half_stroke > MITER_LIMIT) {
            r.kind = JoinResult::Bevel;
            return r;
        }
    }
    // Either growing side within miter limit, or shrinking side (always
    // snap, never bevel — bevel here would leave the crossing visible).
    r.kind = JoinResult::Miter;
    r.miter_point = miter;
    return r;
}

// ──────────────────────────────────────────────────────────────────────────────
// §4 · Pipeline: input cubic segments → offset sub-segments with provenance
// ──────────────────────────────────────────────────────────────────────────────
struct OffsetPiece {
    CubicSegment seg;
    int          input_idx; // originating input segment index; -1 for bevel bridges
};

static std::vector<OffsetPiece> offset_to_pieces(const BezierPath& bp, double d) {
    std::vector<OffsetPiece> pieces;
    int nseg = bp.segment_count();
    for (int i = 0; i < nseg; ++i) {
        CubicSegment s = bp.segment(i);
        // Defensive: skip zero-length input segments. Glyphs produced by
        // text-to-path conversion sometimes have a duplicate node at the
        // contour seam (first and last node coincident). A zero-length
        // segment between them offsets to a zero-length output piece
        // whose tangent is undefined; skipping it produces the same
        // visible geometry without the artifact.
        double chord_len_sq = (s.p3 - s.p0).length_sq();
        double handle_sum_sq = (s.p1 - s.p0).length_sq()
                              + (s.p2 - s.p3).length_sq();
        if (chord_len_sq < 1e-12 && handle_sum_sq < 1e-12) continue;
        std::vector<double> params = find_inflections(s);
        auto cps = find_cusp_params(s, d);
        params.insert(params.end(), cps.begin(), cps.end());
        auto sub = split_at_params(s, std::move(params));
        for (const auto& ss : sub) {
            std::vector<CubicSegment> offs;
            offset_segment(ss, d, 0, offs);
            for (auto& o : offs) pieces.push_back({o, i});
        }
    }
    return pieces;
}

static void apply_joins(std::vector<OffsetPiece>& pieces,
                         const BezierPath& bp,
                         bool closed,
                         double d) {
    int n = (int)pieces.size();
    if (n < 2) return;
    int count = closed ? n : n - 1;
    int node_count = (int)bp.nodes.size();

    // Collect bevel insertions; apply them back-to-front after the loop
    // so indices stay valid.
    std::vector<std::pair<int, Vec2>> bevels;

    for (int i = 0; i < count; ++i) {
        OffsetPiece& cur  = pieces[i];
        OffsetPiece& next = pieces[(i + 1) % n];
        bool same = (cur.input_idx == next.input_idx && cur.input_idx >= 0);

        // Determine the authoritative node type at the input boundary.
        // The boundary between input segments k and k+1 lives at bp.nodes[k+1]
        // (for open paths) or bp.nodes[(k+1) % node_count] (closed). If one
        // side is a bridge (input_idx == -1 from a previous Bevel pass)
        // we don't apply the per-input-node rule — but since we're here
        // during the first apply_joins call, bridges haven't been inserted
        // yet, so input_idx values are in [0, nseg).
        BezierNode::Type nt = BezierNode::Type::Smooth;
        if (!same && cur.input_idx >= 0 && next.input_idx >= 0 && node_count > 0) {
            // The boundary node is the head of `next.input_idx`'s segment,
            // i.e. bp.nodes[next.input_idx] — since segment k runs from
            // nodes[k] to nodes[k+1]. When we wrap around a closed path,
            // next.input_idx may be 0, pointing to nodes[0].
            int node_idx = next.input_idx;
            if (node_idx >= 0 && node_idx < node_count)
                nt = bp.nodes[node_idx].type;
        }

        JoinResult jr = decide_join(cur.seg, next.seg, nt, d, same);
        switch (jr.kind) {
            case JoinResult::Average: {
                Vec2 avg = (cur.seg.p3 + next.seg.p0) * 0.5;
                // Translate the adjacent handle of each piece by the same
                // delta as its anchor, preserving handle-to-anchor offset.
                // For straight inputs (T-H produces p2==p3 and p1==p0), this
                // keeps the output handles retracted on the new anchor —
                // exactly what a pure-corner output like a star expects.
                // For curvy inputs, it preserves the approach curve shape.
                Vec2 cur_delta  = avg - cur.seg.p3;
                Vec2 next_delta = avg - next.seg.p0;
                cur.seg.p2  += cur_delta;
                next.seg.p1 += next_delta;
                cur.seg.p3  = avg;
                next.seg.p0 = avg;
                break;
            }
            case JoinResult::Miter: {
                Vec2 cur_delta  = jr.miter_point - cur.seg.p3;
                Vec2 next_delta = jr.miter_point - next.seg.p0;
                cur.seg.p2  += cur_delta;
                next.seg.p1 += next_delta;
                cur.seg.p3  = jr.miter_point;
                next.seg.p0 = jr.miter_point;
                break;
            }
            case JoinResult::Bevel: {
                bevels.push_back({i, next.seg.p0});
                break;
            }
        }
    }

    std::sort(bevels.begin(), bevels.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& b : bevels) {
        OffsetPiece& cur = pieces[b.first];
        CubicSegment bridge;
        bridge.p0 = cur.seg.p3;
        bridge.p3 = b.second;
        bridge.p1 = bridge.p0; // straight line — handles degenerate on anchors
        bridge.p2 = bridge.p3;
        pieces.insert(pieces.begin() + b.first + 1, OffsetPiece{bridge, -1});
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// §5 · Piece list → PathData
// ──────────────────────────────────────────────────────────────────────────────
static PathData pieces_to_path(const std::vector<OffsetPiece>& pieces, bool closed) {
    PathData pd;
    pd.closed = closed;
    int n = (int)pieces.size();
    if (n == 0) return pd;

    for (int i = 0; i < n; ++i) {
        const CubicSegment& s = pieces[i].seg;
        BezierNode node;
        node.type = BezierNode::Type::Corner;
        node.x   = s.p0.x; node.y   = s.p0.y;
        node.cx2 = s.p1.x; node.cy2 = s.p1.y;
        if (i > 0) {
            node.cx1 = pieces[i - 1].seg.p2.x;
            node.cy1 = pieces[i - 1].seg.p2.y;
        } else if (closed) {
            node.cx1 = pieces[n - 1].seg.p2.x;
            node.cy1 = pieces[n - 1].seg.p2.y;
        } else {
            node.cx1 = s.p0.x;
            node.cy1 = s.p0.y;
        }
        pd.nodes.push_back(node);
    }
    if (!closed) {
        const CubicSegment& last = pieces[n - 1].seg;
        BezierNode tail;
        tail.type = BezierNode::Type::Corner;
        tail.x   = last.p3.x; tail.y   = last.p3.y;
        tail.cx1 = last.p2.x; tail.cy1 = last.p2.y;
        tail.cx2 = last.p3.x; tail.cy2 = last.p3.y;
        pd.nodes.push_back(tail);
    }
    return pd;
}

// Reverse a PathData's winding.
static PathData reversed_path(PathData pd) {
    BezierPath bp = BezierPath::from_path_data(pd);
    bp.reverse();
    return bp.to_path_data();
}

// Join outer (open) + inner reversed (open) → one closed outline (butt caps).
static PathData join_open_as_closed(PathData outer, PathData inner_rev) {
    if (outer.nodes.empty() || inner_rev.nodes.empty()) return outer;
    outer.nodes.back().cx2      = outer.nodes.back().x;
    outer.nodes.back().cy2      = outer.nodes.back().y;
    inner_rev.nodes.front().cx1 = inner_rev.nodes.front().x;
    inner_rev.nodes.front().cy1 = inner_rev.nodes.front().y;
    inner_rev.nodes.back().cx2  = inner_rev.nodes.back().x;
    inner_rev.nodes.back().cy2  = inner_rev.nodes.back().y;
    outer.nodes.front().cx1     = outer.nodes.front().x;
    outer.nodes.front().cy1     = outer.nodes.front().y;
    PathData pd;
    pd.closed = true;
    for (const auto& n : outer.nodes)     pd.nodes.push_back(n);
    for (const auto& n : inner_rev.nodes) pd.nodes.push_back(n);
    return pd;
}

// ──────────────────────────────────────────────────────────────────────────────
// §6 · Public API
// ──────────────────────────────────────────────────────────────────────────────
std::vector<PathData> offset_path(const PathData& path,
                                  double           distance,
                                  OffsetSide       side) {
    if ((int)path.nodes.size() < 2) {
        LOG_WARN("PathOffset: path has fewer than 2 nodes");
        return {};
    }
    BezierPath bp = BezierPath::from_path_data(path);

    // Shoelace sign: doc space is Y-down, positive area = CW winding.
    // CW winding → outward is right side of tangent. perp() returns left-
    // perpendicular so CW paths negate to get outward.
    double area  = bp.signed_area();
    double d_out = (area > 0.0) ? -distance :  distance;
    double d_in  = (area > 0.0) ?  distance : -distance;

    auto build_closed = [&](double d) -> PathData {
        auto pieces = offset_to_pieces(bp, d);
        apply_joins(pieces, bp, true, d);
        return pieces_to_path(pieces, true);
    };
    auto build_open = [&](double d) -> PathData {
        auto pieces = offset_to_pieces(bp, d);
        apply_joins(pieces, bp, false, d);
        return pieces_to_path(pieces, false);
    };

    // Open path — offset both sides, join at endpoints → one closed outline.
    if (!path.closed) {
        PathData outer     = build_open( distance);
        PathData inner_rev = reversed_path(build_open(-distance));
        auto result = join_open_as_closed(outer, inner_rev);
        return { result };
    }

    if (side == OffsetSide::Outside) return { build_closed(d_out) };
    if (side == OffsetSide::Inside)  return { build_closed(d_in)  };

    // Both — stroke outline shape (closed).
    PathData outer     = build_open(d_out);
    PathData inner_rev = reversed_path(build_open(d_in));
    return { join_open_as_closed(outer, inner_rev) };
}

} // namespace Curvz
