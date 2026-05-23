#pragma once
#include "CommandHistory.hpp"
#include "CurvzDocument.hpp"
#include "ImageInfo.hpp"  // s210 m1 — payload struct for signal_request_image_info
#include "SceneNode.hpp"
#include "SelectionContext.hpp"  // s158 m1 — capability classifier
#include "Toolbar.hpp"
#include "color/Paint.hpp"  // SwatchId alias — pure header, no nlohmann/sigc/gtk
#include "math/BezierPath.hpp"
#include "math/BooleanOps.hpp"
#include "math/BooleanOpsClipper.hpp"
#include "math/BooleanOpsRefit.hpp"  // s139 m2 — keeper-set + cleanup_loop post-pass
#include "math/CornerTreatment.hpp"
#include "math/PathOffset.hpp"
#include "tools/PenTool.hpp"
#include "animation/SvgPerformer.hpp"  // s291 m2 — SvgEmitter consumer that
                                       // drives beat construction during an
                                       // SVG parse (was WelcomeAnimator)

// Forward-declare the rest of the swatch-library types we reference in the
// public interface. Keeping SwatchLibrary.hpp out of this header avoids
// pulling nlohmann/json + sigc into every TU that includes Canvas.hpp,
// which is many of them. The implementation in Canvas.cpp includes the
// full header.
//
// enum class PaintSlot must be forward-declared with its underlying type
// named, matching its definition in SwatchLibrary.hpp.
namespace Curvz {
namespace color {
class SwatchLibrary;
enum class PaintSlot : int;
}
namespace style {
class StyleLibrary;
}
}

// s208 m5 — forward-declare substrate Entry so Canvas can hold a pointer
// to it without pulling ScriptableWidget into every TU that includes
// Canvas.hpp. Full include lives in Canvas_ops.cpp (construction) and
// Canvas_input.cpp (deref-heavy text-editing flow).
namespace curvz::widgets { class Entry; }

#include <cairomm/cairomm.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <gtkmm/drawingarea.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/fixed.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/gesturezoom.h>
#include <optional>
#include <pango/pangocairo.h>
#include <pangomm/fontdescription.h>
#include <pangomm/layout.h>
#include <utility>

namespace Curvz {

// Forward decl — Canvas reads project-wide workspace appearance via
// pointer (s116 m6). Avoids pulling CurvzProject.hpp here (which would
// create a transitive include cycle through SwatchLibrary/StyleLibrary
// templates).
struct CurvzProject;

// ─────────────────────────────────────────────────────────────────────────────
// Canvas implementation is split across four TUs (s161). The split is
// purely structural — same class, same surface. Routing convention:
//
//   src/Canvas.cpp          (CORE)  — lifecycle, transforms, selection
//                                     primitives, snapping, zoom,
//                                     copy/cut/paste/delete
//   src/Canvas_input.cpp    (INPUT) — input event state machines
//                                     (on_*_begin/update/end), key dispatchers,
//                                     place_*, tool selection
//   src/Canvas_ops.cpp      (OPS)   — boolean, blend, warp, group, align,
//                                     arrange, transforms-by, fill/stroke
//                                     apply, import
//   src/Canvas_draw.cpp     (DRAW)  — all draw_*, on_draw, render_shadow_under,
//                                     fill/stroke render helpers
//   include/Canvas_internal.hpp     — cross-TU helpers + file-scope statics
//                                     (NOT part of public surface)
//
// Inline member functions defined in this header live with their declarations
// — they are not in any .cpp TU.
// ─────────────────────────────────────────────────────────────────────────────
class Canvas : public Gtk::DrawingArea {
public:
  Canvas();

  void set_document(CurvzDocument *doc);
  void set_zoom(double zoom);
  void set_active_tool(ActiveTool tool);
  ActiveTool active_tool() const { return m_tool; }
  void ruler_place_measurement();
  void ruler_clear(); // Space key — reset for next measurement
  void set_history(CommandHistory *history);
  // Public access to the history stack for callers that need to push
  // commands directly (e.g. MainWindow::on_warp_make orchestrates a
  // scratch-Warp tree mutation and then pushes MakeWarpCommand
  // post-hoc). Returns nullptr if no history was attached.
  CommandHistory *history() { return m_history; }

  // Swatch library — non-owning pointer wired at project load time.
  // Phase 5 M3 introduced: apply_swatch_to_selection routes through
  // library->set_paint() to maintain the reverse usage index. Safe to
  // leave null during early boot (no-ops gracefully).
  void set_swatch_library(color::SwatchLibrary *lib);
  color::SwatchLibrary *swatch_library() { return m_swatch_library; }

  // Style library — non-owning pointer wired at project load time
  // (S78 m3d). Symmetric to set_swatch_library above. The wiring
  // exists so Canvas can subscribe to signal_style_changed and drive
  // the cross-doc-propagation walk: on every style edit, every bound
  // SceneNode in m_doc gets re-materialised from the library and the
  // canvas is queue_draw()'d. Without this, an edit to a user style
  // would only become visible on close-and-reopen.
  //
  // Disconnects the previous library's signal on swap; safe with null
  // (early boot) and across project switches (MainWindow calls this
  // both at boot and from update_all_panels — same lifecycle as
  // set_swatch_library). The Canvas does NOT take ownership.
  void set_style_library(style::StyleLibrary *lib);
  style::StyleLibrary *style_library() { return m_style_library; }

  // ── Project-wide workspace appearance (s116 m6) ─────────────────────
  // Canvas paints the artboard surface and the workspace surround using
  // the project-level fields (motif/artboard_bg/workspace_bg). MainWindow
  // calls this from update_all_panels and on_doc_activated so any path
  // that swaps the active project repoints us. Non-owning. May be null
  // during early boot — paint code falls back to CurvzDocument defaults
  // (matches pre-m6 visuals exactly).
  void set_project(CurvzProject *project) { m_project = project; }
  CurvzProject *project() { return m_project; }

  // s183 m5a — the motif Canvas draws for. Reads m_project->motif
  // when wired; falls back to Motif::Dark when m_project is null
  // (early-boot — preserves pre-s183 default visual which was the
  // dark-mode triple). Every doc colour read in Canvas paint uses
  // this to pick the matching pair.
  Motif doc_motif() const;

  // Public id-minting helper — wraps the file-local next_id() /
  // last_iid() statics so callers outside Canvas.cpp (MainWindow's
  // scratch-Warp build, etc.) can produce fresh SVG ids + internal-
  // ids that don't collide with other mints in the same session.
  //   mint_id(): returns a fresh string id and stores the matching
  //     internal id, which the caller reads via last_minted_iid().
  //   last_minted_iid(): returns the internal id paired with the
  //     most recent mint_id() call.
  std::string mint_id();
  std::string last_minted_iid();

  // Public bbox query. Wraps the private object_bbox so non-Canvas
  // callers (MainWindow::on_warp_make needs the glyph bbox to scale
  // preset envelopes for new Warps) can ask "what's this node's
  // axis-aligned extent?" without duplicating the bbox walker. x/y/
  // w/h written through; returns false if the node has no meaningful
  // bbox (e.g. a Ref point). include_stroke defaults to true to match
  // the internal default.
  bool object_bbox_query(const SceneNode &obj,
                         double &x, double &y, double &w, double &h,
                         bool include_stroke = true) const;

  // ── Polygon tool ──────────────────────────────────────────────────────
  void set_polygon_settings(int sides, double inflection) {
    m_poly_sides = sides;
    m_poly_inflection = inflection;
  }
  void set_spiral_settings(double turns, double inner_pct) {
    m_spiral_turns = turns;
    m_spiral_inner = inner_pct;
  }
  void place_polygon_precise(double cx, double cy, double radius, int sides,
                             double inflection, double angle_rad);
  void place_spiral_precise(double cx, double cy, double outer_r,
                            double inner_r, double turns,
                            double angle_rad);

  // Called by MainWindow when the ContextBar defaults well changes.
  // Applied to every new object created from this point forward.
  void set_default_style(FillStyle fill, StrokeStyle stroke) {
    m_def_fill = std::move(fill);
    m_def_stroke = std::move(stroke);
  }
  double zoom() const { return m_zoom; }
  double fit_zoom_value() const { return fit_zoom(); }
  double pan_x() const { return m_pan_x; }
  double pan_y() const { return m_pan_y; }
  double cursor_doc_x() const { return m_cursor_doc_x; }
  double cursor_doc_y() const { return m_cursor_doc_y; }
  SceneNode *selected_object() const { return m_selected; }
  int selected_node() const { return m_selected_node; }
  void select_object(SceneNode *obj); // called from LayersPanel
  void set_multi_selection(const std::vector<SceneNode *> &sel);
  void group_selection();
  void ungroup_selection();
  void make_compound_path();
  void split_compound_path();

  // ── Clipping paths ──────────────────────────────────────────────────
  // make_clip_group(): requires a non-empty selection. Arms "pick-clip"
  //   mode — the next click on a Path or Compound in the canvas will
  //   be consumed as the clip shape; the current selection becomes the
  //   ClipGroup's children. Clicking anything else cancels the arm.
  // release_clip_group(): requires the primary selection to be a
  //   ClipGroup. Dissolves it: clip_shape and children return as normal
  //   siblings in the parent. Selection becomes the released children.
  // is_clip_pick_armed(): status flag for UI/status bar.
  void make_clip_group();
  void release_clip_group();
  bool is_clip_pick_armed() const { return m_clip_pick_armed; }

  // ── Blend ─────────────────────────────────────────────────────────────
  // make_blend(): requires exactly 2 Path nodes in the current selection
  //   sharing a common parent. In M1 also requires equal node counts and
  //   equal closed flag; rejects with LOG_WARN and status message otherwise.
  //   Removes the two originals and inserts a Blend container in their
  //   place at the lower index. Atomic undo via MakeBlendCommand.
  // ── Blend ─────────────────────────────────────────────────────────────
  // make_blend(): requires exactly 2 Path nodes in the current selection
  //   sharing a common parent. In M1 also requires equal node counts and
  //   equal closed flag; rejects with LOG_WARN and status message otherwise.
  //   Removes the two originals and inserts a Blend container in their
  //   place at the lower index. Atomic undo via MakeBlendCommand.
  //   Parameters (all defaulted so the pre-M3 call site keeps working):
  //     steps:            number of intermediates (1..50).
  //     reverse:          if true, A and B are swapped before building
  //                       (the dialog lets the user flip direction).
  //     stroke_w_override:if true, blend step widths follow
  //                       [stroke_w_start, stroke_w_end] in doc units
  //                       instead of lerping A.stroke.width → B.stroke.width.
  void make_blend(int steps = 4, bool reverse = false,
                  bool stroke_w_override = false,
                  double stroke_w_start = 0.0,
                  double stroke_w_end = 0.0);
  // release_blend(): requires the primary selection to be a Blend. Dissolves:
  //   A, Group("Steps") containing cached intermediates, B become siblings
  //   in the Blend's parent (in ascending z-order — A bottom, B top).
  //   The Blend node is removed. Atomic undo.
  void release_blend();
  // find_blend_owner(target): if `target` is blend_source_a or blend_source_b
  //   of some Blend node in the document, returns that Blend. Used by
  //   delete_selected to route deletion of A/B through dissolve.
  SceneNode *find_blend_owner(SceneNode *target);
  // mark_all_blends_dirty(): walks the doc tree and sets blend_cache_dirty
  //   on every Blend. Called from MainWindow after prop_changed signals so
  //   downstream edits to A/B (colour, stroke width, opacity, path nodes)
  //   trigger a cache rebuild on next draw. Coarse — marks every Blend
  //   regardless of whether its A/B actually changed — but cheap: one bool
  //   per Blend, zero cost when no Blends exist.
  void mark_all_blends_dirty();

