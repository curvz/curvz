#include "math/CubicSegment.hpp"
#include <cmath>
#include <algorithm>

// ══════════════════════════════════════════════════════════════════════════════
// REFERENCE: Sederberg & Nishita, "Curve intersection using Bézier clipping"
//            Computer-Aided Design 22(9), pp 538-549, November 1990.
//
// This file implements CubicSegment::intersect() using the Bézier clipping
// algorithm described in that paper. Key formulas extracted below for reference.
//
// ── Fat Line (§ "Fat lines", p.538-539) ──────────────────────────────────────
// Given cubic curve P(t) with chord line L through P0→P3:
// Normal to L: ax + by + c = 0  (a²+b²=1)  [eq.1]
// Distance of control point Pi from L: d_i = a*xi + b*yi + c  [eq.2,18]
// d0 = 0 always (P0 is on L); d3 = 0 always (P3 is on L).
// Only d1 and d2 determine the fat line width.
//
// Tight fat line bounds for CUBIC curves [equations 13 & 15, p.539]:
//   If d1*d2 > 0 (same side of chord):
//     d_min = (3/4) * min{0, d1, d2}
//     d_max = (3/4) * max{0, d1, d2}
//   If d1*d2 ≤ 0 (opposite sides of chord):
//     d_min = (4/9) * min{0, d1, d2}
//     d_max = (4/9) * max{0, d1, d2}
//
// ── Bézier Clipping (§ "Bézier clipping", p.540) ─────────────────────────────
// The distance function d(t) from P(t) to line L is itself a polynomial in
// Bernstein form [eq.18]:
//   d(t) = Σ d_i * B_i^n(t),   d_i = a*xi + b*yi + c
// This is represented as a "non-parametric" Bézier curve D(t) = (t, d(t))
// with control points D_i = (i/n, d_i) evenly spaced in t [eq.19].
//
// To clip P(t) against fat line L of Q(u):
//   Compute d_i for each control point of P against Q's fat line.
//   Find the t-interval for which the CONVEX HULL of D(t) overlaps [d_min, d_max].
//   The surviving interval is the only region where P(t) can intersect Q(u).
//   If interval is empty → no intersection. [proven by convex hull property]
//
// ── Iterating (§ "Iterating", p.540) ─────────────────────────────────────────
// Algorithm alternates clipping A against B's fat line, then B against A's
// fat line. Converges quadratically near simple intersections.
// Table 1 shows convergence to 6 digits in 3 steps for a typical case.
//
// ── Multiple Intersections (§ "Multiple intersections", p.541) ───────────────
// When a Bézier clip fails to reduce either interval by ≥ 20%, there may be
// multiple intersections in the current sub-problem. Remedy: split the LONGER
// interval at its midpoint and recurse on each half against the shorter curve.
// A stack stores pending (sub_A, sub_B) pairs.
//
// ── Alternative Fat Lines (§ "Clipping to other fat lines", p.540) ───────────
// A fat line perpendicular to P0→Pn sometimes provides a larger clip than
// the parallel fat line. Examining both and using the larger clip is suggested
// but adds overhead. We use only the chord-parallel fat line.
//
// ── Convergence Guarantee ────────────────────────────────────────────────────
// The fat line clip is a PROVEN bound — if clip_to_slab returns empty,
// there is provably no intersection in that sub-problem (convex hull property).
// Do NOT fall back to subdivision when the slab rejects; that overrides the proof.
// The only legitimate reason to subdivide is poor reduction (< 20%) which
// indicates multiple roots, not a false rejection.
//
// ── Degenerate Segments ───────────────────────────────────────────────────────
// For segments where p1==p0 and p2==p3 (e.g. rect edges — retracted handles),
// d1==d2==0 and the fat line has zero width. This is mathematically correct
// (the segment is a straight line on its own chord). Use epsilon width 1e-6
// so clip_to_slab can still detect d=0 crossings. Do NOT use a large minimum
// width (e.g. 1e-4 * chord_len) — that causes false positives for genuinely
// non-intersecting near-tangent curves.
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// ── De Casteljau ──────────────────────────────────────────────────────────────
Vec2 CubicSegment::at(double t) const {
    double u  = 1.0 - t;
    double u2 = u  * u;
    double u3 = u2 * u;
    double t2 = t  * t;
    double t3 = t2 * t;
    return p0 * u3
         + p1 * (3*u2*t)
         + p2 * (3*u*t2)
         + p3 * t3;
}

