#include "math/PathOffset.hpp"
#include "math/BezierPath.hpp"
#include "math/BooleanOpsClipper.hpp"  // s260 — offset_path_clipper_polylines
#include "math/CubicSegment.hpp"
#include "math/Vec2.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <vector>
#include <algorithm>
#include <optional>

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
// §5b · Regular-shape detection
//
// A closed path whose on-curve vertices form a regular polygon or regular
// n-pointed star can be offset by uniform radial scaling from the centre.
// The Tiller-Hanson offset path used for general curves applies a per-vertex
// averaging join that visibly slumps the tips of stars and rounds the
// corners of polygons — it solves the general problem but loses the regular
// shapes' symmetries. The fast path here is exact: tips stay sharp, notches
// stay sharp, proportions are preserved.
//
// Detection runs on vertex POSITIONS (BezierNode.x/y), ignoring handles.
// Two acceptance criteria:
//
//   (a) Regular polygon — all N vertices share a single radius from the
//       centroid C, with uniform angular spacing 2π/N around C.
//
//   (b) Regular star — 2N vertices partition by path order into two
//       interleaved groups (even-indexed = outer, odd-indexed = inner).
//       Each group has uniform radius and uniform 2π/N angular spacing
//       within the group; the angular offset between the two groups is
//       π/N (half a step).
//
// Tolerances: 1% of the mean radius for distance equality, 1% of the
// angular step for angular equality. Tight enough to reject hand-drawn
// near-regular shapes, loose enough to accept floating-point drift from
// round-trip transforms.
//
// Why geometric detection over a parametric NodeSet flag: stars and
// polygons emitted by the shape tool today carry no NodeSet metadata
// (the Polygon and Star kinds in the enum are declared but never created
// — see SceneNode.hpp). Even if they were, NodeSets don't invalidate on
// node drag, so a user who edits a vertex leaves stale parametric data.
// The geometric test gives a correct answer regardless of construction
// history.
// ──────────────────────────────────────────────────────────────────────────────
struct RegularShape {
    Vec2  center;
    bool  is_star;       // false → polygon, true → star
    double r_outer = 0;  // polygon: the single radius; star: outer-tip radius
    double r_inner = 0;  // star only: notch radius
    int   point_count = 0; // polygon: N; star: number of outer points (N)
    // angle_offset is the angle of node 0 from centre (radians, atan2 convention).
    double angle_offset = 0.0;
};

static double wrap_to_pi(double a) {
    // Wrap to (-π, π].
    while (a >   M_PI) a -= 2.0 * M_PI;
    while (a <= -M_PI) a += 2.0 * M_PI;
    return a;
}

static bool angular_step_uniform(const std::vector<double>& angles_in_path_order,
                                 double expected_step,
                                 double angle_tol) {
    // angles_in_path_order: angles of consecutive vertices in PATH order
    // (not sorted by angle). Their successive differences should each equal
    // expected_step modulo 2π, with sign consistent (all positive or all
    // negative — handles both winding directions). Tolerance is absolute
    // in radians.
    int n = (int)angles_in_path_order.size();
    if (n < 2) return false;
    // Determine sign of step from first delta.
    double first_delta = wrap_to_pi(angles_in_path_order[1] - angles_in_path_order[0]);
    double signed_step = (first_delta > 0.0) ? expected_step : -expected_step;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        double delta = wrap_to_pi(angles_in_path_order[j] - angles_in_path_order[i]);
        if (std::abs(delta - signed_step) > angle_tol) return false;
    }
    return true;
}

