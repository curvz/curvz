#include "math/CornerTreatment.hpp"
#include "math/Vec2.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <unordered_map>

namespace Curvz {

// ── s194_m1 diagnostic helpers ────────────────────────────────────────────────
static const char* corner_skip_name(CornerSkipReason r) {
    switch (r) {
        case CornerSkipReason::Ok:                return "Ok";
        case CornerSkipReason::AngleTooSharp:     return "AngleTooSharp";
        case CornerSkipReason::AngleTooFlat:      return "AngleTooFlat";
        case CornerSkipReason::CurvedIncoming:    return "CurvedIncoming";
        case CornerSkipReason::CurvedOutgoing:    return "CurvedOutgoing";
        case CornerSkipReason::TooFewNeighbours:  return "TooFewNeighbours";
    }
    return "?";
}
static const char* corner_type_name(CornerType t) {
    switch (t) {
        case CornerType::Round:        return "Round";
        case CornerType::Chamfer:      return "Chamfer";
        case CornerType::InverseRound: return "InverseRound";
    }
    return "?";
}
static const char* bezier_type_name(BezierNode::Type t) {
    switch (t) {
        case BezierNode::Type::Corner:    return "Corner";
        case BezierNode::Type::Cusp:      return "Cusp";
        case BezierNode::Type::Smooth:    return "Smooth";
        case BezierNode::Type::Symmetric: return "Symmetric";
    }
    return "?";
}

// Kappa: handle distance factor for a circular arc approximation via cubic Bézier.
// For a 90° arc this is the classic 0.5523 value.
// For an arbitrary included angle θ, the correct value is:
//   k = (4/3) * tan(θ/4)
// We use the per-corner angle so the arc looks circular regardless of corner angle.
static double kappa_for_angle(double angle_rad) {
    return (4.0 / 3.0) * std::tan(angle_rad / 4.0);
}

// ── handle_is_degenerate ──────────────────────────────────────────────────────
bool handle_is_degenerate(double ax, double ay, double hx, double hy, double epsilon) {
    double dx = hx - ax, dy = hy - ay;
    return std::sqrt(dx*dx + dy*dy) < epsilon;
}

// ── corner_angle_deg ──────────────────────────────────────────────────────────
double corner_angle_deg(const PathData& path, int idx) {
    const int n = (int)path.nodes.size();
    if (n < 2) return -1.0;

    const bool closed = path.closed;

    // Determine incoming and outgoing neighbour indices
    int prev_idx = idx - 1;
    int next_idx = idx + 1;

    if (closed) {
        prev_idx = (idx - 1 + n) % n;
        next_idx = (idx + 1) % n;
    } else {
        if (prev_idx < 0 || next_idx >= n) return -1.0; // endpoint
    }

    const BezierNode& node = path.nodes[idx];
    const BezierNode& prev = path.nodes[prev_idx];
    const BezierNode& next = path.nodes[next_idx];

    // Check handles are degenerate (straight segments in/out)
    if (!handle_is_degenerate(node.x, node.y, node.cx1, node.cy1)) return -1.0;
    if (!handle_is_degenerate(node.x, node.y, node.cx2, node.cy2)) return -1.0;
    if (!handle_is_degenerate(prev.x, prev.y, prev.cx2, prev.cy2)) return -1.0;
    if (!handle_is_degenerate(next.x, next.y, next.cx1, next.cy1)) return -1.0;

    // Incoming direction: prev → node
    Vec2 in_dir  = Vec2{node.x - prev.x, node.y - prev.y}.normalised();
    // Outgoing direction: node → next
    Vec2 out_dir = Vec2{next.x - node.x, next.y - node.y}.normalised();

    // Angle between the two directions (interior angle at the corner)
    double dot   = std::clamp(in_dir.dot(out_dir), -1.0, 1.0);
    double angle = std::acos(dot) * 180.0 / M_PI;

    // acos gives the angle between the vectors; the interior corner angle
    // is 180° - angle (since in_dir continues and out_dir turns away).
    return 180.0 - angle;
}

// ── apply_corner_treatment ────────────────────────────────────────────────────
PathData apply_corner_treatment(
    const PathData&                path_in,
    const std::unordered_set<int>& node_indices,
    CornerType                     type,
    double                         radius,
    std::vector<CornerResult>*     out_results)
{
    // s194_m1: type-based snap (replaces an earlier epsilon-based attempt).
    //
    // The validity gate below requires four handles around the candidate
    // corner to be degenerate (within 1e-4 of their anchors): prev.cx2,
    // node.cx1, node.cx2, next.cx1.  In practice, paths that the user has
    // edited or that came in via SVG round-trips often have Corner-typed
    // nodes whose handle fields have drifted to non-zero values — observed
    // ~5 doc units off anchor in the s194_m1 log on what the user
    // remembered as retracted corners.  The round-corner op then silently
    // skipped them.
    //
    // The fix is structural: a BezierNode::Type::Corner declares the user's
    // intent as "sharp corner, no handles."  The type is the canonical
    // truth; the numeric handle fields are residue from prior operations.
    // We force handles to match the type at the start of every op, for the
    // whole path (cheap — single pass over nodes).
    //
    // Cusp is left alone: cusps explicitly have handles that need to be
    // preserved, and the validity check below rejects curved cusps with
    // CurvedIncoming/Outgoing — which is the right behaviour for that case.

    // Work on a mutable copy.  The const& parameter signals our intent to
    // the caller; the snap is an internal implementation detail.
    PathData path = path_in;

    const int n = (int)path.nodes.size();
    const bool closed = path.closed;

    LOG_INFO("corner_treat: ENTER type={} radius={:.6f} path_n={} closed={} req_indices={}",
             corner_type_name(type), radius, n, closed, node_indices.size());

    for (auto& nm : path.nodes) {
        if (nm.type != BezierNode::Type::Corner) continue;
        nm.cx1 = nm.x; nm.cy1 = nm.y;
        nm.cx2 = nm.x; nm.cy2 = nm.y;
    }

    for (int idx : node_indices) {
        if (idx >= 0 && idx < n) {
            const auto& nd = path.nodes[idx];
            LOG_INFO("corner_treat:   req idx={} type={} xy=({:.4f},{:.4f}) h1=({:.4f},{:.4f}) h2=({:.4f},{:.4f}) (post-snap)",
                     idx, bezier_type_name(nd.type), nd.x, nd.y, nd.cx1, nd.cy1, nd.cx2, nd.cy2);
        } else {
            LOG_INFO("corner_treat:   req idx={} OUT_OF_RANGE", idx);
        }
    }

    // We'll build an output node list by processing input nodes in order.
    // For each node:
    //   - If it's a treatment target and passes all checks: emit P1 [+ arc] + P2
    //     instead of the original node.
    //   - Otherwise: emit the original node unchanged.
    //
    // We must process in forward order because inserting nodes shifts all
    // subsequent indices — we work on the *original* indices throughout.

    // Collect sorted, deduplicated treatment candidates (Corner/Cusp only)
    // that pass validity checks, and record skip reasons for the rest.
    struct Candidate {
        int              orig_idx;
        double           seg_in_len;   // length of incoming straight segment
        double           seg_out_len;  // length of outgoing straight segment
        double           angle_deg;    // interior corner angle
        double           clamped_r;    // radius after clamping
        Vec2             in_dir;       // unit vector incoming
        Vec2             out_dir;      // unit vector outgoing
        CornerSkipReason skip = CornerSkipReason::Ok;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(node_indices.size());

    for (int idx : node_indices) {
        Candidate c;
        c.orig_idx = idx;

        if (idx < 0 || idx >= n) {
            c.skip = CornerSkipReason::TooFewNeighbours;
            candidates.push_back(c);
            continue;
        }

        const BezierNode& node = path.nodes[idx];

        // Only Corner and Cusp nodes
        if (node.type != BezierNode::Type::Corner &&
            node.type != BezierNode::Type::Cusp) {
            // Silently skip — not recorded in out_results
            LOG_INFO("corner_treat:   idx={} SILENT_SKIP not-Corner/Cusp (type={})",
                     idx, bezier_type_name(node.type));
            continue;
        }

        int prev_idx = closed ? (idx - 1 + n) % n : idx - 1;
        int next_idx = closed ? (idx + 1) % n     : idx + 1;

        if (!closed && (prev_idx < 0 || next_idx >= n)) {
            c.skip = CornerSkipReason::TooFewNeighbours;
            candidates.push_back(c);
            continue;
        }

        const BezierNode& prev = path.nodes[prev_idx];
        const BezierNode& next = path.nodes[next_idx];

        // Per-handle distances for diagnostic clarity
        auto dist = [](double ax, double ay, double bx, double by) {
            double dx = bx - ax, dy = by - ay;
            return std::sqrt(dx*dx + dy*dy);
        };
        double d_prev_out  = dist(prev.x, prev.y, prev.cx2, prev.cy2);
        double d_node_in   = dist(node.x, node.y, node.cx1, node.cy1);
        double d_node_out  = dist(node.x, node.y, node.cx2, node.cy2);
        double d_next_in   = dist(next.x, next.y, next.cx1, next.cy1);
        LOG_INFO("corner_treat:   idx={} handle_dist: prev.out={:.6f} node.in={:.6f} node.out={:.6f} next.in={:.6f}",
                 idx, d_prev_out, d_node_in, d_node_out, d_next_in);

        // Check handles for straightness
        if (!handle_is_degenerate(prev.x, prev.y, prev.cx2, prev.cy2)) {
            c.skip = CornerSkipReason::CurvedIncoming;
            LOG_INFO("corner_treat:   idx={} SKIP CurvedIncoming (prev[{}].out non-deg)",
                     idx, prev_idx);
            candidates.push_back(c);
            continue;
        }
        if (!handle_is_degenerate(node.x, node.y, node.cx1, node.cy1)) {
            c.skip = CornerSkipReason::CurvedIncoming;
            LOG_INFO("corner_treat:   idx={} SKIP CurvedIncoming (node.in non-deg)", idx);
            candidates.push_back(c);
            continue;
        }
        if (!handle_is_degenerate(node.x, node.y, node.cx2, node.cy2)) {
            c.skip = CornerSkipReason::CurvedOutgoing;
            LOG_INFO("corner_treat:   idx={} SKIP CurvedOutgoing (node.out non-deg)", idx);
            candidates.push_back(c);
            continue;
        }
        if (!handle_is_degenerate(next.x, next.y, next.cx1, next.cy1)) {
            c.skip = CornerSkipReason::CurvedOutgoing;
            LOG_INFO("corner_treat:   idx={} SKIP CurvedOutgoing (next[{}].in non-deg)",
                     idx, next_idx);
            candidates.push_back(c);
            continue;
        }

        // Segment lengths
        Vec2 in_vec  = {node.x - prev.x, node.y - prev.y};
        Vec2 out_vec = {next.x - node.x, next.y - node.y};
        double in_len  = in_vec.length();
        double out_len = out_vec.length();

        if (in_len < 1e-9 || out_len < 1e-9) {
            c.skip = CornerSkipReason::TooFewNeighbours;
            LOG_INFO("corner_treat:   idx={} SKIP degenerate-segment in_len={:.6e} out_len={:.6e}",
                     idx, in_len, out_len);
            candidates.push_back(c);
            continue;
        }

        c.in_dir  = in_vec  * (1.0 / in_len);
        c.out_dir = out_vec * (1.0 / out_len);

        // Interior angle
        double dot   = std::clamp(c.in_dir.dot(c.out_dir), -1.0, 1.0);
        double angle = (180.0 - std::acos(dot) * 180.0 / M_PI);
        c.angle_deg = angle;

        LOG_INFO("corner_treat:   idx={} in_len={:.4f} out_len={:.4f} angle_deg={:.4f}",
                 idx, in_len, out_len, angle);

        if (angle < 15.0) {
            c.skip = CornerSkipReason::AngleTooSharp;
            LOG_INFO("corner_treat:   idx={} SKIP AngleTooSharp (angle={:.4f})", idx, angle);
            candidates.push_back(c);
            continue;
        }
        if (angle > 170.0) {
            c.skip = CornerSkipReason::AngleTooFlat;
            LOG_INFO("corner_treat:   idx={} SKIP AngleTooFlat (angle={:.4f})", idx, angle);
            candidates.push_back(c);
            continue;
        }

        c.seg_in_len  = in_len;
        c.seg_out_len = out_len;

        // Clamp radius: each treatment consumes `r` from two adjacent segments.
        // We clamp to half the *shorter* of the two adjacent segments so we
        // don't consume more than the full segment (which would collide with
        // a neighbouring treatment or overshoot the previous node).
        double max_r = std::min(in_len, out_len) * 0.5;
        c.clamped_r  = std::min(radius, max_r);

        LOG_INFO("corner_treat:   idx={} OK clamped_r={:.6f} (max_r={:.6f}, req={:.6f})",
                 idx, c.clamped_r, max_r, radius);

        candidates.push_back(c);
    }

    // Fill out_results
    if (out_results) {
        out_results->clear();
        for (auto& c : candidates) {
            CornerResult r;
            r.node_index  = c.orig_idx;
            r.skip_reason = c.skip;
            out_results->push_back(r);
        }
    }

    // Sort candidates by orig_idx ascending — we'll iterate the original path in order
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b){ return a.orig_idx < b.orig_idx; });