// ── Tangent (first derivative) ────────────────────────────────────────────────
Vec2 CubicSegment::tangent(double t) const {
    double u  = 1.0 - t;
    // B'(t) = 3[(p1-p0)(1-t)^2 + 2(p2-p1)(1-t)t + (p3-p2)t^2]
    return (p1-p0) * (3*u*u)
         + (p2-p1) * (6*u*t)
         + (p3-p2) * (3*t*t);
}

Vec2 CubicSegment::normal(double t) const {
    Vec2 tang = tangent(t);
    return Vec2{-tang.y, tang.x}; // 90° CCW
}

// ── Split at t (De Casteljau) ─────────────────────────────────────────────────
std::pair<CubicSegment, CubicSegment> CubicSegment::split(double t) const {
    Vec2 p01  = lerp(p0, p1, t);
    Vec2 p12  = lerp(p1, p2, t);
    Vec2 p23  = lerp(p2, p3, t);
    Vec2 p012 = lerp(p01, p12, t);
    Vec2 p123 = lerp(p12, p23, t);
    Vec2 mid  = lerp(p012, p123, t);
    return { CubicSegment{p0, p01, p012, mid},
             CubicSegment{mid, p123, p23, p3} };
}

// ── Nearest point — Newton-Raphson ────────────────────────────────────────────
double CubicSegment::nearest_t(Vec2 pt) const {
    // Initial guess: sample 8 points
    double best_t = 0.0;
    double best_d = 1e18;
    for (int i = 0; i <= 8; ++i) {
        double t = i / 8.0;
        double d = at(t).dist_sq(pt);
        if (d < best_d) { best_d = d; best_t = t; }
    }

    // Newton-Raphson: minimise f(t) = |B(t)-pt|^2
    // f'(t) = 2(B(t)-pt)·B'(t)
    // f''(t) = 2(B'(t)·B'(t) + (B(t)-pt)·B''(t))
    double t = best_t;
    for (int iter = 0; iter < 10; ++iter) {
        Vec2 b  = at(t);
        Vec2 b1 = tangent(t);
        Vec2 diff = b - pt;
        double fp  = 2.0 * diff.dot(b1);
        // second derivative of B
        double u = 1.0 - t;
        Vec2 b2 = (p2-p1*2+p0) * (6*u)
                + (p3-p2*2+p1) * (6*t);
        double fpp = 2.0 * (b1.dot(b1) + diff.dot(b2));
        if (std::abs(fpp) < 1e-12) break;
        double dt = fp / fpp;
        t -= dt;
        t = std::clamp(t, 0.0, 1.0);
        if (std::abs(dt) < 1e-7) break;
    }
    return std::clamp(t, 0.0, 1.0);
}

double CubicSegment::dist_to(Vec2 pt) const {
    double t = nearest_t(pt);
    return at(t).dist(pt);
}

// ── Arc length (Gaussian quadrature, 5-point) ─────────────────────────────────
double CubicSegment::length(int subdivisions) const {
    // 5-point Gauss-Legendre quadrature per subdivision interval.
    // Integrates |tangent(t)| over [0,1] split into `subdivisions` sub-intervals.
    static const double w[] = {0.2369268851, 0.4786286705, 0.5688888889,
                                0.4786286705, 0.2369268851};
    static const double x[] = {-0.9061798459, -0.5384693101, 0.0,
                                 0.5384693101,  0.9061798459};
    double len = 0.0;
    double step = 1.0 / subdivisions;
    for (int s = 0; s < subdivisions; ++s) {
        double t0   = s * step;
        double t1   = t0 + step;
        double mid  = (t0 + t1) * 0.5;
        double half = (t1 - t0) * 0.5;
        double seg_len = 0.0;
        for (int i = 0; i < 5; ++i) {
            double t = mid + half * x[i];
            seg_len += w[i] * tangent(t).length();
        }
        len += seg_len * half;
    }
    return len;
}

// ── Bounding box ──────────────────────────────────────────────────────────────
void CubicSegment::bbox(Vec2& mn, Vec2& mx) const {
    mn = {std::min({p0.x,p1.x,p2.x,p3.x}), std::min({p0.y,p1.y,p2.y,p3.y})};
    mx = {std::max({p0.x,p1.x,p2.x,p3.x}), std::max({p0.y,p1.y,p2.y,p3.y})};
}