static std::optional<RegularShape> detect_regular_shape(const PathData& path) {
    if (!path.closed) return std::nullopt;
    int n = (int)path.nodes.size();
    if (n < 3) return std::nullopt;

    // Centroid: arithmetic mean of vertex positions. For regular shapes
    // this coincides with the geometric centre by symmetry; using the
    // mean avoids needing area / weighted formulas.
    double cx = 0.0, cy = 0.0;
    for (const auto& nd : path.nodes) { cx += nd.x; cy += nd.y; }
    cx /= n; cy /= n;
    Vec2 C{cx, cy};

    // Per-vertex radius and angle from C.
    std::vector<double> r(n), a(n);
    double r_mean = 0.0;
    for (int i = 0; i < n; ++i) {
        double dx = path.nodes[i].x - cx;
        double dy = path.nodes[i].y - cy;
        r[i] = std::sqrt(dx * dx + dy * dy);
        a[i] = std::atan2(dy, dx);
        r_mean += r[i];
    }
    r_mean /= n;
    if (r_mean < 1e-9) return std::nullopt; // degenerate — all vertices on top of each other

    // Radius tolerance: 1% of mean radius.
    double r_tol = 0.01 * r_mean;

    // Case A — regular polygon: all radii equal.
    bool all_eq = true;
    for (int i = 0; i < n; ++i) {
        if (std::abs(r[i] - r_mean) > r_tol) { all_eq = false; break; }
    }
    if (all_eq) {
        double step = 2.0 * M_PI / n;
        double angle_tol = 0.01 * step;
        if (angular_step_uniform(a, step, angle_tol)) {
            RegularShape rs;
            rs.center = C;
            rs.is_star = false;
            rs.r_outer = r_mean;
            rs.r_inner = r_mean;
            rs.point_count = n;
            rs.angle_offset = a[0];
            return rs;
        }
        return std::nullopt;
    }

    // Case B — regular star: even N ≥ 6, two interleaved radius groups,
    // each group uniformly spaced.
    if (n < 6 || (n % 2) != 0) return std::nullopt;
    int n_points = n / 2;

    // Partition by path index parity.
    double r_even_mean = 0.0, r_odd_mean = 0.0;
    for (int i = 0; i < n; ++i) {
        if (i % 2 == 0) r_even_mean += r[i]; else r_odd_mean += r[i];
    }
    r_even_mean /= n_points;
    r_odd_mean  /= n_points;

    // Within each group all radii must match the group mean.
    for (int i = 0; i < n; ++i) {
        double target = (i % 2 == 0) ? r_even_mean : r_odd_mean;
        if (std::abs(r[i] - target) > r_tol) return std::nullopt;
    }

    // The two groups must have distinguishable radii — otherwise the
    // shape is actually a regular polygon-with-extra-vertices that
    // happened to pass the all-equal check above. We already rejected
    // that branch, but a near-equal radii configuration here is suspicious.
    if (std::abs(r_even_mean - r_odd_mean) < r_tol) return std::nullopt;

    // Build per-group angle lists in path order.
    std::vector<double> a_even, a_odd;
    a_even.reserve(n_points);
    a_odd.reserve(n_points);
    for (int i = 0; i < n; ++i) {
        if (i % 2 == 0) a_even.push_back(a[i]);
        else            a_odd.push_back(a[i]);
    }

    double group_step = 2.0 * M_PI / n_points;
    double angle_tol  = 0.01 * group_step;
    if (!angular_step_uniform(a_even, group_step, angle_tol)) return std::nullopt;
    if (!angular_step_uniform(a_odd,  group_step, angle_tol)) return std::nullopt;

    // Outer = the larger radius group. Identify which.
    bool even_is_outer = (r_even_mean >= r_odd_mean);
    RegularShape rs;
    rs.center = C;
    rs.is_star = true;
    rs.r_outer = even_is_outer ? r_even_mean : r_odd_mean;
    rs.r_inner = even_is_outer ? r_odd_mean  : r_even_mean;
    rs.point_count = n_points;
    rs.angle_offset = a[0];
    return rs;
}

// ──────────────────────────────────────────────────────────────────────────────
// §5c · Regular-shape offsetters (fast path)
//
// For a regular polygon or star, "offset by d" is implemented as a
// uniform scale-from-centre by the single factor (r_outer + d) / r_outer.
// Every anchor and every handle moves through the same scale relative
// to the centroid. Consequences:
//
//   - The outermost vertices (the only ones the user can directly see
//     reaching the edge of the bounding box) move by exactly d.
//   - Inner vertices move proportionally, keeping the star's inflection
//     ratio r_inner / r_outer invariant. The result looks like the
//     original star made bigger, not a different-shaped star.
//   - All angular positions are preserved, so the n-fold rotational
//     symmetry is exact.
//
// This is NOT the textbook path-offset operation (Minkowski sum with a
// disk of radius d), which would move every point on the path along its
// local outward normal by d. The textbook offset at a sharp convex
// corner moves the vertex by d / sin(θ/2) along the bisector — and
// at a sharp concave corner (a star's inner notch) it would move the
// vertex inward by d / sin(θ/2) which can shoot past the centre on
// large offsets. CAD apps tame the convex spike with a miter limit
// and handle the concave case by boolean-union-with-self. For regular
// shapes specifically, the scale-from-centre alternative is cleaner:
// the result is still a regular shape of the same type, with the
// inflection ratio preserved, which is what users actually want when
// they "make a star bigger."
//
// History: the first draft of this function used a per-vertex offset
// (each vertex moved by d outward along its own radial direction).
// That preserved tip sharpness and notch sharpness, but visibly
// shifted the inflection ratio — see scale_regular's inline note for
// the numeric example. Scott caught it on the first visual; the fix
// was to scale every vertex by ONE shape-wide factor instead of
// offsetting each vertex by d.
//
// Winding sign: at this layer d > 0 means "outward from centre",
// resolved by the public-API dispatch above. The fast path is winding-
// agnostic because it scales from the centroid rather than along the
// path tangent.
//
// Collapse guard: with uniform scale, the only failure mode is
// d <= -r_outer, where the scale factor reaches zero or flips sign.
// Standard CAD apps refuse this; so do we. The inner-notch-collapse
// case from the earlier per-vertex variant doesn't exist anymore —
// inner notches scale with everything else.
// ──────────────────────────────────────────────────────────────────────────────
static bool regular_offset_fits(const RegularShape& rs, double d) {
    // The offset is a uniform scale-from-centre with factor
    // (r_outer + d) / r_outer. The shape collapses through zero when
    // d <= -r_outer (the outer tips reach the centre; everything else
    // is already inside them and reaches the centre simultaneously).
    // For d > 0 there's no limit.
    if (d >= 0.0) return true;
    return (-d) < rs.r_outer - 1e-9;
}