    // Build a set of orig indices that will actually be replaced (not skipped)
    std::unordered_set<int> replace_set;
    for (auto& c : candidates)
        if (c.skip == CornerSkipReason::Ok) replace_set.insert(c.orig_idx);

    LOG_INFO("corner_treat: candidates_total={} replace_set_size={}",
             candidates.size(), replace_set.size());
    for (auto& c : candidates) {
        LOG_INFO("corner_treat:   final: idx={} skip={} clamped_r={:.6f}",
                 c.orig_idx, corner_skip_name(c.skip),
                 c.skip == CornerSkipReason::Ok ? c.clamped_r : 0.0);
    }

    // Build output nodes
    PathData result;
    result.closed    = path.closed;
    result.node_sets = {}; // parametric constraints are no longer valid

    // Map from candidate orig_idx → candidate (for quick lookup)
    std::unordered_map<int, const Candidate*> cmap;
    for (auto& c : candidates)
        if (c.skip == CornerSkipReason::Ok) cmap[c.orig_idx] = &c;

    for (int i = 0; i < n; ++i) {
        const BezierNode& src = path.nodes[i];

        auto it = cmap.find(i);
        if (it == cmap.end()) {
            // Not a treatment node — copy verbatim
            result.nodes.push_back(src);
            continue;
        }

        const Candidate& c = *it->second;
        const double r     = c.clamped_r;

        // P1 = corner - in_dir * r  (on the incoming segment, r before corner)
        Vec2 P1 = Vec2{src.x, src.y} - c.in_dir * r;
        // P2 = corner + out_dir * r (on the outgoing segment, r after corner)
        Vec2 P2 = Vec2{src.x, src.y} + c.out_dir * r;

        // The deflection at the corner, in radians: the turn the path makes.
        // 0 = straight, π/2 = 90° corner, approaches π for very sharp corners.
        //
        // The inscribed circle at the corner is the unique circle tangent to
        // both segments such that the tangent points are at distance r (the
        // user's backoff) from the corner. Its radius is:
        //
        //   R_arc = r · tan(θ_interior / 2)
        //
        // where θ_interior = π - deflection. The arc that replaces the corner
        // is the corner-facing arc of that circle — subtending exactly the
        // deflection angle, with tangent points P1 and P2 on the segments.
        //
        // The cubic Bézier that approximates that arc has handle length:
        //
        //   L = (4/3) · tan(deflection/4) · R_arc
        //
        // — the standard arc-approximation kappa times the actual arc radius.
        //
        // (s257 m1 history.) An earlier pass shipped a kappa fix
        // (`arc_span_rad = between_rad` rather than `π - between_rad`) which
        // got the *shape* right but kept handle length proportional to r
        // rather than R_arc. That made the curve correctly proportioned at
        // 90° (where R_arc == r) and increasingly too flat at gentle corners
        // (where R_arc >> r). This pass scales by R_arc, so the cubic
        // traces the proper inscribed arc at every corner angle.
        double dot          = std::clamp(c.in_dir.dot(c.out_dir), -1.0, 1.0);
        double between_rad  = std::acos(dot);             // deflection
        double interior_rad = M_PI - between_rad;         // interior angle
        double R_arc        = r * std::tan(interior_rad / 2.0);

        switch (type) {

        case CornerType::Chamfer: {
            // Two new corner nodes connected by a straight line: P1 → P2
            BezierNode n1;
            n1.x = P1.x; n1.y = P1.y;
            n1.cx1 = P1.x; n1.cy1 = P1.y; // degenerate handles
            n1.cx2 = P1.x; n1.cy2 = P1.y;
            n1.type = BezierNode::Type::Corner;

            BezierNode n2;
            n2.x = P2.x; n2.y = P2.y;
            n2.cx1 = P2.x; n2.cy1 = P2.y;
            n2.cx2 = P2.x; n2.cy2 = P2.y;
            n2.type = BezierNode::Type::Corner;

            result.nodes.push_back(n1);
            result.nodes.push_back(n2);
            break;
        }

        case CornerType::Round: {
            // Tangent-continuous arc tracing the inscribed circle at the
            // corner. Handles point along the incoming/outgoing segment
            // directions, sized by the kappa-for-arc formula at the
            // deflection angle, scaled by the inscribed arc radius.
            double k = kappa_for_angle(between_rad) * R_arc;

            BezierNode n1;
            n1.x   = P1.x; n1.y   = P1.y;
            n1.cx1 = P1.x; n1.cy1 = P1.y; // in-handle retracted (straight incoming segment)
            // out-handle: along in_dir toward the corner — drives the arc
            n1.cx2 = P1.x + c.in_dir.x * k;
            n1.cy2 = P1.y + c.in_dir.y * k;
            n1.type = BezierNode::Type::Corner;

            BezierNode n2;
            n2.x   = P2.x; n2.y   = P2.y;
            // in-handle: along out_dir back toward the corner — drives the arc
            n2.cx1 = P2.x - c.out_dir.x * k;
            n2.cy1 = P2.y - c.out_dir.y * k;
            n2.cx2 = P2.x; n2.cy2 = P2.y; // out-handle retracted (straight outgoing segment)
            n2.type = BezierNode::Type::Corner;

            result.nodes.push_back(n1);
            result.nodes.push_back(n2);
            break;
        }

        case CornerType::InverseRound: {
            // P1 is offset from P along -in_dir.
            // P2 is offset from P along +out_dir.
            // P1's out-handle points in the direction P2 is offset = +out_dir.
            // P2's in-handle points in the direction P1 is offset = -in_dir.
            // This gives the correct tangent for the inscribed circle arc
            // regardless of corner angle.
            //
            // (s257 m1.) Same handle-length scaling as Round — kappa for
            // the deflection angle, scaled by inscribed arc radius. If
            // the concave-arc geometry calls for a different scale, that's
            // a separate followup.
            double k = kappa_for_angle(between_rad) * R_arc;

            BezierNode n1;
            n1.x   = P1.x; n1.y   = P1.y;
            n1.cx1 = P1.x; n1.cy1 = P1.y;
            n1.cx2 = P1.x + c.out_dir.x * k;
            n1.cy2 = P1.y + c.out_dir.y * k;
            n1.type = BezierNode::Type::Corner;

            BezierNode n2;
            n2.x   = P2.x; n2.y   = P2.y;
            n2.cx1 = P2.x - c.in_dir.x * k;
            n2.cy1 = P2.y - c.in_dir.y * k;
            n2.cx2 = P2.x; n2.cy2 = P2.y;
            n2.type = BezierNode::Type::Corner;

            result.nodes.push_back(n1);
            result.nodes.push_back(n2);
            break;
        }

        } // switch
    }

    LOG_INFO("corner_treat: EXIT input_n={} output_n={} delta={}",
             n, result.nodes.size(), (int)result.nodes.size() - n);

    return result;
}

} // namespace Curvz