// ── Curve-curve intersection — Sederberg-Nishita bezier clipping ─────────────
//
// References: Sederberg & Nishita, "Curve intersection using Bezier clipping"
// Computer-Aided Design 22(9), 1990.
//
// Given curves A and B find all (t_A, t_B) where A(t_A) == B(t_B).
//
// Core idea: compute B's "fat line" — the chord-line slab [d_min,d_max] that
// contains all of B's control polygon.  Express A's control points as signed
// distances from that line (a degree-3 polynomial in Bernstein form).  Clip
// A's parameter interval to the portion whose convex hull overlaps the slab.
// Swap roles and repeat. When both intervals are < epsilon, record a hit.
// When reduction is poor, split the longer interval (handles multi-root case).

// ── Fat line of a cubic segment ───────────────────────────────────────────────
// Fills nx,ny (unit normal of chord p0→p3), and d_min/d_max (signed-distance
// slab containing all four control points from that line).
static void fat_line(const CubicSegment& s,
                     double& nx, double& ny,
                     double& d_min, double& d_max)
{
    double cx = s.p3.x - s.p0.x;
    double cy = s.p3.y - s.p0.y;
    double len = std::sqrt(cx*cx + cy*cy);
    if (len < 1e-12) {
        nx = 1.0; ny = 0.0; d_min = -1e-4; d_max = 1e-4;
        return;
    }
    // Unit normal to chord p0→p3 (left-perpendicular)
    nx =  cy / len;
    ny = -cx / len;

    // Signed distances of control points from chord line through p0.
    // d0 = 0 (p0 is on the line), d3 ≈ 0 (p3 is on the line by construction).
    // Only d1 and d2 matter for the fat line bounds.
    auto dist = [&](Vec2 p) { return nx*(p.x - s.p0.x) + ny*(p.y - s.p0.y); };
    double d1 = dist(s.p1);
    double d2 = dist(s.p2);

    // Sederberg-Nishita §2 equations (13) and (15) for cubic curves:
    // If d1*d2 > 0 (same side):  bounds = (3/4) * {min, max}{0, d1, d2}
    // If d1*d2 ≤ 0 (opp. sides): bounds = (4/9) * {min, max}{0, d1, d2}
    double factor = (d1 * d2 > 0.0) ? (3.0/4.0) : (4.0/9.0);
    d_min = factor * std::min({0.0, d1, d2});
    d_max = factor * std::max({0.0, d1, d2});

    // For degenerate segments where d1==d2==0 (e.g. rect edges where p1==p0
    // and p2==p3), the fat line has zero width. This is mathematically correct —
    // the segment is a straight line lying exactly on the chord. Give it a tiny
    // epsilon width so clip_to_slab can find crossings at d=0.
    if (d_max - d_min < 1e-10) {
        d_min = -1e-6;
        d_max =  1e-6;
    }
}

// ── Clip A's [0,1] parameter interval against a distance slab ────────────────
// Implements Sederberg-Nishita §"Bézier clipping" eq.18-19 exactly.
//
// The distance function d(t) from A(t) to the fat line is a degree-3 polynomial
// in Bernstein form with control values d_i = dot(normal, A.p_i - ref_pt).
// The "non-parametric" Bézier D(t) has control points D_i = (i/3, d_i).
//
// We clip by finding all edges of the CONVEX HULL of {D0,D1,D2,D3} that cross
// d = d_min or d = d_max, and take the union of t-values inside the slab.
// This is the mathematically correct guaranteed bound from the paper.
//
// Returns false if the convex hull lies entirely outside [d_min, d_max].
// new_t0, new_t1 are in [0,1].
static bool clip_to_slab(const CubicSegment& a,
                          double nx, double ny, Vec2 ref_pt,
                          double d_min, double d_max,
                          double& new_t0, double& new_t1)
{
    auto dist = [&](Vec2 p) { return nx*(p.x - ref_pt.x) + ny*(p.y - ref_pt.y); };

    // Control point t-positions and distances (eq.19)
    const double tp[4] = { 0.0, 1.0/3.0, 2.0/3.0, 1.0 };
    double d[4] = { dist(a.p0), dist(a.p1), dist(a.p2), dist(a.p3) };

    // Clip interval: start with "no valid t found"
    double t0 = 2.0, t1 = -1.0;

    // Helper: record a t value as inside the slab
    auto record = [&](double t) {
        t = std::clamp(t, 0.0, 1.0);
        t0 = std::min(t0, t);
        t1 = std::max(t1, t);
    };

    // ── Convex hull of 4 points in (t, d) space ───────────────────────────
    // For 4 points we compute the hull explicitly using the gift-wrap / cross
    // product approach, then intersect every hull edge with d=d_min and d=d_max.
    //
    // Key property (paper §2): d(t) is bounded by its convex hull, so any
    // t where the hull overlaps [d_min, d_max] must be included in the result.

    // Build array of hull points
    struct Pt2 { double t, d; };
    Pt2 pts[4] = { {tp[0],d[0]}, {tp[1],d[1]}, {tp[2],d[2]}, {tp[3],d[3]} };

    // For each pair of hull points, test the edge for slab crossings.
    // We test ALL C(4,2)=6 pairs — for 4 points this is equivalent to
    // iterating over hull edges since every edge of the convex hull of
    // 4 points is a pair of those points. Non-hull edges are interior
    // chords that don't expand the clipped interval, so including them
    // is conservative (safe) and still correct per the convex hull property.
    for (int i = 0; i < 4; ++i) {
        for (int j = i+1; j < 4; ++j) {
            double ta = pts[i].t, da = pts[i].d;
            double tb = pts[j].t, db = pts[j].d;
            double dspan = db - da;

            // If both endpoints inside slab, record them
            bool a_in = (da >= d_min - 1e-12 && da <= d_max + 1e-12);
            bool b_in = (db >= d_min - 1e-12 && db <= d_max + 1e-12);
            if (a_in) record(ta);
            if (b_in) record(tb);

            // Find crossings with d_min and d_max
            if (std::abs(dspan) > 1e-14) {
                for (double boundary : {d_min, d_max}) {
                    double tc = ta + (boundary - da) / dspan * (tb - ta);
                    if (tc >= -1e-12 && tc <= 1.0 + 1e-12)
                        record(tc);
                }
            }
        }
    }

    if (t0 > t1 + 1e-12) return false;
    new_t0 = std::clamp(t0, 0.0, 1.0);
    new_t1 = std::clamp(t1, 0.0, 1.0);
    return true;
}