static PathData scale_regular(const PathData& path, const RegularShape& rs, double d) {
    PathData out = path;
    // Uniform scale-from-centre. ONE scale factor for every point in the
    // path, derived from r_outer so that "offset d" means "the outermost
    // extent of the shape moved by d." All other vertices, handles, and
    // intermediate points scale by the same factor, which preserves:
    //
    //   - The star's inflection ratio r_inner / r_outer (the shape looks
    //     like the original made bigger, not a different-shaped star).
    //   - Angular positions of every vertex (rotational symmetry kept).
    //   - Bezier handle lengths relative to anchor distances (curves
    //     keep their shape; a circle that snuck through detection
    //     because it lost its Ellipse NodeSet still scales correctly).
    //
    // Earlier draft of this function used per-vertex offset (each vertex
    // moves outward by d along its own radial), which is the textbook
    // path-offset operation. But that visibly shifts the star's inflection
    // ratio — a 5-star with r_inner/r_outer = 0.4 offset by +d=0.2*r_outer
    // ends up with ratio 0.6/1.2 = 0.5, looking noticeably fatter than the
    // original. The user-meaningful intent of "offset path on a star" is
    // "make this star bigger," so we scale instead.
    double scale = (rs.r_outer + d) / rs.r_outer;
    int n = (int)out.nodes.size();
    for (int i = 0; i < n; ++i) {
        BezierNode& nd = out.nodes[i];
        auto scale_pt = [&](double& px, double& py) {
            px = rs.center.x + (px - rs.center.x) * scale;
            py = rs.center.y + (py - rs.center.y) * scale;
        };
        scale_pt(nd.x,   nd.y);
        scale_pt(nd.cx1, nd.cy1);
        scale_pt(nd.cx2, nd.cy2);
    }
    // No NodeSet update: polygon / star NodeSet kinds are declared in the
    // enum but no code reads or writes them today, so the result carries
    // none. If a Polygon/Star NodeSet contract is added later, this is
    // where to hydrate it from `rs`.
    return out;
}

// ──────────────────────────────────────────────────────────────────────────────
// §5d · Rect / Ellipse NodeSet offsetters (parametric fast path)
//
// Both shapes carry a NodeSet from their generator (rect_to_path /
// ellipse_to_path) with parameters describing the canonical form. Offset
// by d:
//
//   Rect   — w → w + 2d,  h → h + 2d  (cx,cy unchanged).
//            Rounded:  rx → rx + d,   ry → ry + d  (clamped to half new w/h).
//   Ellipse — rx → rx + d,  ry → ry + d  (cx,cy unchanged).
//
// We rebuild the PathData from scratch via the existing rect_to_path /
// ellipse_to_path generators, which gives us correct handle geometry
// and a fresh NodeSet with the new params — so the result is fully
// editable as a Rect / Ellipse after offset.
//
// Inward-offset guards (negative d):
//   Rect    — w + 2d > 0 and h + 2d > 0.
//   Ellipse — rx + d > 0 and ry + d > 0.
// ──────────────────────────────────────────────────────────────────────────────
static std::optional<PathData> offset_rect_ns(const PathData& path, double d) {
    if (path.node_sets.empty()) return std::nullopt;
    const NodeSet& ns = path.node_sets[0];
    if (ns.kind != NodeSet::Kind::Rect) return std::nullopt;
    double cx = ns.params[0];
    double cy = ns.params[1];
    double w  = ns.params[2];
    double h  = ns.params[3];
    double rx = ns.params[4];
    double ry = ns.params[5];
    double new_w = w + 2.0 * d;
    double new_h = h + 2.0 * d;
    if (new_w < 1e-9 || new_h < 1e-9) return std::nullopt; // would collapse
    double new_rx = std::max(0.0, rx + d);
    double new_ry = std::max(0.0, ry + d);
    return rect_to_path(cx - new_w * 0.5, cy - new_h * 0.5, new_w, new_h,
                        new_rx, new_ry);
}

