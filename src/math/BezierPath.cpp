#include "math/BezierPath.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <algorithm>
#include <unordered_set>

namespace Curvz {

// ── Construction ──────────────────────────────────────────────────────────────
BezierPath BezierPath::from_path_data(const PathData& pd) {
    BezierPath bp;
    bp.nodes     = pd.nodes;
    bp.node_sets = pd.node_sets;
    bp.closed    = pd.closed;
    return bp;
}

PathData BezierPath::to_path_data() const {
    PathData pd;
    pd.nodes     = nodes;
    pd.closed    = closed;
    pd.node_sets = node_sets;
    return pd;
}

// ── Segments ──────────────────────────────────────────────────────────────────
int BezierPath::segment_count() const {
    int n = (int)nodes.size();
    if (n < 2) return 0;
    return closed ? n : n - 1;
}

CubicSegment BezierPath::segment(int i) const {
    int n = (int)nodes.size();
    const BezierNode& a = nodes[i % n];
    const BezierNode& b = nodes[(i + 1) % n];
    return CubicSegment{
        Vec2{a.x,   a.y},
        Vec2{a.cx2, a.cy2}, // out-handle of start node
        Vec2{b.cx1, b.cy1}, // in-handle of end node
        Vec2{b.x,   b.y}
    };
}

// ── Node manipulation ─────────────────────────────────────────────────────────
void BezierPath::insert_node_at(int seg_idx, double t) {
    if (nodes.size() < 2) return;
    CubicSegment seg = segment(seg_idx);
    auto [left, right] = seg.split(t);

    BezierNode newnode;
    newnode.x   = left.p3.x;  newnode.y   = left.p3.y;
    newnode.cx1 = left.p2.x;  newnode.cy1 = left.p2.y;  // in-handle
    newnode.cx2 = right.p1.x; newnode.cy2 = right.p1.y; // out-handle
    newnode.type = BezierNode::Type::Smooth;

    // Update surrounding handles
    int n = (int)nodes.size();
    int next_idx = (seg_idx + 1) % n;
    nodes[seg_idx].cx2 = left.p1.x;
    nodes[seg_idx].cy2 = left.p1.y;
    nodes[next_idx].cx1 = right.p2.x;
    nodes[next_idx].cy1 = right.p2.y;

    nodes.insert(nodes.begin() + seg_idx + 1, newnode);
    node_sets.clear(); // node ordering changed — primitive params no longer valid
    LOG_DEBUG("BezierPath: inserted node at seg {} t={:.2f}", seg_idx, t);
}

void BezierPath::delete_node(int idx) {
    int n = (int)nodes.size();
    if (n <= 0) return;
    // s321 — total guard. A stale/out-of-range idx must never index past
    // the vector: when a path is broken into two and a node deleted, a
    // selection index (m_selected_node) can be left pointing at an index
    // valid for the path it WAS, out of range for the path it now IS.
    // set_node_type (just below) already guards this exact indexing;
    // delete_node did not, and erase(begin()+idx) on a bad idx segfaults.
    // WARN (not silent) so the diagnostic survives even though the crash
    // does not — the root cause is caller-side; this hardens the leaf so
    // it's a safe no-op meanwhile, and the log tells us when it fires.
    if (idx < 0 || idx >= n) {
        LOG_WARN("BezierPath::delete_node: out-of-range idx={} (nodes={} "
                 "closed={}) — ignoring (stale selection index after break?)",
                 idx, n, closed);
        return;
    }
    if (n == 1) {
        // Only node — erase it, caller should delete the object
        nodes.clear();
        node_sets.clear();
        return;
    }
    if (n == 2) {
        // Two nodes — deleting one leaves a single anchor.
        // No shape-fitting needed, just erase and open the path.
        nodes.erase(nodes.begin() + idx);
        closed = false;
        node_sets.clear();
        LOG_DEBUG("BezierPath: deleted node {} (2→1 nodes)", idx);
        return;
    }

    // ── Shape-preserving delete ───────────────────────────────────────────
    // We have two segments: prev→idx and idx→next.
    // Goal: replace them with one segment prev→next that best fits the
    // original shape. Method: sample points from both segments, then solve
    // for the two handle lengths (alpha, beta) that minimise squared error,
    // with the tangent directions fixed from the existing handles.
    //
    // The new segment is:
    //   P(t) = (1-t)^3·A + 3(1-t)^2·t·(A + alpha·dA)
    //                     + 3(1-t)·t^2·(B - beta·dB) + t^3·B
    // where A=prev anchor, B=next anchor,
    //       dA = unit out-direction from prev, dB = unit in-direction from next.
    // This is linear in (alpha, beta) → least-squares 2×2 system.

    int prev_idx = (idx - 1 + n) % n;
    int next_idx = (idx + 1) % n;

    // Only fit if both neighbours exist (always true for n > 2 and valid idx)
    bool has_prev = (idx > 0) || closed;
    bool has_next = (idx < n - 1) || closed;

    if (has_prev && has_next) {
        CubicSegment seg_in  = segment(prev_idx); // prev → idx
        CubicSegment seg_out = segment(idx);       // idx  → next

        Vec2 A{nodes[prev_idx].x, nodes[prev_idx].y};
        Vec2 B{nodes[next_idx].x, nodes[next_idx].y};

        // Tangent directions (unit vectors)
        // dA: leaving prev (out-handle direction from A)
        Vec2 dA = (Vec2{nodes[prev_idx].cx2, nodes[prev_idx].cy2} - A).normalised();
        // dB: arriving at next (in-handle direction into B, reversed so it points toward B)
        Vec2 dB = (B - Vec2{nodes[next_idx].cx1, nodes[next_idx].cy1}).normalised();

        // Fall back to chord direction if handles are degenerate
        Vec2 chord = B - A;
        double chord_len = chord.length();
        if (dA.length_sq() < 1e-12) dA = chord.normalised();
        if (dB.length_sq() < 1e-12) dB = chord.normalised();

        // Sample ~12 evenly-spaced points across both segments
        constexpr int SAMPLES = 12;
        // Estimate relative arc-lengths to distribute parameter evenly
        double len_in  = seg_in.length(8);
        double len_out = seg_out.length(8);
        double total   = len_in + len_out;
        if (total < 1e-9) total = 1.0;
        double t_split = len_in / total; // parameter on combined [0,1] where idx sits

        // Build least-squares system: minimise Σ |P(t_i) - Q(t_i)|^2
        // P(t) = B0·A + B1·(A+α·dA) + B2·(B-β·dB) + B3·B
        // where B0=(1-t)^3, B1=3(1-t)^2t, B2=3(1-t)t^2, B3=t^3
        // Rearrange: P(t) - (B0+B1)·A - (B2+B3)·B = α·B1·dA - β·B2·dB
        // LHS = known residual R(t), RHS is linear in α,β
        double S11=0, S12=0, S22=0, r1=0, r2=0;

        for (int i = 1; i < SAMPLES; ++i) {
            double s = (double)i / SAMPLES; // s ∈ (0,1) along combined length
            Vec2 Q;
            double t_local;
            if (s <= t_split) {
                t_local = (t_split > 1e-9) ? s / t_split : 0.0;
                Q = seg_in.at(t_local);
            } else {
                t_local = (1.0 - t_split > 1e-9) ? (s - t_split) / (1.0 - t_split) : 1.0;
                Q = seg_out.at(t_local);
            }

            double t = s; // parameter in the new single segment
            double u  = 1.0 - t;
            double B0 = u*u*u, B1 = 3*u*u*t, B2 = 3*u*t*t, B3 = t*t*t;

            Vec2 R = Q - A*(B0+B1) - B*(B2+B3);

            double c1x = B1*dA.x, c1y = B1*dA.y;
            double c2x = -B2*dB.x, c2y = -B2*dB.y;

            S11 += c1x*c1x + c1y*c1y;
            S12 += c1x*c2x + c1y*c2y;
            S22 += c2x*c2x + c2y*c2y;
            r1  += R.x*c1x + R.y*c1y;
            r2  += R.x*c2x + R.y*c2y;
        }

        double det = S11*S22 - S12*S12;
        double alpha, beta;
        if (std::abs(det) > 1e-12) {
            alpha = (r1*S22 - r2*S12) / det;
            beta  = (r2*S11 - r1*S12) / det;
        } else {
            // Degenerate — fall back to 1/3 chord length
            alpha = beta = chord_len / 3.0;
        }

        // Clamp to reasonable range (no negative or absurdly large handles)
        double max_len = chord_len * 2.0 + 1.0;
        alpha = std::clamp(alpha, 0.0, max_len);
        beta  = std::clamp(beta,  0.0, max_len);

        // Write new handles onto prev and next
        nodes[prev_idx].cx2 = A.x + dA.x * alpha;
        nodes[prev_idx].cy2 = A.y + dA.y * alpha;
        nodes[next_idx].cx1 = B.x - dB.x * beta;
        nodes[next_idx].cy1 = B.y - dB.y * beta;
    }

    nodes.erase(nodes.begin() + idx);
    node_sets.clear(); // node count changed — primitive params no longer valid
    LOG_DEBUG("BezierPath: deleted node {} with shape-preserving fit", idx);
}

void BezierPath::set_node_type(int idx, BezierNode::Type type) {
    if (idx < 0 || idx >= (int)nodes.size()) return;
    BezierNode& n = nodes[idx];
    BezierNode::Type old = n.type;
    n.type = type;

    if (type == BezierNode::Type::Corner) {
        // No constraint — handles stay exactly where they are.
        // The designer can drag them freely after switching.
    } else if (type == BezierNode::Type::Cusp) {
        // Lock handles to incoming/outgoing segment vectors at current lengths,
        // or compute mathematically correct arc-approximation fillet if converting
        // from a corner/zero-handle state.
        recompute_cusp_handles(idx);
    } else if (type == BezierNode::Type::Symmetric && old != BezierNode::Type::Symmetric) {
        Vec2 out{n.cx2 - n.x, n.cy2 - n.y};
        Vec2 in {n.cx1 - n.x, n.cy1 - n.y};
        double len_out = out.length();
        double len_in  = in.length();
        bool out_degen = (len_out < 1e-9);
        bool in_degen  = (len_in  < 1e-9);

        if (old == BezierNode::Type::Corner) {
            // Corner → Symmetric: preserve existing handle positions.
            // The type tag changes immediately but handle geometry is only
            // enforced (mirrored) when the user next drags a handle.
            // This avoids surprising jumps when switching from Corner.
            if (out_degen && in_degen) {
                // Both degenerate — must compute initial handles from chord.
                int nn = (int)nodes.size();
                int prev_i = (idx - 1 + nn) % nn;
                int next_i = (idx + 1)      % nn;
                bool has_prev = (idx > 0) || closed;
                bool has_next = (idx < nn - 1) || closed;
                if (has_prev && has_next) {
                    Vec2 prev_a{nodes[prev_i].x, nodes[prev_i].y};
                    Vec2 next_a{nodes[next_i].x, nodes[next_i].y};
                    Vec2 anchor{n.x, n.y};
                    double seed_len = ((anchor - prev_a).length() +
                                      (next_a - anchor).length()) * 0.5 * 0.333;
                    Vec2 in_dir  = (anchor - prev_a).normalised();
                    Vec2 out_dir = (next_a - anchor).normalised();
                    Vec2 bisect  = (in_dir + out_dir).normalised();
                    if (bisect.length() < 1e-9) bisect = out_dir;
                    n.cx1 = n.x - bisect.x * seed_len;
                    n.cy1 = n.y - bisect.y * seed_len;
                    n.cx2 = n.x + bisect.x * seed_len;
                    n.cy2 = n.y + bisect.y * seed_len;
                }
            }
            // else: handles already have geometry — keep them as-is
        } else if (!in_degen) {
            // Non-Corner → Symmetric: in-handle is primary, mirror onto out.
            n.cx2 = n.x - in.x;
            n.cy2 = n.y - in.y;
        } else if (!out_degen) {
            // In degenerate, out live — mirror out onto in.
            n.cx1 = n.x - out.x;
            n.cy1 = n.y - out.y;
        } else {
            // Both degenerate — compute from neighbour chord bisect.
            int nn = (int)nodes.size();
            int prev_i = (idx - 1 + nn) % nn;
            int next_i = (idx + 1)      % nn;
            bool has_prev = (idx > 0) || closed;
            bool has_next = (idx < nn - 1) || closed;
            double seed_len = 0.0;
            if (has_prev && has_next) {
                Vec2 prev_a{nodes[prev_i].x, nodes[prev_i].y};
                Vec2 next_a{nodes[next_i].x, nodes[next_i].y};
                Vec2 anchor{n.x, n.y};
                seed_len = ((anchor - prev_a).length() +
                            (next_a - anchor).length()) * 0.5 * 0.333;
                Vec2 in_dir  = (anchor - prev_a).normalised();
                Vec2 out_dir = (next_a - anchor).normalised();
                Vec2 bisect  = (in_dir + out_dir).normalised();
                if (bisect.length() < 1e-9) bisect = out_dir;
                n.cx1 = n.x - bisect.x * seed_len;
                n.cy1 = n.y - bisect.y * seed_len;
                n.cx2 = n.x + bisect.x * seed_len;
                n.cy2 = n.y + bisect.y * seed_len;
            } else if (has_next) {
                Vec2 next_a{nodes[next_i].x, nodes[next_i].y};
                Vec2 dir = (next_a - Vec2{n.x, n.y}).normalised();
                seed_len = (next_a - Vec2{n.x, n.y}).length() * 0.333;
                n.cx1 = n.x - dir.x * seed_len; n.cy1 = n.y - dir.y * seed_len;
                n.cx2 = n.x + dir.x * seed_len; n.cy2 = n.y + dir.y * seed_len;
            } else if (has_prev) {
                Vec2 prev_a{nodes[prev_i].x, nodes[prev_i].y};
                Vec2 dir = (Vec2{n.x, n.y} - prev_a).normalised();
                seed_len = (Vec2{n.x, n.y} - prev_a).length() * 0.333;
                n.cx1 = n.x - dir.x * seed_len; n.cy1 = n.y - dir.y * seed_len;
                n.cx2 = n.x + dir.x * seed_len; n.cy2 = n.y + dir.y * seed_len;
            }
        }
    } else if (type == BezierNode::Type::Smooth && old == BezierNode::Type::Cusp) {
        // Align handles along the same axis, keeping lengths
        Vec2 out{n.cx2 - n.x, n.cy2 - n.y};
        Vec2 in {n.cx1 - n.x, n.cy1 - n.y};
        double len_in = in.length();
        if (len_in > 1e-9 && out.length() > 1e-9) {
            Vec2 dir = out.normalised();
            n.cx1 = n.x - dir.x * len_in;
            n.cy1 = n.y - dir.y * len_in;
        }
    } else if (type == BezierNode::Type::Smooth && old == BezierNode::Type::Corner) {
        // Restore reasonable handles along the line to neighbours
        int nn = (int)nodes.size();
        int prev_i = (idx - 1 + nn) % nn;
        int next_i = (idx + 1) % nn;
        bool has_prev = (idx > 0) || closed;
        bool has_next = (idx < nn - 1) || closed;
        if (has_prev && has_next) {
            Vec2 prev_a{nodes[prev_i].x, nodes[prev_i].y};
            Vec2 next_a{nodes[next_i].x, nodes[next_i].y};
            Vec2 anchor{n.x, n.y};
            double seg_len = (next_a - anchor).length() * 0.333;
            Vec2 dir = (next_a - prev_a).normalised();
            n.cx1 = n.x - dir.x * seg_len;
            n.cy1 = n.y - dir.y * seg_len;
            n.cx2 = n.x + dir.x * seg_len;
            n.cy2 = n.y + dir.y * seg_len;
        }
    }
}

void BezierPath::move_node(int idx, Vec2 pos) {
    if (idx < 0 || idx >= (int)nodes.size()) return;
    BezierNode& n = nodes[idx];
    double dx = pos.x - n.x;
    double dy = pos.y - n.y;
    n.x = pos.x; n.y = pos.y;
    // Move handles with the node
    n.cx1 += dx; n.cy1 += dy;
    n.cx2 += dx; n.cy2 += dy;

    // Cascade cusp recomputation to this node and its immediate neighbours
    int nn = (int)nodes.size();
    recompute_cusp_handles(idx);
    if (idx > 0 || closed)      recompute_cusp_handles((idx - 1 + nn) % nn);
    if (idx < nn - 1 || closed) recompute_cusp_handles((idx + 1) % nn);
}

void BezierPath::recompute_cusp_handles(int idx) {
    int nn = (int)nodes.size();
    if (idx < 0 || idx >= nn) return;
    BezierNode& n = nodes[idx];
    if (n.type != BezierNode::Type::Cusp) return;

    Vec2 anchor{n.x, n.y};
    bool has_prev = (idx > 0) || closed;
    bool has_next = (idx < nn - 1) || closed;

    // The arriving tangent at this node is the direction from the previous
    // segment's out-handle (prev.cx2) to this anchor. That is the slope of
    // the curve in the last infinitesimal bit before reaching this node.
    // Both handles lock to this same line — cx1 backward, cx2 forward.
    // Only lengths are free.

    if (!has_prev) return; // no incoming segment, nothing to lock to

    int prev_i = (idx - 1 + nn) % nn;
    Vec2 prev_cx2{nodes[prev_i].cx2, nodes[prev_i].cy2};

    // Arriving tangent: prev.cx2 → anchor
    Vec2 tang = (anchor - prev_cx2).normalised();

    // If prev.cx2 is degenerate (coincident with anchor), fall back to
    // anchor - prev_anchor chord
    if ((anchor - prev_cx2).length() < 1e-9) {
        Vec2 prev_a{nodes[prev_i].x, nodes[prev_i].y};
        tang = (anchor - prev_a).normalised();
    }

    // Preserve existing handle lengths, bootstrap from segment length if zero
    double in_len  = Vec2{n.cx1 - n.x, n.cy1 - n.y}.length();
    double out_len = Vec2{n.cx2 - n.x, n.cy2 - n.y}.length();

    if (in_len  < 1e-9) {
        Vec2 prev_a{nodes[prev_i].x, nodes[prev_i].y};
        in_len = (anchor - prev_a).length() * 0.333;
    }
    if (out_len < 1e-9 && has_next) {
        int next_i = (idx + 1) % nn;
        Vec2 next_a{nodes[next_i].x, nodes[next_i].y};
        out_len = (next_a - anchor).length() * 0.333;
    }

    // cx1 stays exactly where it is — it already encodes the arriving tangent
    // by its position. Moving it would change the incoming curve shape.

    // cx2 points forward along the arriving tangent at its current length.
    // If out_len was zero, use the bootstrapped value.
    n.cx2 = n.x + tang.x * out_len;
    n.cy2 = n.y + tang.y * out_len;
}

void BezierPath::recompute_join_handles(int idx) {
    // Called after a path is closed/opened at idx — recomputes handles
    // to honour the node type constraints at the join point.
    int nn = (int)nodes.size();
    if (idx < 0 || idx >= nn || nn < 2) return;
    BezierNode& n = nodes[idx];
    Vec2 anchor{n.x, n.y};

    int prev_i = (idx - 1 + nn) % nn;
    int next_i = (idx + 1) % nn;
    Vec2 prev_a{nodes[prev_i].x, nodes[prev_i].y};
    Vec2 next_a{nodes[next_i].x, nodes[next_i].y};

    switch (n.type) {
        case BezierNode::Type::Cusp:
            recompute_cusp_handles(idx);
            break;

        case BezierNode::Type::Corner:
            // Zero handles — pure straight lines in/out
            n.cx1 = n.x; n.cy1 = n.y;
            n.cx2 = n.x; n.cy2 = n.y;
            break;

        case BezierNode::Type::Symmetric: {
            // Bisect the angle between incoming and outgoing directions,
            // set both handles at 1/3 segment length along that axis
            Vec2 in_dir  = (anchor - prev_a).normalised();
            Vec2 out_dir = (next_a - anchor).normalised();
            Vec2 bisect  = (in_dir + out_dir).normalised();
            double len_in  = (anchor - prev_a).length() * 0.333;
            double len_out = (next_a - anchor).length() * 0.333;
            double len = (len_in + len_out) * 0.5;
            n.cx1 = n.x - bisect.x * len;
            n.cy1 = n.y - bisect.y * len;
            n.cx2 = n.x + bisect.x * len;
            n.cy2 = n.y + bisect.y * len;
            break;
        }

        case BezierNode::Type::Smooth: {
            // Keep existing out-handle direction, extend in-handle collinear
            Vec2 out{n.cx2 - n.x, n.cy2 - n.y};
            double out_len = out.length();
            double in_len  = (anchor - prev_a).length() * 0.333;
            if (out_len > 1e-9) {
                Vec2 dir = out.normalised();
                n.cx1 = n.x - dir.x * in_len;
                n.cy1 = n.y - dir.y * in_len;
            } else {
                // Out-handle degenerate — use chord directions
                Vec2 dir = (next_a - anchor).normalised();
                n.cx1 = n.x - dir.x * in_len;
                n.cy1 = n.y - dir.y * in_len;
                n.cx2 = n.x + dir.x * (anchor - prev_a).length() * 0.333;
                n.cy2 = n.y + dir.y * (anchor - prev_a).length() * 0.333;
            }
            break;
        }
    }
}

void BezierPath::move_handle_in(int idx, Vec2 pos) {
    if (idx < 0 || idx >= (int)nodes.size()) return;
    BezierNode& n = nodes[idx];

    if (n.type == BezierNode::Type::Cusp) {
        // Cusp: project drag onto the locked tangent line — length only, no direction change.
        // The tangent direction is the current cx1 direction from anchor.
        Vec2 anchor{n.x, n.y};
        Vec2 cur_in{n.cx1 - n.x, n.cy1 - n.y};
        double cur_len = cur_in.length();
        if (cur_len < 1e-9) { n.cx1 = pos.x; n.cy1 = pos.y; return; }
        Vec2 tang = cur_in * (1.0 / cur_len); // unit vector: anchor → cx1 direction
        Vec2 delta = pos - anchor;
        double projected = delta.dot(tang);
        double new_len = std::max(0.0, projected);
        n.cx1 = n.x + tang.x * new_len;
        n.cy1 = n.y + tang.y * new_len;
        // cx2 stays on the opposite direction of the same line
        double out_len = Vec2{n.cx2 - n.x, n.cy2 - n.y}.length();
        n.cx2 = n.x - tang.x * out_len;
        n.cy2 = n.y - tang.y * out_len;
        return;
    }

    n.cx1 = pos.x; n.cy1 = pos.y;

    if (n.type == BezierNode::Type::Symmetric) {
        n.cx2 = n.x - (pos.x - n.x);
        n.cy2 = n.y - (pos.y - n.y);
    } else if (n.type == BezierNode::Type::Smooth) {
        Vec2 in_dir = Vec2{n.x - pos.x, n.y - pos.y}.normalised();
        double out_len = Vec2{n.cx2-n.x, n.cy2-n.y}.length();
        n.cx2 = n.x + in_dir.x * out_len;
        n.cy2 = n.y + in_dir.y * out_len;
    }
}

void BezierPath::move_handle_out(int idx, Vec2 pos) {
    if (idx < 0 || idx >= (int)nodes.size()) return;
    BezierNode& n = nodes[idx];

    if (n.type == BezierNode::Type::Cusp) {
        // Cusp: project drag onto the locked tangent line — length only.
        // The tangent direction is the current cx2 direction from anchor.
        Vec2 anchor{n.x, n.y};
        Vec2 cur_out{n.cx2 - n.x, n.cy2 - n.y};
        double cur_len = cur_out.length();
        if (cur_len < 1e-9) { n.cx2 = pos.x; n.cy2 = pos.y; return; }
        Vec2 tang = cur_out * (1.0 / cur_len);
        Vec2 delta = pos - anchor;
        double projected = delta.dot(tang);
        double new_len = std::max(0.0, projected);
        n.cx2 = n.x + tang.x * new_len;
        n.cy2 = n.y + tang.y * new_len;
        // cx1 stays on the opposite direction of the same line
        double in_len = Vec2{n.cx1 - n.x, n.cy1 - n.y}.length();
        n.cx1 = n.x - tang.x * in_len;
        n.cy1 = n.y - tang.y * in_len;
        return;
    }

    n.cx2 = pos.x; n.cy2 = pos.y;

    if (n.type == BezierNode::Type::Symmetric) {
        n.cx1 = n.x - (pos.x - n.x);
        n.cy1 = n.y - (pos.y - n.y);
    } else if (n.type == BezierNode::Type::Smooth) {
        Vec2 out_dir = Vec2{n.x - pos.x, n.y - pos.y}.normalised();
        double in_len = Vec2{n.cx1-n.x, n.cy1-n.y}.length();
        n.cx1 = n.x + out_dir.x * in_len;
        n.cy1 = n.y + out_dir.y * in_len;
    }
}

// ── Hit testing ───────────────────────────────────────────────────────────────
HitResult BezierPath::hit_test(Vec2 doc_pt, double zoom, double tol_px) const {
    HitResult best;
    double tol_doc = tol_px / zoom; // tolerance in document space

    // 1. Test node anchors — iterate in reverse so higher indices (tail) win on ties
    for (int i = (int)nodes.size() - 1; i >= 0; --i) {
        Vec2 np{nodes[i].x, nodes[i].y};
        double d = np.dist(doc_pt);
        if (d < tol_doc && d < best.distance) {
            best = {HitResult::Kind::Node, i, 0, -1, d};
        }
    }
    if (best.kind != HitResult::Kind::None) return best;

    // 2. Test handles (only when node editor is active — caller decides)
    for (int i = 0; i < (int)nodes.size(); ++i) {
        Vec2 anchor{nodes[i].x, nodes[i].y};
        Vec2 hin {nodes[i].cx1, nodes[i].cy1};
        Vec2 hout{nodes[i].cx2, nodes[i].cy2};
        // Skip degenerate handles (coincident with anchor) — they have no
        // visual presence and cannot be meaningfully dragged.
        double din  = hin.dist(doc_pt);
        double dout = hout.dist(doc_pt);
        if (hin.dist(anchor) > 1e-9 && din < tol_doc && din < best.distance)
            best = {HitResult::Kind::HandleIn,  i, 0, -1, din};
        if (hout.dist(anchor) > 1e-9 && dout < tol_doc && dout < best.distance)
            best = {HitResult::Kind::HandleOut, i, 0, -1, dout};
    }
    if (best.kind != HitResult::Kind::None) return best;

    // 3. Test curve segments
    for (int s = 0; s < segment_count(); ++s) {
        CubicSegment seg = segment(s);
        double t = seg.nearest_t(doc_pt);
        double d = seg.at(t).dist(doc_pt);
        if (d < tol_doc && d < best.distance)
            best = {HitResult::Kind::OnCurve, -1, t, s, d};
    }

    return best;
}

// ── Cairo rendering ───────────────────────────────────────────────────────────
void BezierPath::apply_to_cairo(const Cairo::RefPtr<Cairo::Context>& cr) const {
    if (nodes.empty()) return;

    cr->move_to(nodes[0].x, nodes[0].y);
    int n = (int)nodes.size();
    for (int s = 0; s < segment_count(); ++s) {
        const BezierNode& a = nodes[s % n];
        const BezierNode& b = nodes[(s + 1) % n];
        bool a_degen = (std::abs(a.cx2 - a.x) < 1e-9 && std::abs(a.cy2 - a.y) < 1e-9);
        bool b_degen = (std::abs(b.cx1 - b.x) < 1e-9 && std::abs(b.cy1 - b.y) < 1e-9);
        if (a_degen && b_degen) {
            cr->line_to(b.x, b.y);
        } else {
            cr->curve_to(a.cx2, a.cy2, b.cx1, b.cy1, b.x, b.y);
        }
    }
    if (closed) cr->close_path();
}

// ── Editor overlay ────────────────────────────────────────────────────────────
void BezierPath::draw_editor_overlay(
    const Cairo::RefPtr<Cairo::Context>& cr,
    double zoom,
    int selected_node,
    bool is_closed) const
{
    if (nodes.empty()) return;

    // All sizes in document space (we are inside cr->scale(zoom,zoom))
    // Canon (HANDOFF): 5px unselected, 7px selected/hover — screen pixels, no zoom scaling.
    const double nr     = 2.5  / zoom;   // node half-size unselected  (→ 5px diameter)
    const double nr_sel = 3.5  / zoom;   // node half-size selected     (→ 7px diameter)
    const double ring_r = 6.0  / zoom;   // origin ring clearance
    const double hr     = 2.5  / zoom;   // handle dot radius
    const double lw     = 1.0  / zoom;   // standard line width
    const double lw_h   = 0.8  / zoom;   // handle line width
    const double lw_r   = 1.2  / zoom;   // origin ring line width

    // Colours
    // node fill (unselected = white so it reads on dark AND light artboards)
    const double NF[3]  = {0.96, 0.96, 0.96};  // unselected fill — near-white
    const double NS[3]  = {0.20, 0.50, 1.00};  // selected fill / stroke accent
    const double NK[3]  = {0.88, 0.88, 0.88};  // unselected stroke — light, reads on dark artboard
    const double NB[4]  = {0.00, 0.00, 0.00, 0.55}; // node border (dark halo behind shape)
    const double HL[4]  = {0.55, 0.68, 1.00, 0.70}; // handle line
    const double HC[4]  = {0.30, 0.60, 1.00, 0.95}; // handle circle fill
    const double OR[4]  = {0.88, 0.88, 0.88, 0.50}; // origin ring — light, reads on dark artboard

    cr->save();

    // ── Helper: draw handle line + circle ────────────────────────────────
    auto draw_handle = [&](Vec2 anchor, Vec2 h, bool active) {
        if (h.dist(anchor) < 0.4/zoom) return;
        cr->set_line_width(lw_h);
        cr->set_source_rgba(HL[0], HL[1], HL[2], HL[3]);
        cr->move_to(anchor.x, anchor.y);
        cr->line_to(h.x, h.y);
        cr->stroke();
        // handle dot
        cr->arc(h.x, h.y, hr, 0, 2*M_PI);
        if (active)
            cr->set_source_rgba(NS[0], NS[1], NS[2], 1.0);
        else
            cr->set_source_rgba(HC[0], HC[1], HC[2], HC[3]);
        cr->fill_preserve();
        cr->set_source_rgb(1.0, 1.0, 1.0);
        cr->set_line_width(0.5/zoom);
        cr->stroke();
    };

    // ── Helper: draw typed node shape (filled + stroked) ─────────────────
    // type: 0=symmetric(filled circle), 1=smooth(hollow circle),
    //       2=cusp(triangle), 3=corner(square), 4=tail(diamond)
    // Draws a dark border pass first so nodes read on both dark and light artboards.
    auto draw_node_shape = [&](Vec2 p, int type, bool sel, bool is_tail = false) {
        double r = sel ? nr_sel : nr;
        const double amber[3] = {0.88, 0.75, 0.30};

        // Build the shape path at radius rr
        auto build_path = [&](double rr) {
            switch (type) {
                case 0: case 1:
                    cr->arc(p.x, p.y, rr, 0, 2*M_PI);
                    break;
                case 2: {
                    double h3 = rr * 1.732;
                    cr->move_to(p.x,          p.y - rr * 1.2);
                    cr->line_to(p.x - h3*0.6, p.y + rr * 0.6);
                    cr->line_to(p.x + h3*0.6, p.y + rr * 0.6);
                    cr->close_path();
                    break;
                }
                case 3:
                    cr->rectangle(p.x - rr, p.y - rr, rr*2, rr*2);
                    break;
                default:
                    cr->move_to(p.x,         p.y - rr*1.3);
                    cr->line_to(p.x + rr*1.3, p.y);
                    cr->line_to(p.x,         p.y + rr*1.3);
                    cr->line_to(p.x - rr*1.3, p.y);
                    cr->close_path();
                    break;
            }
        };

        // Dark border pass — readable on white artboards (unselected only)
        if (!sel) {
            cr->set_source_rgba(NB[0], NB[1], NB[2], NB[3]);
            build_path(r + 1.2 / zoom);
            cr->fill();
        } else {
            // White halo behind selected node so shape reads clearly
            cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
            build_path(r + 1.5 / zoom);
            cr->fill();
        }

        // Coloured shape pass
        if (sel)
            cr->set_source_rgb(NS[0], NS[1], NS[2]);
        else if (type == 1)
            cr->set_source_rgba(NF[0], NF[1], NF[2], 0.0); // smooth: hollow
        else if (is_tail)
            cr->set_source_rgb(amber[0], amber[1], amber[2]);
        else
            cr->set_source_rgb(NF[0], NF[1], NF[2]);

        build_path(r);
        cr->fill_preserve();

        // Stroke
        if (sel)
            cr->set_source_rgba(NS[0], NS[1], NS[2], 0.8);
        else if (type == 1)
            cr->set_source_rgb(NK[0], NK[1], NK[2]);
        else if (is_tail)
            cr->set_source_rgb(amber[0], amber[1], amber[2]);
        else
            cr->set_source_rgb(NK[0], NK[1], NK[2]);

        cr->set_line_width(type == 1 ? lw * 2.0 : lw * 1.5);
        cr->stroke();
    };

    // ── Helper: draw origin ring (same shape, larger, semi-transparent) ──
    auto draw_origin_ring = [&](Vec2 p, int type) {
        double r = ring_r;
        cr->set_source_rgba(OR[0], OR[1], OR[2], OR[3]);
        cr->set_line_width(lw_r);
        switch (type) {
            case 0:
            case 1: // circle ring
                cr->arc(p.x, p.y, r, 0, 2*M_PI);
                break;
            case 2: // triangle ring
            {
                double h3 = r * 1.732;
                cr->move_to(p.x,          p.y - r * 1.2);
                cr->line_to(p.x - h3*0.6, p.y + r * 0.6);
                cr->line_to(p.x + h3*0.6, p.y + r * 0.6);
                cr->close_path();
                break;
            }
            case 4: // diamond ring (tail)
                cr->move_to(p.x,       p.y - r*1.5);
                cr->line_to(p.x + r*1.5, p.y);
                cr->line_to(p.x,       p.y + r*1.5);
                cr->line_to(p.x - r*1.5, p.y);
                cr->close_path();
                break;
            default: // square ring
                cr->rectangle(p.x - r, p.y - r, r*2, r*2);
                break;
        }
        cr->stroke();
    };

    // ── Map BezierNode::Type → shape index ─────────────────────────────────
    auto shape_for = [](BezierNode::Type t) -> int {
        switch (t) {
            case BezierNode::Type::Symmetric: return 0;
            case BezierNode::Type::Smooth:    return 1;
            case BezierNode::Type::Cusp:      return 2;
            case BezierNode::Type::Corner:    return 3;
        }
        return 3;
    };

    // ── 1. Direction arrow on first segment ──────────────────────────────
    if (segment_count() > 0) {
        CubicSegment seg0 = segment(0);
        Vec2 mid = seg0.at(0.5);
        Vec2 tan = seg0.tangent(0.5).normalised();
        double alen = 6.0 / zoom;  // arrowhead arm length

        double angle = std::atan2(tan.y, tan.x);
        double a1 = angle + M_PI * 0.8;
        double a2 = angle - M_PI * 0.8;

        // Dark halo pass — reads on white/light artboards
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.55);
        cr->set_line_width(lw * 3.5);
        cr->set_line_cap(Cairo::Context::LineCap::ROUND);
        cr->move_to(mid.x + std::cos(a1)*alen, mid.y + std::sin(a1)*alen);
        cr->line_to(mid.x, mid.y);
        cr->line_to(mid.x + std::cos(a2)*alen, mid.y + std::sin(a2)*alen);
        cr->stroke();

        // Amber chevron on top
        cr->set_source_rgba(0.88, 0.75, 0.30, 0.95);
        cr->set_line_width(lw * 1.5);
        cr->move_to(mid.x + std::cos(a1)*alen, mid.y + std::sin(a1)*alen);
        cr->line_to(mid.x, mid.y);
        cr->line_to(mid.x + std::cos(a2)*alen, mid.y + std::sin(a2)*alen);
        cr->stroke();
    }