  // equalize_blend_sources(a, b): insert nodes into whichever of a/b has
  //   fewer nodes so both end up with the same count. Uses greedy
  //   longest-segment-midpoint-split: repeatedly find the longest remaining
  //   segment (by arc length) and split it at t=0.5 via insert_node_at,
  //   which preserves the curve shape exactly (De Casteljau subdivision).
  //   Precondition: both non-null, both Path, both path->closed matches.
  //   Returns true if equalization was performed (or already equal).
  //   Atomic undo via EditPathCommand on whichever side was mutated.
  bool equalize_blend_sources(SceneNode *a, SceneNode *b);

  // rebuild_blend(b): force a Blend to regenerate its intermediate cache
  //   on the next draw. Called from the right-click context menu (canvas
  //   and Layers panel) as a user-facing manual refresh — covers the
  //   case where some combination of signals doesn't fire a doc_changed
  //   or prop_changed and the cache silently goes stale. Cheap: just
  //   sets the dirty flag and requests a redraw.
  void rebuild_blend(SceneNode *b);

  // move_top_level_selection_to_layer(dst_li): move every top-level
  //   member of the current canvas selection to layer index `dst_li`,
  //   pushing one MoveObjectToLayerCommand per moved object. Mirrors
  //   the LayersPanel version (s274 m11) — same command shape, same
  //   command granularity, same hidden-destination-clears-selection
  //   behaviour. Distinguished by selection source: this one acts on
  //   `m_selection`; the panel's acts on `m_canvas_selection`.
  //
  //   Caller is the canvas right-click "Move to layer ▸ <name>" menu
  //   item (s275 m12). Gating belongs to the menu — by the time this
  //   runs every selection member is expected to be top-level and
  //   the destination is expected to be an ordinary unlocked layer.
  //   Re-checks both inside for belt-and-braces. No-ops cleanly on a
  //   bad index or empty selection.
  void move_top_level_selection_to_layer(int dst_layer_idx);

  // ── Warp (envelope distortion) — M1 stub API ─────────────────────────
  // find_warp_owner(target): if `target` is warp_source (or lives
  //   inside warp_source's subtree) of some Warp node in the document,
  //   returns that Warp. Used by delete_selected to route deletion of
  //   the source through release_warp (mirrors ClipGroup/Blend owner
  //   semantics: deleting the payload dissolves the wrapper).
  SceneNode *find_warp_owner(SceneNode *target);
  // mark_all_warps_dirty(): walks the doc tree and sets both
  //   warp_glyph_cache_dirty and warp_cache_dirty on every Warp. Called
  //   from the prop_changed signal pathway so that downstream inspector
  //   edits to the source invalidate cached outlines/geometry on next
  //   draw. Coarse but cheap — one bool flip per Warp, no-op when no
  //   Warps exist.
  void mark_all_warps_dirty();

  // rebuild_warp_caches(w): force a Warp to regenerate glyph_cache and
  //   warp_cache now. Both dirty flags are checked — if only warp_cache
  //   is dirty (envelope changed but source hasn't), only the final
  //   pass runs. Called from draw_object's Warp branch (lazy-on-read)
  //   and from user-facing refresh actions when they land. Safe to
  //   call on a Warp with null source: clears caches and logs a warn.
  void rebuild_warp_caches(SceneNode *w);

  // warp_source_bbox(): canonical "what bbox does the warp interpret
  //   the envelope against." This is the SAME rectangle that
  //   rebuild_warp_caches feeds into warp_subtree as the source
  //   reference space — derived from walking the actual path data in
  //   warp_glyph_cache (NOT from object_bbox, which can include
  //   stroke/recipe metadata and produce a slightly different rect).
  //
  //   Inspector preset-pick uses this so the envelope it generates
  //   is sized to the same rectangle warp_subtree will remap against;
  //   any mismatch produces a warp that "looks like nothing happened"
  //   because envelope and source disagree on coordinate frame.
  //
  //   Side-effect: ensures warp_glyph_cache is current (rebuilds if
  //   dirty or missing). The cache is the only place the source bbox
  //   can be measured the same way warp_subtree will measure it, so
  //   currency is required for a correct answer.
  //
  //   Returns false if w is null/non-Warp, has null source, or if the
  //   glyph_cache contains no path data (degenerate). Callers should
  //   bail without disturbing envelope on false.
  bool warp_source_bbox(SceneNode &w, double &bx, double &by, double &bw,
                        double &bh);

  // make_warp(): create a Warp container around the single selected
  //   Path / Compound / Group. The selected node is cloned into the
  //   Warp's warp_source slot and replaced in its parent's children
  //   by the Warp. Envelope starts empty; rebuild_warp_caches seeds
  //   a default identity envelope on first draw (M3a has no dialog
  //   yet — the dialog lands in M3b). Undoable via MakeWarpCommand.
  //   On success, selection becomes the new Warp.
  void make_warp();
  // release_warp(): requires the primary selection to be a Warp.
  //   Replaces the Warp with a clone of its warp_source at the same
  //   position. Envelope data is discarded (use Flatten if the baked
  //   geometry matters). Atomic undo via ReleaseWarpCommand.
  void release_warp();
  // flatten_warp(): requires the primary selection to be a Warp.
  //   Forces a fresh rebuild_warp_caches, then replaces the Warp with
  //   a clone of its warp_cache. Envelope data is discarded; the
  //   resulting node is plain (editable as normal paths). Undoable
  //   via FlattenWarpCommand.
  void flatten_warp();

  // Boolean operations — requires exactly 2 paths selected.
  void boolean_op(BooleanOpType op);
  void offset_path_op(double distance, OffsetSide side, bool keep_original);
  void expand_stroke_op();
  void text_to_paths_op();       // convert selected Text nodes to Path outlines
  void release_text_from_path(); // detach PTT text nodes in current selection
                                 // (undoable)
  // s162 m3: set_selection_single now folds in notify_object_selection_changed
  // so SelectionContext stays in sync no matter who calls it. Previously
  // inline; moved to Canvas.cpp because the inline body would otherwise need
  // to reach a private member via header — and the s159 audit was scoped
  // to m_sig_selection.emit() sites in Canvas.cpp, missing MainWindow callers
  // (e.g. on_warp_make) that mutated selection through this accessor without
  // refreshing m_sel_ctx. Folding the notify here is the structural fix:
  // every caller becomes correct by construction.
  void set_selection_single(SceneNode *node);

  // Called by MainWindow after the canvas is placed in its Overlay.
  // fixed is the Gtk::Fixed child of the overlay that floats above the canvas.
  void set_text_overlay(Gtk::Fixed *fixed);

  // Multi-select accessors
  const std::vector<SceneNode *> &selection() const { return m_selection; }
  // Primary selection — the inspector target. Always a member of
  // selection() when non-null. Used by MainWindow to gate envelope
  // pick-set shortcuts and similar context-sensitive key routing.
  SceneNode *primary_selection() const { return m_selected; }
  bool is_selected(SceneNode *obj) const {
    return std::find(m_selection.begin(), m_selection.end(), obj) !=
           m_selection.end();
  }

  // Convert screen ↔ document coords
  void screen_to_doc(double sx, double sy, double &dx, double &dy) const;
  void doc_to_screen(double dx, double dy, double &sx, double &sy) const;

  // Signals
  using ZoomChangedSignal = sigc::signal<void(double)>;
  using CursorMovedSignal = sigc::signal<void(double, double)>;
  using ZoomAltChangedSignal = sigc::signal<void(bool)>; // true = alt held
  using SelectionChangedSignal = sigc::signal<void(SceneNode *)>;
  using DocumentChangedSignal = sigc::signal<void()>;
  // Fired whenever the selected node index changes in Node tool (-1 = none).
  using NodeChangedSignal = sigc::signal<void(SceneNode *, int)>;

  ZoomChangedSignal &signal_zoom_changed() { return m_sig_zoom; }
  ZoomAltChangedSignal &signal_zoom_alt_changed() { return m_sig_zoom_alt; }
  CursorMovedSignal &signal_cursor_moved() { return m_sig_cursor; }
  SelectionChangedSignal &signal_selection_changed() { return m_sig_selection; }
  DocumentChangedSignal &signal_document_changed() { return m_sig_doc_changed; }

  // s158 m1 — SelectionContext is the canonical answer to "what's
  // selected and what can it do?" Refreshed automatically whenever
  // m_sig_selection emits (Canvas wires the recompute internally).
  // Consumers (context menus, action-enable, inspector, status bar)
  // read object/node Info+Actions and listen on
  // m_sel_ctx.signal_changed().
  SelectionContext       &selection_context()       { return m_sel_ctx; }
  const SelectionContext &selection_context() const { return m_sel_ctx; }

  // notify_document_changed(): public trigger for the doc-changed
  // cascade. Internally emits m_sig_doc_changed, the same signal
  // canvas-driven structural mutations (drag-move, transform, etc.)
  // already raise. External callers that mutate the doc tree without
  // going through Canvas (theme apply, future bulk ops, plugins)
  // should call this after their mutation so the cascade — layers
  // panel rebuild, status count refresh, gallery thumb refresh,
  // blend cache invalidate — runs uniformly.
  //
  // s147 m3: required by ThemesPanel apply path. apply_theme_to_doc
  // can add/remove grid/margin layers; without this notification,
  // the active doc's canvas keeps drawing the pre-apply scene
  // structure until something else triggers a structural redraw
  // (doc switch, object edit, etc.).
  void notify_document_changed() { m_sig_doc_changed.emit(); }

  // s156 — pointer-validation pump for deferred idle handlers.
  // Returns true iff `target` is still present in the active doc tree
  // (any layer's children, one level into Group/Compound/ClipGroup).
  // SAFE TO CALL WITH A POSSIBLY-FREED POINTER — only does pointer
  // equality checks against live unique_ptr.get() values, never derefs
  // `target`. Used by deferred idle handlers in MainWindow that captured
  // a SceneNode* before a destructive op (Join, Delete) had a chance to
  // free it: idle wakes up after the free, validates the captured ptr,
  // returns early if dead.
  bool is_node_alive(const SceneNode *target) const;

  // s156 — Scrub all Canvas state pointing at a SceneNode about to be
  // erased. MUST be called BEFORE the layer-children erase, with the
  // still-alive pointer. Does not touch m_selected — callers reassign
  // that immediately after the erase. See implementation for full
  // coverage list. Safe to call with a not-currently-referenced pointer
  // (no-op if no member holds it).
  void scrub_node_refs(const SceneNode *target);
  // S89: fired after a measurement is appended to or modified within the
  // measure layer (auto-save on completion, Enter commit). Listeners refresh
  // the inspector's saved-measurements list and the layers panel. Distinct
  // from signal_selection_changed which dedups against the current object.
  using MeasurementsChangedSignal = sigc::signal<void()>;
  MeasurementsChangedSignal &signal_measurements_changed() {
    return m_sig_measurements_changed;
  }

  // Emitted when canvas wants to switch tool (e.g. after placing a shape)
  using RequestToolSignal = sigc::signal<void(ActiveTool)>;
  RequestToolSignal &signal_request_tool() { return m_sig_request_tool; }
  NodeChangedSignal &signal_node_changed() { return m_sig_node_changed; }

  // s205 m1 — fired whenever the rotation-pivot state changes: enable,
  // disable, position move (programmatic, R+drag, popover edit, inspector
  // edit). Pivot is session-only state (no undo) and is independent of
  // doc_changed — emitted from inside set_custom_pivot / clear_custom_pivot
  // and from notify_r_pressed when it flips m_has_custom_pivot or seeds the
  // bbox-centre default. Inspector subscribes to live-sync its pivot picker.
  using PivotChangedSignal = sigc::signal<void()>;
  PivotChangedSignal &signal_pivot_changed() { return m_sig_pivot_changed; }

  // s125 m1a: emitted when the user picks "Save to Library…" from the
  // canvas right-click context menu. MainWindow opens the folder picker
  // and routes to on_save_selection_to_library. No params — Canvas's
  // current selection is the implicit payload, mirroring the
  // LibraryPanel + button's signal_add_to_library flow.
  using RequestSaveToLibrarySignal = sigc::signal<void()>;
  RequestSaveToLibrarySignal &signal_request_save_to_library() {
    return m_sig_request_save_to_library;
  }

