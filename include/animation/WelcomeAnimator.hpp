// animation/WelcomeAnimator.hpp ──────────────────────────────────────────────
//
// s288 m2 — OPENS the welcome-demo theatre arc.
//
// The welcome demo banked in HANDOFF.md s287→s288 ("Banked for end-of-Curvz")
// shows Curvz constructing an avatar SVG anchor-by-anchor on first launch,
// then revealing the filled portrait. The previous milestone (s288 m1) shipped
// `Canvas::animate_handle` — a literal mouse-drag impersonation that drove
// real model state. m2 takes a different shape: THEATRE.
//
// The audience sees what looks like the Pen tool drawing a path. The Pen tool
// itself is not active; no real SceneNode exists during the performance.
// Instead, WelcomeAnimator paints PHANTOM shapes on a new Canvas overlay layer
// — anchor squares, handle lever lines, partial Bezier segments — using
// interpolated values from a Glib::Timeout loop. At the end of the
// performance, ONE real Path SceneNode is committed with the final geometry,
// the phantom shapes clear, and the canvas has gained a real artifact.
//
// This is the right shape for several reasons:
//
//   1. The animator is FREE to compose the gesture. It doesn't have to fit
//      through the Pen tool's mouse-event vocabulary. Beat ordering, easing,
//      wobble, brush quality — all live in the animator's value pump.
//   2. Humanization (per-beat variance, handle wobble, occasional pauses)
//      goes in at the value-feeding layer, not by perturbing real input.
//   3. Cosmetic UI flourishes (highlighted toolbar buttons, faux color
//      pickers) can live as overlays without touching real action dispatch.
//   4. The final commit is one model write, one undo entry — no flood from
//      per-frame ticks.
//
// ── Architecture ────────────────────────────────────────────────────────────
//
// One WelcomeAnimator instance lives on Canvas as a member. The performance
// runs in three phases per beat type:
//
//   IDLE       — no performance in flight. Canvas's draw routine skips the
//                welcome-overlay branch entirely.
//
//   PLAYING    — a Glib::Timeout fires at ~60fps. Each tick computes the
//                current t (wall-clock-derived), updates the PHANTOM state
//                (which anchors are visible, where each handle's tip is,
//                how far each segment has grown), and calls queue_draw().
//                Canvas's on_draw sees is_playing()==true and calls
//                draw_overlay() to render the phantoms.
//
//   COMMITTING — at the final tick (t reaches the end of the last beat),
//                the animator mints a real Path SceneNode with the final
//                geometry, adds it to the active layer, clears its phantom
//                state, and returns to IDLE. The canvas redraws once more
//                with the real path visible (in outline only — fill/stroke
//                are Phase-2 work for a future milestone).
//
// ── Beat vocabulary (m2) ────────────────────────────────────────────────────
//
// m2 ships ONE performance kind: PEN PATH. The performance composes from
// per-anchor beats; each anchor in the source path contributes:
//
//   1. anchor-place : the anchor square appears, animated as a small
//                     scale-in (the cursor "lowering"). Duration: ~80ms.
//
//   2. handle-out   : the out-handle lever extends from the anchor to its
//                     target position over a duration. Duration: ~120ms.
//
//   3. segment-grow : the cubic from the previous anchor (or start) to this
//                     anchor materialises along its parametric length t=0
//                     to t=1. Duration: ~250ms (longest beat — the body of
//                     the performance).
//
//   4. (interlude)  : a brief pause before the next anchor's beats begin.
//                     Duration: ~40ms.
//
// Multiple subpaths inside one d-string (multiple `M` commands) pause
// briefly between subpaths (~120ms). The final anchor of a closed path
// gets a "closing segment" beat from the last anchor back to the first.
//
// m2 ships LINEAR easing on all beats. Pluggable easing per beat is m3.
// Per-beat variance (humanization) is m3. Handle wobble is m3.
//
// ── Phantom render vocabulary ───────────────────────────────────────────────
//
// draw_overlay() consumes the current phantom state and emits Cairo
// drawing commands. The vocabulary is intentionally narrow:
//
//   anchor square    — small filled square at a doc-point, in the doc's
//                      creation colour. Same visual reading as a real
//                      Pen-tool anchor.
//   handle line      — line from anchor to handle-tip, with a small
//                      square at the tip. Lever-line treatment matches
//                      the Pen tool's draw_preview (lightened tint).
//   partial segment  — a cubic Bezier from p1 to p2 with control points
//                      c1, c2, rendered from t=0 to t=current (the
//                      growing-segment effect).
//
// Each shape is doc-space; draw_overlay applies the same translate(ox, oy)
// + scale(zoom) transform Canvas's other overlays do.
//
// ── Non-blocking by construction ────────────────────────────────────────────
//
// enact_pen_path() returns IMMEDIATELY. The performance plays out on the
// GTK main loop via the Glib::Timeout; the script that called the verb
// dispatches its next line and finishes. Orchestration of multi-path
// scenes (the eventual welcome demo) composes via a future non-blocking
// wait verb between enact_pen_path calls — `sleep` blocks the main loop
// and would freeze the animation.
//
// ── m2 deliberate non-goals ─────────────────────────────────────────────────
//
//   - SVG ingest. m2 takes an SVG-d string directly; the SVG-walker that
//     reads <path>/<g>/<svg> tags lands m3+.
//   - Multiple paths. m2 plays one path; queueing multiple performances
//     and chaining them is m3+.
//   - Fill / stroke. m2's commit lands an outline-only Path. The Phase-2
//     "color picker theatre" lands m4+.
//   - View-mode toggle. m2 leaves the doc in whatever mode it was in.
//     Outline→Preview cross-fade is m5+.
//   - Fade-delete. m2 leaves the committed Path on the canvas.
//   - Humanization. m2 is mechanical; per-beat variance lands m3.
//   - Pluggable easing. m2 is linear.
//   - Cosmetic UI flourishes (toolbar highlight, faux picker). m4+.
//   - Cancellation mid-performance. m2 runs to completion.

