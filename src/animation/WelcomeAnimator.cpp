// animation/WelcomeAnimator.cpp ──────────────────────────────────────────────
//
// s288 m2 — see animation/WelcomeAnimator.hpp for the arc header and
// architectural rationale. This file implements the value-pump and the
// per-tick phantom-shape construction; the renderer draws what the pump
// produces.

#include "animation/WelcomeAnimator.hpp"

#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"
#include "CurvzProject.hpp"
#include "CurvzLog.hpp"    // s289 m1 — instrumentation for fill/stroke propagation
#include "SvgParser.hpp"   // s288 m3 — parse SVG into temp doc for orchestrator walk
#include "curvz_utils.hpp"

#include <glibmm/main.h>
#include <algorithm>   // s288 m5 — std::min for bbox-fit scale
#include <cmath>
#include <functional>

namespace Curvz {
namespace animation {

namespace {

// Beat durations at speed=1.0 (nominal). The welcome's speed parameter
// is a multiplier on these — at speed=0.5 they halve (kinetic), at
// speed=2.0 they double (meditative). The handoff design rationale
// landed on per-node ~200ms as the sweet spot; these decompose 200ms
// into the AnchorPlace + HandleOut + SegmentGrow + Interlude beats.
constexpr double kAnchorPlaceMs   =  80.0;
constexpr double kHandleOutMs     = 120.0;
constexpr double kSegmentGrowMs   = 250.0;
constexpr double kInterludeMs     =  40.0;
constexpr double kSubpathBreakMs  = 120.0;

// Per-tick interval. 60fps target — close enough to 1/60s that the
// cadence reads smooth. The loop derives t from wall-clock elapsed
// (not tick count) so a stuttery main loop still lands at the right
// fractional position when the next tick fires.
constexpr int kTickIntervalMs = 16;

// Evaluate the cubic Bezier from p1 to p2 with control points c1, c2
// at parameter t. Doc-space. Standard de Casteljau formulation.
inline void cubic_eval(double p1x, double p1y,
                       double c1x, double c1y,
                       double c2x, double c2y,
                       double p2x, double p2y,
                       double t,
                       double& out_x, double& out_y) {
    const double u  = 1.0 - t;
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double t2 = t * t;
    const double t3 = t2 * t;
    out_x = u3*p1x + 3.0*u2*t*c1x + 3.0*u*t2*c2x + t3*p2x;
    out_y = u3*p1y + 3.0*u2*t*c1y + 3.0*u*t2*c2y + t3*p2y;
}

} // anon namespace

WelcomeAnimator::WelcomeAnimator(Canvas* canvas)
    : m_canvas(canvas) {}

void WelcomeAnimator::enact_pen_path(const std::string& d_string,
                                     double speed,
                                     CurvzDocument* doc,
                                     SceneNode* layer) {
    // No-op early returns. Gift-shape graceful degradation: if anything's
    // wrong, the verb dispatches "ok" but nothing happens. The audience
    // is left looking at an empty canvas which is exactly where they were
    // before — no error dialog, no half-broken state.
    if (m_playing) return;          // another performance in flight
    if (d_string.empty()) return;
    if (!m_canvas) return;
    if (!doc) return;
    if (!layer) return;
    if (speed <= 0.0) speed = 1.0;  // defensive

    // Parse the d-string. svg_d_to_path_data is the same pump
    // set_path_data uses (s236 m1), producing user-space PathData.
    PathData parsed = curvz::utils::svg_d_to_path_data(d_string);
    if (parsed.nodes.empty()) return;

    // Flip user-space to doc-space. The parser produces Y-up bottom-
    // left; the model stores Y-down top-left. Same convention
    // set_path_data adopted in s237 m1.
    curvz::utils::path_data_user_to_doc(parsed, (double)doc->canvas_height());

    m_target        = std::move(parsed);
    m_target_doc    = doc;
    m_target_layer  = layer;
    // s288 m3 — script-facing single-path verb uses default fill/stroke.
    // The path lands with CurrentColor fill + zero-width stroke (i.e.
    // outline only in the absence of an explicit picker theatre).
    m_current_fill   = FillStyle{};
    m_current_stroke = StrokeStyle{};

    // Build the beat list. Each anchor's beats land sequentially; multi-
    // subpath paths (detected by walking the path for non-monotonic
    // segment continuity) would need SubpathBreak insertion — m2 treats
    // the entire path as one subpath since svg_d_to_path_data flattens
    // multi-M paths into a single nodes vector without preserving the
    // subpath boundaries. (Recovering subpath info from the parser is a
    // future enhancement; for m2's smoke we feed one-subpath paths only.)
    build_beat_list(m_target, speed);

    if (m_beats.empty()) return;

    // Reset per-performance state.
    m_beat_idx              = 0;
    m_anchors_finished      = 0;
    m_current_anchor_t      = 0.0;
    m_current_handle_t      = 0.0;
    m_current_segment_t     = 0.0;
    m_current_segment_dest  = -1;
    m_phantoms.clear();
    m_t_beat_start          = std::chrono::steady_clock::now();
    m_playing               = true;

    // Spin up the Glib::Timeout. The lambda holds a raw `this` pointer —
    // safe because the animator's lifetime is tied to Canvas, and Canvas
    // outlives any reasonable animation. (If a Canvas teardown ever
    // happens mid-animation, the timeout would dereference invalid
    // memory. m2 doesn't address that; the welcome demo is launch-time
    // only and the user can't close the doc during a 30-second welcome
    // without going out of their way.)
    Glib::signal_timeout().connect(
        [this]() -> bool { return tick(); },
        kTickIntervalMs);
}

// s288 m3 — SVG orchestrator. Parses an SVG file via Curvz's own
// SvgParser, walks the resulting temp doc tree, and enqueues one
// PendingPerformance per Path SceneNode in document order. Kicks off
// the queue runner.
//
// SvgParser returns a CurvzDocument with the SVG's geometry / styles
// already materialised as a SceneNode tree (rects-as-Path, ellipses-
// as-Path, etc. — the parser converts primitives to PathData), with
// transforms already flattened to doc-space coordinates. We just walk
// the resulting tree, extract each Path's PathData + fill + stroke,
// enqueue. The temp doc is dropped at end of this function; the
// committed paths land in our REAL target doc.
//
// Non-Path SceneNodes (Group containers without their own geometry,
// Image, Ref, etc.) are skipped — m3 scope is path-only. The scott-
// bug's <circle> background is silently dropped (parser converts it
// to PathData on a circle SceneNode subtype — m3 walker filters on
// Type::Path strictly to keep the scope tight; future enhancement
// will accept the parser's converted primitives).
void WelcomeAnimator::animate_svg_file(const std::string& svg_path,
                                       double speed,
                                       CurvzDocument* doc,
                                       SceneNode* layer) {
    if (m_playing) return;          // chain owns its own queue
    if (svg_path.empty()) return;
    if (!m_canvas) return;
    if (!doc) return;
    if (!layer) return;
    if (speed <= 0.0) speed = 1.0;

    // Parse the SVG file into a TEMP CurvzDocument. SvgParser does all
    // the work — XML walk, transform flattening, primitive→PathData
    // conversion, fill/stroke attribute parsing. We only consume the
    // parsed geometry; we never insert the temp doc into the project.
    auto temp_doc = parse_svg_file(svg_path);
    if (!temp_doc) return;  // parse failed; gift-shape graceful no-op

    // Walk the parsed tree, collecting every Path-typed SceneNode.
    // SceneNode children form an arbitrary tree (Layer → Group → Path,
    // or Layer → Path, or Layer → ClipGroup → Path), so we recurse.
    // s289 m1 — instrumentation: log every node the walker visits with
    // its type/fill/stroke so we can see WHERE the fill values live in
    // the parsed tree. The handoff's primary suspicion is that the SVG
    // <g fill="..."> attribute attaches to a parent Group SceneNode and
    // doesn't propagate to children. The reality may differ (e.g. fills
    // on path style="" attrs land on the child directly). This log run
    // is read-only — no walker behavior change in m1, just log dumps —
    // so we can confirm the diagnosis before changing walker shape.
    LOG_INFO("WelcomeAnimator: animate_svg_file walk begin path='{}' "
             "temp_doc layers={}", svg_path, temp_doc->layers.size());

    std::vector<PendingPerformance> queued;
    std::function<void(SceneNode*, int)> walk = [&](SceneNode* n, int depth) {
        if (!n) return;
        // Indent by depth for readability. Cap at sane depth.
        std::string indent(std::min(depth, 12) * 2, ' ');
        LOG_INFO("WelcomeAnimator: {}visit type={} name='{}' "
                 "fill.type={} fill=({:.2f},{:.2f},{:.2f},{:.2f}) "
                 "stroke.paint.type={} stroke.w={:.2f} "
                 "children={} has_path={}",
                 indent,
                 (int)n->type, n->name,
                 (int)n->fill.type, n->fill.r, n->fill.g, n->fill.b, n->fill.a,
                 (int)n->stroke.paint.type, n->stroke.width,
                 n->children.size(),
                 (n->path && !n->path->nodes.empty()) ? 1 : 0);

        if (n->type == SceneNode::Type::Path && n->path
            && !n->path->nodes.empty()) {
            PendingPerformance pp;
            pp.path_data = *n->path;  // doc-space already (parser landed it there)
            pp.fill      = n->fill;
            pp.stroke    = n->stroke;
            pp.speed     = speed;
            LOG_INFO("WelcomeAnimator: {}-> queued path nodes={} "
                     "pp.fill.type={} pp.fill=({:.2f},{:.2f},{:.2f}) "
                     "pp.stroke.paint.type={} pp.stroke.w={:.2f}",
                     indent,
                     pp.path_data.nodes.size(),
                     (int)pp.fill.type, pp.fill.r, pp.fill.g, pp.fill.b,
                     (int)pp.stroke.paint.type, pp.stroke.width);
            queued.push_back(std::move(pp));
        }
        for (auto& c : n->children) walk(c.get(), depth + 1);
        // Recurse into composite slots so paths inside ClipGroups /
        // Compounds / etc. are picked up too. Matches walk_doc_tree in
        // ObjectsScriptable.cpp.
        if (n->clip_shape)     walk(n->clip_shape.get(),     depth + 1);
        if (n->blend_source_a) walk(n->blend_source_a.get(), depth + 1);
        if (n->blend_source_b) walk(n->blend_source_b.get(), depth + 1);
        if (n->warp_source)    walk(n->warp_source.get(),    depth + 1);
    };
    for (auto& l : temp_doc->layers) walk(l.get(), 0);

    LOG_INFO("WelcomeAnimator: walk done — queued {} performances", queued.size());

    if (queued.empty()) return;  // SVG had no paths

    // ── Coordinate-space adjustment ─────────────────────────────────────
    // The temp doc and the target doc may have DIFFERENT canvas heights
    // (the SVG's viewBox vs the user's current document). PathData
    // coords from the temp doc are doc-space in the TEMP doc's frame.
    // Without adjustment, the paths would land at the same Y-down
    // coords in the target — fine if heights match, but visually wrong
    // if they don't.
    //
    // Strategy: convert each path's PathData from temp-doc-space to
    // user-space via the temp doc's canvas_height, then convert from
    // user-space to target-doc-space via the target doc's canvas_height.
    // (Equivalent: shift Y by target_h - temp_h. The two-step makes the
    // semantic clear and reuses existing pumps.)
    const double temp_h   = (double)temp_doc->canvas_height();
    const double target_h = (double)doc->canvas_height();
    const double target_w = (double)doc->canvas_width();
    if (temp_h != target_h) {
        for (auto& pp : queued) {
            curvz::utils::path_data_doc_to_user(pp.path_data, temp_h);
            curvz::utils::path_data_user_to_doc(pp.path_data, target_h);
        }
    }

    // ── s288 m5 — bbox-fit scale + center ───────────────────────────────
    //
    // After the Y-flip, the SVG paths sit at their native scale in the
    // target doc's coordinate space. If the SVG was authored at 300×300
    // and the user's doc is 1000×1000, the bug occupies only 30% of the
    // smaller dimension — looks small.
    //
    // The gift framing: the engine has opinions about presentation. We
    // compute the actual geometric bbox across ALL queued paths (not
    // viewBox — actual extents), derive a uniform scale that fits 70%
    // of the target doc's smaller dimension, and center on the canvas.
    // Whitespace around the geometry inside the SVG's viewBox is
    // stripped before scaling; the user always sees the avatar at a
    // generous, comfortable size centered on their canvas, regardless
    // of how the SVG was authored.
    //
    // 70% is the engine's chosen margin — gives the work room to
    // breathe at the edges while being unambiguously the focal point.
    // Not exposed to users.
    {
        double minx =  1e300, miny =  1e300;
        double maxx = -1e300, maxy = -1e300;
        for (const auto& pp : queued) {
            for (const auto& bn : pp.path_data.nodes) {
                // Anchor extent
                if (bn.x < minx) minx = bn.x;
                if (bn.y < miny) miny = bn.y;
                if (bn.x > maxx) maxx = bn.x;
                if (bn.y > maxy) maxy = bn.y;
                // Handle extents (cx1/cy1 and cx2/cy2 can poke outside
                // the anchor bbox). Including them gives a tighter
                // visual fit since the curve geometry uses them.
                if (bn.cx1 < minx) minx = bn.cx1;
                if (bn.cy1 < miny) miny = bn.cy1;
                if (bn.cx1 > maxx) maxx = bn.cx1;
                if (bn.cy1 > maxy) maxy = bn.cy1;
                if (bn.cx2 < minx) minx = bn.cx2;
                if (bn.cy2 < miny) miny = bn.cy2;
                if (bn.cx2 > maxx) maxx = bn.cx2;
                if (bn.cy2 > maxy) maxy = bn.cy2;
            }
        }
        const double bbox_w = maxx - minx;
        const double bbox_h = maxy - miny;
        if (bbox_w > 0.0 && bbox_h > 0.0) {
            constexpr double kFitMargin = 0.70;
            const double scale_w = (target_w * kFitMargin) / bbox_w;
            const double scale_h = (target_h * kFitMargin) / bbox_h;
            const double scale   = std::min(scale_w, scale_h);

            // Translate so bbox center lands at doc center after scaling.
            // For each point p: new_p = (p - bbox_center) * scale + doc_center.
            // Equivalently: new_p = p * scale + (doc_center - bbox_center * scale).
            const double bbox_cx = (minx + maxx) * 0.5;
            const double bbox_cy = (miny + maxy) * 0.5;
            const double doc_cx  = target_w * 0.5;
            const double doc_cy  = target_h * 0.5;
            const double tx = doc_cx - bbox_cx * scale;
            const double ty = doc_cy - bbox_cy * scale;

            auto apply = [&](double& x, double& y) {
                x = x * scale + tx;
                y = y * scale + ty;
            };
            for (auto& pp : queued) {
                for (auto& bn : pp.path_data.nodes) {
                    apply(bn.x,   bn.y);
                    apply(bn.cx1, bn.cy1);
                    apply(bn.cx2, bn.cy2);
                }
            }
        }
    }

    // Stash targets and seed the queue. start_next_queued kicks off the
    // first performance; commit() will chain into subsequent ones.
    m_target_doc   = doc;
    m_target_layer = layer;
    m_queue        = std::move(queued);
    m_playing      = true;
    start_next_queued();
}

void WelcomeAnimator::build_beat_list(const PathData& pd, double speed) {
    m_beats.clear();
    if (pd.nodes.empty()) return;

    const int n = (int)pd.nodes.size();

    // Anchor 0 — the first anchor has no incoming segment; it just
    // appears and extends its out-handle. There's no "growing segment
    // into anchor 0" beat.
    m_beats.push_back({BeatKind::AnchorPlace, 0, kAnchorPlaceMs * speed});
    m_beats.push_back({BeatKind::HandleOut,   0, kHandleOutMs   * speed});
    m_beats.push_back({BeatKind::Interlude,  -1, kInterludeMs   * speed});

    // Anchors 1..n-1 — each gets a segment grow from the previous
    // anchor, then the anchor place, then the out-handle, then interlude.
    for (int i = 1; i < n; ++i) {
        m_beats.push_back({BeatKind::SegmentGrow, i,
                           kSegmentGrowMs * speed});
        m_beats.push_back({BeatKind::AnchorPlace, i,
                           kAnchorPlaceMs * speed});
        m_beats.push_back({BeatKind::HandleOut,   i,
                           kHandleOutMs   * speed});
        m_beats.push_back({BeatKind::Interlude,  -1,
                           kInterludeMs   * speed});
    }

    // Closing segment — if the path is closed, one final SegmentGrow
    // from the last anchor back to anchor 0. The destination index is
    // 0 (where we close TO); the segment is constructed from anchor
    // n-1's out-handle, anchor 0's in-handle, etc.
    if (pd.closed && n >= 2) {
        m_beats.push_back({BeatKind::SegmentGrow, 0,
                           kSegmentGrowMs * speed});
    }
}

bool WelcomeAnimator::tick() {
    if (!m_playing) return false;
    if (m_beat_idx >= m_beats.size()) {
        // Finished — commit and stop.
        commit();
        return false;
    }

    const Beat& beat = m_beats[m_beat_idx];

    const auto now = std::chrono::steady_clock::now();
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - m_t_beat_start)
            .count();