  // s210 m1 — emitted when the user right-clicks an image SceneNode.
  // The signal payload is a pre-baked ImageInfo (filename, path, pixel
  // dims, format, depth, file size, mtime, placed size, linkage) —
  // Canvas does the file-system reads (read_image_meta, last_write_time,
  // format_file_size) before emitting; MainWindow's ImageInfoDialog
  // member is a pure presenter. This replaces the prior inline
  // `new Gtk::Window` self-deleting dialog that lived in Canvas.cpp
  // (s125 m1g..m1j); the dialog is now an app-lifetime hide-on-close
  // singleton living on MainWindow (mirrors s200 ThemeEditDialog and
  // s201 StyleEditorDialog).
  using RequestImageInfoSignal = sigc::signal<void(ImageInfo)>;
  RequestImageInfoSignal &signal_request_image_info() {
    return m_sig_request_image_info;
  }

  // Pen tool — invoked from MainWindow key handler
  void commit_pen_path();
  void cancel_pen_path();

  // Line tool — invoked from MainWindow key handler
  void commit_line_path();
  void cancel_line_path();

  // Text tool — invoked from MainWindow key handler and entry signals
  void commit_text_edit();
  void cancel_text_edit();

  // Ref tool — place a ref point at display/user-space coordinates
  void place_ref_at_display(double ux, double uy);
  void place_rect_precise(double ux, double uy, double w, double h);
  void place_ellipse_precise(double ucx, double ucy, double rx, double ry);
  void place_line_precise(double ux1, double uy1, double ux2, double uy2);
  void place_text_precise(double ux, double uy, const std::string &family,
                          double size, bool bold, bool italic,
                          const std::string &anchor, const std::string &align);
  void place_shape_node(SceneNode obj);
  void import_svg_to_canvas(const std::string &path);
  // Place a raster image (PNG/JPG/GIF/WebP) in the active layer.
  // fit_canvas_to_image=false (default): scales the image to ≤80% of the
  //   current canvas, centres it, leaves the canvas size unchanged.
  // fit_canvas_to_image=true (s125 m1c): resizes the document's canvas to
  //   match the image's natural pixel dimensions and places the image at
  //   (0, 0) at 1:1. Used by File → Place Image as Document for the
  //   manual-screenshot annotation workflow.
  // Note: the canvas resize is not currently undoable (matches the rest of
  //   the canvas-resize paths in the app). Backlog item.
  void import_image_to_canvas(const std::string &path,
                              bool fit_canvas_to_image = false);
  void flip_selection(bool horizontal);
  void step_repeat(int copies, double dx, double dy);

  // Extended step-and-repeat — translate (dx*i, dy*i) then rotate by
  // (angle_deg*i) around (pivot_x, pivot_y) for each copy i=1..copies.
  // When rotate_enabled=false this is equivalent to step_repeat(copies,dx,dy).
  // pivot is in doc coords (Y-down, matches node storage).
  void step_repeat(int copies, double dx, double dy, bool rotate_enabled,
                   double angle_deg, double pivot_x, double pivot_y);

  // Selection bbox helpers (doc coords, Y-down). Return false if selection
  // is empty or all objects lack bboxes.
  bool selection_bbox(double &x, double &y, double &w, double &h) const;
  bool selection_bbox_center(double &cx, double &cy) const;

  // s259 — Weighted ("true") center of the selection, computed as the
  // centre of the minimum enclosing circle of all path-vertex and image-
  // corner points in the selection (in doc coords, with each leaf's
  // transform applied). For rectangles, ellipses, regular polygons /
  // stars, and circles, this coincides with the bbox centre. For
  // irregular shapes it gives the no-wobble rotation pivot — rotation
  // around this point keeps the shape inside the same bounding disc
  // throughout the rotation, where bbox-centre rotation would visibly
  // swing the shape's extremes out past their start positions.
  //
  // Used as the default pivot for:
  //   - R-key pivot mode seed (notify_r_pressed)
  //   - Alt-modified scale handle drag (symmetric scale about centre)
  //   - Corner-handle rotate drag with no custom pivot
  //   - Programmatic rotate_selection_by / scale_selection_by fallbacks
  //
  // NOT used for direct-manipulation scale (corner-handle drag without
  // Alt) — that path keeps the opposite-corner pivot so the dragged
  // handle tracks the cursor.
  //
  // Returns false on empty selection or no extractable points.
  bool selection_true_center(double &cx, double &cy) const;

  // Temporary crosshair overlay for dialogs (Step and Repeat pivot preview).
  // Separate from m_custom_pivot_x/y so it doesn't collide with the
  // rotate-from-point system. When active, crosshair renders at (px, py) in
  // doc coords.  While active, canvas clicks move the refpt and support drag.
  void set_step_repeat_preview(bool active, double px, double py);

  // Install a callback fired while the user drags / clicks the SR crosshair
  // on canvas.  (px, py) are doc coords (Y-down).  Set to nullptr to clear.
  void set_step_repeat_pivot_callback(std::function<void(double, double)> cb);

  // ── Guide construct mode (two-point guide creation) ───────────────────
  // Activated from the Guides inspector.  Pre-empts the current tool for
  // input until the user commits or cancels.
  //   Phase 0: awaiting click 1
  //   Phase 1: awaiting click 2 (preview chases mouse from p1)
  //   Phase 2: review — preview locked; a dialog is shown for OK/Cancel
  //
  // perpendicular: if true, the committed guide is perpendicular to the
  // vector p1→p2 through the midpoint (anchor).  Otherwise guide is along
  // the vector through the midpoint.  Anchor in both cases is the midpoint.
  void begin_guide_construct();
  void cancel_guide_construct();
  void set_guide_construct_perpendicular(bool on);
  bool guide_construct_active() const { return m_guide_construct_active; }
  int  guide_construct_phase()  const { return m_guide_construct_phase; }
  // Callback fired when the user clicks p2 — MainWindow opens the dialog.
  using GuideConstructReviewCb = std::function<void()>;
  void set_guide_construct_review_callback(GuideConstructReviewCb cb);
  // Commit: constructs a Guide SceneNode with midpoint anchor + computed
  // angle (vector or perpendicular depending on flag) and pushes into the
  // guide layer.  Exits construct mode.  Returns true on success.
  bool commit_guide_construct();

  // ── Macro playback ────────────────────────────────────────────────────
  void run_macro(const std::string &macro_id, int from_step = 0);

  // Direct style setters used by macro playback (bypass inspector pipeline)
  void apply_fill_to_selection(const std::string &hex_or_empty);
  void apply_stroke_to_selection(const std::string &hex_or_empty);
  void apply_stroke_width_to_selection(double width);
  void apply_opacity_to_selection(double opacity);

  // S90 Stage 1 — debug helper. Drops a hand-built linear gradient
  // (red → yellow → blue, horizontal across the shape's bbox) onto every
  // selected shape's fill. No undo, no swatch link, no SVG round-trip yet —
  // this is the "prove the render path on screen" lever for Stage 1 and gets
  // removed once the gradient editor lands. Triggered via the
  // `win.debug-test-gradient` action.
  void debug_apply_test_gradient();

  // ── Swatch apply (Phase 5 M3) ─────────────────────────────────────────
  // Route a swatch application to each object in the selection through
  // the SwatchLibrary::set_paint choke point, writing a SwatchRef so the
  // reverse usage index picks up the binding. The FillStyle on each
  // object is still written with a concrete colour (set_paint's
  // responsibility) so the render path stays unchanged.
  //
  // Requires the swatch library to have been wired via set_swatch_library.
  // No-op if the library or selection is empty.
  //
  // Called from SwatchesPanel::apply_swatch. Other paint-write paths
  // (toolbar, inspector, eyedropper) still use apply_fill_to_selection /
  // apply_stroke_to_selection — those write raw colours and remain
  // un-indexed in M3. Later milestones reroute them through set_paint.
  void apply_swatch_to_selection(const color::SwatchId &swatch_id,
                                 color::PaintSlot slot);

  // S83 m4h v4: walk the active document and clear fill_swatch_id /
  // stroke_swatch_id on every node bound to `id`. Cached fill / stroke
  // are preserved (break-on-override v1 — the moment-of-unbind
  // appearance is what the user keeps). Used by SwatchesPanel's
  // delete path BEFORE SwatchLibrary::remove_swatch runs, so deleted
  // swatches don't leave dangling ids on bound objects. Not undoable
  // in v1 (matches the existing un-undoable swatch-create path);
  // DeleteSwatchCommand + AddSwatchCommand are backlog items.
  void unbind_swatch_from_doc(const color::SwatchId& id);

  // Rotate selection by angle_deg around pivot (doc space).
  // If pivot_explicit=false, rotates around selection bbox centre.
  void rotate_selection_by(double angle_deg, double pivot_x = 0.0,
                           double pivot_y = 0.0, bool pivot_explicit = false);

  // Scale selection by factors around bbox centre
  void scale_selection_by(double sx, double sy);

  // Node tool — invoked from MainWindow key handler
  // Returns true if the key was consumed.
  bool node_tool_key(guint keyval, bool shift, bool ctrl, bool alt = false);

  // Selection tool — invoked from MainWindow key handler
  // Returns true if the key was consumed.
  bool selection_tool_key(guint keyval, bool shift, bool ctrl,
                          bool alt = false);

  // Zoom helpers — public so MainWindow/ContextBar can call them
  void zoom_toward(double wx, double wy, double factor);
  void zoom_fit();
  void zoom_to_rect(double sx1, double sy1, double sx2, double sy2);
  void zoom_to_all_objects(); // Ctrl+Shift+0 — fits entire scene including
                              // off-canvas
  void zoom_to_selection();   // Ctrl+3 — fits selection, centres on screen

  // Called by MainWindow key handler to forward Alt state — more reliable
  // than a Canvas-local EventControllerKey which requires canvas focus.
  void notify_alt_pressed();
  void notify_alt_released();

  // R held over a selection → pivot placement mode (rotate from point).
  void notify_r_pressed();
  void notify_r_released();

  // ── Custom-pivot public surface (s205 m1) ──────────────────────────────
  // Pivot state mutations from outside Canvas — currently the inspector's
  // pivot picker section under Selection. The popover in Canvas.cpp's
  // right-click handler also writes the pivot, but does so via direct
  // member access since it lives in Canvas's own translation unit. Both
  // direct edits and these setters end with an m_sig_pivot_changed.emit()
  // (the popover does this explicitly in its signal_point_changed lambda)
  // so all pivot edits converge on the same notification path.
  //
  // Pivot is session-only — no undo, no doc_changed, no persistence.
  // Cleared by selecting a different object (or none) and by the second
  // R-toggle press (notify_r_pressed's clear branch).
  bool   has_custom_pivot() const { return m_has_custom_pivot; }
  double custom_pivot_x()   const { return m_custom_pivot_x; }
  double custom_pivot_y()   const { return m_custom_pivot_y; }
  void   set_custom_pivot(double x, double y);
  void   clear_custom_pivot();

  // s210 m2 — public seam for the rotate-from-point Apply path.
  // Lifted out of Canvas_input.cpp's inline `on_pivot_dialog` apply
  // handler when the dialog itself moved out to RotateFromPointDialog
  // (the s200/s201 lifetime-conversion idiom). The dialog hands the
  // three input values here via the CommittedFn closure; this method
  // updates the pivot (delegated to set_custom_pivot so the
  // pivot_changed signal still routes through that centralised seam),
  // walks the selection (paths / text / image leaves), rotates every
  // anchor + control around the pivot, and pushes one
  // CompositeCommand ("Rotate from point") of EditPathCommand +
  // MoveObjectCommand entries against m_history. No-op rotation when
  // |angle_deg| < 0.0001 — the dialog can be Apply'd with angle 0 to
  // commit only the pivot move, matching the pre-conversion behaviour.
  void apply_rotate_from_point(double pivot_x, double pivot_y,
                               double angle_deg);