#pragma once

#include "math/BezierPath.hpp"
#include "SceneNode.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Curvz {

class CurvzDocument;
class Canvas;

namespace animation {

// One phantom shape the renderer should draw. Tagged union; the kind
// determines which fields are meaningful. Doc-space coordinates throughout
// — the renderer applies the canvas transform.
struct PhantomShape {
    enum class Kind {
        Anchor,         // single point with optional scale-in fraction
        HandleLine,     // anchor → tip with handle-square at tip
        PartialSegment, // cubic from p1→p2 along control points, fractional t
    };

    Kind   kind = Kind::Anchor;

    // Anchor + handle-line share the anchor position (a, b).
    // PartialSegment uses p1/p2/c1/c2 + t.
    double a_x = 0.0, a_y = 0.0;   // anchor or segment p1
    double b_x = 0.0, b_y = 0.0;   // handle tip OR segment p2
    double c1_x = 0.0, c1_y = 0.0; // segment control 1
    double c2_x = 0.0, c2_y = 0.0; // segment control 2
    double t   = 1.0;              // anchor scale-in OR segment growth fraction
};

class WelcomeAnimator {
public:
    // Non-owning Canvas pointer. The animator is held as a Canvas member,
    // so the pointer is always valid for the animator's lifetime; it's
    // used to call queue_draw() per tick and to find the active document
    // for the final commit.
    explicit WelcomeAnimator(Canvas* canvas);

    // No copies; the animator owns its Glib::Timeout connection and a
    // shared self-keepalive in the closure.
    WelcomeAnimator(const WelcomeAnimator&)            = delete;
    WelcomeAnimator& operator=(const WelcomeAnimator&) = delete;
    WelcomeAnimator(WelcomeAnimator&&)                 = delete;
    WelcomeAnimator& operator=(WelcomeAnimator&&)      = delete;

    // Kick off a Pen-path performance. Parses the SVG-d string into
    // a BezierPath, builds an internal beat list, starts the timeout
    // loop. Returns immediately.
    //
    // Parameters:
    //   d_string  : the path's SVG d attribute (e.g. "M 100 100 C ...")
    //               in USER-SPACE (Y-up, bottom-left origin). The animator
    //               flips to doc-space internally.
    //   speed     : multiplier on beat durations. 1.0 = nominal. 0.5 = double
    //               speed (kinetic). 2.0 = half speed (meditative). Threads
    //               through every timed beat.
    //   doc       : the document the commit will land in. The animator
    //               reads canvas_height for the user→doc Y-flip and stashes
    //               the doc+layer pointers for use at commit time.
    //   layer     : the layer the committed Path SceneNode will be added
    //               to. Caller resolves this (typically doc->active_layer()).
    //
    // No-op early returns (the verb silently fails — gift-shape graceful
    // degradation):
    //   - empty d-string
    //   - null doc or layer
    //   - parsing produces a PathData with fewer than 1 anchor
    //   - another performance is already in flight (queueing is via the
    //     orchestrator path — animate_svg_file — not via re-entry here)
    void enact_pen_path(const std::string& d_string, double speed,
                        CurvzDocument* doc, SceneNode* layer);