    // ── 2. Handle lines for selected node ────────────────────────────────
    if (selected_node >= 0 && selected_node < (int)nodes.size()) {
        const BezierNode& n = nodes[selected_node];
        Vec2 anchor{n.x, n.y};
        bool has_prev = (selected_node > 0) || closed;
        bool has_next = (selected_node < (int)nodes.size()-1) || closed;
        if (has_prev) draw_handle(anchor, {n.cx1, n.cy1}, false);
        if (has_next) draw_handle(anchor, {n.cx2, n.cy2}, false);
    }

    // ── 3. Node shapes + origin rings ────────────────────────────────────
    int tail_idx = (!is_closed && (int)nodes.size() > 1) ? (int)nodes.size() - 1 : -1;

    // If tail is coincident with head, suppress origin ring on node 0
    bool tail_on_head = false;
    if (tail_idx >= 0) {
        double d = Vec2{nodes[tail_idx].x - nodes[0].x,
                        nodes[tail_idx].y - nodes[0].y}.length();
        tail_on_head = (d < 1.0 / zoom);
    }

    // Draw all non-tail nodes first
    for (int i = 0; i < (int)nodes.size(); ++i) {
        if (i == tail_idx) continue; // draw tail last
        const BezierNode& n = nodes[i];
        Vec2 p{n.x, n.y};
        int  shape = shape_for(n.type);
        bool sel   = (i == selected_node);

        if (i == 0 && !tail_on_head)
            draw_origin_ring(p, shape);

        draw_node_shape(p, shape, sel, false);
    }