  // s210 m2 — emitted from on_pivot_dialog after the pivot is placed
  // (right-click while R is held). MainWindow's RotateFromPointDialog
  // member presents the angle entry and routes Apply back through
  // apply_rotate_from_point. Replaces the prior inline
  // `new Gtk::Window` self-deleting dialog. Payload is the doc-Y-down
  // internal-coords pivot the dialog should seed its X/Y spinners
  // from.
  using RequestRotateFromPointSignal =
      sigc::signal<void(double pivot_x, double pivot_y)>;
  RequestRotateFromPointSignal& signal_request_rotate_from_point() {
    return m_sig_request_rotate_from_point;
  }

  // Called by MainWindow key handler to forward Space state — same reason.
  // Space held during left-drag → pan instead of tool action.
  void notify_space_pressed();
  void notify_space_released();

  // Guide creation — called from MainWindow when ruler drag fires
  void begin_guide_drag(double doc_pos, bool horizontal);
  void update_guide_drag(double doc_pos);
  void end_guide_drag(double doc_pos);
  void cancel_guide_drag();
  bool guide_drag_active() const { return m_guide_drag_active; }

  // Guide selection — multi-select set, mirrors object selection model.
  // selected_guide() returns the primary (first) selected guide or nullptr.
  const std::vector<SceneNode *> &guide_selection() const {
    return m_guide_selection;
  }
  SceneNode *selected_guide() const {
    return m_guide_selection.empty() ? nullptr : m_guide_selection[0];
  }
  void set_guide_selection(const std::vector<SceneNode *>
                               &sel); // from external (LayersPanel/inspector)
  void clear_guide_selection();

  // Emitted when canvas changes guide selection. Carries full selection set.
  using GuideSelectionChangedSignal =
      sigc::signal<void(std::vector<SceneNode *>)>;
  GuideSelectionChangedSignal &signal_guide_selection_changed() {
    return m_sig_guide_selection_changed;
  }

  // Emitted when canvas wants to show a non-blocking info/warning message.
  // MainWindow connects this to an AlertDialog.
  using ShowMessageSignal =
      sigc::signal<void(std::string /*title*/, std::string /*body*/)>;
  ShowMessageSignal &signal_show_message() { return m_sig_show_message; }

  // Emitted after a successful eyedropper pick.
  // Carries the sampled colour and whether it targets stroke (true) or fill
  // (false).
  using EyedropperPickSignal =
      sigc::signal<void(FillStyle /*color*/, bool /*to_stroke*/)>;
  EyedropperPickSignal &signal_eyedropper_pick() {
    return m_sig_eyedropper_pick;
  }

  // Emitted when the corner tool selection count changes (0 = deselected all)
  using CornerSelChangedSignal = sigc::signal<void(int /*count*/)>;
  CornerSelChangedSignal &signal_corner_sel_changed() {
    return m_sig_corner_sel_changed;
  }

  // Apply corner treatment to currently selected corner nodes, push undo.
  // Called by the corner panel Apply button.
  void apply_corner_treatment_op(CornerType type, double radius);

  // Delete a specific guide node by pointer.  No-op if not found.
  void delete_guide(SceneNode *g);
  // Delete all currently selected guides.
  void delete_selected_guides();

  // Delete the currently selected object (Selection tool).
  // Returns true if an object was deleted.
  bool delete_selected();

  // Clipboard operations
  void select_all();
  void clear_selection();  // s136 m5: counterpart to select_all (Ctrl+Shift+A)
  // s290 — Node-tool counterparts. Ctrl+A in Node mode selects every node
  // on every visible non-special-layer path; Ctrl+Shift+A clears node
  // selection. These operate on m_node_selection (and m_selected /
  // m_selected_node for primary), independent of the object-world
  // m_selection that select_all/clear_selection above mutate.
  void node_select_all();
  void node_clear_selection();
  void copy_selected();
  void cut_selected();
  void paste_clipboard();
  void duplicate_selected();
  void duplicate_in_place_selected(); // s181: renamed from clone_selected.
                                       // Zero-offset duplicate; lands on top of
                                       // original. Honest name — no source/
                                       // instance link, no propagation.
  bool has_clipboard() const { return !m_clipboard.empty(); }

  // Reverse the winding direction of the selected path.
  // Pushes an EditPathCommand for undo. No-op if no path selected.
  void reverse_selected_path();

  // Align / distribute the current multi-selection.
  // No-op if fewer than 2 objects are selected.
  void align_selection(AlignOp op);

  // Z-order operations on the primary selected object.
  enum class ArrangeOp { BringToFront, BringForward, SendBackward, SendToBack };
  void arrange(ArrangeOp op);

  // Open a closed path at the currently selected node (rotate + open).
  // No-op if path is open, no node selected, or fewer than 2 nodes.
  void open_selected_at_node();

  // Split an open path at the currently selected node into two objects.
  // No-op if path is closed, node is head/tail, or fewer than 3 nodes.
  void split_selected_at_node();

  // Called by the inspector to apply a node coordinate edit through the
  // same pipeline as a canvas drag — canvas owns all writes to obj->path.
  void apply_node_edit(int node_idx, double x, double y, double cx1, double cy1,
                       double cx2, double cy2);

  // Apply a node type to all nodes in the current multi-node selection.
  void set_selected_nodes_type(BezierNode::Type type);

  // ── animate_handle (s288 m1) ───────────────────────────────────────────
  // BANKED but compiled in. m1's literal-mouse-drag-impersonation approach
  // turned out to be the wrong architecture for the welcome demo (the
  // Node-tool overlay gates on selection state, which makes script-minted
  // paths invisible during animation). m2 supersedes with theatre: the
  // SvgPerformer's phantom overlay (see below). This method is kept
  // as a primitive for future edit-style beats — e.g. a color picker
  // slider that visibly drags during Phase 2 — but is not invoked by the
  // welcome-demo spine.
  void animate_handle(SceneNode* path, int node_idx,
                      HitResult::Kind which,
                      Vec2 start, Vec2 end,
                      double duration_ms);

  // ── Pen-path performance (s288 m2, renamed s291 m2) ───────────────────
  // Kick off a Pen-path performance from a raw SVG d-string: a phantom-
  // overlay animation that simulates the Pen tool building one path
  // anchor-by-anchor, then commits a real Path SceneNode at the end.
  // The actual heavy lifting lives in animation::SvgPerformer; this
  // method is a thin entry point that resolves the active doc/layer
  // from m_doc and forwards.
  //
  // Returns immediately; the animation runs asynchronously via a
  // Glib::Timeout owned by the performer. See animation/SvgPerformer.hpp
  // for the full architecture and m2 scope.
  void perform_pen_path(const std::string& d_string, double speed);

  // ── SVG-file performance (s288 m3, renamed s291 m2) ───────────────────
  // Kick off a multi-path performance from an SVG file: constructs an
  // AnimatingSvgParser with the performer as SvgEmitter, the parser
  // fires on_path callbacks during the parse which enqueue one
  // PendingPerformance per Path; the queue plays back-to-back with a
  // small inter-path breath. Each committed Path lands with the SVG's
  // original fill/stroke.
  //
  // Returns immediately; the performance runs asynchronously on the
  // GTK main loop. Same thin-forwarder pattern as perform_pen_path.
  void perform_svg_file(const std::string& svg_path, double speed);

  // ── Warp envelope drag (M4b) ─────────────────────────────────────────
  // Which part of an envelope is being dragged. None = not dragging.
  enum class WarpDragKind { None, Anchor, HandleIn, HandleOut };

  // ── Warp envelope pick set (M4c-2) ───────────────────────────────────
  // Which sub-part of an anchor is picked. Anchors have three addressable
  // parts; the pick set can hold any combination across both envelopes.
  enum class EnvelopePart : uint8_t { Anchor, HandleIn, HandleOut };

  struct EnvelopePick {
    bool         is_top;   // true=env_top, false=env_bottom
    int          idx;      // index into env.nodes
    EnvelopePart part;
    bool operator==(const EnvelopePick &o) const {
      return is_top == o.is_top && idx == o.idx && part == o.part;
    }
  };

  // Envelope pick-set state. Rendered in yellow over the orange/cyan base;
  // consulted by mouse handlers and keyboard shortcuts. Always empty when
  // no Warp is primary-selected.
  const std::vector<EnvelopePick> &warp_env_picks() const { return m_warp_env_picks; }

  // Public helpers — called by MainWindow key handler (Escape + T/B/L/R/C/A).
  void warp_env_picks_clear();
  void warp_env_picks_select_all_top_anchors();
  void warp_env_picks_select_all_bottom_anchors();
  void warp_env_picks_select_leftmost_pair();
  void warp_env_picks_select_rightmost_pair();
  void warp_env_picks_select_interior_anchors();
  void warp_env_picks_select_all_anchors();
  void warp_env_picks_select_all();  // anchors + handles on both envelopes

  // hit_test_warp_envelope(x_screen, y_screen, warp, kind, is_top, idx):
  //   returns true if the screen-space point (x, y) is within hit
  //   threshold of an envelope anchor or handle. On match, kind/
  //   is_top/idx describe which part was hit. Handles are preferred
  //   over anchors when both overlap (their hit regions are offset).
  bool hit_test_warp_envelope(double x_screen, double y_screen,
                              const SceneNode &warp,
                              WarpDragKind &kind, bool &is_top,
                              int &idx) const;

private:
  // ── Selection-change fanout (s158 m1) ────────────────────────────────
  // Single seam that (a) refreshes m_sel_ctx from current m_selection
  // and m_selected, and (b) emits m_sig_selection. Call this in place
  // of bare m_sig_selection.emit() at object-selection mutation sites.
  //
  // m1 wires this helper at one canonical site only (Canvas::clear_selection)
  // to prove the pipeline. The audit pass to migrate every existing
  // m_sig_selection.emit() call is m2 work — until then, sites that
  // emit directly will not refresh the SelectionContext, and consumers
  // listening on m_sel_ctx.signal_changed() will see staleness from
  // those sites. That's expected for m1; no current consumer reads
  // m_sel_ctx, so the staleness is invisible.
  void notify_object_selection_changed();

  // Same shape for the node world — refreshes m_sel_ctx from
  // m_node_selection plus the secondary slot, then (caller's choice
  // whether to also signal_node_changed). Wired at one canonical
  // site in m1 for parity with the object side.
  void notify_node_selection_changed();

  // ── Draw ─────────────────────────────────────────────────────────────
  void on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h);
  void draw_grid(const Cairo::RefPtr<Cairo::Context> &cr, int cw, int ch);
  void draw_objects(const Cairo::RefPtr<Cairo::Context> &cr);
  void draw_object(const Cairo::RefPtr<Cairo::Context> &cr,
                   const SceneNode &obj, double layer_r = 0.88,
                   double layer_g = 0.88, double layer_b = 0.88);
  // S97 m2 — drop shadow render. Called from draw_object's end-of-wrap
  // when obj.shadow_enabled is set. host_pat is the popped cairo group
  // pattern containing the host's full unshadowed render. Paints a
  // tinted, blurred, offset copy onto cr; caller paints the host on top.
  // No-op if the host's bbox is unavailable or the blur radius is too
  // small to be visible.
  void render_shadow_under(const Cairo::RefPtr<Cairo::Context> &cr,
                           const SceneNode &obj,
                           const Cairo::RefPtr<Cairo::Pattern> &host_pat);
  void draw_rubber_band(const Cairo::RefPtr<Cairo::Context> &cr);
  void draw_marquee(const Cairo::RefPtr<Cairo::Context> &cr);
  void draw_ref_coord_overlay(const Cairo::RefPtr<Cairo::Context> &cr);
  void draw_eyedropper_overlay(const Cairo::RefPtr<Cairo::Context> &cr);
  // S66 — Phase 3 loupe. draw_eyedropper_loupe replaces the old swatch
  // overlay when the eyedropper tool is active. refresh_loupe_buffer
  // renders a small region of the canvas into m_loupe_surface and reads
  // back its centre pixel; called on every hover tick while Shift is held.
  void draw_eyedropper_loupe(const Cairo::RefPtr<Cairo::Context> &cr);
  void refresh_loupe_buffer();
  void draw_selection_handles(const Cairo::RefPtr<Cairo::Context> &cr);
  void draw_guides(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h);
  void draw_ruler_overlay(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                          int h);
  void draw_guides_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                             const SceneNode *gl);
  void draw_grid_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                           const SceneNode *gl);
  void draw_margin_doc_space(const Cairo::RefPtr<Cairo::Context> &cr,
                             const SceneNode *ml);

  // M4c-2: Lazy invalidation for the envelope pick set. Call at every
  // read site; clears m_warp_env_picks when the primary selection has
  // changed away from the pick set's recorded owner Warp.
  void sync_warp_env_picks_to_selection();

  // snap_move is internal — used by the selection-drag handler to snap
  // a moving BBX. Stays private; the public point-snap helpers below
  // are the supported entry point for outside callers.
  std::pair<double, double> snap_move(double raw_dx, double raw_dy);

