#include "tools/PenTool.hpp"
#include "CurvzLog.hpp"
#include <cmath>
#include <algorithm>

namespace Curvz {

// ── Helpers ───────────────────────────────────────────────────────────────────

bool PenTool::is_near_start(Vec2 pos, double zoom) const {
    if (!has_wip || wip.nodes.empty()) return false;
    return pos.dist({wip.nodes[0].x, wip.nodes[0].y}) * zoom < CLOSE_RADIUS_PX;
}

Vec2 PenTool::incoming_tangent_at_last() const {
    int n = (int)wip.nodes.size();
    if (n < 2) return {1, 0}; // fallback: horizontal
    const BezierNode& prev = wip.nodes[n-2];
    const BezierNode& last = wip.nodes[n-1];
    // Tangent arriving at `last` = direction from prev out-handle → last in-handle → last anchor
    // Approximation: direction from prev anchor's out-handle to last anchor
    Vec2 d{last.x - prev.cx2, last.y - prev.cy2};
    if (d.length_sq() < 1e-10) {
        d = {last.x - prev.x, last.y - prev.y};
    }
    return d.normalised();
}

// Resolve the in-handle of the last node based on modifier state.
// Called continuously during motion so the preview is always live.
void PenTool::resolve_in_handle(int idx, Vec2 in_tangent,
                                 double prev_seg_len, PenModifiers m)
{
    if (idx <= 0 || idx >= (int)wip.nodes.size()) {
        live_in_valid = false;
        return;
    }
    BezierNode& n = wip.nodes[idx];

    if (m.shift) {
        // Cusp — cx1 sits at anchor (zero length), matching on_drag behaviour
        n.cx1 = n.x;
        n.cy1 = n.y;
        n.type = BezierNode::Type::Cusp;
        live_in_mode = InHandleMode::Cusp;

    } else if (m.alt) {
        // Corner — in-handle degenerate (zero length), handles fully free
        n.cx1 = n.x;
        n.cy1 = n.y;
        n.type = BezierNode::Type::Corner;
        live_in_mode = InHandleMode::Degenerate;

    } else {
        // Symmetric — in-handle mirrors out-handle through anchor
        n.cx1 = n.x - (n.cx2 - n.x);
        n.cy1 = n.y - (n.cy2 - n.y);
        n.type = BezierNode::Type::Symmetric;
        live_in_mode = InHandleMode::Symmetric;
    }

    live_in       = {n.cx1, n.cy1};
    live_in_valid = true;

    LOG_TRACE("PenTool: in-handle node {} resolved — mode={} pos=({:.2f},{:.2f})",
              idx, (int)live_in_mode, live_in.x, live_in.y);
}

// ── on_press ─────────────────────────────────────────────────────────────────
bool PenTool::on_press(Vec2 doc_pos, double zoom) {
    doc_pos = snap_pos(doc_pos);

    if (!has_wip) {
        // ── First node ────────────────────────────────────────────────────
        wip     = BezierPath{};
        has_wip = true;
        BezierNode n;
        n.x = n.cx1 = n.cx2 = doc_pos.x;
        n.y = n.cy1 = n.cy2 = doc_pos.y;
        // In-handle degenerate until path closes or user pulls it
        n.type = BezierNode::Type::Smooth;
        wip.nodes.push_back(n);
        drag_node_idx  = 0;
        drag_anchor    = doc_pos;
        state          = State::DraggingHandle;
        live_in_valid  = false;
        live_in_mode   = InHandleMode::Degenerate;
        LOG_DEBUG("PenTool: first node at ({:.2f},{:.2f})", doc_pos.x, doc_pos.y);
        return false;
    }

    // ── Close path ────────────────────────────────────────────────────────
    if (wip.nodes.size() >= 2 && is_near_start(doc_pos, zoom)) {
        // Commit live in-handle of last node
        if (live_in_valid) {
            int last = (int)wip.nodes.size() - 1;
            wip.nodes[last].cx1 = live_in.x;
            wip.nodes[last].cy1 = live_in.y;
        }
        // Set up drag on node 0 so user can pull its in-handle (closing handle)
        // Canvas will set wip.closed and commit after drag_end
        wip.closed     = true;
        drag_node_idx  = 0;
        drag_anchor    = {wip.nodes[0].x, wip.nodes[0].y};
        state          = State::DraggingHandle;
        live_in_valid  = false;
        LOG_INFO("PenTool: closed path ({} nodes)", wip.nodes.size());
        for (int _i = 0; _i < (int)wip.nodes.size(); ++_i) {
            const auto& _n = wip.nodes[_i];
            LOG_INFO("  node[{}] anchor=({:.2f},{:.2f}) cx1=({:.2f},{:.2f}) cx2=({:.2f},{:.2f})",
                     _i, _n.x, _n.y, _n.cx1, _n.cy1, _n.cx2, _n.cy2);
        }
        return true; // signal Canvas to enter m_pen_closing mode
    }

    // ── Subsequent node ───────────────────────────────────────────────────
    // First: commit the live in-handle resolution on the previous node
    int prev_idx = (int)wip.nodes.size() - 1;
    if (live_in_valid) {
        wip.nodes[prev_idx].cx1 = live_in.x;
        wip.nodes[prev_idx].cy1 = live_in.y;
    }

    // Place new node — out-handle starts at anchor, will be pulled by drag
    BezierNode n;
    n.x = n.cx1 = n.cx2 = doc_pos.x;
    n.y = n.cy1 = n.cy2 = doc_pos.y;
    n.type = BezierNode::Type::Smooth;
    wip.nodes.push_back(n);

    drag_node_idx = (int)wip.nodes.size() - 1;
    drag_anchor   = doc_pos;
    state         = State::DraggingHandle;
    live_in_valid = false;

    LOG_DEBUG("PenTool: node {} placed at ({:.2f},{:.2f})",
              drag_node_idx, doc_pos.x, doc_pos.y);
    return false;
}

// ── on_drag ───────────────────────────────────────────────────────────────────
// Called while button is held and mouse moves after on_press.
void PenTool::on_drag(Vec2 doc_pos) {
    if (drag_node_idx < 0 || drag_node_idx >= (int)wip.nodes.size()) return;
    BezierNode& n = wip.nodes[drag_node_idx];

    if (drag_node_idx == 0 && wip.closed) {
        // Closing drag — out-handle tracks drag (controls curve leaving node 0),
        // in-handle mirrors opposite (controls closing segment arriving at node 0)
        n.cx2 = doc_pos.x;
        n.cy2 = doc_pos.y;
        if (!mods.alt) {
            // Symmetric: in-handle mirrors out-handle through anchor
            n.cx1 = n.x - (doc_pos.x - n.x);
            n.cy1 = n.y - (doc_pos.y - n.y);
        }
        live_in       = {n.cx1, n.cy1};
        live_in_valid = true;
        live_in_mode  = InHandleMode::Symmetric;
    } else if (drag_node_idx == 0 && wip.nodes.size() == 1) {
        // First node — out-handle tracks drag, in-handle mirrors
        n.cx2 = doc_pos.x;
        n.cy2 = doc_pos.y;
        n.cx1 = n.x - (doc_pos.x - n.x);
        n.cy1 = n.y - (doc_pos.y - n.y);
        live_in       = {n.cx1, n.cy1};
        live_in_valid = true;
        live_in_mode  = InHandleMode::Symmetric;
    } else {
        // Subsequent node
        n.cx2 = doc_pos.x;
        n.cy2 = doc_pos.y;

        if (mods.shift) {
            // Shift — Cusp: cx1 at anchor, cx2 locks to incoming tangent
            n.cx1 = n.x;
            n.cy1 = n.y;
            Vec2 tang = incoming_tangent_at_last();
            double len = Vec2{doc_pos.x - n.x, doc_pos.y - n.y}.length();
            n.cx2 = n.x + tang.x * len;
            n.cy2 = n.y + tang.y * len;
            n.type = BezierNode::Type::Cusp;

        } else if (mods.alt) {
            // Alt — Corner: cx1 at anchor, cx2 moves freely
            n.cx1 = n.x;
            n.cy1 = n.y;
            n.type = BezierNode::Type::Corner;

        } else {
            // No modifier — Symmetric: mirror cx1 from current cx2
            n.cx1 = n.x - (n.cx2 - n.x);
            n.cy1 = n.y - (n.cy2 - n.y);
            n.type = BezierNode::Type::Symmetric;
        }
    }
}

// ── on_release ────────────────────────────────────────────────────────────────
void PenTool::on_release(Vec2 doc_pos) {
    on_drag(doc_pos); // finalise handle position
    state = State::Placing;
    LOG_DEBUG("PenTool: released — node {} handle set", drag_node_idx);
}

// ── on_motion ─────────────────────────────────────────────────────────────────
// Called every mouse-move with no button held.
// This is where the live in-handle and rubber-band preview are updated.
void PenTool::on_motion(Vec2 doc_pos, PenModifiers new_mods, double zoom) {
    mods        = new_mods;
    mouse_doc   = doc_pos;
    mouse_valid = true;

    if (!has_wip || wip.nodes.empty()) return;

    int last_idx = (int)wip.nodes.size() - 1;

    // Continuously resolve the in-handle of the last placed node
    if (last_idx >= 1) {
        // Incoming tangent at last node — from prev segment
        Vec2 in_tang = incoming_tangent_at_last();

        // Prev segment length — distance from prev anchor to last anchor
        const BezierNode& prev = wip.nodes[last_idx - 1];
        const BezierNode& last = wip.nodes[last_idx];
        double prev_seg_len = Vec2{last.x - prev.x, last.y - prev.y}.length();
        prev_seg_len = std::max(prev_seg_len, 4.0); // minimum sensible length

        resolve_in_handle(last_idx, in_tang, prev_seg_len, mods);
    }
}

// ── finish ────────────────────────────────────────────────────────────────────
std::optional<PathData> PenTool::finish() {
    if (!has_wip || wip.nodes.size() < 2) {
        cancel();
        return std::nullopt;
    }
    // Commit any pending live in-handle
    // During closing drag live_in belongs to node 0 (drag_node_idx==0),
    // otherwise it belongs to the last node.
    if (live_in_valid) {
        int target = (drag_node_idx == 0 && wip.closed) ? 0
                                                        : (int)wip.nodes.size() - 1;
        wip.nodes[target].cx1 = live_in.x;
        wip.nodes[target].cy1 = live_in.y;
    }
    PathData pd = wip.to_path_data();
    cancel();
    LOG_INFO("PenTool: finished open path ({} nodes)", pd.nodes.size());
    for (int _i = 0; _i < (int)pd.nodes.size(); ++_i) {
        const auto& _n = pd.nodes[_i];
        LOG_INFO("  node[{}] anchor=({:.2f},{:.2f}) cx1=({:.2f},{:.2f}) cx2=({:.2f},{:.2f})",
                 _i, _n.x, _n.y, _n.cx1, _n.cy1, _n.cx2, _n.cy2);
    }
    return pd;
}

// ── cancel ────────────────────────────────────────────────────────────────────
void PenTool::cancel() {
    wip           = BezierPath{};
    has_wip       = false;
    state         = State::Idle;
    drag_node_idx = -1;
    mouse_valid   = false;
    live_in_valid = false;
    LOG_DEBUG("PenTool: cancelled");
}

// ── draw_preview ──────────────────────────────────────────────────────────────
void PenTool::draw_preview(const Cairo::RefPtr<Cairo::Context>& cr, double zoom) const {
    if (!has_wip || wip.nodes.empty()) return;

    cr->save();

    // Build a render copy with live handles applied
    BezierPath render_wip = wip;
    bool closing_drag = (drag_node_idx == 0 && wip.closed);

    if (!render_wip.nodes.empty()) {
        if (closing_drag) {
            // During closing drag: apply live_in to node 0 (not last node)
            if (live_in_valid) {
                render_wip.nodes[0].cx1 = live_in.x;
                render_wip.nodes[0].cy1 = live_in.y;
            }
        } else {
            // Normal: apply live_in to last node
            BezierNode& last = render_wip.nodes.back();
            if (live_in_valid) {
                last.cx1 = live_in.x;
                last.cy1 = live_in.y;
            }
        }
    }

    // During closing drag: draw as open path to avoid showing closing segment
    // control point lines. We'll show the closing segment as rubber-band below.
    if (closing_drag) render_wip.closed = false;

    // ── Draw committed segments ────────────────────────────────────────────
    if (render_wip.nodes.size() >= 2) {
        render_wip.apply_to_cairo(cr);
        cr->set_source_rgba(0.88, 0.88, 0.88, 0.9);
        cr->set_line_width(1.5 / zoom);
        cr->stroke();
    }

    // ── Live rubber-band ─────────────────────────────────────────────────
    // During closing drag: show the closing segment (node N-1 → node 0) live
    // Use live_in for node 0's in-handle (the one being dragged)
    if (closing_drag && render_wip.nodes.size() >= 2) {
        const BezierNode& n_last = render_wip.nodes.back();
        const BezierNode& n0     = render_wip.nodes.front();
        Vec2 n0_cx1 = live_in_valid ? live_in : Vec2{n0.cx1, n0.cy1};
        cr->move_to(n_last.x, n_last.y);
        cr->curve_to(n_last.cx2, n_last.cy2, n0_cx1.x, n0_cx1.y, n0.x, n0.y);
        cr->set_source_rgba(0.88, 0.88, 0.88, 0.9);
        cr->set_line_width(1.5 / zoom);
        cr->stroke();
    }

    if (mouse_valid && !closing_drag) {
        const BezierNode& last = wip.nodes.back();

        // Build a preview segment using:
        // p0 = last anchor
        // p1 = last out-handle (cx2)
        // p2 = live in-handle of last node mirrored to mouse side, OR mouse itself
        // p3 = mouse position

        Vec2 p0{last.x,   last.y};
        Vec2 p1{last.cx2, last.cy2};

        // Estimate what the in-handle of the *next* node would be:
        // Mirror of the out-handle from the mouse position
        // (approximate — we don't know the next node's handles yet)
        Vec2 p3 = mouse_doc;

        // p2: use the in-handle of the next node if it were symmetric
        // That's: mouse - (mouse - mouse) = mouse (degenerate) for now
        // Better: use 1/3 of the distance along the incoming tangent as a guess
        Vec2 seg_dir = (p3 - p0).normalised();
        double seg_len = p0.dist(p3);
        Vec2 p2 = p3 - seg_dir * (seg_len * HANDLE_THIRD);

        cr->move_to(p0.x, p0.y);
        cr->curve_to(p1.x, p1.y, p2.x, p2.y, p3.x, p3.y);
        cr->set_source_rgba(0.75, 0.75, 0.75, 0.6);
        cr->set_line_width(1.0 / zoom);
        std::vector<double> dashes = {4.0/zoom, 3.0/zoom};
        cr->set_dash(dashes, 0);
        cr->stroke();
        cr->set_dash(std::vector<double>{}, 0);
    }

    // ── Draw handles for the active drag node only ────────────────────────
    // Always runs — not gated on mouse_valid or closing_drag
    {
        int active_idx = (drag_node_idx >= 0) ? drag_node_idx
                                               : (int)render_wip.nodes.size() - 1;
        if (active_idx >= 0 && active_idx < (int)render_wip.nodes.size()) {
            const BezierNode& an = render_wip.nodes[active_idx];
            Vec2 anchor{an.x, an.y};

            auto draw_h = [&](Vec2 h) {
                if (h.dist(anchor) < 0.5/zoom) return;
                cr->set_source_rgba(0.5, 0.65, 1.0, 0.7);
                cr->set_line_width(0.8/zoom);
                cr->move_to(anchor.x, anchor.y);
                cr->line_to(h.x, h.y);
                cr->stroke();
                double hr = 3.0/zoom;
                cr->save();
                cr->translate(h.x, h.y);
                cr->rotate(M_PI/4);
                cr->rectangle(-hr,-hr,hr*2,hr*2);
                cr->restore();
                cr->set_source_rgba(0.3,0.6,1.0,0.9);
                cr->fill_preserve();
                cr->set_source_rgb(1,1,1);
                cr->set_line_width(0.5/zoom);
                cr->stroke();
            };

            draw_h({an.cx2, an.cy2}); // out-handle
            if (live_in_valid)
                draw_h(live_in);      // in-handle (live resolved)
            else if (active_idx > 0 || (wip.closed && active_idx == 0))
                draw_h({an.cx1, an.cy1}); // committed in-handle
        }
    }

    // ── Node overlay (anchors + live handles) ─────────────────────────────
    // During closing drag: use open path for overlay to avoid phantom handle lines
    // between last node and node 0. Pass -1 so no node shows handles in overlay
    // (we draw node 0 handles explicitly in the rubber-band section above).
    if (drag_node_idx == 0 && wip.closed) {
        BezierPath open_wip = render_wip;
        open_wip.closed = false; // suppress has_next on last node
        open_wip.draw_editor_overlay(cr, zoom, -1, false); // anchors only, no handles
    } else {
        render_wip.draw_editor_overlay(cr, zoom, (int)render_wip.nodes.size() - 1, render_wip.closed);
    }

    // ── Close indicator ───────────────────────────────────────────────────
    if (wip.nodes.size() >= 2 && mouse_valid && is_near_start(mouse_doc, zoom)) {
        const BezierNode& first = wip.nodes[0];
        cr->arc(first.x, first.y, 6.0/zoom, 0, 2*M_PI);
        cr->set_source_rgba(0.2, 0.85, 0.3, 0.9);
        cr->set_line_width(1.5/zoom);
        cr->stroke();
    }

    cr->restore();
}

} // namespace Curvz