    // Draw tail diamond last so it's always on top
    if (tail_idx >= 0) {
        const BezierNode& t = nodes[tail_idx];
        Vec2 p{t.x, t.y};
        bool sel = (tail_idx == selected_node);
        draw_origin_ring(p, 4); // diamond ring
        draw_node_shape(p, 4, sel, true);
    }

    cr->restore();
}

void BezierPath::draw_editor_overlay(const Cairo::RefPtr<Cairo::Context>& cr,
                                     double zoom,
                                     const std::unordered_set<int>& selected_nodes,
                                     bool is_closed) const {
    if (nodes.empty()) return;

    // Find primary (first selected node) for handle drawing
    int primary = -1;
    for (int i = 0; i < (int)nodes.size(); ++i) {
        if (selected_nodes.count(i)) { primary = i; break; }
    }

    // Use single-int version as base — draws direction arrow, handles for primary,
    // and all node shapes with primary as selected.
    // Then overdraw all other selected nodes in selected style.
    draw_editor_overlay(cr, zoom, primary, is_closed);

    if (selected_nodes.size() <= 1) return;

    cr->save();

    // ── Draw handles for all selected nodes (Illustrator convention) ──────
    const double nr_sel = 3.5 / zoom;
    const double lw     = 1.0 / zoom;
    const double lw_h   = 0.8 / zoom;
    const double hr     = 2.5 / zoom;
    const double NS[3]  = {0.20, 0.50, 1.00};
    const double NB[4]  = {0.00, 0.00, 0.00, 0.55};
    const double HL[4]  = {0.55, 0.68, 1.00, 0.70};
    const double HC[4]  = {0.30, 0.60, 1.00, 0.95};

    for (int i : selected_nodes) {
        if (i == primary) continue; // already drawn by single-int call
        if (i < 0 || i >= (int)nodes.size()) continue;
        const BezierNode& n = nodes[i];
        Vec2 anchor{n.x, n.y};
        bool has_prev = (i > 0) || is_closed;
        bool has_next = (i < (int)nodes.size()-1) || is_closed;

        auto draw_h = [&](Vec2 h) {
            if (h.dist(anchor) < 0.4/zoom) return;
            cr->set_line_width(lw_h);
            cr->set_source_rgba(HL[0], HL[1], HL[2], HL[3]);
            cr->move_to(anchor.x, anchor.y);
            cr->line_to(h.x, h.y);
            cr->stroke();
            cr->set_source_rgba(HC[0], HC[1], HC[2], HC[3]);
            cr->arc(h.x, h.y, hr, 0, 2*M_PI);
            cr->fill();
        };
        if (has_prev) draw_h({n.cx1, n.cy1});
        if (has_next) draw_h({n.cx2, n.cy2});
    }
    for (int i : selected_nodes) {
        if (i == primary) continue;
        if (i < 0 || i >= (int)nodes.size()) continue;
        const BezierNode& n = nodes[i];
        bool is_smooth = (n.type == BezierNode::Type::Smooth);

        // Dark halo
        cr->set_source_rgba(NB[0], NB[1], NB[2], NB[3]);
        if (is_smooth) {
            cr->arc(n.x, n.y, nr_sel + lw * 1.5, 0, 2 * M_PI);
            cr->fill();
        } else {
            cr->save();
            cr->translate(n.x, n.y);
            cr->rotate(M_PI / 4);
            double halo = nr_sel + lw * 1.5;
            cr->rectangle(-halo, -halo, halo * 2, halo * 2);
            cr->restore();
            cr->fill();
        }

        // Blue fill
        cr->set_source_rgb(NS[0], NS[1], NS[2]);
        if (is_smooth) {
            cr->arc(n.x, n.y, nr_sel, 0, 2 * M_PI);
            cr->fill();
        } else {
            cr->save();
            cr->translate(n.x, n.y);
            cr->rotate(M_PI / 4);
            cr->rectangle(-nr_sel, -nr_sel, nr_sel * 2, nr_sel * 2);
            cr->restore();
            cr->fill();
        }

        // Blue stroke
        cr->set_source_rgba(NS[0], NS[1], NS[2], 0.8);
        cr->set_line_width(lw);
        if (is_smooth) {
            cr->arc(n.x, n.y, nr_sel, 0, 2 * M_PI);
            cr->stroke();
        } else {
            cr->save();
            cr->translate(n.x, n.y);
            cr->rotate(M_PI / 4);
            cr->rectangle(-nr_sel, -nr_sel, nr_sel * 2, nr_sel * 2);
            cr->restore();
            cr->stroke();
        }
    }
    cr->restore();
}
// Reverses winding direction. All segment geometry is preserved exactly —
// only direction of traversal flips. cx1↔cx2 swap on every node, then
// the node array is reversed.
void BezierPath::reverse() {
    for (auto& n : nodes) {
        std::swap(n.cx1, n.cx2);
        std::swap(n.cy1, n.cy2);
    }
    std::reverse(nodes.begin(), nodes.end());
    node_sets.clear(); // node order reversed — primitive indices no longer valid
    LOG_DEBUG("BezierPath: reversed ({} nodes)", nodes.size());
}