public:
  // Snap helpers — return snapped doc coordinate, or original if no snap.
  // Tolerance is in screen pixels. Only snaps when doc->snap.enabled and
  // the relevant behavior flag is set. Public so binding-layer code
  // (ruler-origin commit, ruler-pulled guide creation, etc.) can route
  // doc-space coords through the same snap stack the canvas uses.
  double snap_x(double doc_x, double tolerance_px = 12.0) const;
  double snap_y(double doc_y, double tolerance_px = 12.0) const;

private:

  // ── Rect for an object (document space) ──────────────────────────────
  // Defined here (above apply_fill) so the gradient-aware fill overload can
  // name BBox in its signature. object_bbox() declaration lives below in the
  // coordinate-helpers section.
  struct BBox {
    double x, y, w, h;
  };

  void apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                  const FillStyle &fill);
  // Bbox-aware overload: required for gradient fills, since their endpoints
  // are stored in objectBoundingBox-fraction space and have to be lerped into
  // doc coordinates using the shape's actual bbox at render time. Solid /
  // CurrentColor / None fall through to the simple path; bbox is ignored.
  // Pass the shape's doc-space bbox; on render the gradient endpoints are
  // mapped (g_x1,g_y1)→(bbox.x + g_x1*bbox.w, bbox.y + g_y1*bbox.h) etc.
  void apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                  const FillStyle &fill,
                  const BBox &bbox);

  // ── Cache-aware overload (S106 m1) ────────────────────────────────────
  // For gradient fills on a SceneNode: route through the per-node
  // gradient_cache (rasterised once, blit on subsequent frames). Avoids
  // Cairo re-rasterising the same gradient ~60 times per second during
  // a drag.
  //
  // Falls through to the bbox-aware overload above for non-gradient
  // fills (Solid / CurrentColor / None) — there's nothing to cache.
  //
  // Cache coherence — see SceneNode.hpp for full doc:
  //   * obj.gradient_cache_dirty (set by mutate_appearance et al) →
  //     fill content changed; rebuild.
  //   * obj.gradient_cache_bbox_* != bbox → geometry changed (path
  //     mutation, scale, etc.); rebuild. Auto-detected here, no caller
  //     instrumentation needed.
  //   * Cache is rasterised at doc resolution (zoom=1). Cairo bilinear-
  //     upscales at higher zooms; gradients are smooth fields so the
  //     softening is imperceptible. Re-rasterisation at zoom-settle is
  //     a future refinement.
  void apply_fill(const Cairo::RefPtr<Cairo::Context> &cr,
                  const SceneNode &obj,
                  const BBox &bbox);

  void apply_stroke_style(const Cairo::RefPtr<Cairo::Context> &cr,
                          const StrokeStyle &stroke);

  // ── Coordinate helpers ────────────────────────────────────────────────
  double doc_origin_x() const;
  double doc_origin_y() const;
  double fit_zoom() const; // zoom that fits canvas in widget with margin
  void clamp_pan();

  std::optional<BBox> object_bbox(const SceneNode &obj,
                                  bool include_stroke = true) const;

  // s204 m2 — Edge-snap candidate gathering.
  //
  // Walks every visible, non-special-layer top-level object and collects
  // bbox snap candidates: 3 X-values (left, midX, right) and 3 Y-values
  // (top, midY, bottom) per bbox — these 6 numbers encode all 9 of the
  // bbox's grid points (4 corners + 4 edge midpoints + center), because
  // the snap engine is axis-independent. snap_x only ever asks "is doc_x
  // close to one of these X-values?" — it doesn't care which Y it pairs
  // with.
  //
  // Groups and compounds contribute their union bbox, not their children's
  // bboxes, mirroring how the rest of the canvas treats a group as one
  // object. Recursion into group children would multiply candidates by an
  // arbitrary factor without matching user mental model.
  //
  // `exclude` is a list of object pointers to skip — typically the objects
  // being moved during a drag, so a moving bbox doesn't snap to its own
  // current position. Linear scan over `exclude` is fine here; the moving
  // set is typically 1–10 pointers, and gather is called at drag-start
  // (cached for the drag's lifetime), not per motion event.
  //
  // For the non-drag entry points (snap_x / snap_y called during a draw
  // gesture), `exclude` is empty and the gather is a one-shot per click.
  void gather_object_edge_snap_candidates(
      const std::vector<const SceneNode*>& exclude,
      std::vector<double>& out_x_candidates,
      std::vector<double>& out_y_candidates) const;

  // ── Hit test ─────────────────────────────────────────────────────────
  SceneNode *hit_test(double doc_x, double doc_y);
  SceneNode *hit_test_next(double doc_x, double doc_y, SceneNode *skip);

  // s204 m1: the no-op private snap(double) helper used to live here. It
  // returned its argument unchanged and was a footgun — ~18 call sites
  // across Canvas_input.cpp and Canvas_draw.cpp called it instead of the
  // real snap_x / snap_y above, silently dropping all snap behaviour for
  // pivot setters, the Line / Ref / shape tools, and the cursor readout.
  // Deleted per the "pumps need to be the only path" rule (CANON s203 m3):
  // forcing callers to pick an axis is a compile-time enforcement that
  // can't be skipped.

  // ── Input controllers ─────────────────────────────────────────────────
  bool on_scroll(double dx, double dy);
  void on_pan_begin(double x, double y);
  void on_pan_update(double dx, double dy);
  void on_pan_end(double dx, double dy);
  void on_draw_begin(double x, double y);
  void on_draw_update(double dx, double dy);
  void on_draw_end(double dx, double dy);

  // Pen tool
  void on_pen_begin(double x, double y);

  // Node editor
  void on_node_begin(double x, double y);
  void on_node_update(double dx, double dy);
  void on_node_end();
  void on_select_begin(double x, double y);
  void on_select_update(double dx, double dy);
  void on_select_end(double dx, double dy);
  void on_motion(double x, double y);

  // ── State ─────────────────────────────────────────────────────────────
  CurvzDocument *m_doc = nullptr;

  // Non-owning pointer to the active project (s116 m6). Used to read the
  // project-wide workspace appearance fields (motif/artboard_bg/
  // workspace_bg) when painting. May be null during early boot — paint
  // code falls back to CurvzDocument's legacy bg fields, which match
  // the project defaults exactly so visuals are consistent.
  CurvzProject *m_project = nullptr;

  ActiveTool m_tool = ActiveTool::Selection;
  ActiveTool m_prev_tool = ActiveTool::Selection;
  SceneNode *m_selected = nullptr;
  CommandHistory *m_history = nullptr;

  // ── s277 m2 — macro-replay guard for long-op progress dialog ──────
  //
  // When set true (only inside run_macro's replay loop), Canvas's
  // long-op verbs (currently boolean_op; future Step and Repeat,
  // Expand Stroke, etc.) skip the modal progress dialog and run
  // synchronously. Macros are batch — the user initiated the macro
  // and is happy to wait; mid-macro modals would interrupt that
  // flow with one dialog per slow step.
  //
  // Set/cleared with RAII around run_steps in run_macro. A bool not
  // a counter — Curvz macros don't currently invoke other macros, so
  // nesting isn't a case to handle. If recursive macros land someday,
  // change to an int and increment/decrement.
  bool m_in_macro_replay = false;

  // Non-owning pointer to the project's swatch library. Used by
  // apply_swatch_to_selection to route writes through the set_paint
  // choke point. May be null during early boot — callers guard.
  color::SwatchLibrary *m_swatch_library = nullptr;

  // Live-recolour connection (s70 M3). Wired in set_swatch_library() so
  // it follows project switches — MainWindow calls set_swatch_library at
  // startup and again when a new document becomes active. The handler
  // walks m_doc, refreshes the cached FillStyle on bound SceneNodes, and
  // queues a draw. Default-constructed connection is a harmless no-op if
  // set_swatch_library is called with a null library, or if a second
  // call replaces the previous library.
  sigc::connection m_library_swatch_changed_conn;

  // Non-owning pointer to the project's style library (S78 m3d).
  // Symmetric to m_swatch_library above. Wired by MainWindow at the
  // same lifecycle moments as the swatch pointer (boot + project
  // switch). May be null during early boot — handlers guard.
  style::StyleLibrary *m_style_library = nullptr;

  // Cross-doc style-propagation connection (S78 m3d). Wired in
  // set_style_library() so it follows project switches. On
  // signal_style_changed(id), walks m_doc and re-materialises every
  // SceneNode whose bound_style matches `id`, then queues a draw —
  // making style edits visible across every open canvas without a
  // close-and-reopen. Default-constructed connection is a harmless
  // no-op if set_style_library is called with null or replaces a
  // prior library.
  sigc::connection m_library_style_changed_conn;

  // ── Multi-select (Selection tool) ─────────────────────────────────────
  // m_selected is the primary (inspector target). m_selection is the full set.
  // m_selected is always in m_selection when non-null.
  std::vector<SceneNode *> m_selection;

  // ── SelectionContext (s158 m1) ────────────────────────────────────────
  // Live capability cache. Refreshed by Canvas at every selection
  // mutation site (alongside m_sig_selection.emit). Consumers read
  // its Info+Actions and listen on its signal_changed.
  SelectionContext m_sel_ctx;

  // ── Align anchor (ephemeral key-object) ───────────────────────────────
  // Selection-time mark, NOT a persistent property of the object. Set via
  // Ctrl+Alt+click in the Selection tool (toggle); align_selection
  // consults it to use the anchor's bbox as the alignment target and
  // pins the anchor in place. Cleared whenever the anchored object
  // leaves m_selection — enforced by align_anchor() (validator-on-read)
  // rather than by instrumenting every selection-mutation site, so the
  // invariant holds automatically across the ~40+ places m_selection
  // is rewritten. Distribute ops ignore the anchor (matches Affinity).
  // Plain Alt+click stays bound to the Alt+drag duplicate-in-place
  // idiom (m_alt_drag_dup); Ctrl+Alt is the free slot.
  SceneNode *m_align_anchor = nullptr;
  // Validator-on-read: returns m_align_anchor only if still in
  // m_selection, otherwise nulls the stored pointer and returns null.
  // All readers go through this; callers MUST NOT touch m_align_anchor
  // directly. Defined out-of-line in Canvas.cpp.
  SceneNode *align_anchor();
  // Anchor-glyph pixbuf, loaded once on first render from the
  // /com/curvz/app/icons/scalable/apps/curvz-anchor-symbolic.svg
  // resource. The SVG declares 48×48 native size so the pixbuf
  // rasterises at 48 px; Cairo bilinear-downscales to the rendered
  // 22 px (same convention as Gtk::Picture's can_shrink elsewhere).
  Glib::RefPtr<Gdk::Pixbuf> m_anchor_glyph_pixbuf;

  // ── Clipping — pick mode ──────────────────────────────────────────────
  // Armed by make_clip_group() when a selection exists. The next click
  // on a Path/Compound is intercepted in on_select_begin and consumed
  // as the clip shape. m_clip_pick_selection is the selection snapshot
  // taken at arm time — used because arming the pick may require the
  // user to click outside the current selection, which would otherwise
  // reset it. Canceled by any other input (empty click, Esc, tool change).
  bool m_clip_pick_armed = false;
  std::vector<SceneNode *> m_clip_pick_selection;

  // Consumes the click during pick mode. Returns true if the press was
  // handled (regardless of whether the clip actually got built — a
  // cancel still eats the click so normal selection logic doesn't
  // overwrite state mid-arm). Always disarms before returning.
  bool finish_clip_pick(SceneNode *clicked);

  // Per-object move snapshots for multi-object drag
  struct MoveSnap {
    SceneNode *obj;
    std::vector<BezierNode> orig_nodes;
    PathData before_path;
  };
  std::vector<MoveSnap> m_move_snaps;

  // Parallel snap list for Text nodes (no path, just x/y)
  struct TextMoveSnap {
    SceneNode *obj;
    double orig_x;
    double orig_y;
  };
  std::vector<TextMoveSnap> m_text_move_snaps;
  // Parallel snap list for ref points dragged from the Selection tool.
  // Ref points aren't found by hit_test (skipped via is_special_layer)
  // so on_select_begin gains an explicit refpt-hit branch that populates
  // this snap list. on_select_update applies the move delta to ref_x/ref_y.
  // Reuses TextMoveSnap's shape — same per-point translation semantics.
  std::vector<TextMoveSnap> m_ref_move_snaps;

  // Parallel snap list for Warp envelopes — when a Warp is dragged as
  // an object (via filled-body drag), the envelope must translate
  // together with warp_source / glyph_cache / warp_cache. Without this,
  // source moves but envelope stays, producing "geometry elsewhere,
  // control handles stranded at origin" behavior.
  struct WarpEnvMoveSnap {
    SceneNode *warp;
    PathData   orig_env_top;
    PathData   orig_env_bottom;
  };
  std::vector<WarpEnvMoveSnap> m_warp_env_move_snaps;

  // ── Multi-select (Node tool) ──────────────────────────────────────────
  // Set of (path, node_index) pairs. m_selected/m_selected_node is primary.
  struct NodeSel {
    SceneNode *obj;
    int node_idx;
  };
  std::vector<NodeSel> m_node_selection;

  // ── Marquee drag ──────────────────────────────────────────────────────
  bool m_marquee_active = false;
  double m_marquee_start_dx = 0.0; // document space
  double m_marquee_start_dy = 0.0;
  double m_marquee_cur_dx = 0.0;
  double m_marquee_cur_dy = 0.0;

  // Default fill/stroke applied to every newly created object.
  // Updated from ContextBar::signal_defaults_changed() via MainWindow.
  FillStyle m_def_fill;     // default: CurrentColor
  StrokeStyle m_def_stroke; // default: CurrentColor stroke, width 0.0

  // Internal clipboard — deep clones of copied/cut objects.
  // m_clipboard_was_cut: true if last clipboard op was Cut (ids reused on
  // paste).
  std::vector<std::unique_ptr<SceneNode>> m_clipboard;
  bool m_clipboard_was_cut = false;

  // Nudge coalescing — rapid arrow-key nudges collapse into one undo step
  std::chrono::steady_clock::time_point m_nudge_last_time;
  SceneNode *m_nudge_last_obj = nullptr;

  double m_zoom = 1.0;
  bool m_fit_pending = true; // fit on first draw
  // Zoom tool state
  double m_zoom_anchor_x = 0.0;
  double m_zoom_anchor_y = 0.0;
  bool m_zoom_alt_prev =
      false; // tracks last emitted alt state to avoid duplicates
  double m_pan_x = 0.0;
  double m_pan_y = 0.0;

  // Middle-drag pan
  double m_pan_drag_start_x = 0.0;
  double m_pan_drag_start_y = 0.0;

  // Space+left-drag pan
  bool m_space_held = false;        // true while Space is held
  bool m_space_panning = false;     // true during an active Space+drag pan
  double m_space_pan_start_x = 0.0; // pan_x at drag start
  double m_space_pan_start_y = 0.0;

  // R-held custom pivot (rotate-from-point)
  bool m_r_held = false;           // true while R is held over a selection
  bool m_has_custom_pivot = false; // true when user has set a custom pivot
  double m_custom_pivot_x = 0.0;   // doc coords
  double m_custom_pivot_y = 0.0;
  bool m_pivot_dragging = false; // true during R+drag to reposition pivot

  // ── Warp envelope drag state (M4b) ─────────────────────────────────
  // Populated on mouse-press when a Warp is selected and the click lands
  // on an envelope anchor or handle; consumed by on_draw_update for
  // live writethrough, cleared by on_draw_end which also pushes the
  // EditWarpCommand.
  WarpDragKind m_warp_drag_kind   = WarpDragKind::None;
  bool         m_warp_drag_is_top = false;        // true=env_top, false=env_bottom
  int          m_warp_drag_idx    = -1;           // index into env.nodes
  // Pre-drag snapshots for undo — captured on mouse-press.
  PathData     m_warp_drag_pre_top;
  PathData     m_warp_drag_pre_bottom;
  int          m_warp_drag_pre_quality = 4;
  // s147 m2: capture preset_idx at drag start so undo can restore the
  // preset label that was in effect before drift. Drag commit clears
  // live preset_idx to -1 (envelope no longer matches preset shape);
  // undo restores both envelope AND the original preset_idx atomically.
  int          m_warp_drag_pre_preset_idx = -1;
  // Anchor drag offset: when dragging an anchor, the click point is
  // usually not exactly on the anchor center, so we capture the offset
  // to avoid a jump on first motion.
  double       m_warp_drag_click_offset_x = 0.0;  // doc units
  double       m_warp_drag_click_offset_y = 0.0;

  // M4c-2c: Multi-drag state. When true, drag update translates every
  // element in m_warp_env_picks by the same delta (pure translate, no
  // mirroring). Press-doc captures the press point for delta math.
  bool         m_warp_drag_is_multi = false;
  double       m_warp_drag_press_doc_x = 0.0;
  double       m_warp_drag_press_doc_y = 0.0;

  // M4c-2d: Set when an envelope marquee was started with Shift held —
  // release adds anchors to the existing pick set rather than replacing.
  // Cleared at drag-end. Only meaningful while m_marquee_active AND
  // primary is a Warp.
  bool         m_warp_env_marquee_additive = false;

  // M4c-2: Pick set for multi-element envelope editing. Populated by
  // click/Shift+click on envelope elements and by keyboard shortcuts.
  // Rendered in yellow over the orange/cyan base. Cleared when the
  // primary selection changes away from its current Warp (see
  // sync_warp_env_picks_to_selection()).
  std::vector<EnvelopePick> m_warp_env_picks;
  // Track which Warp the pick set belongs to, so a selection change can
  // invalidate the set without the pick set carrying over to a different
  // Warp.
  const SceneNode *m_warp_env_picks_owner = nullptr;

  // Step-and-Repeat dialog pivot preview (separate from rotate-from-point).
  // Active only while the dialog is open.  When active, any click on canvas
  // moves the refpt to that location (optionally followed by a drag).  The
  // pivot change callback fires live so the dialog's spins/preview stay in
  // sync.
  bool   m_sr_preview_active = false;
  double m_sr_preview_x = 0.0;   // doc coords (Y-down)
  double m_sr_preview_y = 0.0;
  bool   m_sr_pivot_dragging = false;
  std::function<void(double, double)> m_sr_pivot_change_cb;

  // Guide construct mode state (two-point guide creation).
  bool   m_guide_construct_active = false;
  int    m_guide_construct_phase = 0;       // 0 = click1, 1 = click2, 2 = review
  double m_guide_construct_p1_x = 0.0;      // doc coords (Y-down)
  double m_guide_construct_p1_y = 0.0;
  double m_guide_construct_p2_x = 0.0;
  double m_guide_construct_p2_y = 0.0;
  double m_guide_construct_preview_x = 0.0; // live preview endpoint (phase 1)
  double m_guide_construct_preview_y = 0.0;
  bool   m_guide_construct_perpendicular = false;
  GuideConstructReviewCb m_guide_construct_review_cb;

  // Draw drag (rect/ellipse)
  bool m_drawing = false;
  double m_draw_start_dx = 0.0; // document space — actual mouse-down position
  double m_draw_start_dy = 0.0;
  double m_draw_cur_dx = 0.0;
  double m_draw_cur_dy = 0.0;
  // Alt-from-center: effective start (mirrored through actual start when Alt
  // held)
  double m_draw_start_effective_dx = 0.0;
  double m_draw_start_effective_dy = 0.0;

  // Selection drag (move object)
  bool m_moving = false;
  bool m_alt_drag_dup = false; // Alt+drag: duplicated originals, moving clones

  // ── Polygon tool state ────────────────────────────────────────────────
  double m_poly_drag_angle =
      0.0;              // current rotation angle during drag (Shift snaps)
  int m_poly_sides = 6; // set by toolbar popover
  double m_poly_inflection = 1.0; // set by toolbar popover
  // Spiral tool state
  double m_spiral_drag_angle = 0.0;
  double m_spiral_turns = 3.0;
  double m_spiral_inner = 4.0;   // inner radius % (default tuned for ~nautilus)
  double m_move_start_dx = 0.0;
  double m_move_start_dy = 0.0;
  // Legacy single-object move fields (still used when selection has one object)
  double m_move_obj_orig_x = 0.0;
  double m_move_obj_orig_y = 0.0;
  double m_move_node0_x = 0.0;
  double m_move_node0_y = 0.0;
  std::vector<BezierNode> m_move_orig_nodes;
  PathData m_move_before_path;

  // Last mouse position (widget space)
  double m_mouse_x = 0.0;
  double m_mouse_y = 0.0;

  // Last cursor position in document space — updated in on_motion
  // Exposed via cursor_doc_x()/cursor_doc_y() for rulers
  double m_cursor_doc_x = 0.0;
  double m_cursor_doc_y = 0.0;

  static constexpr double MIN_ZOOM =
      0.0001; // allows viewing further out (smaller scale)
  static constexpr double MAX_ZOOM =
      1024.0; // ~256× short-axis units per screen pixel

  // ── Transform handle state (Selection tool) ───────────────────────────
  // Identifies which of the 8 BBX handles is being dragged.
  // None   = no handle drag in flight (move or marquee may still be active)
  // NW/NE/SE/SW = corner handles (scale, 2-axis)
  // N/S/E/W     = edge midpoint handles (scale, 1-axis)
  // RotateNW/NE/SE/SW = corner rotate zones (just outside the corner squares)
  enum class HandleKind {
    None,
    NW,
    N,
    NE,
    E,
    SE,
    S,
    SW,
    W,
    RotateNW,
    RotateNE,
    RotateSE,
    RotateSW
  };
  HandleKind m_handle_drag = HandleKind::None;

  // BBX at the moment the handle drag started (document space).
  struct BBoxF {
    double x, y, w, h;
  };
  BBoxF m_handle_start_bb{};

  // Pivot point for the current handle drag (document space).
  // For corner handles: opposite corner (or center when Alt held).
  double m_handle_pivot_x = 0.0;
  double m_handle_pivot_y = 0.0;

  // Per-leaf path snapshots taken when handle drag begins — used for
  // live re-application during drag and undo push on drag-end.
  struct ScaleSnap {
    SceneNode *obj;
    std::vector<BezierNode> orig_nodes;
    PathData before_path;
  };
  std::vector<ScaleSnap> m_scale_snaps;

  // Image transform snapshots — parallel to m_scale_snaps for Image nodes
  struct ImageTransformSnap {
    SceneNode *obj;
    double orig_x, orig_y, orig_w, orig_h;
    Transform orig_transform;
  };
  std::vector<ImageTransformSnap> m_image_transform_snaps;

  // Rotate drag — cursor doc position at drag-start, used to compute
  // angle delta each frame against the live (possibly Alt-toggled) pivot.
  double m_rotate_start_dx = 0.0;
  double m_rotate_start_dy = 0.0;
  double m_last_rotate_angle_deg =
      0.0; // set during rotate drag, read at commit

  // Skew / scale intent resolution for edge-midpoint handles (N/S/E/W).
  // Intent is ambiguous until the cursor moves >= SKEW_INTENT_PX from the
  // drag-start position. Once locked, m_skew_intent_locked = true and
  // m_skew_is_skew selects skew vs scale for the rest of the drag.
  // Both are cleared when m_handle_drag resets to None.
  bool m_skew_intent_locked = false;
  bool m_skew_is_skew = false;
  double m_skew_start_dx = 0.0; // doc position at edge-mid drag start
  double m_skew_start_dy = 0.0;

  // ── Guide drag state ──────────────────────────────────────────────────────
  // Set when a guide is being dragged — either newly created from ruler or
  // repositioned from canvas. m_guide_drag_node is the live SceneNode*.
  SceneNode *m_guide_drag_node = nullptr;
  bool m_guide_drag_active = false;

  // Guide hover hit-test — set in on_motion, used for cursor and drag-start
  SceneNode *m_guide_hovered = nullptr;

  // Eyedropper tool — object under cursor and its sampled colour
  SceneNode *m_eyedropper_hovered = nullptr;
  FillStyle m_eyedropper_hovered_color; // colour that would be picked
  // S66 — Color System Phase 3 loupe state.
  // Buffer mode (Shift-held) renders a small region of the canvas into
  // m_loupe_surface and samples its centre pixel. Hit-test mode uses
  // m_eyedropper_hovered_color (preserves FillStyle semantics including
  // CurrentColor). See Canvas.cpp draw_eyedropper_loupe for render logic.
  Cairo::RefPtr<Cairo::ImageSurface> m_loupe_surface; // magnified source pixels
  int m_loupe_sample_w = 0;         // source-pixel width of m_loupe_surface
  int m_loupe_sample_h = 0;         // source-pixel height of m_loupe_surface
  double m_loupe_buffer_r = 0.0;    // centre-pixel RGBA (what click commits)
  double m_loupe_buffer_g = 0.0;
  double m_loupe_buffer_b = 0.0;
  double m_loupe_buffer_a = 1.0;
  bool m_loupe_buffer_valid = false; // true only while Shift held + surface fresh

  // Corner Treatment tool state
  struct CornerSel {
    SceneNode *obj;
    int node_idx;
  };
  std::vector<CornerSel> m_corner_selection; // selected corner/cusp nodes

  bool corner_sel_contains(SceneNode *obj, int idx) const;
  // Rubber-band state (reuses m_drawing / m_draw_start / m_draw_cur naming)
  bool m_corner_rubber_active = false;
  double m_corner_rubber_x0 = 0, m_corner_rubber_y0 = 0; // screen space
  double m_corner_rubber_x1 = 0, m_corner_rubber_y1 = 0;
  // Tool params — persist for the session
  CornerType m_corner_type = CornerType::Round;
  double m_corner_radius = 8.0;

  // Corner tool handlers
  void on_corner_begin(double x, double y);
  void on_corner_motion(double x, double y);
  void on_corner_end(double x, double y);
  void draw_corner_tool_overlay(const Cairo::RefPtr<Cairo::Context> &cr);

  // Guide construct tool overlay
  void draw_guide_construct_overlay(const Cairo::RefPtr<Cairo::Context> &cr);

  // ── Ruler tool state ─────────────────────────────────────────────────────
  // Two selected nodes being measured (pointers into scene; may be null)
  SceneNode *m_ruler_node_a_obj = nullptr;
  int m_ruler_node_a_idx = -1;
  SceneNode *m_ruler_node_b_obj = nullptr;
  int m_ruler_node_b_idx = -1;
  // Marquee selection reuses m_marquee_active / m_marquee_start/cur fields
  // Toast for clipboard feedback
  std::string m_ruler_toast_text;
  double m_ruler_toast_x = 0.0;
  double m_ruler_toast_y = 0.0;
  int m_ruler_toast_ms = 0; // countdown in ms; 0 = hidden
  sigc::connection m_ruler_toast_conn;

  // Per-frame hit-test rects for click-to-copy
  struct RulerLabel {
    std::string copy_value;   // what gets copied on single click
    std::string display_text; // what is drawn
    double sx, sy, sw, sh;    // screen-space bounding rect
  };
  mutable std::vector<RulerLabel> m_ruler_labels;

  void on_ruler_begin(double x, double y);
  void on_ruler_motion(double x, double y);
  void on_ruler_end(double x, double y);
  // ruler_place_measurement() is public (called from MainWindow on Enter)
  void ruler_try_inherit_node_selection(); // called on tool switch
  // S89: auto-save on completion when m_doc->measure_save_to_layer is ON.
  // Called from the user-initiated paths that produce a fresh {A,B} pair
  // (shift+click promote in on_ruler_begin, marquee-with-2 in on_ruler_end).
  // NOT called from ruler_try_inherit_node_selection — inheritance from
  // node-tool selection is not a user completion event. On save the live
  // A/B is cleared (acts like an implicit Space) — the persistent overlay
  // takes over and renders the saved entry with the same triangle + full
  // structured annotations the live overlay was showing, so the user sees
  // visual continuity. Returns true if a measurement was saved.
  bool ruler_try_auto_save();
  // s182 m4 — show a toast at the given screen-space position. Used both
  // by the click-to-copy success path ("Copied measurement data") and by
  // the no-candidate rejection path ("No measurement point at this
  // location"). Same green-pill style; same 1500ms TTL with 50ms tick.
  void ruler_show_toast(const std::string &text, double screen_x,
                        double screen_y);
  // S89 endpoint-kinds: an endpoint is either a path node or a refpt.
  //   kind=Node: obj is a Path SceneNode, idx is the BezierNode index.
  //   kind=Ref:  obj is a Ref SceneNode, idx is unused (-1 by convention).
  // Positions are returned in doc space, Y-down (raw canvas pixels) — the
  // same space BezierNode and ref_x/ref_y already live in. User-space
  // (Y-up) conversion happens once at save-time in ruler_place_measurement.
  // Returns false if the SceneNode isn't a valid endpoint kind.
  bool ruler_endpoint_pos(SceneNode *obj, int idx,
                          double &x_doc, double &y_doc) const;
  // Collects all visible, unlocked endpoints across all layers — both
  // path nodes (kind=Node) and refpts (kind=Ref). Refpts are emitted
  // with idx = -1. Renamed from ruler_collect_all_nodes in S89 when
  // refpts joined the candidate set.
  void
  ruler_collect_all_endpoints(std::vector<std::pair<SceneNode *, int>> &out)
      const;
  // Path-node-only collector for non-ruler callers (guide-construct snap,
  // selection node-snap, hover highlight) that read obj->path->nodes
  // directly and would crash on a refpt entry. Same set as the pre-S89
  // ruler_collect_all_nodes.
  void
  ruler_collect_all_path_nodes(std::vector<std::pair<SceneNode *, int>> &out)
      const;
  // Returns the structured copy block for the current live A/B pair.
  // Thin wrapper over format_structured_block_for after resolving the
  // endpoints to user-space coords.
  std::string ruler_structured_block() const;