    // s288 m3 — orchestrator. Parses an SVG file via Curvz's own SvgParser,
    // walks the resulting doc tree, and enqueues one Pen-path performance
    // for every Path SceneNode found (in document order). Plays the queue
    // back-to-back with a small inter-path "breath" (~200ms × speed).
    //
    // Non-Path SceneNodes in the parsed SVG (Group containers, raw Image
    // nodes, the scott-bug's <circle> background, text nodes) are silently
    // skipped in m3 — the welcome's avatar is path-only by author
    // convention. A future milestone may convert rect/ellipse primitives
    // to PathData and animate them as Pen paths.
    //
    // Fill/stroke from the SVG are captured at parse time and carried
    // through to the committed Path SceneNodes — so by end-of-orchestration
    // the doc has fully-painted paths. Phase-2 work (the cosmetic colour-
    // picker theatre) will later retrofit the painting beats on top of
    // this baseline; for m3 the paths land already painted, just not
    // through the picker theatre.
    //
    // Parameters:
    //   svg_path : absolute filesystem path to an SVG file. Gresource-alias
    //              resolution is a future enhancement; m3 takes literal
    //              filesystem paths only.
    //   speed    : same multiplier as enact_pen_path; threads through all
    //              beats of all queued performances + the inter-path breath.
    //   doc      : commit target. Same role as enact_pen_path.
    //   layer    : commit target layer. Same role.
    //
    // No-op early returns (gift-shape graceful degradation):
    //   - empty svg_path / file doesn't exist / parse fails
    //   - parsed doc has zero Path SceneNodes
    //   - null doc or layer
    //   - another performance is in flight (the queue takes care of
    //     chaining within one orchestrate; concurrent orchestrates
    //     would interleave unpredictably and are refused).
    void animate_svg_file(const std::string& svg_path, double speed,
                          CurvzDocument* doc, SceneNode* layer);

    // Queries used by Canvas's draw routine. is_playing() gates the
    // welcome-overlay branch in on_draw; if false, draw_overlay is not
    // called. shapes() returns the current phantom shape list (read-only
    // snapshot for the renderer).
    bool is_playing() const { return m_playing; }

    // Render the current phantom state. Caller has already done
    // cr->translate(ox, oy); cr->scale(zoom, zoom). Doc-space coords.
    // creation_r/g/b is the doc's current motif-resolved creation colour
    // (same source PenTool uses for its preview render).
    void draw_overlay(const Cairo::RefPtr<Cairo::Context>& cr,
                      double zoom,
                      double creation_r,
                      double creation_g,
                      double creation_b) const;

private:
    // The beat list — built once at enact_pen_path() entry, consumed
    // tick-by-tick by the loop.
    enum class BeatKind {
        AnchorPlace,    // anchor scale-in
        HandleOut,      // out-handle extension
        SegmentGrow,    // partial segment from prev anchor to this
        Interlude,      // silent pause
        SubpathBreak,   // longer pause between subpaths
    };

    struct Beat {
        BeatKind kind = BeatKind::Interlude;
        int      node_idx = -1;  // which BezierNode this beat operates on
                                 // (or destination node for SegmentGrow)
        double   duration_ms = 0.0;
    };

    // Internal: build the beat list from a parsed PathData. Each anchor
    // produces a fixed sequence of beats; multi-subpath paths get a
    // SubpathBreak between them. The final anchor of a closed path
    // gets a closing-segment beat (segment from last node back to first).
    void build_beat_list(const PathData& pd, double speed);

    // Internal: one timeout tick. Computes the current beat and the
    // intra-beat t, builds the phantom shape list, calls queue_draw.
    // Returns true to continue, false to stop (end of last beat).
    bool tick();

    // Internal: at end-of-performance, commit a real Path SceneNode
    // with the cumulative geometry. Clears phantoms. If the orchestrator
    // queued more performances, schedules the next one via a small
    // inter-path breath; otherwise sets m_playing = false.
    void commit();