static std::optional<PathData> offset_ellipse_ns(const PathData& path, double d) {
    if (path.node_sets.empty()) return std::nullopt;
    const NodeSet& ns = path.node_sets[0];
    if (ns.kind != NodeSet::Kind::Ellipse) return std::nullopt;
    double cx = ns.params[0];
    double cy = ns.params[1];
    double rx = ns.params[2];
    double ry = ns.params[3];
    double new_rx = rx + d;
    double new_ry = ry + d;
    if (new_rx < 1e-9 || new_ry < 1e-9) return std::nullopt; // would collapse
    return ellipse_to_path(cx, cy, new_rx, new_ry);
}

// ──────────────────────────────────────────────────────────────────────────────
// §5e · Clipper2 polyline → Bézier passthrough (s260)
//
// Clipper2's InflatePaths is a polygon engine: every output vertex is the
// junction of two straight line segments. We pass each polyline vertex
// through directly as a Corner-type BezierNode with handles degenerate
// at the anchor — i.e. the result is geometrically a polygon, stored in
// the codebase's BezierNode representation (a straight-cubic-with-
// colocated-handles is exactly equivalent to a straight line segment).
//
// Why no smart refit:
//
// Clipper2 has no notion of "this vertex is on a smooth curve, that one
// is a real corner" — every vertex looks the same. Reconstructing curve
// vs. corner from the polygon is a heuristic, and any heuristic risks
// introducing visual error (oversmooth a real corner, or undersample a
// smooth run and chord-cut across its wiggle). The polygon output is
// geometrically faithful to what InflatePaths produced; users who want
// fewer nodes can decimate manually via the Node tool or a future
// Simplify Path operation, where they retain full control.
//
// At normal zoom the flatten tolerance (FLATTEN_TOL_DOC = 0.1 doc units)
// produces sub-pixel segments — the polygon renders as a smooth curve
// without visible faceting.
// ──────────────────────────────────────────────────────────────────────────────
static PathData polyline_to_bezier_path(const std::vector<Vec2>& poly, bool closed) {
    PathData pd;
    pd.closed = closed;
    int n = (int)poly.size();
    if (n < 3) return pd;
    pd.nodes.reserve(n);
    for (int i = 0; i < n; ++i) {
        BezierNode nd;
        nd.x   = poly[i].x;
        nd.y   = poly[i].y;
        nd.cx1 = poly[i].x;
        nd.cy1 = poly[i].y;
        nd.cx2 = poly[i].x;
        nd.cy2 = poly[i].y;
        nd.type = BezierNode::Type::Corner;
        pd.nodes.push_back(nd);
    }
    return pd;
}