public:
  // S89: structured copy block formatter — takes user-space coords
  // explicitly so it can be called for both live and persistent (saved)
  // measurements. Returns the multi-line "x1 = ..., y1 = ...\n..." block
  // with values formatted in the doc's display unit. Public so the
  // inspector and layers panel can format their copy-button payloads
  // through the same code path that drives the on-canvas overlays.
  static std::string
  format_structured_block_for(const CurvzDocument *doc, double ax_user,
                              double ay_user, double bx_user, double by_user);
private:
  // S89: shared annotation render — draws hypotenuse + legs + endpoints
  // + the seven structured labels (distance / Δx / Δy / α / β at A and B)
  // plus a tiny coord tag at each endpoint. Used by both the live ruler
  // overlay and the persistent (saved) measurement overlay so they look
  // identical on screen. push_labels: when true, each label is appended
  // to m_ruler_labels with copy_value = the structured block, enabling
  // click-to-copy on saved entries during ruler mode.
  void draw_measurement_annotations(const Cairo::RefPtr<Cairo::Context> &cr,
                                    double ax_user, double ay_user,
                                    double bx_user, double by_user,
                                    bool push_labels);

  // ── Text-on-Path tool state ───────────────────────────────────────────────
  // Phase 0 = waiting for text pick
  // Phase 1 = text selected, waiting for path pick
  // Phase 2 = linked, may drag offset handle
  int m_top_phase = 0;
  SceneNode *m_top_text = nullptr;
  SceneNode *m_top_path_node = nullptr;
  bool m_top_dragging = false;
  double m_top_drag_start_off = 0.0;
  double m_top_drag_start_x = 0.0;
  double m_top_drag_start_y = 0.0;

  void on_top_begin(double x, double y);
  void on_top_motion(double x, double y);
  void on_top_end(double x, double y);
  void on_top_rclick(double x, double y);
  void draw_top_overlay(const Cairo::RefPtr<Cairo::Context> &cr);
  bool is_top_guide_path(const SceneNode &node) const;
  SceneNode *top_pair_partner(SceneNode *node) const;
  bool top_compute_detach_position(const SceneNode &tn, double &out_x,
                                   double &out_y) const;
  SceneNode *top_find_path_by_id(const std::string &id) const;
  void on_pivot_dialog(double doc_x, double doc_y);
  double build_arc_table(const BezierPath &bp,
                         std::vector<double> &arc_table) const;
  bool path_point_at(const BezierPath &bp, const std::vector<double> &arc_table,
                     double total_len, double arc_offset, Vec2 &pos,
                     double &angle) const;

  // Guide selection — multi-select set (mirrors m_selection for objects)
  std::vector<SceneNode *> m_guide_selection;
  GuideSelectionChangedSignal m_sig_guide_selection_changed;

  // Press position used to distinguish guide click from guide drag
  double m_guide_press_x = 0.0;
  double m_guide_press_y = 0.0;
  static constexpr double GUIDE_DRAG_THRESHOLD_PX = 4.0;

  // s180 m1: pre-drag snapshot for GuideMoveCommand undo capture. Recorded
  // when the press lands on a guide (on_select_begin); used at drag-end to
  // build the before/after deltas that feed the command. Mirrors
  // m_ref_drag_orig_x/y for refpts.
  double m_guide_drag_orig_x = 0.0;
  double m_guide_drag_orig_y = 0.0;
  double m_guide_drag_orig_angle = 0.0;

  double m_snap_bias_x = 0.0; // unused, kept for ABI compat
  double m_snap_bias_y = 0.0;

  // Hysteresis snap state — per axis locked guide position (NaN = no lock)
  // mutable so snap_move can update them while called from const contexts
  bool m_snap_x_locked = false;
  double m_snap_locked_x = 0.0;
  bool m_snap_y_locked = false;
  double m_snap_locked_y = 0.0;

  // ── Helper: hit-test the 8 BBX handles in screen space ───────────────
  // Returns HandleKind::None if (sx,sy) is not over any handle.
  // HANDLE_HIT_PX is the square half-size used for hit detection.
  static constexpr double HANDLE_HIT_PX = 7.0;
  HandleKind handle_hit_test(double sx, double sy) const;

  // ── Modifier state (Alt, Shift) ──────────────────────────────────────
  bool m_mod_alt = false;
  bool m_mod_ctrl = false;

  // ── View modes ───────────────────────────────────────────────────────
  bool m_outline_mode = false;

  // ── Boolean-op engine selector (REMOVED in S93 m7) ───────────────────
  // Boolean ops are now permanently routed through Clipper2. The
  // hand-rolled Sederberg-Nishita engine (math/BooleanOps.cpp) is
  // retained on disk for reference — see header banner there — but is
  // no longer reachable from the running app: no flag, no action, no
  // menu, no dispatch branch.

  // ── Boolean-op cleanup quality (s143 m1) ─────────────────────────────
  // Replaces the s139 m2 boolean_cleanup_enabled bool with an int 0..10
  // quality knob. Canvas::boolean_op maps it to the two cleanup_loop_v4
  // tunables (apex_min_turn_deg, max_untagged_run) — see the mapping
  // at the top of Canvas.cpp's boolean_op implementation.
  //
  //   q = 0   → most aggressive cleanup  (apex=30°, max_run=20)
  //   q = 5   → default                  (apex=15°, max_run=10  — s142 m6)
  //   q = 10  → no cleanup, raw Clipper2 polyline (algorithm bypassed)
  //
  // q=10 is the "off" position: enrichment skipped, cleanup_loop_v4
  // not called, Clipper2 output passed through verbatim.  Higher q
  // means more nodes preserved; q=10 preserves all of them by not
  // running the algorithm at all.
  //
  // Persisted via AppPreferences::boolean_cleanup_quality. MainWindow
  // keeps this synced on startup and on every signal_changed emission
  // from AppPreferences (the inspector's slider drives the pref; the
  // pref drives this field).
  int m_boolean_cleanup_quality = 5;