// ── Extract sub-segment [t0,t1] (t values in [0,1] relative to seg) ──────────
static CubicSegment sub_segment(const CubicSegment& seg, double t0, double t1)
{
    t0 = std::clamp(t0, 0.0, 1.0);
    t1 = std::clamp(t1, 0.0, 1.0);
    if (t1 <= t0 + 1e-12) return seg;
    CubicSegment result = seg;
    // Trim right end first (split at t1, take left piece)
    if (t1 < 1.0 - 1e-9) {
        auto [left, _r] = result.split(t1);
        result = left;
    }
    // Trim left end (split at t0/t1 scaled into trimmed piece)
    if (t0 > 1e-9) {
        double inner = (t1 > 1e-9) ? (t0 / t1) : 0.0;
        inner = std::clamp(inner, 0.0, 1.0);
        auto [_l, right] = result.split(inner);
        result = right;
    }
    return result;
}

// ── Bezier clipping recursion ─────────────────────────────────────────────────
// at0/at1, bt0/bt1: current sub-intervals in the original [0,1] parameter space.
// a, b: the actual CubicSegment geometry for those sub-intervals.
static void bc_recurse(
    const CubicSegment& a, double at0, double at1,
    const CubicSegment& b, double bt0, double bt1,
    double epsilon, int depth,
    std::vector<std::pair<double,double>>& results)
{
    // ── Convergence / termination ─────────────────────────────────────────
    // Both intervals tiny → converged to an intersection point
    if ((at1-at0) < epsilon && (bt1-bt0) < epsilon) {
        results.push_back({(at0+at1)*0.5, (bt0+bt1)*0.5});
        return;
    }

    if (depth <= 0) {
        // Depth exhausted but intervals not converged — this sub-problem
        // contains either a genuine intersection or a near-miss. Only report
        // if the intervals are reasonably small (< 0.01 of original [0,1]).
        // Otherwise discard — the bbox test above would have caught a real hit
        // if the curves were close enough.
        if ((at1-at0) < 0.01 && (bt1-bt0) < 0.01) {
            results.push_back({(at0+at1)*0.5, (bt0+bt1)*0.5});
        }
        return;
    }

    // Quick axis-aligned bbox reject
    Vec2 amn, amx, bmn, bmx;
    a.bbox(amn, amx); b.bbox(bmn, bmx);
    const double pad = epsilon;
    if (amx.x < bmn.x-pad || bmx.x < amn.x-pad) return;
    if (amx.y < bmn.y-pad || bmx.y < amn.y-pad) return;

    // ── Step 1: clip A using B's fat line ─────────────────────────────────
    double nx_b, ny_b, dmin_b, dmax_b;
    fat_line(b, nx_b, ny_b, dmin_b, dmax_b);

    double na0, na1;  // new at0, at1 in [0,1]-local-to-a
    if (!clip_to_slab(a, nx_b, ny_b, b.p0, dmin_b, dmax_b, na0, na1)) {
        // Convex hull clipping is a proven bound (Sederberg-Nishita §2).
        // Empty clip means no intersection in this sub-problem — discard.
        return;
    }

    // Map na0/na1 back to global parameter space
    double a_span = at1 - at0;
    double new_at0 = at0 + na0 * a_span;
    double new_at1 = at0 + na1 * a_span;
    double a_reduction = (a_span > 1e-12) ? 1.0 - (new_at1-new_at0)/a_span : 1.0;

    // If clipped A interval is essentially a point, the intersection is converged
    if (new_at1 - new_at0 < epsilon) {
        results.push_back({(new_at0+new_at1)*0.5, (bt0+bt1)*0.5});
        return;
    }

    CubicSegment a_clipped = sub_segment(a, na0, na1);

    // ── Step 2: clip B using clipped-A's fat line ─────────────────────────
    double nx_a, ny_a, dmin_a, dmax_a;
    fat_line(a_clipped, nx_a, ny_a, dmin_a, dmax_a);

    double nb0, nb1;
    if (!clip_to_slab(b, nx_a, ny_a, a_clipped.p0, dmin_a, dmax_a, nb0, nb1))
        return;  // Genuine rejection after Step 1 succeeded

    double b_span = bt1 - bt0;
    double new_bt0 = bt0 + nb0 * b_span;
    double new_bt1 = bt0 + nb1 * b_span;
    double b_reduction = (b_span > 1e-12) ? 1.0 - (new_bt1-new_bt0)/b_span : 1.0;

    // If clipped B interval is essentially a point, converged
    if (new_bt1 - new_bt0 < epsilon) {
        results.push_back({(new_at0+new_at1)*0.5, (new_bt0+new_bt1)*0.5});
        return;
    }

    CubicSegment b_clipped = sub_segment(b, nb0, nb1);

    // Combined reduction across both clipping steps
    double total_reduction = 1.0 - (1.0 - a_reduction) * (1.0 - b_reduction);

    if (total_reduction >= 0.20) {
        // Good convergence — recurse on clipped sub-intervals
        bc_recurse(a_clipped, new_at0, new_at1,
                   b_clipped, new_bt0, new_bt1,
                   epsilon, depth - 1, results);
    } else {
        // Poor reduction — split the longer interval to separate multiple roots
        if ((new_at1 - new_at0) >= (new_bt1 - new_bt0)) {
            double a_mid = (new_at0 + new_at1) * 0.5;
            auto [a_left, a_right] = a_clipped.split(0.5);
            bc_recurse(a_left,  new_at0, a_mid,   b_clipped, new_bt0, new_bt1, epsilon, depth-1, results);
            bc_recurse(a_right, a_mid,   new_at1, b_clipped, new_bt0, new_bt1, epsilon, depth-1, results);
        } else {
            double b_mid = (new_bt0 + new_bt1) * 0.5;
            auto [b_left, b_right] = b_clipped.split(0.5);
            bc_recurse(a_clipped, new_at0, new_at1, b_left,  new_bt0, b_mid,   epsilon, depth-1, results);
            bc_recurse(a_clipped, new_at0, new_at1, b_right, b_mid,   new_bt1, epsilon, depth-1, results);
        }
    }
}

// ── Deduplicate hits closer than eps in both parameters ───────────────────────
static void dedup_hits(std::vector<std::pair<double,double>>& hits, double eps)
{
    if (hits.size() < 2) return;
    std::sort(hits.begin(), hits.end());
    std::vector<std::pair<double,double>> out;
    out.push_back(hits[0]);
    for (size_t i = 1; i < hits.size(); ++i) {
        if (std::abs(hits[i].first  - out.back().first)  > eps ||
            std::abs(hits[i].second - out.back().second) > eps)
            out.push_back(hits[i]);
    }
    hits = std::move(out);
}

std::vector<std::pair<double,double>> CubicSegment::intersect(
    const CubicSegment& other, double epsilon, int max_depth) const
{
    std::vector<std::pair<double,double>> results;
    bc_recurse(*this, 0.0, 1.0, other, 0.0, 1.0, epsilon, max_depth, results);
    dedup_hits(results, epsilon * 2.0);
    return results;
}

} // namespace Curvz