    double t = (beat.duration_ms > 0.0)
                 ? (elapsed_ms / beat.duration_ms)
                 : 1.0;
    bool beat_done = (t >= 1.0);
    if (beat_done) t = 1.0;

    // Update beat-specific fractional state. Each beat type contributes
    // to a different slice of m_current_* variables. Note that beats
    // chain: AnchorPlace completing leaves m_current_anchor_t == 1.0,
    // which the NEXT beat (HandleOut) reads as "the anchor is fully in,
    // and now I'm extending the handle"; HandleOut completing leaves
    // m_current_handle_t == 1.0, and so on.
    switch (beat.kind) {
        case BeatKind::AnchorPlace:
            m_current_anchor_t = t;
            // The handle of the new anchor starts collapsed (t=0) when
            // its AnchorPlace begins; it'll extend in the HandleOut beat
            // that follows. Reset m_current_handle_t so the previous
            // anchor's handle state doesn't leak in.
            m_current_handle_t = 0.0;
            break;

        case BeatKind::HandleOut:
            m_current_handle_t = t;
            break;

        case BeatKind::SegmentGrow:
            m_current_segment_t    = t;
            m_current_segment_dest = beat.node_idx;
            // The destination anchor isn't placed yet during the
            // segment-grow beat — the segment grows TOWARD it; the
            // anchor appears AFTER. m_current_anchor_t is meaningful
            // for the PREVIOUS anchor's state until the AnchorPlace
            // beat for the destination begins.
            break;

        case BeatKind::Interlude:
        case BeatKind::SubpathBreak:
            // Silent pause — phantom state stays as it was.
            break;
    }

