#pragma once
#include "GlyphObject.hpp"
#include "math/BezierPath.hpp"
#include <cairomm/cairomm.h>
#include <optional>

namespace Curvz {

// Modifier state threaded in from GTK events
struct PenModifiers {
    bool shift = false; // cusp — out-handle locks to incoming tangent
    bool alt   = false; // corner — handles fully free, no constraint
    // No modifier = symmetric — mirrors drag
};

// How the previous node's in-handle was resolved
enum class InHandleMode {
    Symmetric,  // mirrors out-handle exactly
    Cusp,       // locks to incoming tangent at 1/3 prev segment length
    Degenerate  // corner or first node — zero length
};

struct PenTool {
    enum class State { Idle, Placing, DraggingHandle };

    State        state    = State::Idle;
    BezierPath   wip;
    bool         has_wip  = false;

    // Current modifier state (updated every motion event)
    PenModifiers mods;

    // Out-handle drag state
    int  drag_node_idx = -1;
    Vec2 drag_anchor{};      // anchor of node being dragged

    // Live preview state — continuously updated in on_motion
    Vec2   mouse_doc{};
    bool   mouse_valid = false;

    // The live-resolved in-handle of the last placed node
    // (recomputed every motion event, committed at next click)
    Vec2   live_in{};        // absolute position of in-handle
    bool   live_in_valid = false;
    InHandleMode live_in_mode = InHandleMode::Degenerate;

    // ── Primary interface ──────────────────────────────────────────────────

    // Place a new node at doc_pos with current mods.
    // Returns true if path should be committed (closed).
    bool on_press(Vec2 doc_pos, double zoom);

    // Pull out-handle of the current node while mouse is held.
    void on_drag(Vec2 doc_pos);

    // Release — finalise out-handle.
    void on_release(Vec2 doc_pos);

    // Mouse moved (no button held) — update live preview, resolve in-handle.
    // zoom needed for close-node detection and length calculations.
    void on_motion(Vec2 doc_pos, PenModifiers mods, double zoom);

    // Commit open path (Enter / tool switch). Returns PathData or nullopt.
    std::optional<PathData> finish();

    // Cancel, discard WIP.
    void cancel();

    // Draw WIP + live preview rubber-band bezier.
    // Render the in-progress path. Caller passes the project's current
    // motif-resolved Creation colour; PenTool stays project-agnostic and
    // uses these values for committed segments, the live rubber-band, and
    // the handle squares. The handle lever lines use a derived lightened
    // shade (blended toward white) so they read as a softer accessory
    // colour against the same family as the path itself.
    void draw_preview(const Cairo::RefPtr<Cairo::Context>& cr, double zoom,
                      double creation_r, double creation_g, double creation_b) const;

    // ── Helpers ───────────────────────────────────────────────────────────
    bool is_near_start(Vec2 pos, double zoom) const;

    // Resolve the in-handle of node `idx` given the incoming tangent and mods.
    // incoming_tangent: direction of curve arriving at idx (from prev segment).
    // mouse_dist: distance from last anchor to current mouse — used for Smooth.
    void resolve_in_handle(int idx, Vec2 incoming_tangent,
                           double prev_seg_len, PenModifiers m);

    // Compute incoming tangent at the last placed node from current WIP geometry.
    Vec2 incoming_tangent_at_last() const;

    static double snap_val(double v) { return std::round(v); }
    static Vec2   snap_pos(Vec2 v)   { return {snap_val(v.x), snap_val(v.y)}; }

    static constexpr double CLOSE_RADIUS_PX   = 8.0;
    static constexpr double HANDLE_THIRD      = 1.0/3.0; // rule of thirds
};

} // namespace Curvz
