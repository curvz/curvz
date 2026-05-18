#include "math/BooleanOpsClipper.hpp"
#include "CurvzLog.hpp"

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

// ══════════════════════════════════════════════════════════════════════════════
// BooleanOpsClipper — S58 Ship 2
//
// Bridge from cubic Bézier PathData to Clipper2's polygon engine and back.
//
// Pipeline per call:
//   1. Flatten each input BezierPath subpath to a polyline via adaptive
//      De Casteljau subdivision. Target per-segment flatness ≤ FLATTEN_TOL_DOC
//      doc units. Closed subpaths produce closed polylines (no duplicate
//      final point — Clipper2 assumes closure for PathsD inputs).
//   2. Build Clipper2Lib::PathsD — one PathD per subpath. Operand A becomes
//      `subjects`, operand B becomes `clips`.
//   3. Pick FillRule: NonZero when both operands are single-subpath; EvenOdd
//      when either side is multi-subpath (matches Canvas Compound drawing).
//   4. Call Clipper2Lib::Union / Difference / Intersect with DECIMAL_PREC
//      coordinate precision.
//   5. Refit each output PathD into a PathData of cubic Bézier nodes. First-
//      cut refit is per-polyline-edge: each edge becomes one straight cubic
//      with colinear handles at 1/3 and 2/3 of the edge. Nodes are marked
//      Corner — result curves are polygonal but geometrically exact to the
//      flatten tolerance. Proper polyline-to-cubic least-squares refit is
//      held for a later ship once the pipeline is proven end-to-end.
// ══════════════════════════════════════════════════════════════════════════════