    rebuild_phantoms();
    m_canvas->queue_draw();

    if (beat_done) {
        // Beat transition. Promote the per-beat fractional state into
        // the "finished" tallies that subsequent beats read as their
        // initial conditions.
        switch (beat.kind) {
            case BeatKind::HandleOut:
                // An anchor is "finished" once its handle has fully
                // extended. (AnchorPlace doesn't finish an anchor —
                // it just makes the square visible; the handle still
                // has to extend before we count it done.)
                if (beat.node_idx >= m_anchors_finished) {
                    m_anchors_finished = beat.node_idx + 1;
                }
                m_current_handle_t = 1.0;
                break;

            case BeatKind::SegmentGrow:
                // Segment is done; the destination anchor still needs
                // its AnchorPlace beat to make the anchor square visible.
                m_current_segment_t = 1.0;
                // m_current_segment_dest stays set until the next
                // SegmentGrow overwrites it; that's what tells the
                // phantom renderer "draw this segment fully" between
                // segments.
                break;

            default:
                break;
        }

        // Advance to next beat. Reset the beat-start clock so the next
        // tick measures from t=0 of the new beat.
        ++m_beat_idx;
        m_t_beat_start = std::chrono::steady_clock::now();
    }

    return true;
}

void WelcomeAnimator::rebuild_phantoms() {
    m_phantoms.clear();
    if (m_target.nodes.empty()) return;

    const int n = (int)m_target.nodes.size();

    // ── Fully-placed anchors (indices 0..m_anchors_finished-1) ──────────
    // These get full-opacity anchor squares + their handle lines drawn
    // as static (no scale-in, no extension fraction — they're at rest).
    for (int i = 0; i < m_anchors_finished && i < n; ++i) {
        const BezierNode& bn = m_target.nodes[i];
        PhantomShape sq;
        sq.kind = PhantomShape::Kind::Anchor;
        sq.a_x = bn.x;
        sq.a_y = bn.y;
        sq.t   = 1.0;  // fully scaled in
        m_phantoms.push_back(sq);

        // Out-handle, drawn if not collapsed at the anchor (anchor
        // squares and handle squares overlap visually when collapsed;
        // detect zero-length and skip to avoid a half-pixel artefact).
        const double dx = bn.cx2 - bn.x;
        const double dy = bn.cy2 - bn.y;
        if (dx*dx + dy*dy > 1e-6) {
            PhantomShape h;
            h.kind = PhantomShape::Kind::HandleLine;
            h.a_x = bn.x;
            h.a_y = bn.y;
            h.b_x = bn.cx2;
            h.b_y = bn.cy2;
            h.t   = 1.0;
            m_phantoms.push_back(h);
        }
    }

    // ── Segments between fully-placed anchors (no growth fraction) ──────
    for (int i = 1; i < m_anchors_finished && i < n; ++i) {
        const BezierNode& prev = m_target.nodes[i - 1];
        const BezierNode& curr = m_target.nodes[i];
        PhantomShape s;
        s.kind = PhantomShape::Kind::PartialSegment;
        s.a_x = prev.x;   s.a_y = prev.y;
        s.b_x = curr.x;   s.b_y = curr.y;
        s.c1_x = prev.cx2; s.c1_y = prev.cy2;
        s.c2_x = curr.cx1; s.c2_y = curr.cy1;
        s.t   = 1.0;
        m_phantoms.push_back(s);
    }

    // ── In-flight segment (growing along its parametric length) ─────────
    // If a SegmentGrow beat is currently active OR was the most recent
    // completed beat, m_current_segment_dest names the destination anchor
    // and m_current_segment_t names the growth fraction (0..1). The
    // segment starts at m_current_segment_dest - 1 and ends at
    // m_current_segment_dest. Closing segments (dest=0) start at n-1.
    if (m_current_segment_dest >= 0 && m_current_segment_dest < n) {
        int src_idx = (m_current_segment_dest == 0)
                        ? (n - 1)  // closing segment
                        : (m_current_segment_dest - 1);
        // Only draw if the source anchor is among the finished set —
        // otherwise we'd be drawing a segment from nothing.
        if (src_idx >= 0 && src_idx < m_anchors_finished
            && m_current_segment_t < 1.0) {
            const BezierNode& src  = m_target.nodes[src_idx];
            const BezierNode& dest = m_target.nodes[m_current_segment_dest];
            PhantomShape s;
            s.kind = PhantomShape::Kind::PartialSegment;
            s.a_x = src.x;    s.a_y = src.y;
            s.b_x = dest.x;   s.b_y = dest.y;
            s.c1_x = src.cx2; s.c1_y = src.cy2;
            s.c2_x = dest.cx1; s.c2_y = dest.cy1;
            s.t   = m_current_segment_t;
            m_phantoms.push_back(s);
        }
    }

    // ── Anchor currently in its AnchorPlace beat (scale-in) ─────────────
    // The current beat is AnchorPlace if m_current_anchor_t is in (0, 1)
    // OR if the beat's node_idx == m_anchors_finished. We use the beat
    // list as the source of truth.
    if (m_beat_idx < m_beats.size()) {
        const Beat& cur = m_beats[m_beat_idx];
        if (cur.kind == BeatKind::AnchorPlace && cur.node_idx >= 0
            && cur.node_idx < n) {
            const BezierNode& bn = m_target.nodes[cur.node_idx];
            PhantomShape sq;
            sq.kind = PhantomShape::Kind::Anchor;
            sq.a_x = bn.x;
            sq.a_y = bn.y;
            sq.t   = m_current_anchor_t;
            m_phantoms.push_back(sq);
        }
        // ── Out-handle currently being extended ─────────────────────────
        if (cur.kind == BeatKind::HandleOut && cur.node_idx >= 0
            && cur.node_idx < n) {
            const BezierNode& bn = m_target.nodes[cur.node_idx];
            // Anchor was placed by previous beat; draw it at full opacity.
            PhantomShape sq;
            sq.kind = PhantomShape::Kind::Anchor;
            sq.a_x = bn.x;
            sq.a_y = bn.y;
            sq.t   = 1.0;
            m_phantoms.push_back(sq);
            // Handle interpolates from anchor (collapsed) to target.
            const double tip_x = bn.x + (bn.cx2 - bn.x) * m_current_handle_t;
            const double tip_y = bn.y + (bn.cy2 - bn.y) * m_current_handle_t;
            // Skip rendering if collapsed (avoid the same overlap artifact).
            const double dx = tip_x - bn.x;
            const double dy = tip_y - bn.y;
            if (dx*dx + dy*dy > 1e-6) {
                PhantomShape h;
                h.kind = PhantomShape::Kind::HandleLine;
                h.a_x = bn.x;
                h.a_y = bn.y;
                h.b_x = tip_x;
                h.b_y = tip_y;
                h.t   = 1.0;
                m_phantoms.push_back(h);
            }
        }
    }
}

void WelcomeAnimator::commit() {
    // Mint the real Path SceneNode and add it to the active layer. After
    // this, the canvas's normal Path-rendering path takes over — the
    // phantom shapes clear and the next on_draw sees a real outline
    // path in the doc.
    if (!m_target_layer) {
        m_playing = false;
        m_queue.clear();
        return;
    }

    auto obj = std::make_unique<SceneNode>();
    obj->type = SceneNode::Type::Path;
    obj->id = "welcome_path_" + std::to_string(
        (long long)std::chrono::steady_clock::now()
            .time_since_epoch().count());
    obj->internal_id = generate_internal_id();
    obj->path = std::make_unique<PathData>(m_target);
    // s288 m3 — paint with whatever fill/stroke this performance was
    // staged with. For script-facing enact_pen_path the values are
    // defaults (FillStyle CurrentColor, zero-width stroke); for queued
    // SVG-orchestrator paths the values come from the SVG attributes.
    // By orchestration end, the avatar is fully painted (Phase-2 colour-
    // picker theatre will later retrofit animated painting on top of
    // this baseline).
    obj->fill   = m_current_fill;
    obj->stroke = m_current_stroke;
    // s289 m1 — instrumentation: confirm what actually lands on the
    // minted SceneNode. If the values match what start_next_queued
    // logged, the data flow is clean and the visual loss is downstream
    // of the animator (renderer / view-mode / motif resolution). If the
    // values mismatch, the loss is between m_current_* and obj->* —
    // which would be surprising since these are direct assignments.
    LOG_INFO("WelcomeAnimator: commit minted obj id='{}' "
             "obj->fill.type={} fill=({:.2f},{:.2f},{:.2f}) "
             "obj->stroke.paint.type={} stroke.w={:.2f}",
             obj->id,
             (int)obj->fill.type, obj->fill.r, obj->fill.g, obj->fill.b,
             (int)obj->stroke.paint.type, obj->stroke.width);
    obj->visible = true;

    // Cache layer pointer before potentially zeroing it for the chain.
    SceneNode* commit_layer = m_target_layer;
    CurvzDocument* commit_doc = m_target_doc;

    // s288 m5 — insert at children.end() rather than children.begin().
    //
    // Curvz's children-vector convention: children[0] renders LAST (on
    // top); children[size-1] renders FIRST (at bottom). For user-drawn
    // objects, that maps "newest goes on top" to "insert at begin".
    //
    // For SVG paint order, the convention reverses: the SVG file lists
    // paths in paint order (first listed paints first, lands at bottom;
    // last listed paints last, lands at top). The orchestrator walks
    // the SVG in document order and commits in that same order — so
    // the FIRST committed path should be at the BOTTOM of the children
    // stack (highest index), and each subsequent path layers on top.
    //
    // children.end() insertion produces exactly that: first commit at
    // index 0 (last in the eventual draw iteration, on top, currently
    // alone), second commit appended at index 1 (drawn before index 0,
    // i.e. below it as more land)... resulting in the first SVG path
    // at the bottom and the last on top. SVG paint order preserved.
    //
    // This applies to BOTH single-path enact_pen_path commits and
    // orchestrator-driven commits — using consistent end-insertion
    // means a script that calls enact_pen_path twice would see the
    // second path land below the first, which arguably matches an
    // intuition of "the next thing I draw goes after the previous"
    // for a temporal-sequence drawing tool. For the welcome demo's
    // gift framing this is the right semantic; for general user
    // drawing this is a subtle change from the Pen tool's convention,
    // but the welcome animator isn't on that workflow.
    commit_layer->children.insert(commit_layer->children.end(),
                                  std::move(obj));

    // Invalidate the iid index so the just-minted Path's internal_id
    // resolves on the next find_by_iid call.
    if (commit_doc) commit_doc->invalidate_iid_index();

    // Clear per-performance phantom state — next on_draw sees the real
    // path, not the phantoms.
    m_phantoms.clear();

    if (m_canvas) m_canvas->queue_draw();

    // ── Chain into the next queued performance, if any ──────────────────
    if (!m_queue.empty()) {
        // Schedule the next performance after an inter-path breath. The
        // doc/layer pointers stay set; only m_target / m_beats / per-
        // performance fractional state reset when start_next_queued
        // fires. m_playing stays true throughout the chain.
        const double breath_ms = m_inter_path_breath_ms_base
                               * (m_queue.front().speed > 0.0
                                    ? m_queue.front().speed : 1.0);
        Glib::signal_timeout().connect_once(
            [this]() { start_next_queued(); },
            (int)breath_ms);
        return;
    }

    // ── s288 m5 — Phase 3 reveal ────────────────────────────────────────
    //
    // Queue is empty: the construction phase is complete. The doc has
    // all the SVG's paths committed with their fill/stroke attributes,
    // but the canvas is presumably in outline mode (where the user has
    // been watching the paths construct, since fills don't render in
    // outline). The reveal turns preview mode on, the fills appear,
    // and the audience sees the painted artwork emerge.
    //
    // Sequencing:
    //   1. Hold the outlined composite visible for kRevealConstructionHoldMs
    //      so the audience reads "all the paths are done" before anything
    //      else happens.
    //   2. Toggle view-mode Outline→Preview (only if currently in outline;
    //      if user is already in preview, the toggle is a no-op).
    //   3. Hold the painted reveal for kRevealPaintedHoldMs so the gift
    //      lands. This is the moment the welcome ceremony exists for.
    //   4. Clear target pointers and flip m_playing = false.
    //
    // Fade-delete is m6 work — for now the painted bug stays on the
    // canvas after the reveal so the user can interact with it.
    //
    // The hold durations are wall-clock, not scaled by speed — the
    // reveal moments are perceptual landmarks, not part of the
    // construction tempo. (A future enhancement could scale them
    // optionally; for m5 they're constant.)
    constexpr int kRevealConstructionHoldMs = 500;
    constexpr int kRevealPaintedHoldMs      = 1500;

    Glib::signal_timeout().connect_once(
        [this]() {
            // Step 2: toggle view-mode if currently in outline. If the
            // user happened to be in preview already, this branch is
            // skipped — the audience already sees fills, no toggle
            // needed and a toggle would put them in outline which
            // would be wrong.
            if (m_canvas && m_canvas->is_outline_mode()) {
                m_canvas->toggle_outline_mode();
            }
            // Step 3 → 4: hold the painted reveal, then clear state.
            Glib::signal_timeout().connect_once(
                [this]() {
                    m_target_doc   = nullptr;
                    m_target_layer = nullptr;
                    m_playing      = false;
                    if (m_canvas) m_canvas->queue_draw();
                },
                kRevealPaintedHoldMs);
        },
        kRevealConstructionHoldMs);

    // Note we do NOT clear m_target_doc / m_target_layer or flip
    // m_playing here — the deferred chain owns that. Until the second
    // timeout fires, is_playing() returns true so the welcome-overlay
    // branch in Canvas::on_draw stays active (cosmetically harmless —
    // m_phantoms is empty at this point so draw_overlay does nothing
    // — but conceptually the welcome ceremony isn't done yet).
    return;
}

// s288 m3 — pop the head of the queue and kick off its performance.
// Called from commit() via a deferred timeout (the inter-path breath).
// Sets up per-performance state (target geometry, fill/stroke, beat
// list, beat clock) and resumes the tick loop.
bool WelcomeAnimator::start_next_queued() {
    if (m_queue.empty()) {
        m_playing = false;
        return false;
    }

    PendingPerformance pp = std::move(m_queue.front());
    m_queue.erase(m_queue.begin());

    m_target          = std::move(pp.path_data);
    m_current_fill    = std::move(pp.fill);
    m_current_stroke  = std::move(pp.stroke);

    // s289 m1 — instrumentation: confirm fill/stroke survived the queue.
    LOG_INFO("WelcomeAnimator: start_next_queued nodes={} "
             "m_current_fill.type={} fill=({:.2f},{:.2f},{:.2f}) "
             "m_current_stroke.paint.type={} stroke.w={:.2f} remaining={}",
             m_target.nodes.size(),
             (int)m_current_fill.type,
             m_current_fill.r, m_current_fill.g, m_current_fill.b,
             (int)m_current_stroke.paint.type, m_current_stroke.width,
             m_queue.size());

    build_beat_list(m_target, pp.speed);

    if (m_beats.empty()) {
        // Degenerate: empty path or single-node path with no beats.
        // Commit immediately (will land an empty/single-node Path) and
        // continue chaining if more queued.
        commit();
        return false;
    }

    // Reset per-performance fractional state.
    m_beat_idx              = 0;
    m_anchors_finished      = 0;
    m_current_anchor_t      = 0.0;
    m_current_handle_t      = 0.0;
    m_current_segment_t     = 0.0;
    m_current_segment_dest  = -1;
    m_phantoms.clear();
    m_t_beat_start          = std::chrono::steady_clock::now();
    // m_playing stays true (chain).

    Glib::signal_timeout().connect(
        [this]() -> bool { return tick(); },
        kTickIntervalMs);
    return true;
}

void WelcomeAnimator::draw_overlay(
    const Cairo::RefPtr<Cairo::Context>& cr,
    double zoom,
    double creation_r,
    double creation_g,
    double creation_b) const {

    // Caller has already done translate(ox, oy) + scale(zoom). All our
    // coords are doc-space.

    if (m_phantoms.empty()) return;

    // Lever-line tint — lightened version of the creation colour. Same
    // recipe PenTool::draw_preview uses (blend toward white at ~0.5).
    const double lever_r = creation_r + (1.0 - creation_r) * 0.5;
    const double lever_g = creation_g + (1.0 - creation_g) * 0.5;
    const double lever_b = creation_b + (1.0 - creation_b) * 0.5;

    // Anchor / handle-tip square half-size in screen pixels. We divide
    // by zoom because the transform scales line widths and shape sizes
    // — the visual size on screen stays constant as the user zooms.
    const double sq_half = 3.0 / zoom;
    const double lw      = 1.0 / zoom;

    for (const auto& s : m_phantoms) {
        switch (s.kind) {
            case PhantomShape::Kind::Anchor: {
                // Scale-in: at t<1, the square is smaller. Lerp from 0
                // to sq_half — at t=0 the square is invisible, at t=1
                // it's the full anchor square size.
                const double half = sq_half * s.t;
                if (half <= 0.0) break;
                cr->rectangle(s.a_x - half, s.a_y - half,
                              half * 2.0, half * 2.0);
                cr->set_source_rgba(creation_r, creation_g, creation_b, 1.0);
                cr->fill();
                break;
            }

            case PhantomShape::Kind::HandleLine: {
                // Lever line: thin, lightened tint. Plus a small square
                // at the tip in the full creation colour.
                cr->set_source_rgba(lever_r, lever_g, lever_b, 0.9);
                cr->set_line_width(lw);
                cr->move_to(s.a_x, s.a_y);
                cr->line_to(s.b_x, s.b_y);
                cr->stroke();

                // Handle square at tip
                cr->rectangle(s.b_x - sq_half, s.b_y - sq_half,
                              sq_half * 2.0, sq_half * 2.0);
                cr->set_source_rgba(creation_r, creation_g, creation_b, 1.0);
                cr->fill();
                break;
            }

            case PhantomShape::Kind::PartialSegment: {
                // Draw the cubic from a→b with control points c1/c2,
                // truncated to t. We sample the curve at fine intervals
                // and stroke a polyline; Cairo doesn't expose a "draw
                // partial cubic to parameter t" primitive.
                cr->set_source_rgba(creation_r, creation_g, creation_b, 1.0);
                cr->set_line_width(2.0 / zoom);
                cr->move_to(s.a_x, s.a_y);
                constexpr int kSamples = 32;
                const double t_end = s.t;
                for (int i = 1; i <= kSamples; ++i) {
                    const double ti = t_end * (double)i / (double)kSamples;
                    double px, py;
                    cubic_eval(s.a_x, s.a_y, s.c1_x, s.c1_y,
                               s.c2_x, s.c2_y, s.b_x, s.b_y,
                               ti, px, py);
                    cr->line_to(px, py);
                }
                cr->stroke();
                break;
            }
        }
    }
}

} // namespace animation
} // namespace Curvz
