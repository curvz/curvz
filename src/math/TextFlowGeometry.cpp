#include "math/TextFlowGeometry.hpp"
#include "math/PathOffset.hpp"
#include "math/BezierPath.hpp"
#include "math/BooleanOpsClipper.hpp"
#include "math/CubicSegment.hpp"
#include "math/Vec2.hpp"
#include "CurvzLog.hpp"

#include <algorithm>
#include <cmath>

namespace Curvz {

std::vector<PathData> erode_outline(const PathData& shape, double inset) {
    // A closed shape needs at least a triangle. An open path has no
    // well-defined "inside" to erode; offset_path returns empty for it too,
    // but we reject up front so the log line names the real reason.
    if (shape.nodes.size() < 3) {
        LOG_DEBUG("erode_outline: shape has {} nodes (<3) — no interior",
                  shape.nodes.size());
        return {};
    }

    // No margin -> the whole interior is fair game. Return the shape itself
    // as the single eroded region (degenerate erosion = identity), so the
    // caller's downstream intersect sees the real boundary, not the bbox.
    if (inset <= 1e-9) {
        return { shape };
    }

    // The margin is a uniform inward offset of the whole outline. offset_path
    // routes a rect through the (s321-fixed) square-corner fast path and an
    // irregular shape through Clipper2; either way Inside returns the inset
    // region(s). A pinch can split the result into multiple closed pieces,
    // which is exactly what the vector return carries through.
    std::vector<PathData> eroded =
        offset_path(shape, inset, OffsetSide::Inside);

    if (eroded.empty()) {
        // Over-erosion (shape thinner than 2*inset everywhere) or a
        // degenerate/open input. Either way there is no room for text once
        // the margin is applied; the caller emits no baselines.
        LOG_DEBUG("erode_outline: inset {:.3f} consumed the shape "
                  "(no region survives)", inset);
    }
    return eroded;
}

// ── helpers (ribbon) ──────────────────────────────────────────────────────────

namespace {

// A cubic segment is "straight" when both handles sit on their anchors, so
// flattening it adds nothing between the endpoints.
bool segment_is_line(const CubicSegment& s) {
    const double eps = 1e-6;
    return (s.p1 - s.p0).length() < eps && (s.p2 - s.p3).length() < eps;
}

// Append p to poly unless it duplicates the last point (keeps Clipper input
// clean; coincident segment joins otherwise emit the shared endpoint twice).
void push_unique(std::vector<Vec2>& poly, Vec2 p) {
    if (poly.empty() || (poly.back() - p).length() > 1e-7)
        poly.push_back(p);
}

// Flatten an (open) baseline path to a polyline in its own frame. Straight
// segments contribute only endpoints; curved segments are uniformly sampled
// at ~flatten_step spacing.
std::vector<Vec2> flatten_baseline(const PathData& base, double flatten_step) {
    std::vector<Vec2> poly;
    if (base.nodes.size() < 2) return poly;

    BezierPath bp = BezierPath::from_path_data(base);
    const int segs = bp.segment_count();
    if (segs <= 0) return poly;

    const double step = std::max(flatten_step, 1e-3);
    push_unique(poly, Vec2{base.nodes[0].x, base.nodes[0].y});

    for (int i = 0; i < segs; ++i) {
        CubicSegment s = bp.segment(i);
        if (segment_is_line(s)) {
            push_unique(poly, s.p3);
            continue;
        }
        const double len   = s.length();
        const int    steps = std::clamp((int)std::ceil(len / step), 2, 256);
        for (int k = 1; k <= steps; ++k)
            push_unique(poly, s.at((double)k / steps));
    }
    return poly;
}

// Distance from p to an open polyline: min point-to-segment over its edges.
// Used to classify which intersection vertices lie ON the baseline (the
// ribbon's top edge) versus on the bottom edge / margin wall.
double dist_point_to_polyline(Vec2 p, const std::vector<Vec2>& poly) {
    if (poly.empty()) return 1e18;
    if (poly.size() == 1) return (p - poly[0]).length();
    double best = 1e18;
    for (size_t i = 0; i + 1 < poly.size(); ++i) {
        Vec2 a = poly[i], b = poly[i + 1];
        Vec2 ab = b - a;
        double L2 = ab.length_sq();
        double t = (L2 > 1e-12)
                       ? ((p - a).x * ab.x + (p - a).y * ab.y) / L2
                       : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        Vec2 proj{a.x + ab.x * t, a.y + ab.y * t};
        best = std::min(best, (p - proj).length());
    }
    return best;
}

} // namespace

PathData baseline_ribbon(const PathData& base_baseline,
                         double dy,
                         double thickness,
                         double flatten_step) {
    PathData out;

    std::vector<Vec2> top = flatten_baseline(base_baseline, flatten_step);
    if (top.size() < 2) {
        LOG_DEBUG("baseline_ribbon: baseline has {} distinct points (<2)",
                  top.size());
        return out;  // empty, not closed — caller treats as no ribbon
    }

    const double t = std::max(thickness, 1e-3);

    // Emit a corner node (handles on the anchor -> straight edges) at p.
    auto corner = [](double x, double y) {
        BezierNode n;
        n.x = x;  n.y = y;
        n.cx1 = x; n.cy1 = y;
        n.cx2 = x; n.cy2 = y;
        n.type = BezierNode::Type::Corner;
        return n;
    };

    out.nodes.reserve(top.size() * 2);

    // Top edge = the (translated) baseline, in baseline order. This is the
    // edge the intersect reads run endpoints off of.
    for (const Vec2& p : top)
        out.nodes.push_back(corner(p.x, p.y + dy));

    // Bottom edge = top edge pushed straight down by thickness, reversed so
    // the ring is a single non-self-intersecting loop. "Straight down" keeps
    // the top edge exactly on the baseline; on a steep curvy baseline the
    // ribbon narrows but the top edge (all that matters) stays true.
    for (auto it = top.rbegin(); it != top.rend(); ++it)
        out.nodes.push_back(corner(it->x, it->y + dy + t));

    out.closed = true;
    return out;
}

std::vector<BaselineSpan> intervals_for_baseline(
    const std::vector<PathData>& eroded_margin,
    const PathData&              base_baseline,
    double                       dy,
    double                       thickness,
    double                       flatten_step) {
    std::vector<BaselineSpan> spans;
    if (eroded_margin.empty()) return spans;

    // The ribbon for this baseline (top edge = baseline translated by dy).
    PathData ribbon = baseline_ribbon(base_baseline, dy, thickness, flatten_step);
    if (ribbon.nodes.size() < 3) return spans;

    // The polyline we classify intersection vertices against: the same
    // flattened baseline, translated by dy — identical to the ribbon top edge.
    std::vector<Vec2> top = flatten_baseline(base_baseline, flatten_step);
    if (top.size() < 2) return spans;
    for (auto& v : top) v.y += dy;

    // Operands: eroded margin region(s) ∩ the ribbon. Raw entry — no refit,
    // so the crossings come back exact.
    std::vector<BezierPath> margin_op;
    margin_op.reserve(eroded_margin.size());
    for (const auto& pd : eroded_margin)
        margin_op.push_back(BezierPath::from_path_data(pd));
    std::vector<BezierPath> ribbon_op{ BezierPath::from_path_data(ribbon) };

    std::vector<std::vector<Vec2>> runs =
        intersect_regions_polylines(margin_op, ribbon_op);
    if (runs.empty()) return spans;

    // A vertex is ON the baseline when it sits far closer to the top edge than
    // the ribbon is thick. True crossings lie on the baseline (dist ~ grid);
    // bottom-edge vertices are ~thickness away; margin-wall vertices fall in
    // between, so the band stays a small fraction of thickness.
    const double on_band = std::max(0.05, thickness * 0.1);

    for (const auto& loop : runs) {
        bool have = false;
        Vec2 lo{}, hi{};
        for (const auto& v : loop) {
            if (dist_point_to_polyline(v, top) > on_band)
                continue;  // bottom edge or margin wall — not a baseline point
            if (!have) { lo = hi = v; have = true; }
            else { if (v.x < lo.x) lo = v; if (v.x > hi.x) hi = v; }
        }
        if (have && hi.x - lo.x > 1e-6)
            spans.push_back({ lo, hi });
    }

    std::sort(spans.begin(), spans.end(),
              [](const BaselineSpan& a, const BaselineSpan& b) {
                  return a.start.x < b.start.x;
              });

    LOG_DEBUG("intervals_for_baseline: dy={:.2f} -> {} run(s) from {} "
              "intersect loop(s)", dy, spans.size(), runs.size());
    return spans;
}

} // namespace Curvz