public:
  void toggle_outline_mode() {
    m_outline_mode = !m_outline_mode;
    m_sig_outline_mode_changed.emit();
    queue_draw();
  }
  bool is_outline_mode() const { return m_outline_mode; }
  // s113 m3: outline-mode change signal — see m_sig_outline_mode_changed
  // declaration. Connected by MainWindow to sync the action checkmark
  // and statusbar mode label.
  sigc::signal<void()> &signal_outline_mode_changed() {
    return m_sig_outline_mode_changed;
  }

  // s143 m1: boolean-op cleanup quality setter (replaces the s139 m2
  // bool toggle). MainWindow calls this on startup (from the loaded
  // AppPreferences value) and on every AppPreferences::signal_changed
  // emission. Cheap setter — no signal, no redraw; the value is read
  // only at the next boolean_op invocation. Range 0..10; out-of-range
  // values are clamped here so callers can't accidentally pass a slider
  // mid-drag value out of band.
  void set_boolean_cleanup_quality(int v) {
    if (v < 0)  v = 0;
    if (v > 10) v = 10;
    m_boolean_cleanup_quality = v;
  }
  int boolean_cleanup_quality() const { return m_boolean_cleanup_quality; }


  // s113 m2: preview-mode safety threshold.
  //
  // In preview/normal mode, drop-shadow rendering allocates a Cairo buffer
  // sized to the doc bbox in *device pixels* (doc_size × m_zoom). At
  // extreme zoom the allocation grows unbounded and crashes the app. The
  // hazard is concentrated on the outline→preview *transition* (a cliff,
  // not a ramp), so the gate is on the toggle, not on zoom.
  //
  // Empirical measurement (s113): on a 1000² doc the comfortable preview
  // ceiling is ~50× zoom (50,000 device pixels). Threshold set at 40,000
  // for one-click safety margin. Bigger docs hit the ceiling at
  // proportionally lower zoom — correct, since the hazard is pixel-count.
  //
  // Returns true if entering preview at the current zoom is safe.
  // Always true when already in preview (we don't second-guess the
  // current state) or when there's no document.
  static constexpr double PREVIEW_SAFE_DEVICE_PIXELS = 40000.0;
  bool preview_safe_at_current_zoom() const {
    if (!m_doc) return true;
    if (!m_outline_mode) return true; // already in preview, no transition
    const double dw = static_cast<double>(m_doc->canvas_width()) * m_zoom;
    const double dh = static_cast<double>(m_doc->canvas_height()) * m_zoom;
    return std::max(dw, dh) <= PREVIEW_SAFE_DEVICE_PIXELS;
  }

  // Origin drag preview — full-screen dashed crosshair during corner drag
  void set_origin_preview(double user_x, double user_y) {
    m_origin_preview = true;
    m_origin_preview_ux = user_x;
    m_origin_preview_uy = user_y;
    queue_draw();
  }
  void clear_origin_preview() {
    m_origin_preview = false;
    queue_draw();
  }