// ──────────────────────────────────────────────────────────────────────────────
// §6 · Public API
// ──────────────────────────────────────────────────────────────────────────────
std::vector<PathData> offset_path(const PathData& path,
                                  double           distance,
                                  OffsetSide       side,
                                  LineCap          cap,
                                  LineJoin         join,
                                  double           miter_limit_ratio) {
    if ((int)path.nodes.size() < 2) {
        LOG_WARN("PathOffset: path has fewer than 2 nodes");
        return {};
    }

    // ── Fast paths for regular / parametric shapes (closed only) ────────────
    // Sign rule: d > 0 means "move outward from centre", independent of
    // winding. (Unlike Tiller-Hanson, which fights winding because it works
    // in tangent-perpendicular space.) So for the fast paths,
    // d_out = +distance and d_in = -distance regardless of area sign.
    //
    // Dispatch is two-stage:
    //   1. Detect shape category once. Categories tried in order:
    //        - Rect NodeSet     (parametric rebuild)
    //        - Ellipse NodeSet  (parametric rebuild)
    //        - Regular polygon  (radial scale from centroid)
    //        - Regular star     (radial scale from centroid)
    //   2. If detected, commit to the fast path. An impossible request
    //      (inward offset through the centre) returns empty rather than
    //      falling through to Tiller-Hanson — T-H on an impossible inward
    //      offset produces an inverted / self-intersecting geometry that
    //      lies about the answer.
    //   3. Un-detected (irregular) paths fall through to T-H below.
    if (path.closed) {
        bool is_rect    = !path.node_sets.empty()
                          && path.node_sets[0].kind == NodeSet::Kind::Rect;
        bool is_ellipse = !path.node_sets.empty()
                          && path.node_sets[0].kind == NodeSet::Kind::Ellipse;
        std::optional<RegularShape> rs;
        if (!is_rect && !is_ellipse) rs = detect_regular_shape(path);

        if (is_rect || is_ellipse || rs.has_value()) {
            const char* kind = is_rect    ? "Rect"
                             : is_ellipse ? "Ellipse"
                             : rs->is_star ? "Star" : "Polygon";
            LOG_DEBUG("PathOffset: fast path ({}) distance={}", kind, distance);

            auto fast = [&](double d) -> std::optional<PathData> {
                if (is_rect)    return offset_rect_ns(path, d);
                if (is_ellipse) return offset_ellipse_ns(path, d);
                if (!regular_offset_fits(*rs, d)) {
                    LOG_WARN("PathOffset: regular-shape inward offset "
                             "would collapse through centre (d={}, r_min={})",
                             d, rs->is_star ? rs->r_inner : rs->r_outer);
                    return std::nullopt;
                }
                return scale_regular(path, *rs, d);
            };

            if (side == OffsetSide::Outside) {
                auto pd = fast(+distance);
                return pd ? std::vector<PathData>{*pd} : std::vector<PathData>{};
            }
            if (side == OffsetSide::Inside) {
                auto pd = fast(-distance);
                return pd ? std::vector<PathData>{*pd} : std::vector<PathData>{};
            }
            // Both — produce TWO independent PathDatas, outer at +d and
            // inner at -d. The Offset Path dialog reaches this via its
            // "Both sides" dropdown; offset_path_op (Canvas_ops.cpp) walks
            // the returned vector and creates one SceneNode per result, so
            // the user ends up with two concentric stars / polygons /
            // rects / ellipses surrounding the original. We don't stitch
            // them into a single closed band for closed inputs — that
            // would be the stroke-expand outline shape, which the stroke-
            // outliner builds itself by calling Outside and Inside
            // separately (see expand_stroke_op).
            std::vector<PathData> results;
            if (auto pd = fast(+distance)) results.push_back(*pd);
            if (auto pd = fast(-distance)) results.push_back(*pd);
            return results;
        }
    }

    // ── Clipper2 path (irregular shapes) — s260 ─────────────────────────────
    // All paths that didn't match a regular-shape fast path go through
    // Clipper2's InflatePaths. This is the "stroke-then-trace" approach:
    // the input's filled region is grown (or shrunk) by `distance` and
    // we trace the boundary of the result. Self-intersections in the
    // input are resolved by Clipper2's pre-clean Union step, so a self-
    // crossing path produces a clean offset with any topological holes
    // emerging as separate output loops. Each output loop becomes one
    // PathData.
    //
    // Anchor selection: corner+apex detection on the polyline output.
    // Polyline vertices where the turn angle exceeds OFFSET_CORNER_RAD
    // (sharp corners — original-anchor preserves, plus miter spikes
    // Clipper2 inserts at sharp originals) become anchors. Polyline
    // vertices that are LOCAL MAXIMA of turn angle within a window
    // (the apex of smooth curves) also become anchors. Everything else
    // is dense-sampling noise from the flatten step and is dropped.
    //
    // Bézier emission: between two consecutive anchors A and B, place
    // handles along the polyline tangents at A and B, with handle length
    // proportional to the arc-length between them. This produces a
    // smooth-curve refit between corners and a clean cubic per smooth
    // arc. T-H below remains as defensive fallback for empty Clipper2
    // output (degenerate inputs).
    {
        auto polys = offset_path_clipper_polylines(
            path, distance, side, cap, join, miter_limit_ratio);
        if (!polys.empty()) {
            std::vector<PathData> results;
            results.reserve(polys.size());
            for (const auto& poly : polys) {
                PathData pd = polyline_to_bezier_path(poly, /*closed=*/path.closed);
                if (!pd.nodes.empty()) results.push_back(std::move(pd));
            }
            if (!results.empty()) return results;
        }
        LOG_WARN("PathOffset: Clipper2 produced no output, falling through to T-H");
    }

    // ── Tiller-Hanson fallback (general curves) ─────────────────────────────
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