namespace Curvz {

// ── Tunables ─────────────────────────────────────────────────────────────────
// Flatten tolerance in doc units. 0.1 is a starting point — at typical icon
// zooms this is sub-pixel. If result polygons look visibly faceted the user
// can lower this, at cost of more polyline edges (and a denser refit).
static constexpr double FLATTEN_TOL_DOC = 0.1;

// Hard cap on recursive subdivision depth to prevent pathological curves
// (cusps, near-zero-length control polygons) from runaway subdivision.
static constexpr int FLATTEN_MAX_DEPTH = 16;

// Clipper2 decimal precision. 3 decimal places = 0.001 unit grid, which is
// well below our flatten tolerance so no extra quantization loss is injected
// by the Int64 internal representation.
static constexpr int DECIMAL_PREC = 3;

// Below this squared chord length we drop a polyline vertex as a duplicate.
// In doc units; matches DECIMAL_PREC grid.
static constexpr double DEDUP_EPS_SQ = 1e-6;

// ── Flatness test ────────────────────────────────────────────────────────────
// "Flat enough" = all interior control points lie within FLATTEN_TOL_DOC
// of the chord p0→p3. This is a standard conservative flatness measure for
// cubic Béziers: if the handles are close to the chord, the curve is too.
static bool segment_is_flat(const CubicSegment& s, double tol) {
    const double dx = s.p3.x - s.p0.x;
    const double dy = s.p3.y - s.p0.y;
    const double chord_len2 = dx * dx + dy * dy;

    // Degenerate chord — just check handles against p0.
    if (chord_len2 < 1e-12) {
        const double d1x = s.p1.x - s.p0.x, d1y = s.p1.y - s.p0.y;
        const double d2x = s.p2.x - s.p0.x, d2y = s.p2.y - s.p0.y;
        return (d1x * d1x + d1y * d1y) <= tol * tol
            && (d2x * d2x + d2y * d2y) <= tol * tol;
    }

    // Perpendicular distance from p1 and p2 to the chord.
    // distance = |cross(handle - p0, chord)| / |chord|
    // We compare squared distances against tol² * |chord|² to avoid sqrt.
    const double c1 = (s.p1.x - s.p0.x) * dy - (s.p1.y - s.p0.y) * dx;
    const double c2 = (s.p2.x - s.p0.x) * dy - (s.p2.y - s.p0.y) * dx;
    const double tol2_chord2 = tol * tol * chord_len2;
    return (c1 * c1) <= tol2_chord2 && (c2 * c2) <= tol2_chord2;
}

// ── Adaptive flatten (recursive De Casteljau) ────────────────────────────────
// Appends vertices for this segment to `out`, EXCLUDING s.p0 (the caller is
// responsible for emitting the starting vertex once). The final vertex s.p3
// IS appended.
static void flatten_segment(const CubicSegment& s,
                            Clipper2Lib::PathD& out,
                            double tol,
                            int depth)
{
    if (depth >= FLATTEN_MAX_DEPTH || segment_is_flat(s, tol)) {
        out.emplace_back(s.p3.x, s.p3.y);
        return;
    }
    auto pr = s.split(0.5);
    flatten_segment(pr.first,  out, tol, depth + 1);
    flatten_segment(pr.second, out, tol, depth + 1);
}

// ── Dedup consecutive near-duplicate vertices ────────────────────────────────
// Clipper2 is robust to coincident points but they waste Int64 conversion and
// produce tiny refit segments. Filter them here.
static void dedup_polyline(Clipper2Lib::PathD& poly) {
    if (poly.size() < 2) return;
    Clipper2Lib::PathD clean;
    clean.reserve(poly.size());
    clean.push_back(poly[0]);
    for (std::size_t i = 1; i < poly.size(); ++i) {
        const auto& prev = clean.back();
        const auto& cur  = poly[i];
        const double dx = cur.x - prev.x;
        const double dy = cur.y - prev.y;
        if ((dx * dx + dy * dy) > DEDUP_EPS_SQ) {
            clean.push_back(cur);
        }
    }
    // For closed PathD, also drop a trailing point that coincides with the
    // first (Clipper2 closes implicitly).
    if (clean.size() >= 2) {
        const auto& f = clean.front();
        const auto& b = clean.back();
        const double dx = b.x - f.x, dy = b.y - f.y;
        if ((dx * dx + dy * dy) < DEDUP_EPS_SQ) clean.pop_back();
    }
    poly = std::move(clean);
}

// ── Flatten one BezierPath subpath to a PathD ────────────────────────────────
// Open subpaths are flattened as polyline fragments and handed to Clipper2
// anyway — but Canvas::boolean_op already rejects open paths upstream, so
// this path is defensive. Compounds with a mix of closed subpaths are fine:
// each subpath becomes one PathD in the same PathsD list.
static Clipper2Lib::PathD flatten_subpath(const BezierPath& bp) {
    Clipper2Lib::PathD out;
    const int nsegs = bp.segment_count();
    if (nsegs <= 0 || bp.nodes.empty()) return out;

    // Emit the starting anchor, then walk segments.
    out.emplace_back(bp.nodes[0].x, bp.nodes[0].y);
    for (int i = 0; i < nsegs; ++i) {
        flatten_segment(bp.segment(i), out, FLATTEN_TOL_DOC, 0);
    }
    dedup_polyline(out);
    return out;
}

// ── Build PathsD for an operand (vector of subpaths) ─────────────────────────
static Clipper2Lib::PathsD build_pathsd(const std::vector<BezierPath>& subpaths) {
    Clipper2Lib::PathsD paths;
    paths.reserve(subpaths.size());
    for (const auto& bp : subpaths) {
        auto pd = flatten_subpath(bp);
        if (pd.size() >= 3) paths.push_back(std::move(pd));
    }
    return paths;
}

// ── Refit: PathD → PathData of straight cubic segments ───────────────────────
// Each polyline edge (v[i] → v[i+1]) becomes a cubic with handles positioned
// 1/3 and 2/3 along the edge. This is exactly equivalent to a line segment
// in Bézier form. All nodes are Corner type (no smooth-continuity claim).
//
// This is Ship 2's first-cut refit. The result will look polygonal on close
// inspection but it is geometrically correct to FLATTEN_TOL_DOC. Proper
// least-squares cubic refit is deferred; this lets us prove the pipeline end
// to end before investing in curve-quality work.
static PathData refit_path_straight_cubics(const Clipper2Lib::PathD& poly) {
    PathData pd;
    pd.closed = true;
    if (poly.size() < 3) return pd;

    pd.nodes.reserve(poly.size());
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const auto& p    = poly[i];
        const auto& prev = poly[(i == 0) ? poly.size() - 1 : i - 1];
        const auto& next = poly[(i + 1) % poly.size()];

        BezierNode n;
        n.x = p.x;
        n.y = p.y;
        // In-handle: 1/3 of the way from this anchor back toward prev.
        n.cx1 = p.x + (prev.x - p.x) / 3.0;
        n.cy1 = p.y + (prev.y - p.y) / 3.0;
        // Out-handle: 1/3 of the way from this anchor toward next.
        n.cx2 = p.x + (next.x - p.x) / 3.0;
        n.cy2 = p.y + (next.y - p.y) / 3.0;
        n.type = BezierNode::Type::Corner;
        pd.nodes.push_back(n);
    }
    return pd;
}