private:
  bool m_origin_preview = false;
  double m_origin_preview_ux = 0.0;
  double m_origin_preview_uy = 0.0;

public:
  // True while a node drag or object move is actively in-flight,
  // including the pre-threshold window after mouse-down on a node/handle.
  bool is_dragging() const {
    return m_node_drag_started || m_moving ||
           m_handle_drag != HandleKind::None ||
           m_node_drag_kind != HitResult::Kind::None;
  }

private:
  bool m_mod_shift = false;

  // Scroll controller — kept as member so on_scroll can read event modifiers
  Glib::RefPtr<Gtk::EventControllerScroll> m_scroll_ctrl;
  // Pinch zoom — last scale for delta computation
  double m_pinch_last_scale = 1.0;

  // ── Pen tool state ───────────────────────────────────────────────────
  PenTool m_pen_tool;
  bool m_pen_closing = false;

  // ── SVG performer (s288 m2, renamed s291 m2) ─────────────────────────
  // Phantom-overlay animation engine that subscribes to the parse stream
  // of AnimatingSvgParser (via SvgEmitter) and drives anchor-by-anchor
  // beat construction. is_playing() gates the performer-overlay branch
  // in Canvas_draw.cpp's on_draw; when idle it costs nothing. Initialised
  // in the Canvas ctor with `this` so the performer can call queue_draw
  // per tick. See animation/SvgPerformer.hpp.
  animation::SvgPerformer m_svg_performer{this};

  // Line tool WIP state
  struct LineTool {
    std::vector<std::pair<double, double>>
        points;                    // placed anchor points in doc space
    double live_x = 0, live_y = 0; // current cursor position
    bool close_snap = false;       // cursor snapped to start point
    bool active() const { return !points.empty(); }
    void reset() {
      points.clear();
      close_snap = false;
    }
  } m_line_tool;

  // Ref tool state
  SceneNode *m_ref_hovered = nullptr;  // ref point under cursor
  SceneNode *m_ref_selected = nullptr; // ref point being moved
  double m_ref_drag_ox = 0.0;          // doc-space offset at drag start
  double m_ref_drag_oy = 0.0;
  // s168 m4: pre-drag position snapshot for the Ref-tool single-refpt
  // drag, so mouse-up can push a RefMoveCommand. Selection-tool drag
  // and nudge use m_ref_move_snaps for the same purpose; the Ref tool
  // doesn't go through that path because it only ever drags one refpt.
  double m_ref_drag_orig_x = 0.0;
  double m_ref_drag_orig_y = 0.0;

  // ── Text tool state ──────────────────────────────────────────────────
  // The entry is parented to m_text_fixed which the parent Overlay places
  // over the canvas.  MainWindow calls set_text_overlay() at startup.
  // s208 m5: m_text_entry flipped to curvz::widgets::Entry. Substrate
  // IS-A Gtk::Entry so all existing call sites (set_visible, add_css_class,
  // get_text, set_text, signal_changed, signal_activate, position_text_entry)
  // work unchanged. Forward-declared above to keep this header light;
  // Canvas_ops.cpp / Canvas_input.cpp include curvz/widgets/Entry.hpp.
  Gtk::Fixed *m_text_fixed = nullptr; // overlay container (owned by parent)
  curvz::widgets::Entry *m_text_entry =
      nullptr; // floating inline editor (owned by m_text_fixed)
  SceneNode *m_text_editing = nullptr; // node being edited (nullptr = none)
  bool m_text_is_new = false; // true = node was just created; cancel → delete
  bool m_text_has_snapshot = false; // true = m_text_snapshot is valid
  TextEditCommand m_text_snapshot;  // before-state for undo
  sigc::connection m_text_entry_conn_activate;
  sigc::connection m_text_entry_conn_changed;

  void on_text_begin(double sx, double sy); // click handler
  void position_text_entry(); // move entry widget to node's screen pos
  void draw_text_on_path(const Cairo::RefPtr<Cairo::Context> &cr,
                         const SceneNode &text_obj,
                         const SceneNode &guide_path);
  void draw_text_node(const Cairo::RefPtr<Cairo::Context> &cr,
                      const SceneNode &obj);

  // Continue-path state — set when pen resumes an existing open path
  SceneNode *m_continue_target = nullptr; // original object (already erased)
  PathData m_continue_before;             // snapshot for undo

  // ── Node editor state ────────────────────────────────────────────────
  int m_selected_node = -1;
  HitResult::Kind m_node_drag_kind = HitResult::Kind::None;
  PathData m_node_drag_before; // snapshot of primary path before drag
  std::vector<std::pair<SceneNode *, PathData>>
      m_node_drag_before_multi; // multi-node snaps

  // Secondary node selection — used for cross-path operations (join).
  // Set when Shift+click lands on a node belonging to a different path.
  // Cleared on plain click, tool switch, or after a join.
  SceneNode *m_selected2 = nullptr;
  int m_selected_node2 = -1;

  // Dead-zone: drag only starts after the mouse moves > threshold from press.
  static constexpr double NODE_DRAG_THRESHOLD_PX = 4.0;
  double m_node_press_x = 0.0;
  double m_node_press_y = 0.0;
  bool m_node_drag_started = false;

  // Endpoint snap — set during on_node_update when dragging an endpoint
  // near another path's endpoint. Cleared on drag end.
  // m_snap_target_obj  : the other open path we're snapping to (not owned)
  // m_snap_target_end  : 0 = head (node 0), 1 = tail (last node)
  SceneNode *m_snap_target_obj = nullptr;
  int m_snap_target_end = -1; // 0=head, 1=tail

  // Shift+click cycling
  Vec2 m_cycle_last_pos{};
  int m_cycle_index = 0;

  ZoomChangedSignal m_sig_zoom;
  ZoomAltChangedSignal m_sig_zoom_alt;
  CursorMovedSignal m_sig_cursor;
  SelectionChangedSignal m_sig_selection;
  DocumentChangedSignal m_sig_doc_changed;
  RequestToolSignal m_sig_request_tool;
  NodeChangedSignal m_sig_node_changed;
  ShowMessageSignal m_sig_show_message;
  EyedropperPickSignal m_sig_eyedropper_pick;
  CornerSelChangedSignal m_sig_corner_sel_changed;
  MeasurementsChangedSignal m_sig_measurements_changed;
  // s125 m1a: emitted from the canvas right-click context menu's
  // "Save to Library…" entry. See accessor banner above.
  RequestSaveToLibrarySignal m_sig_request_save_to_library;
  // s210 m1: emitted from the canvas right-click handler on an image
  // SceneNode. Payload pre-baked by Canvas; MainWindow's ImageInfoDialog
  // presents it. See accessor banner above.
  RequestImageInfoSignal m_sig_request_image_info;
  // s210 m2: emitted from on_pivot_dialog after right-click-while-R-held
  // places a custom pivot. Payload is the pivot doc-Y-down coords;
  // MainWindow's RotateFromPointDialog presents the dialog and routes
  // Apply back through Canvas::apply_rotate_from_point. See accessor
  // banner above.
  RequestRotateFromPointSignal m_sig_request_rotate_from_point;
  // s113 m3: emitted when outline mode flips for any reason (manual
  // toggle OR auto-engaged by the zoom-safety gate). MainWindow connects
  // this to sync the action checkmark and statusbar mode label so all
  // outline-mode change paths converge on a single sync point.
  sigc::signal<void()> m_sig_outline_mode_changed;
  // s205 m1: pivot state change notification — see signal_pivot_changed
  // accessor banner. Inspector pivot picker listens.
  PivotChangedSignal m_sig_pivot_changed;
};

} // namespace Curvz