    // Internal: pop the next queued performance and kick it off. Called
    // from commit() via a deferred Glib::Timeout (the inter-path breath).
    // Returns false if the queue was empty (m_playing flips to false).
    bool start_next_queued();

    // Build the phantom shape list for the current state. Called per
    // tick AND once at commit (to ensure the final frame is the full
    // path before the real SceneNode replaces it). Doc-space.
    void rebuild_phantoms();

    // s288 m3 — pending performance in the queue. One per Path SceneNode
    // found in the parsed SVG. Doc-space PathData (so no flip needed
    // at dequeue time — the orchestrator flipped already). Carries fill
    // and stroke so the commit lands a fully-painted Path.
    struct PendingPerformance {
        PathData    path_data;
        FillStyle   fill;
        StrokeStyle stroke;
        double      speed = 1.0;
    };

    Canvas*  m_canvas = nullptr;

    // ── Performance state ──────────────────────────────────────────────────
    bool m_playing = false;

    // Where the commit lands. Captured at enact_pen_path() entry; cleared
    // at commit() exit. The animator does NOT track doc/layer lifetimes;
    // if the user closed the doc during a multi-second performance the
    // pointers would dangle. m2 is launch-time only and that's not a
    // practical risk.
    CurvzDocument* m_target_doc   = nullptr;
    SceneNode*     m_target_layer = nullptr;

    // The geometry being constructed. Built from the parsed d-string,
    // in doc-space, never mutated during the performance — the animator
    // interpolates VISUAL state on top, the underlying geometry is fixed.
    PathData m_target;

    // s288 m3 — per-current-performance fill and stroke, captured at
    // performance kickoff. Applied to the committed Path SceneNode at
    // commit() time so the avatar lands fully-painted by orchestration
    // end (before any future Phase-2 colour-picker theatre retrofits).
    // Default-constructed when the performance comes from enact_pen_path
    // (the script-facing single-path verb), giving the path the default
    // currentColor fill + zero-width stroke. Set from SVG attributes when
    // the performance comes from the orchestrator's queue.
    FillStyle   m_current_fill;
    StrokeStyle m_current_stroke;

    // s288 m3 — queue of pending performances. Populated by
    // animate_svg_file from the parsed SVG; consumed by commit() via
    // start_next_queued(). Empty at rest.
    std::vector<PendingPerformance> m_queue;

    // s288 m3 — inter-path "breath". After commit() lands a path and
    // before start_next_queued() fires the next, this many ms pass so
    // the audience sees a beat of completed-shape before the next path
    // begins. The handoff design called for "minimal overlap" between
    // paths; this is what implements that. Multiplied by the per-
    // performance speed so kinetic runs have a shorter breath and
    // meditative runs have a longer one.
    double m_inter_path_breath_ms_base = 200.0;

    // The beat list and current position. Linear walk.
    std::vector<Beat> m_beats;
    size_t            m_beat_idx = 0;

    // Per-beat timer. t_beat_start is reset at the moment each beat
    // begins; the per-tick `t` is (now - t_beat_start) / current_beat.duration_ms.
    std::chrono::steady_clock::time_point m_t_beat_start;

    // Per-anchor visibility tracking. By the time we reach beat N for
    // anchor K, anchors 0..K-1 are fully placed (their AnchorPlace and
    // HandleOut beats have completed), and the segment-grow into K is
    // either in flight or completed. We track which anchors are
    // "finished" so the renderer knows what to draw at full opacity vs
    // the current in-flight anchor's scale-in fraction.
    int    m_anchors_finished = 0;     // count of anchors past their
                                       // AnchorPlace+HandleOut beats
    double m_current_anchor_t = 0.0;   // scale-in fraction for the
                                       // anchor currently in its
                                       // AnchorPlace beat (0..1)
    double m_current_handle_t = 0.0;   // extension fraction for the
                                       // out-handle currently in its
                                       // HandleOut beat (0..1)
    double m_current_segment_t = 0.0;  // growth fraction for the
                                       // segment currently in its
                                       // SegmentGrow beat (0..1)
    int    m_current_segment_dest = -1; // destination anchor of the
                                       // in-flight SegmentGrow

    // Rebuilt every tick by rebuild_phantoms; read by draw_overlay.
    std::vector<PhantomShape> m_phantoms;
};

} // namespace animation
} // namespace Curvz