// ── FillRule heuristic ───────────────────────────────────────────────────────
// Canvas draws Compounds with even/odd — nested subpath windings produce
// holes. Single-subpath operands have no topology concern, so non-zero is
// fine (and more forgiving of accidentally-reversed windings from SVG
// round trips). The N-operand entry computes this inline from the total
// subpath count across all operands; see boolean_op_clipper below.
//
// (s122 m3: previous binary-only `pick_fill_rule(subj_n, clip_n)` helper
// was removed when the binary entry became a shim over the N-operand
// implementation.)

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — N-operand
// ══════════════════════════════════════════════════════════════════════════════
// s122 m3: N-operand boolean ops.
//
// For each op, pack/dispatch differs:
//
//   Union N≥2:
//     Combine every subpath of every operand into one PathsD, run a
//     binary Clipper2 Union with an EMPTY PathsD as the clips argument.
//     Clipper2 fuses all overlaps inside `subj` in a single pass — no
//     iterative folding (which was the corruption source for N≥3 in
//     earlier ships). The C++ API has no single-input Union overload
//     (that's a C# convenience); empty-clips is the idiomatic equivalent.
//
//   Subtract N≥2:
//     "Bottommost minus union of the rest" — Affinity / Illustrator
//     Pathfinder convention. Sort by Z is the caller's job; we treat
//     operands[0] as the subject and operands[1..] as the clip set.
//
//   Intersect N≥2:
//     Iterative: running = Intersect(running, operands[i]). Intersect
//     is associative and monotonically shrinking, so iteration is safe
//     (unlike Union's iterative fold which corrupted on overlap webs).
//
// FillRule selection follows the same heuristic as the binary path — if
// any operand is multi-subpath, EvenOdd; otherwise NonZero. We compute
// it from the merged subject for Union, and from the participating sides
// for the other ops.
// ══════════════════════════════════════════════════════════════════════════════
std::vector<PathData> boolean_op_clipper(
    const std::vector<std::vector<BezierPath>>& operands,
    BooleanOpType op)
{
    const char* op_name =
        (op == BooleanOpType::Union)     ? "Union" :
        (op == BooleanOpType::Subtract)  ? "Subtract" :
        (op == BooleanOpType::Intersect) ? "Intersect" : "?";

    if (operands.size() < 2) {
        LOG_WARN("BooleanOpsClipper: {} requires >=2 operands (got {})",
                 op_name, operands.size());
        return {};
    }

    // Flatten every operand to PathsD up front. Track total subpath count
    // across all operands for the FillRule heuristic.
    std::vector<Clipper2Lib::PathsD> packed;
    packed.reserve(operands.size());
    std::size_t total_subpaths = 0;
    for (std::size_t i = 0; i < operands.size(); ++i) {
        auto pd = build_pathsd(operands[i]);
        if (pd.empty()) {
            LOG_WARN("BooleanOpsClipper: {} — operand[{}] empty after flatten "
                     "(input subpaths={})",
                     op_name, i, operands[i].size());
            // For Subtract, an empty subject is fatal. For others, an empty
            // operand may still leave a meaningful result — but Canvas
            // upstream rejects open/empty paths so this is defensive.
            if (op == BooleanOpType::Subtract && i == 0) return {};
            // Empty Intersect operand → result is empty (intersection with
            // nothing). Empty Union operand → just skip it.
            if (op == BooleanOpType::Intersect) return {};
        }
        total_subpaths += operands[i].size();
        packed.push_back(std::move(pd));
    }

    // Pick a global fill rule from total subpath count across all sides.
    const auto fill_rule = (total_subpaths > operands.size())
                               ? Clipper2Lib::FillRule::EvenOdd
                               : Clipper2Lib::FillRule::NonZero;

    Clipper2Lib::PathsD result;
    try {
        switch (op) {
            case BooleanOpType::Union: {
                // Pack every subpath from every operand into one PathsD.
                Clipper2Lib::PathsD subj;
                std::size_t reserve_n = 0;
                for (const auto& p : packed) reserve_n += p.size();
                subj.reserve(reserve_n);
                for (auto& p : packed)
                    for (auto& path : p) subj.push_back(std::move(path));
                std::size_t verts = 0;
                for (const auto& p : subj) verts += p.size();
                LOG_INFO("BooleanOpsClipper: Union N={} — merged subj paths={}/{} verts",
                         operands.size(), subj.size(), verts);
                // C++ Clipper2 has no single-input Union overload (that's a
                // C#-only convenience). The idiomatic C++ way to union a
                // single PathsD against itself is to pass an EMPTY PathsD
                // as the clips argument — Clipper2 still fuses every
                // overlap inside `subj` in a single pass. (s122 m3 fix2:
                // earlier 3-arg call matched a wrong overload via implicit
                // conversion and segfaulted at runtime.)
                Clipper2Lib::PathsD empty_clips;
                result = Clipper2Lib::Union(subj, empty_clips, fill_rule, DECIMAL_PREC);
                break;
            }
            case BooleanOpType::Subtract: {
                // operands[0] is the subject (kept). All others go in clip.
                Clipper2Lib::PathsD subj = std::move(packed[0]);
                Clipper2Lib::PathsD clip;
                std::size_t reserve_n = 0;
                for (std::size_t i = 1; i < packed.size(); ++i)
                    reserve_n += packed[i].size();
                clip.reserve(reserve_n);
                for (std::size_t i = 1; i < packed.size(); ++i)
                    for (auto& path : packed[i]) clip.push_back(std::move(path));
                LOG_INFO("BooleanOpsClipper: Subtract N={} — subj paths={}, clip paths={}",
                         operands.size(), subj.size(), clip.size());
                result = Clipper2Lib::Difference(subj, clip, fill_rule, DECIMAL_PREC);
                break;
            }
            case BooleanOpType::Intersect: {
                // Iterative: running = Intersect(running, next). Each step
                // is a fresh well-defined binary op; the running result
                // only shrinks. No fold corruption hazard.
                Clipper2Lib::PathsD running = std::move(packed[0]);
                LOG_INFO("BooleanOpsClipper: Intersect N={} — initial subj paths={}",
                         operands.size(), running.size());
                for (std::size_t i = 1; i < packed.size(); ++i) {
                    if (running.empty()) {
                        LOG_INFO("BooleanOpsClipper: Intersect — running empty "
                                 "after step {}, short-circuit", i - 1);
                        break;
                    }
                    running = Clipper2Lib::Intersect(
                        running, packed[i], fill_rule, DECIMAL_PREC);
                    LOG_INFO("BooleanOpsClipper: Intersect step {} -> {} paths",
                             i, running.size());
                }
                result = std::move(running);
                break;
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("BooleanOpsClipper: {} threw — {}", op_name, e.what());
        return {};
    }

    LOG_INFO("BooleanOpsClipper: {} produced {} result path(s)", op_name, result.size());

    // ── Refit each result PathD to a cubic PathData ──────────────────────────
    std::vector<PathData> loops;
    loops.reserve(result.size());
    for (const auto& poly : result) {
        if (poly.size() < 3) continue;
        PathData pd = refit_path_straight_cubics(poly);
        if (pd.nodes.size() >= 3) loops.push_back(std::move(pd));
    }

    if (loops.empty()) {
        LOG_WARN("BooleanOpsClipper: {} — no refitted loops survived filtering",
                 op_name);
    }

    return loops;
}

// ══════════════════════════════════════════════════════════════════════════════
// Public entry point — 2-operand back-compat shim
// ══════════════════════════════════════════════════════════════════════════════
std::vector<PathData> boolean_op_clipper(
    const std::vector<BezierPath>& A,
    const std::vector<BezierPath>& B,
    BooleanOpType op)
{
    std::vector<std::vector<BezierPath>> operands;
    operands.reserve(2);
    operands.push_back(A);
    operands.push_back(B);
    return boolean_op_clipper(operands, op);
}

// ══════════════════════════════════════════════════════════════════════════════
// s260 — offset_path_clipper_polylines
//
// Path offset for irregular shapes via Clipper2's InflatePaths. Returns
// raw output polylines (one per output loop). Caller (PathOffset.cpp)
// does anchor selection and Bézier refit.
//
// Pipeline:
//   1. Flatten the input PathData to a PathD using the existing boolean-
//      op flatten machinery (FLATTEN_TOL_DOC tolerance).
//   2. Pre-clean: Clipper2::Union the path with an empty clip set. This
//      resolves any self-intersections in the input — a self-crossing
//      path gets a well-defined filled-region representation, which is
//      exactly the user's "stroke-then-trace" mental model for offset.
//   3. Sign convention: positive `distance` always means outward growth.
//      Clipper2's InflatePaths grows in the direction of positive
//      winding; if the cleaned region has negative winding (Clipper2's
//      IsPositive returns false) we flip the delta sign.
//   4. Call InflatePaths(±distance, Miter join, Polygon/Butt end).
//   5. Convert each output PathD into a vector<Vec2> and return.
//
// For OffsetSide::Both: the function returns outer (positive delta)
// loops followed by inner (negative delta) loops. Caller can distinguish
// by ordering if needed, but typically just emits all loops as separate
// PathDatas.
// ══════════════════════════════════════════════════════════════════════════════

// Inflate one PathsD (already pre-cleaned via union) by `delta` doc units.
// Returns the resulting polylines as Curvz Vec2 vectors. Clipper2 emits
// every boundary loop of the inflated region — outer envelopes AND
// inner hole boundaries. We return them all; caller decides what to do
// with each.
//
// Cap and join control:
//   - `cap` applies only to open inputs (closed inputs always use
//     EndType::Polygon, which doesn't cap because there are no endpoints).
//   - `join` and `miter_limit` apply to all corner joins along the path.
//
// Cap mapping:
//     LineCap::Butt   → EndType::Butt    (flat ends flush with endpoint)
//     LineCap::Square → EndType::Square  (square ends extending past endpoint by delta)
//     LineCap::Round  → EndType::Round   (semicircular ends)
//
// Join mapping:
//     LineJoin::Miter → JoinType::Miter  (sharp pointed, with limit)
//     LineJoin::Round → JoinType::Round  (arc-tolerance controlled)
//     LineJoin::Bevel → JoinType::Square (chopped corner — Clipper2's "Square"
//                                         join is SVG's "Bevel"; the naming
//                                         is a Clipper2 quirk worth flagging)
static std::vector<std::vector<Vec2>>
inflate_one_side(const Clipper2Lib::PathsD& cleaned,
                 double   delta,
                 bool     closed_input,
                 LineCap  cap,
                 LineJoin join,
                 double   miter_limit_ratio)
{
    std::vector<std::vector<Vec2>> out;
    if (cleaned.empty() || std::abs(delta) < 1e-9) return out;

    // ── Map Curvz LineCap → Clipper2 EndType.
    // Closed inputs override: no endpoints to cap, use EndType::Polygon.
    Clipper2Lib::EndType et = Clipper2Lib::EndType::Polygon;
    if (!closed_input) {
        switch (cap) {
            case LineCap::Butt:   et = Clipper2Lib::EndType::Butt;   break;
            case LineCap::Square: et = Clipper2Lib::EndType::Square; break;
            case LineCap::Round:  et = Clipper2Lib::EndType::Round;  break;
        }
    }

    // ── Map Curvz LineJoin → Clipper2 JoinType.
    // Note: Clipper2's JoinType::Square is what SVG calls Bevel.
    Clipper2Lib::JoinType jt = Clipper2Lib::JoinType::Miter;
    switch (join) {
        case LineJoin::Miter: jt = Clipper2Lib::JoinType::Miter;  break;
        case LineJoin::Round: jt = Clipper2Lib::JoinType::Round;  break;
        case LineJoin::Bevel: jt = Clipper2Lib::JoinType::Square; break;
    }

    Clipper2Lib::PathsD inflated;
    try {
        inflated = Clipper2Lib::InflatePaths(
            cleaned, delta, jt, et, miter_limit_ratio, DECIMAL_PREC);
    } catch (const std::exception& e) {
        LOG_WARN("offset_path_clipper: InflatePaths threw: {}", e.what());
        return out;
    }

    out.reserve(inflated.size());
    for (const auto& loop : inflated) {
        if (loop.size() < 3) continue;
        std::vector<Vec2> verts;
        verts.reserve(loop.size());
        for (const auto& p : loop) verts.push_back({p.x, p.y});
        out.push_back(std::move(verts));
    }
    return out;
}

std::vector<std::vector<Vec2>> offset_path_clipper_polylines(
    const PathData& path,
    double          distance,
    OffsetSide      side,
    LineCap         cap,
    LineJoin        join,
    double          miter_limit_ratio)
{
    std::vector<std::vector<Vec2>> result;
    if (path.nodes.size() < 2) return result;

    // ── Step 1 — flatten input.
    // Reuse the boolean-op flatten by going through BezierPath. The
    // existing flatten_subpath is static to this file so we can call
    // it directly.
    BezierPath bp = BezierPath::from_path_data(path);
    Clipper2Lib::PathD raw = flatten_subpath(bp);
    if (raw.size() < 3) {
        LOG_WARN("offset_path_clipper: flatten produced <3 points");
        return result;
    }

    Clipper2Lib::PathsD raw_paths;
    raw_paths.push_back(std::move(raw));

    // ── Step 2 — pre-clean via union-with-self.
    // Closed inputs: Union(raw, empty) under NonZero (single-subpath) cleans
    // self-intersections into a well-defined filled region. Open inputs
    // skip this step — open polylines don't have a filled interior to
    // resolve; InflatePaths handles them directly with EndType::Butt.
    Clipper2Lib::PathsD cleaned;
    if (path.closed) {
        try {
            Clipper2Lib::PathsD empty_clips;
            cleaned = Clipper2Lib::Union(
                raw_paths, empty_clips,
                Clipper2Lib::FillRule::NonZero,
                DECIMAL_PREC);
        } catch (const std::exception& e) {
            LOG_WARN("offset_path_clipper: pre-clean Union threw: {}", e.what());
            return result;
        }
        if (cleaned.empty()) {
            // Degenerate input (zero-area). Bail.
            return result;
        }
    } else {
        // Open path — feed raw polyline directly to InflatePaths.
        cleaned = std::move(raw_paths);
    }

    // ── Step 3 — sign convention.
    // distance > 0 = outward at the API boundary. For closed paths,
    // Clipper2's InflatePaths grows in the positive-winding direction;
    // we test the first cleaned loop's IsPositive and flip the delta if
    // necessary so the user-facing semantics are winding-independent.
    // Open paths have no inside/outside so the sign is just literal.
    double sign_flip = 1.0;
    if (path.closed && !cleaned.empty() &&
        !Clipper2Lib::IsPositive(cleaned.front())) {
        sign_flip = -1.0;
    }

    // ── Step 4 — inflate and collect.
    //
    // Closed inputs:
    //   OffsetSide::Outside → one batch at +distance.
    //   OffsetSide::Inside  → one batch at -distance.
    //   OffsetSide::Both    → outer batch then inner batch, concatenated.
    //
    // Open inputs: the side parameter is meaningless — an open polyline
    // has no inside or outside. A single InflatePaths call at +distance
    // produces one closed loop that IS the stroke band around the
    // polyline (with the requested cap at the endpoints). Any OffsetSide
    // value the caller passes is treated as this single stroke-band
    // call.
    auto append = [&](double delta) {
        auto loops = inflate_one_side(cleaned, delta * sign_flip,
                                      path.closed, cap, join,
                                      miter_limit_ratio);
        for (auto& l : loops) result.push_back(std::move(l));
    };

    if (!path.closed) {
        // Open path: one call, regardless of side. Cap and join are
        // honored from the source's StrokeStyle via the function args.
        append(+distance);
    } else if (side == OffsetSide::Outside) {
        append(+distance);
    } else if (side == OffsetSide::Inside) {
        append(-distance);
    } else {
        append(+distance);
        append(-distance);
    }
    return result;
}

} // namespace Curvz