// ── signed_area ───────────────────────────────────────────────────────────────
// Shoelace formula on anchor points only.
// Doc space is Y-down, so positive result = CW traversal as seen on screen.
double BezierPath::signed_area() const {
    int n = (int)nodes.size();
    if (n < 3) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const auto& a = nodes[i];
        const auto& b = nodes[(i + 1) % n];
        sum += (a.x * b.y) - (b.x * a.y);
    }
    return sum * 0.5;
}

// ── open_at_node ──────────────────────────────────────────────────────────────
// Rotate a closed path so idx becomes node 0, duplicate it as tail, open.
void BezierPath::open_at_node(int idx) {
    int n = (int)nodes.size();
    if (!closed || n < 2 || idx < 0 || idx >= n) return;

    // Rotate so idx is at front
    std::rotate(nodes.begin(), nodes.begin() + idx, nodes.end());

    // Duplicate node 0 as the new tail.
    // Tail's in-handle keeps the arriving curve; out-handle is degenerate.
    BezierNode tail  = nodes[0];
    tail.cx2         = tail.x;
    tail.cy2         = tail.y;
    nodes.push_back(tail);
    closed = false;
    node_sets.clear(); // node order rotated — primitive indices no longer valid
    LOG_DEBUG("BezierPath::open_at_node idx={} → {} nodes open", idx, nodes.size());
}

// ── split_at_node ─────────────────────────────────────────────────────────────
// Split an open path at idx into two PathData halves.
std::pair<PathData, PathData> BezierPath::split_at_node(int idx) const {
    int n = (int)nodes.size();
    if (closed || n < 3 || idx <= 0 || idx >= n - 1)
        return {};

    // Left: nodes [0 .. idx]  —  idx node's out-handle is degenerate (new tail)
    BezierPath left;
    left.closed = false;
    for (int i = 0; i <= idx; ++i)
        left.nodes.push_back(nodes[i]);
    left.nodes.back().cx2 = left.nodes.back().x;
    left.nodes.back().cy2 = left.nodes.back().y;

    // Right: nodes [idx .. n-1]  —  idx node's in-handle is degenerate (new head)
    BezierPath right;
    right.closed = false;
    for (int i = idx; i < n; ++i)
        right.nodes.push_back(nodes[i]);
    right.nodes.front().cx1 = right.nodes.front().x;
    right.nodes.front().cy1 = right.nodes.front().y;

    LOG_DEBUG("BezierPath::split_at_node idx={} → left {} / right {} nodes",
              idx, left.nodes.size(), right.nodes.size());
    return { left.to_path_data(), right.to_path_data() };
}

} // namespace Curvz
