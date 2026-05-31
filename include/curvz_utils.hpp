// curvz_utils.hpp ─────────────────────────────────────────────────────────
//
// General-purpose static pump methods. One header, one .cpp, one
// namespace — utilities accumulate here rather than in one-off helper
// files. Compile is cheap; reuse is the win.
//
// Style: small composable units. Each function does one thing. Larger
// jobs compose these primitives rather than inlining the work.
//
// ── Current contents ─────────────────────────────────────────────────
//   sanitise_for_xml_id    name → safe XML-id token (no spaces/specials)
//   short_iid              first 8 chars of an iid for compact id use
//   encode_svg_id          (name, iid) → SVG id attribute value
//   count_anchors          recursive BezierNode count under a SceneNode
//   doc_anchor_count       sum of count_anchors across non-guide layers
//   doc_object_count       count of top-level visible objects in a doc
//   find_by_iid              project-level SceneNode lookup by internal_id
//                            (s167 m1 — id-based command captures pump)
//   force_unregister_subtree synchronous registry cleanup for a widget
//                            subtree about to be destroyed (s199 m1)
//   trim_heap                ask glibc to return free chunks to the OS
//                            (s269 m2 — no-op on non-glibc)
//   box_blur_argb32          in-place 3-pass box blur on ARGB32 pixels
//                          (Cairo premultiplied, ≈ Gaussian)
//   build_gradient_pattern FillStyle + bbox → Cairo::Pattern (linear/radial)
//   render_drop_shadow_under blur+composite a host_pat shadow under cr
//                            (offscreen surface + box blur + masked tint)
//   svg_d_to_path_data     SVG `d` attribute string → PathData
//                          (s236 m1 — bridges to SvgParser::parse_path_d
//                           via non-static wrapper; full lift deferred)
//   path_data_to_svg_d     PathData → SVG `d` attribute string
//                          (s236 m1 — full lift from SvgWriter inline
//                           emitters; writer keeps its copies for now)
//   path_data_user_to_doc  user-space PathData (Y-up, bottom-left origin)
//                          → doc-space PathData (Y-down, top-left origin)
//   path_data_doc_to_user  doc-space PathData → user-space PathData
//                          (s237 m1 — script-side user-space pump pair;
//                           internally uses CoordSpace, the canonical
//                           Y-flip seam. Mutates in place.)
//   fill_attr_to_fill_style  SVG fill= attribute string → FillStyle
//   fill_style_to_fill_attr  FillStyle → SVG fill= attribute string
//                          (s238 m1 — script-side fill pump pair; covers
//                           the four context-free fill types: None,
//                           CurrentColor, Solid (hex/named), and graceful
//                           degrade for gradient / unknown. Legacy file-
//                           statics in SvgParser/SvgWriter retain the
//                           full gradient-server branches for now.)
//
// ── SVG id contract ──────────────────────────────────────────────────
//
// The SVG `id` attribute is a derived, write-only handle from Curvz's
// perspective. Curvz's source of truth for a node is:
//
//   data-curvz-name = the human-readable name (round-trips verbatim)
//   data-curvz-iid  = the internal UUID (round-trips verbatim)
//
// The SVG `id` is constructed from those two so external consumers
// (browsers, foreign editors, <use> references, CSS selectors) get a
// readable, unique handle. Curvz never decodes it back — on load,
// SvgParser reads name from data-curvz-name and iid from data-curvz-iid
// directly. If those attributes are missing (foreign or legacy SVG),
// the parser generates a fresh name+iid; the foreign id is irrelevant.
//
// Encoded form: "<sanitised_name>¶<short_iid>"
//   - separator: U+00B6 PILCROW SIGN (UTF-8 0xC2 0xB6) — a key trap.
//                Stripped from names on encode, so it never appears in
//                the name half. Unique by construction.
//   - short_iid: first 8 hex chars of the full uuid. Compact and
//                effectively unique inside one SVG document; the full
//                iid lives in data-curvz-iid for actual identity.
// ─────────────────────────────────────────────────────────────────────

#pragma once

#include <cairomm/cairomm.h>
#include <functional>  // s125 m2a — std::function for callbacks
#include <gdkmm/pixbuf.h>  // s135 m2 — Gdk::Pixbuf for cairo_set_source_pixbuf pump
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <map>         // s125 m2a — result map for show_form
#include <string>
#include <variant>     // s125 m2a v2 — FormFieldSpec / FormFieldValue::Value
#include <vector>      // s125 m2a — buttons + fields lists

// Forward-declare to keep SceneNode.hpp out of this header. The pump
// signatures only need a const reference, so declarations alone suffice.
namespace Curvz {
struct FillStyle;
struct SceneNode;       // s132 m2 — count_anchors / count_objects pumps
struct CurvzDocument;   // s132 m2 — document-level wrappers
struct CurvzProject;    // s167 m1 — project-level find_by_iid wrapper
struct PathData;        // s146 m2 — warp_presets pump signatures
struct AttrSpan;        // s326 m2 — per-run formatting span pump signatures
}

namespace curvz::utils {

// ── Dialog motif inheritance ────────────────────────────────────────
// Dialogs are separate top-level GTK windows from MainWindow, so the
// motif class on MainWindow doesn't propagate. Each dialog calls this
// after `set_transient_for(parent)` and before `present()`. It checks
// whether the parent window already wears the `curvz-light` class
// (true iff the project motif is Light), and if so, copies the class
// onto the dialog. Setting the class before first present means the
// dialog body resolves the light token block on its very first style
// pass — no dark-then-flip flash.
//
// Inheriting via the parent window (rather than a project pointer)
// means sub-dialogs spawned from another dialog also work: their
// parent is the parent dialog (which already wears the class from
// its own apply call), so the class chains through correctly.
//
// Idempotent: if the parent doesn't have the class, nothing happens.
// Calling on an already-classed dialog is a no-op (add_css_class is
// set-membership). S117 m18 v2.
inline void apply_motif_class_from_parent(Gtk::Window& dlg, Gtk::Window& parent) {
    if (parent.has_css_class("curvz-light"))
        dlg.add_css_class("curvz-light");
}

// ── set_name ─────────────────────────────────────────────────────────
// Tag a widget with a short abbrev (visible in GTK Inspector's Name
// column) and a long descriptive name (compile-time documentation).
//
// Only the abbrev is stored on the widget — the long name lives in
// the call site as a self-documenting argument and is harvested by
// `widget_names_sync` to regenerate `widget_names.db`. Source code is
// the single source of truth: the DB is a generated artefact.
//
// Calling this rather than Gtk::Widget::set_name() directly is the
// trap-character convention that lets the sync script grep for
// curvz::utils::set_name unambiguously, with no risk of confusion
// with Gtk::FileFilter::set_name() and other unrelated set_name calls.
//
// Usage:
//   curvz::utils::set_name(*this, "mw", "main_window_root");
//   curvz::utils::set_name(btn,   "tb_mb", "main_toolbar_macro_btn");
//
// Both reference and pointer overloads exist so call sites stay clean
// regardless of whether the widget is an automatic, *this, or a raw
// pointer from make_managed.
inline void set_name(Gtk::Widget& w, const char* abbrev,
                     const char* /*long_name — registry annotation*/) {
    w.set_name(abbrev);
}

inline void set_name(Gtk::Widget* w, const char* abbrev,
                     const char* /*long_name — registry annotation*/) {
    if (w) w->set_name(abbrev);
}

// Sanitise a human name into an XML-id-legal token.
//
//   - Allow ASCII letters, digits, '-', '_', '.'
//   - Replace whitespace runs with single '_'
//   - Drop the pilcrow separator (key trap)
//   - Drop everything else (specials, Unicode letters)
//   - If result is empty or starts with a digit/'-'/'.', prepend '_'
//
// Lossy on purpose. The original name is preserved in data-curvz-name;
// this token only needs to be stable, unique-ish, and XML-id-legal.
std::string sanitise_for_xml_id(const std::string& name);

// Compact form of an iid for use in an SVG id. Returns the first 8
// characters of iid, or the full iid if shorter than 8. Empty in →
// empty out.
std::string short_iid(const std::string& iid);

// Compose an SVG id from a human name and an internal iid.
//
//   ("Layer 1", "12345678-1234-...") → "Layer_1¶12345678"
//   ("",        "12345678-1234-...") → "12345678"
//   ("Layer 1", "")                  → "Layer_1"
//   ("",        "")                  → ""
std::string encode_svg_id(const std::string& name,
                          const std::string& iid);

// Turn a display name like "Untitled - Default" into a clean SVG filename
// stem like "Untitled-Default" (preserving case). Runs of non-alphanumeric
// characters collapse to a single '-'; leading/trailing dashes are stripped.
// Returns "icon" if the result would be empty.
//
// s164: hoisted from MainWindow.cpp's static helper. Used from the new-doc
// flow (on_new and the bindings file's two NDD slots) to derive a filename
// stem from the user-visible doc name. Pure string→string transform with no
// dependencies on Curvz types — belongs in utils.
std::string doc_stem_from_name(const std::string& raw);

// ── Document counting pumps (s132 m2) ────────────────────────────────
// Single source of truth for the StatusBar's "N objects · N nodes"
// readout. MainWindow had five copies of "iterate doc.layers and sum
// children.size()" with `nodes=0` hardcoded — same hand-rolled walk
// duplicated across selection, undo, redo, doc-active-changed, and
// doc-changed handlers. These pumps replace all five with a recursive
// walk that handles Compound, Group, ClipGroup, Blend, Warp containers
// and ignores Guide / GridLayer / MarginLayer / RefLayer / MeasureLayer.
//
// "Anchor" = BezierNode entry on a Path. Compound / Group / ClipGroup
// recurse into children. Text / Image / Ref / Guide / Measurement do
// not contribute — they are not paths in the editing-surface sense
// the StatusBar means by "nodes". Matches what node-count means in
// every vector editor (anchor points, not glyph runs or pixel corners).
//
// "Object" = a top-level direct child of a non-guide layer. Same
// definition the existing duplicated loops used; now centralised.
// Groups / compounds count as a single object regardless of children
// (a 100-path group is still "one object" to the user).

// Recursively count BezierNode anchors under any SceneNode.
//   • Path                   → path->nodes.size() if path is non-null
//   • Compound, Group,
//     ClipGroup, Blend, Warp → recurse over children
//   • Anything else          → 0
int count_anchors(const Curvz::SceneNode& n);

// Sum of count_anchors across every non-guide / non-overlay layer in
// a document. Skips GuideLayer, GridLayer, MarginLayer, RefLayer,
// MeasureLayer — those are scaffolding, not user content.
int doc_anchor_count(const Curvz::CurvzDocument& doc);

// Number of top-level user-visible objects across non-overlay layers.
// "User-visible" here means "what shows in the Layers panel as a row" —
// direct children of regular Layer nodes, regardless of node count.
int doc_object_count(const Curvz::CurvzDocument& doc);

// ── Doc normalization pump (s292 m3) ─────────────────────────────────
// Scale every path/text in `doc` so the document fits inside
// (target_w × target_h), uniform-scaled by the longest axis. Reassigns
// `doc.canvas` to a fresh Pixel-mode CanvasModel at the scaled dims.
//
// Use cases:
//   * import_svg_impl's normalize_to_1000 icon workflow — call with
//     (1000, 1000) to rescale a foreign SVG so its longest axis is 1000
//     working units (lossless icon-design canvas).
//   * SvgPerformer::perform — call with the user's active doc canvas
//     dims so a foreign SVG fits inside the target before plan-build.
//     Without this the performer draws at SVG-native coords, which can
//     blow out of the target canvas (scott-bug: native 1000×1000 doc
//     animated into a 300×300 canvas).
//
// What gets scaled: path anchor coords + handles, text x/y/font-size,
// recursing into n.children. Matches import_svg_impl's pre-s292 scaler
// scope verbatim — composite slots (clip_shape, blend_source_a/b,
// warp_source) and Image/Ref geometry are NOT walked; that's a
// pre-existing gap banked for a future focused milestone.
//
// Returns the scale factor applied (1.0 if doc already fits, < 1.0 if
// shrunk, > 1.0 if grown). Returns 1.0 and is a no-op when target dims
// are non-positive or the doc has degenerate dims.
double normalize_doc_for_target(Curvz::CurvzDocument& doc,
                                int target_w, int target_h);

// ── Iid resolver pump (s167 m1) ──────────────────────────────────────
// Project-level lookup of a SceneNode by internal_id (the stable UUID).
// Walks every document in the project, returning the first hit.
//
// UUIDs are globally unique by construction (RFC 4122 random; see
// SceneNode::generate_internal_id), so "first hit" and "only hit" are
// the same thing in practice — the walk stops as soon as a doc's
// find_by_iid returns non-null.
//
// Use case: id-based command captures (s167 stages 0-6). A command
// stores the iid of the node it edits; at execute/undo time it calls
// this to resolve the iid back to a live pointer, early-returning if
// the node has been destroyed since capture. Replaces the dangling-
// SceneNode*-in-undo-stack class of bug.
//
// Performance: each doc's find_by_iid is O(1) when its index is fresh,
// O(N) the first time after a structural mutation (rebuild). Project
// walk is O(D) where D = number of docs (typically <10). Microseconds
// at our scale.
//
// Returns nullptr if iid is empty or no document contains a match.
Curvz::SceneNode* find_by_iid(const Curvz::CurvzProject& proj,
                              const std::string& iid);

// ── Per-run formatting span pump (s326 m2) ───────────────────────────
// The flat-span apply seam from text_formatting_design.md §1/§7. A
// TextBoxMgr's text_attr_spans is a flat, overlapping bag of per-attribute
// ranges (a mirror of PangoAttrList). These pumps mutate that bag for one
// attribute type over one byte range [a, b) into text_content, with the
// pango_attr_list_change semantics expressed in the flat model:
//
//   - SAME type is exclusive over a byte: setting weight over [a,b) clips
//     any existing weight span in that window (split left/right, drop the
//     middle) before inserting the new one — a byte can't hold two weights.
//   - OTHER types are independent: weight, style, colour, size overlap
//     freely; the pump never touches a type it wasn't asked about.
//
// All three operate purely on the span vector + byte offsets (offsets are
// absolute into text_content, the cursor's own coordinate system — the
// selection_range() pair drops straight in, no rebasing). Round-trip and
// render are already banked (s325), so these are the only new logic on the
// edit path. Ranges with a >= b are no-ops.

// True iff EVERY byte in [a,b) is covered by a span of `type` whose value
// matches (ivalue for integer/colour attrs, svalue for string attrs). This
// is the toggle predicate: "is the whole selection already bold?" Empty
// range (a >= b) returns false (nothing to be "everywhere true" over).
bool range_has_attr(const std::vector<Curvz::AttrSpan>& spans,
                    int type, long ivalue, const std::string& svalue,
                    unsigned a, unsigned b);

// Remove `type` over [a,b): clip every span of that type to the parts
// OUTSIDE the range (keep [s,a) and [b,e) slivers, drop the overlap). The
// range becomes un-attributed for `type` and inherits the node default.
void clear_attr_over_range(std::vector<Curvz::AttrSpan>& spans,
                           int type, unsigned a, unsigned b);

// Set `type` = (ivalue/svalue) over [a,b): clear that type in the range,
// then insert one span carrying the value. Adjacent same-value spans of
// the type are merged so the list doesn't fragment as edits churn it.
void set_attr_over_range(std::vector<Curvz::AttrSpan>& spans,
                         int type, long ivalue, const std::string& svalue,
                         unsigned a, unsigned b);

// The B/I/U entry: if [a,b) is already entirely (type==value), clear it;
// otherwise set it. Returns the resulting on/off state (true = now set).
// Composed from the three above.
bool toggle_attr_over_range(std::vector<Curvz::AttrSpan>& spans,
                            int type, long ivalue, const std::string& svalue,
                            unsigned a, unsigned b);

// ── Layer-index resolver (s171 m1) ───────────────────────────────────
// Layers live at the top of the document tree (`doc->layers`) and are
// identified by index for insertion / erasure / reorder operations.
// `find_by_iid` returns a SceneNode* but doesn't tell us where the layer
// sits in the layers vector — and walking `children` to find it would
// be wrong (groups don't live under layers' children, layers live
// directly under the doc). This pump answers "which slot in
// doc->layers holds the layer with this iid?"
//
// Use case: layer-undoable command bodies (s171). Add/Delete/Reorder
// commands need vector indices to insert and erase by position; they
// store layer iids and resolve to indices at apply time.
//
// Returns the index in the supplied document's `layers` vector, or -1
// if iid is empty or no top-level layer in this doc matches. Does NOT
// walk into layer children — only matches against `doc->layers[i]->
// internal_id`. Linear scan (top-level layer count is small, no index
// caching needed).
//
// Caller chooses the doc — typically `proj.active_doc()` or a specific
// CurvzDocument*. Project-wide resolution doesn't fit here because the
// returned index is doc-relative.
int find_layer_index_by_iid(const Curvz::CurvzDocument& doc,
                            const std::string& iid);

// ── Doc-by-iid resolver (s171 m1) ────────────────────────────────────
// Project-level scan: which document in this project contains the
// SceneNode with the given iid? Returns a non-owning pointer to the
// CurvzDocument, or nullptr if iid is empty / no document contains it.
//
// Use case: layer-undoable commands need to know which doc to mutate
// without storing a raw `CurvzDocument*` (which would dangle if the
// user closes the doc between push and undo). The command stores an
// iid that's expected to live in the target doc — typically a sibling
// layer's iid as an "anchor" — and resolves to the live doc at apply
// time. If the anchor's gone too (whole doc closed), the command
// no-ops cleanly, same partial-recovery shape as Cut/Duplicate.
//
// Linear over docs and per-doc index lookup; documents are few (<10
// typical) and the per-doc lookup is O(1) when index is fresh.
Curvz::CurvzDocument* find_doc_by_iid(const Curvz::CurvzProject& proj,
                                      const std::string& iid);

// ── Doc-tree helpers (s275 m12 — hoisted from LayersPanel.cpp) ───────
//
// These three helpers were file-statics in LayersPanel.cpp's anonymous
// namespace, used by the row-context-menu wiring (s274 m11). The s275
// m12 work brings the same Move-to-layer ▸ verb to the canvas right-
// click menu — which means two call sites now want the same answers
// from the same shape of question, so the helpers move here.
//
// Why now rather than during s274 m11: only one consumer existed at
// that point, so anonymous-namespace placement was correct (locality
// over premature generalisation). The second consumer is the seam
// that justifies the lift.

// Walk the doc tree to find the immediate parent of `target`.
//
//   - Returns nullptr if `target` is a top-level layer child (i.e. it
//     sits directly in some `doc->layers[i]->children`).
//   - Returns nullptr if `target` is not found anywhere under any
//     container in the doc.
//   - Otherwise returns a non-owning pointer to the container SceneNode
//     (Group / Compound / ClipGroup / Blend / Warp) that holds it.
//
// Use case: gating "Move to layer ▸" — only top-level objects can move
// directly between layers; nested objects must be released from their
// container first.
//
// Performance: recursive descent through the container types listed
// above. The recursion never enters non-container SceneNodes, so the
// walk visits each container once.
Curvz::SceneNode* find_object_parent(Curvz::CurvzDocument* doc,
                                     Curvz::SceneNode* target);

// Return the display name of a layer for menu/UI labels.
//
//   - If `layer.name` is non-empty, returns that.
//   - Else if `layer.id` is non-empty, returns that.
//   - Else returns "Layer N" where N = idx + 1 (1-based).
//
// Mirrors the fallback chain in the LayersPanel row renderer, so a
// menu label and the on-panel row label agree on what to call a
// layer that the user hasn't bothered to name.
std::string layer_display_name(const Curvz::SceneNode& layer, int idx);

// Is this layer a valid Move-to-layer destination for ordinary objects?
//
//   - Special layers (Guide / Grid / Margin / Ref / Measurement) are
//     never targets — they hold typed contents that ordinary objects
//     don't belong in.
//   - Locked layers are never targets — locked is locked.
//
// Centralising this here means the canvas right-click menu and the
// LayersPanel right-click menu compute eligibility identically; a
// future tightening (e.g. excluding hidden layers) lands in one place.
bool is_ordinary_target_layer(const Curvz::SceneNode& layer);

// ── iid_breadcrumb (s175 m3) ─────────────────────────────────────────
// Resolves an iid to a human-readable "Layer Name → Node Name" string
// suitable for inspector labels and any other place where users would
// otherwise see a raw UUID.
//
// Why this exists: iids are stable UUIDs (good for code), but exposing
// them in UI labels is a UX leak — users see things like
// "e0677496-dfb3-4001-810a-97050f9c7aa0" where they expect a name.
// First user-visible case: the Text-on-Path inspector row, which used
// to render `text_path_id` directly as the label text.
//
// Format: "<Layer Name> → <Node Name>". If the node is nested under a
// group inside a layer, intermediate group names are skipped — the
// breadcrumb shows only the owning layer and the leaf node, matching
// what the LayersPanel surfaces for hierarchy at a glance. (Today the
// only UI that consumes this is text-on-path, where group nesting
// isn't reachable; the layer-only walk handles future cases too.)
//
// Returns:
//   - "<Layer> → <Name>" on a clean resolve
//   - "—" if iid is empty (treat as "no link")
//   - "(unresolved)" if iid is non-empty but doesn't match any node
//     in the project (e.g. linked path was deleted; the link is stale
//     but the value is preserved for round-trip)
//
// Never returns the raw iid — that's the bug this helper exists to
// prevent. If callers want the iid (e.g. for diagnostic logging),
// they should use the iid string directly, not this label.
std::string iid_breadcrumb(const Curvz::CurvzProject& proj,
                           const std::string& iid);

// ── force_unregister_subtree (s199 m1) ───────────────────────────────
// Walk a widget subtree and synchronously force-unregister every
// curvz::scripting::Scriptable in it. Lifted from PropertiesPanel::
// do_clear's inline lambda (s191 m7); generalised here so any caller
// that's about to destroy a subtree containing substrate widgets can
// do the registry cleanup synchronously, before GTK4's deferred
// idle-priority widget destruction runs.
//
// Use cases:
//   1. Inspector self-rebuild (PropertiesPanel::do_clear) — the
//      original s191 m7 site, now delegating to this pump.
//   2. Heap-allocated dialogs that self-delete on close
//      (ThemeEditDialog, StyleEditorDialog, ManageTemplatesDialog,
//      NewDocumentDialog). The dialog's signal_close_request handler
//      calls this on the dialog's top-level window BEFORE scheduling
//      idle-delete; on the next open, the registry is clean and the
//      new substrate ctors don't collide with the soon-to-be-destroyed
//      old widgets.
//
// The walk is recursive: Scriptable widgets can contain non-Scriptable
// children which can contain more Scriptable widgets. Each Scriptable
// encountered gets unregister'd synchronously; its dtor's unregister
// is idempotent (registry::erase on missing name is a no-op) so the
// eventual GTK-driven destruction is safe.
//
// Null-tolerant: passing nullptr is a no-op. Safe to call after a
// widget has already been removed from its parent (no children → no
// walk).
//
// This is the structural fix for the "two simultaneously-live
// Scriptables under the same name" hazard that the registry's
// throw-on-duplicate enforces — same shape every time:
// deferred-destruction of the old leaves the registry entry alive
// past when the new tries to register.
void force_unregister_subtree(Gtk::Widget* root);

// ── trim_heap (s269 m2) ──────────────────────────────────────────────
// Ask the C allocator to return free chunks to the OS. Wraps glibc's
// malloc_trim(0); no-op on non-glibc platforms.
//
// Background: glibc malloc retains freed chunks in its own free lists
// for reuse and almost never releases them back to the OS. RSS climbs
// to the high-water mark of what has ever been allocated, not what is
// currently live. Bursty mixed-size allocation (e.g. fast Ctrl+Tab
// through documents, where each switch allocates Cairo surfaces,
// shadow blur intermediaries, and gradient pattern surfaces of varying
// sizes) fragments the heap and RSS stays climbed even though most
// chunks are free.
//
// malloc_trim(0) walks the free lists and madvise(MADV_DONTNEED) or
// munmap's what it can. Microseconds to low milliseconds typically.
// Safe — does not free any live memory; glibc only acts on chunks that
// are already free.
//
// Threading note: NOT safe to run on a worker thread. malloc_trim
// holds glibc's arena mutexes for the duration of the walk; any
// concurrent allocation/free on another thread blocks behind the same
// mutex. The right "make it cheap" lever is a low-priority GTK idle
// (Glib::PRIORITY_LOW or below), not a thread — runs only when the
// main loop is otherwise quiet.
//
// Returns true if glibc reported releasing memory, false otherwise.
// First-deployment diagnostic: logs the returned flag and elapsed
// microseconds at LOG_INFO. Once we trust the call sites, the log can
// be demoted or removed.
bool trim_heap();

// ── box_blur_argb32 ──────────────────────────────────────────────────
// In-place blur of an ARGB32 (Cairo premultiplied, BGRA byte order on
// little-endian) image buffer. Approximates a Gaussian blur via three
// passes of a separable box filter — Wells (1986); also what most
// browser <feGaussianBlur> implementations do internally. The visual
// difference from a true Gaussian is below threshold for the radii
// used in drop shadows.
//
// Parameters:
//   data        — pointer to the ImageSurface's pixel buffer (from
//                 cairo_image_surface_get_data).
//   stride      — row stride in bytes (from get_stride). Cairo guarantees
//                 stride >= width * 4 and 4-byte alignment.
//   width, height — surface dimensions in pixels.
//   radius      — box-pass radius in pixels. 0 = no-op. Each pass averages
//                 (2*radius + 1) samples in each direction; three passes
//                 of radius r approximate a Gaussian with stddev sigma
//                 ≈ r * sqrt(3/4) ≈ 0.866*r. To target a desired sigma,
//                 caller passes radius = round(sigma / 0.866). For SVG
//                 stdDeviation parity, radius = round(stdDeviation).
//
// Premultiplied alpha invariant is preserved: averaging premultiplied
// channels yields the correct premultiplied result, no un/re-mul needed.
//
// The caller is responsible for calling Cairo::ImageSurface::flush()
// before this function (so any pending Cairo writes are visible in the
// buffer) and ::mark_dirty() after (so subsequent Cairo reads see the
// blurred bytes).
void box_blur_argb32(unsigned char* data, int stride,
                     int width, int height, int radius);

// ── build_gradient_pattern ────────────────────────────────────────────
// Build a Cairo gradient pattern from a FillStyle of type LinearGradient
// or RadialGradient, with endpoints lerped from objectBoundingBox-fraction
// space (0..1 of the shape's bbox) into doc-space using the supplied
// bbox. Mirrors Canvas::apply_fill(cr, fill, bbox)'s gradient branch so
// printers, exporters, or any other Cairo consumer can render a node's
// gradient identically to the canvas.
//
// Parameters:
//   fill            — must be LinearGradient or RadialGradient. Other
//                     types produce an empty RefPtr (caller checks).
//   bbox_x, bbox_y  — top-left of the shape's bbox in the same coordinate
//                     space as cr's current user-space.
//   bbox_w, bbox_h  — width/height of the shape's bbox in user-space.
//
// Returns: a refcounted pattern with stops added, or an empty RefPtr
// when fill isn't a gradient or has no stops. Caller is responsible for
// cr->set_source(pat) and the fill/stroke call that consumes it.
//
// Empty stops produces a fully transparent pattern (matches Canvas).
Cairo::RefPtr<Cairo::Pattern> build_gradient_pattern(
    const Curvz::FillStyle& fill,
    double bbox_x, double bbox_y, double bbox_w, double bbox_h);

// ── cairo_set_source_pixbuf ───────────────────────────────────────────
// Set a GdkPixbuf as the source pattern for a Cairo context, with the
// pixbuf's origin at (x, y) in cr's user space. Drop-in replacement for
// the deprecated gdk_cairo_set_source_pixbuf (deprecated GTK 4.20).
//
// s135 m2: Curvz had eight callsites of the deprecated function across
// thumbnail/preview render paths (Canvas anchor glyph + image objects,
// NewDocumentDialog, ManageTemplatesDialog, DocumentGallery, PngExporter,
// PrintManager). The official 4.20 migration path is texture-download-
// then-cairo, which adds an allocation and a copy per call. For Curvz's
// use case — small icon and thumbnail rasters, not video frames — a
// direct format conversion into a Cairo image surface is simpler, has
// no GTK-version dependency, and matches the per-call cost of the old
// function.
//
// Format conversion: GdkPixbuf is RGBA byte-order with straight (non-
// premultiplied) alpha. Cairo ARGB32 is BGRA byte-order in little-endian
// memory with premultiplied alpha. Per-pixel: swap R and B, multiply
// each colour channel by alpha/255. RGB pixbufs (no alpha) are filled
// with A=0xff and skip the premultiply step.
//
// The created Cairo surface owns a deep copy of the pixel data, so the
// caller's Pixbuf RefPtr is free to drop after the pump returns. This
// matches the contract of the deprecated function.
//
// Empty pixbuf or zero-dimensioned pixbuf: no-op.
void cairo_set_source_pixbuf(
    const Cairo::RefPtr<Cairo::Context>& cr,
    const Glib::RefPtr<Gdk::Pixbuf>& pb,
    double x, double y);

// ── render_drop_shadow_under ──────────────────────────────────────────
// Paint a tinted, blurred, offset shadow of host_pat onto cr, in the
// region described by host_bbox (doc-space). Reads CTM scale via
// user_to_device so it works in any Cairo context — canvas widget,
// print page, SVG export — without knowing the zoom factor up front.
//
// Pipeline (matches Canvas::render_shadow_under):
//   1. Pad host_bbox by blur reach + |dx|, |dy| + safety constant.
//   2. Map padded rect through cr's CTM → device pixels.
//   3. Allocate ARGB32 ImageSurface at those pixel dimensions.
//   4. Paint host_pat into surface with cr's CTM offset by surface origin
//      (matrix-correct so source pixels land at correct surface pixels).
//   5. Three-pass box blur in place (radius = round(blur_doc * ctm_scale)).
//   6. On cr at identity CTM, mask shadow colour through blurred alpha
//      at (surface_origin + shadow_offset_device).
//
// Caller paints the unblurred host on top after this returns.
//
// No-op when host_pat is empty or surface dimensions degenerate.
//
// Note: this function omits the canvas-viewport intersect that
// Canvas::render_shadow_under uses. The intersect is a zoom-bounded
// optimisation for live editing; print/export contexts have CTM scales
// bounded by the output device (paper points, SVG units), so the
// surface size is naturally bounded. Skipping it keeps the function
// general-purpose without sacrificing print performance.
void render_drop_shadow_under(
    const Cairo::RefPtr<Cairo::Context>& cr,
    const Cairo::RefPtr<Cairo::Pattern>& host_pat,
    double host_bbox_x, double host_bbox_y,
    double host_bbox_w, double host_bbox_h,
    double blur_doc, double dx_doc, double dy_doc,
    double color_r, double color_g, double color_b, double color_a,
    double opacity);

// ── Curvz dialog factory (s125 m2a) ────────────────────────────────────
//
// A small toolkit for showing modal dialogs that look and behave like
// part of the app. Three entry points: show_alert (one-button info),
// show_confirm (multi-button confirm), show_form (input fields).
//
// ── Why this exists ─────────────────────────────────────────────────
//
// GTK4's Gtk::AlertDialog is a system-level alert primitive. It paints
// itself using the OS theme — not the app's — so a lightmode Curvz
// running on a dark desktop produces a dark popup. Its body is a
// non-selectable Pango string, so users can't copy paths or error
// messages out. And because it's the system's dialog, we have no
// hooks to extend or theme it.
//
// This factory builds plain Gtk::Window dialogs that live INSIDE the
// app's CSS scope. They inherit motif from the parent window via
// apply_motif_class_from_parent — light app, light dialog. They use
// stable CSS classes so visual treatment is a stylesheet edit, not a
// code change. And they're regular widgets, so they extend naturally
// when we want a new feature (selectable text, structured fields,
// custom buttons) instead of wrestling with the OS dialog API.
//
// ── Why three functions instead of one ──────────────────────────────
//
// show_alert and show_confirm are conveniences over the same primitive
// — show_alert is "show_confirm with one button and no result." But
// "click OK on a warning" is the most common dialog by an order of
// magnitude in this app, and forcing every callsite to construct a
// button list, a callback, and an index just to say `show_alert("Save
// failed", path)` would be friction nobody wanted.
//
// show_form is a different beast: it accepts typed input fields, and
// the result is a map keyed by field id. It's the form-builder of
// the trio. See its own banner below for the design concepts that
// drive it.
//
// ── CSS hooks ───────────────────────────────────────────────────────
//
// Visual styling lives in css.hpp under `Curvz alert / confirm / form
// dialog`. Stable hooks the factory always emits:
//
//   .curvz-alert            window root
//   .curvz-alert-title      headline label
//   .curvz-alert-detail     detail entry (read-only, selectable)
//   .curvz-alert-fields     form field grid (show_form only)
//   .curvz-alert-buttons    button row
//   .curvz-alert-default    suggested-action button
//   .curvz-alert-cancel     destructive/cancel button
//
// Don't add layout-equivalent properties (margins, colors, font sizes)
// inline in the factory. Only structural decisions live in code.

// ── show_alert ──────────────────────────────────────────────────────
//
// Fire-and-forget info / warning. One OK button, no callback. Use this
// when you have something to TELL the user and don't need a decision
// back. The user can dismiss with OK, Esc, or X — all equivalent.
//
// Most common use: error messages, permission denials, "nothing
// selected" feedback. If you find yourself wanting a callback after the
// user dismisses, you probably want show_confirm with one button
// instead.
void show_alert(Gtk::Window& parent,
                const std::string& title,
                const std::string& detail = "");

// ── show_confirm ────────────────────────────────────────────────────
//
// Multi-button decision. Callback fires with the index of whichever
// button the user picked, OR with `cancel_button`'s index if they
// dismissed via Esc / X. Pass cancel_button=-1 if there's nothing
// cancel-equivalent in your button list (callback gets -1 on dismiss).
//
// `default_button` is the suggested-action button — gets the accent
// styling and activates on Enter.
// `cancel_button` is the destructive/escape button — gets neutral
// styling (loud destructive override is a per-callsite CSS class) and
// activates on Esc.
//
// Buttons are 0-indexed in the vector you pass. By convention put the
// destructive option first ("Cancel", "Discard") and the affirmative
// last ("Save", "Replace") — that's the order callers tend to
// remember when reading the callback's index.
void show_confirm(Gtk::Window& parent,
                  const std::string& title,
                  const std::string& detail,
                  const std::vector<std::string>& buttons,
                  int default_button,
                  int cancel_button,
                  std::function<void(int)> callback);

// ── show_form ───────────────────────────────────────────────────────
//
// A spec-driven input dialog. Borland-inspired.
//
// ── Concept 1: the dialog is data, not code ─────────────────────────
//
// Most dialog APIs are imperative: you instantiate a dialog object,
// call .add_field(), .add_button(), wire signals, present, wait. By
// the time the dialog runs, "what dialog is this" is spread across
// dozens of stateful method calls.
//
// show_form inverts that. The caller describes the dialog as a value
// — a vector<FormField> plus a few scalars — and hands it over. The
// factory translates the value into widgets. The data IS the
// contract; widgets are the implementation.
//
// Consequences:
//   • You can build the spec inline, deserialise it from JSON, generate
//     it from runtime state (e.g. a crash dialog reflecting on app
//     state), or fetch it from a server. The factory doesn't care.
//   • Two callsites with different specs share zero code. Adding a
//     dialog never requires editing the factory.
//   • The spec is inspectable: log it, diff it, hash it, store it.
//
// This is why FormField is a plain struct with no pointers, callbacks,
// or references. Pure data. JSON-shaped.
//
// ── Concept 2: open type set, closed operations ─────────────────────
//
// We need to support different kinds of input (text, numbers, checks,
// combos) and we need to know we'll add more later (date, multiline,
// radio group, file picker). The natural OOP answer is a base class
// with virtual build/extract methods — open for extension, but the
// type set leaks into every consumer that needs to allocate one.
//
// Variant + visitor inverts the trade-off. The TYPE SET is open: add
// a new struct, add it to FormFieldSpec, done. The OPERATIONS are
// closed: build_field_widget and extract_value enumerate every arm,
// and the compiler enforces it via static_assert in std::visit. So
// "what kinds of fields exist" is a one-line edit, but "what we do
// with each field" is rigorously checked.
//
// You cannot ship a half-supported field type. Either the build pass
// compiles (every arm handled) or it doesn't. This is the property
// that makes the factory maintainable as it grows.
//
// ── Concept 3: identity by name, not position ───────────────────────
//
// Fields are declared with stable string ids; results come back as a
// map keyed by id. Field ORDER in the spec is purely visual — does
// not affect callsite code. This means:
//
//   • Reordering fields in the spec doesn't break consumers.
//   • Adding a new field doesn't break consumers (they ignore the new
//     key in the result map).
//   • Conditionally including a field (e.g. show "category" only if
//     advanced=true) doesn't break consumers — they just don't see
//     that key when it wasn't shown.
//
// This is why FormField has an `id` string member that's required and
// must be unique. The id is the API contract; the position isn't.
//
// ── Concept 4: cleanup is automatic ─────────────────────────────────
//
// The dialog window self-deletes on hide. The callback is fired exactly
// once, on dismissal, with whatever button the user picked (or the
// cancel button index if they hit Esc / X). Caller does not manage
// window lifetime, signal connections, or modal state.
//
// This is what makes show_form safe to call from anywhere — including
// places where holding a dialog object across an async boundary would
// be awkward (signal handlers, idle callbacks, error paths).
//
// ── Extending: adding a new field type ──────────────────────────────
//
// Concretely, given the three concepts above:
//
//   1. Define `struct FooField { … config … };` next to the others —
//      this is your "spec for a Foo." Pure data, no widgets.
//   2. Add it to the FormFieldSpec variant — now the spec layer
//      accepts FooField values.
//   3. Add a `[](const FooField&)` arm in build_field_widget (in
//      curvz_utils.cpp) — this is what the factory does with one.
//   4. Add an arm in extract_value — this is how its widget's state
//      becomes a FormFieldValue.
//
// If you forget step 3 or 4 the build fails with a static_assert. If
// you forget steps 1 or 2, you can't even construct a spec. The
// concepts steer you — there's no fifth place to update because the
// design has only those four moving parts.
//
// ── Styling ─────────────────────────────────────────────────────────
//
// Visual appearance is entirely in css.hpp under `Curvz alert /
// confirm / form dialog`. Don't add CSS-equivalent properties (margins,
// colors, font sizes) inline in build_dialog. The only inline styling
// is structural: layout direction, widget existence, alignment.
//
// Field type configs follow.

struct TextField {
    std::string default_text;
    std::string placeholder;  // greyed hint when empty
};

struct NumberField {
    double default_value = 0.0;
    double min      = 0.0;
    double max      = 100.0;
    double step     = 1.0;
    int    decimals = 0;
};

struct CheckboxField {
    bool default_value = false;
};

struct ComboField {
    std::vector<std::string> items;
    int default_index = 0;
};

// FormFieldSpec — discriminated union of field-type configs. Add new
// field types here. std::variant gives us compile-time exhaustiveness
// for the visitor in build_dialog.
using FormFieldSpec = std::variant<TextField, NumberField,
                                   CheckboxField, ComboField>;

struct FormField {
    std::string   id;     // key in the result map; required, must be unique
    std::string   label;  // human-readable label shown left of the widget
    FormFieldSpec spec;   // the per-type config
};

// FormFieldValue — the result for one field. Accessors throw if the
// wrong type is requested; callers should know what type they asked for
// (or use std::visit on .value for fully type-safe handling).
struct FormFieldValue {
    using Value = std::variant<std::string,  // Text
                               double,       // Number
                               bool,         // Checkbox
                               int>;         // Combo (selected index)
    Value       value;
    std::string combo_item;   // Combo only — the text of the selected
                              // item, since callers usually want the
                              // string not the index. Empty for other types.

    // Convenience accessors. `text()` works for both Text fields (the
    // entered string) and Combo fields (the selected item text — uses
    // combo_item rather than reaching into the variant).
    const std::string& text() const {
        if (std::holds_alternative<std::string>(value))
            return std::get<std::string>(value);
        return combo_item;  // Combo case
    }
    double num()   const { return std::get<double>(value); }
    bool   flag()  const { return std::get<bool>(value); }
    int    index() const { return std::get<int>(value); }
};

void show_form(Gtk::Window& parent,
               const std::string& title,
               const std::string& detail,  // "" to omit the detail row
               const std::vector<FormField>& fields,
               const std::vector<std::string>& buttons,
               int default_button,
               int cancel_button,
               std::function<void(int button_index,
                                  const std::map<std::string, FormFieldValue>& values)>
                   callback);

// ── make_path_override_row ────────────────────────────────────────────
//
// Builds a path-override pref row for the inspector's Application
// group. The row layout is:
//
//   [Label]  [Entry: current value (placeholder = default)]  [Browse] [Reset]
//
// Behaviour:
//   - The Entry shows current_value. When current_value is empty, the
//     placeholder shows the default_path so the user sees what's in
//     effect. Empty Entry = "use default."
//   - Pressing Enter (or losing focus) commits the Entry contents via
//     on_commit. Whitespace trimming is the caller's job (typically
//     the AppPreferences setter does it).
//   - Browse opens a Gtk::FileDialog. pick_folder = true selects a
//     directory; false selects a file. The chosen path is written
//     into the Entry and committed.
//   - Reset clears the Entry and commits an empty string.
//
// dialog_parent is the Gtk::Window the FileDialog modal-roots against.
// Should not be nullptr in this codebase — every call site has a
// MainWindow available. A nullptr parent will silently no-op the
// Browse button (Reset still works).
//
// The returned widget has prop-row CSS class and standard margins so it
// drops into a collapsible body alongside switch/spin rows without
// alignment fuss. It's a Gtk::Box internally; the return type is
// Gtk::Widget* to keep gtkmm/box.h out of this header (callers
// append it via body->append(*widget), which works on any Widget).
Gtk::Widget* make_path_override_row(
    const char* label_text,
    const std::string& current_value,
    const std::string& default_path,
    const char* tooltip,
    bool pick_folder,
    Gtk::Window* dialog_parent,
    std::function<void(const std::string&)> on_commit);

// ── Warp envelope presets ───────────────────────────────────────────────
//
// s146 m2: lifted from WarpDialog.cpp's anonymous namespace so the
// inspector's build_warp_section can call the same preset generators
// the dialog uses. Single source of truth — preset shapes never drift
// between the two editing surfaces.
//
// Preset indices (must match dropdown order in both call sites):
//   0 Flat            1 Arc Up          2 Arc Down
//   3 Bulge           4 Squeeze         5 Perspective Near
//   6 Perspective Far 7 Wave
//
// Each preset produces two PathData envelopes (top + bottom) given the
// glyph-cache bbox in doc space (Y-down) and an anchor count for each
// side (clamped 2..4). The outputs always have count anchors; handle
// positions follow the colinear-1/3 convention so straight segments
// render straight and curved peaks smooth out.
//
// generate_warp_preset is a pure function — no side effects, no GTK,
// safe to call from any thread. Both surfaces call this and write the
// result into warp->warp_env_top / warp->warp_env_bottom.
namespace warp_presets {

constexpr int FLAT             = 0;
constexpr int ARC_UP           = 1;
constexpr int ARC_DOWN         = 2;
constexpr int BULGE            = 3;
constexpr int SQUEEZE          = 4;
constexpr int PERSPECTIVE_NEAR = 5;
constexpr int PERSPECTIVE_FAR  = 6;
constexpr int WAVE             = 7;
constexpr int PRESET_COUNT     = 8;

// Human-readable names in display order. Index 0 = "Flat", etc.
const char* const* preset_names();

// True iff the given preset requires anchor count >= 3 to express
// itself meaningfully. Wave is the only such case currently — count==2
// is a single segment and can't carry the alternating displacement.
bool requires_three_anchors(int preset_idx);

} // namespace warp_presets

// Generate a top + bottom envelope for a Warp from preset, bbox, and
// anchor counts. Pure function. preset_idx is clamped via switch
// default. top_count and bot_count are clamped to 2..4 internally.
void generate_warp_preset(int preset_idx,
                          double bx, double by, double bw, double bh,
                          int top_count, int bot_count,
                          ::Curvz::PathData& top_env,
                          ::Curvz::PathData& bot_env);

// ── SVG path d-string pump (s236 m1) ──────────────────────────────────
//
// Bidirectional converters between SVG `d`-attribute strings and the
// in-memory `PathData` model. Promoted to curvz_utils so script-side
// callers (ObjectProxy::set_path_data, the `path_data` read query)
// reach the same shape SvgWriter / SvgParser use, without each surface
// open-coding its own parser/emitter. Encode/decode live next to
// each other — same pump-pair convention as encode_svg_id, the
// warp_presets generator, etc.
//
// ── Scope of the m1 promotion ────────────────────────────────────────
//
// `svg_d_to_path_data` BRIDGES to SvgParser.cpp's existing
// `parse_path_d` static helper via a non-static wrapper added in m1.
// The 260-line parser body (plus its 60-line arc_to_bezier helper)
// stays put for now — lifting it cleanly would touch the most-tested
// load path in the project and bloats the m1 delta. The wrapper is
// the visible-from-outside seam; a future "SvgParser sweep" milestone
// can move the bodies here once the file-static glue is teased apart
// (parse_path_d_multi shares arc_to_bezier; both want to come over
// together).
//
// `path_data_to_svg_d` IS the full lift. The SvgWriter has two
// byte-for-byte inline d-string emitters (compound path / single
// path), both ~25 lines, both file-local. Promoted here; the writer
// keeps its inline copies for now (sweep deferred — same shape as the
// parser case, paired with the full parser lift in a future
// milestone). Calling sites in script land reach the pump directly.
//
// ── Contract ─────────────────────────────────────────────────────────
//
// svg_d_to_path_data:
//   - Empty input → empty PathData (zero nodes, closed == false).
//     Matches the s231 m2 `objects new "path"` mint contract — the
//     same shape a freshly-minted Path carries.
//   - Whole-string parse failure (malformed d-string) → empty
//     PathData. The existing parser is permissive and skips bad
//     tokens; we surface that as the empty-PathData sentinel rather
//     than introducing an error channel m1 doesn't ship.
//   - Successfully parsed d-string → PathData with one BezierNode
//     per anchor point, closed flag set iff a Z/z terminator was
//     seen.
//
// path_data_to_svg_d:
//   - Empty PathData → empty string.
//   - Non-empty PathData → "M x y" prefix + L / C segments per
//     consecutive node pair (L when both flanking handles degenerate
//     to anchors; C otherwise) + " Z" suffix if closed.
//   - All coordinates formatted via the same fmt2 (`%.2f`-style)
//     convention SvgWriter uses, so re-emission matches what the
//     file save path produces. Not byte-identical to the input — the
//     normalised shape is the documented round-trip contract.
//
// Both are pure functions. No GTK, no Cairo, no I/O. Safe to call
// from any thread.
::Curvz::PathData svg_d_to_path_data(const std::string& d);
std::string       path_data_to_svg_d(const ::Curvz::PathData& pd);

// ── User-space / doc-space pump pair (s237 m1) ───────────────────────
//
// The scripter is a user surface — an extension of the app *to* the
// user, not a window into the engine *for* the user. Coords entering
// script verbs and leaving script queries must be in the space the
// user sees in rulers and on the canvas: Y-up, origin at the
// bottom-left of the artboard, page-unit pixels. The engine stores
// geometry in doc-space: Y-down, origin at the top-left, same px
// magnitude. The two spaces share X and differ in Y.
//
// These pumps are the **script-side seam** between the two spaces.
// Every coord-bearing verb arg (set_path_data, eventually set_x /
// set_y / set_bbox / polygon cx/cy / line endpoints / etc.) calls
// `path_data_user_to_doc` after parsing the user's input; every
// coord-bearing query return calls `path_data_doc_to_user` before
// re-emitting. The user types `M 0 0 L 100 0 L 100 100 L 0 100 Z`
// and gets a 100×100 rect oriented the way they'd see it on a
// ruler — anchored at user-origin (page-bottom-left), apex
// upward, no mental Y-flip required.
//
// Internally both pumps walk every BezierNode in the PathData and
// apply CoordSpace's Y-flip to the anchor (x, y), the in-handle
// (cx1, cy1), and the out-handle (cx2, cy2). X is a pass-through;
// only Y crosses the seam. CoordSpace is the canonical Y-flip seam
// — the "(canvas_height - y) appears nowhere else" rule from
// CoordSpace.hpp continues to hold; the math has exactly one home.
//
// **Bidirectional and involutive.** Calling user_to_doc then
// doc_to_user (or vice versa) round-trips to the input exactly,
// modulo floating-point precision. The smoke exercises this with
// `set_path_data → get path_data → set_path_data` round-trips
// asserting node_count survives.
//
// **Why a PathData walker rather than a d-string walker.** Y-flip
// at the PathData layer composes cleanly with the existing
// svg_d_to_path_data / path_data_to_svg_d pumps (parse → flip,
// flip → emit). A d-string walker would have to re-parse-and-
// re-emit just to find the numbers; same work, more error surface.
//
// **Why not CoordSpace::flip_path_data directly.** CoordSpace.hpp
// includes only math/Vec2.hpp; adding a PathData-aware method
// would force it to pull in SceneNode.hpp and turn the lightweight
// pump-pair header into a SceneNode dependency. The PathData
// walker lives in curvz_utils alongside its siblings; CoordSpace
// stays focused on the primitive Y-flip operation.
//
// In-place by convention — matches the "walker" shape used elsewhere
// in curvz_utils (e.g. force_unregister_subtree). The callers in
// ObjectsScriptable.cpp already hold the PathData by value (after
// the svg_d_to_path_data parse, or as a local copy in the query
// path); mutating in place avoids an extra copy.
//
// Both pumps are pure functions on the input PathData. No GTK, no
// Cairo, no I/O. canvas_h is taken as a double so callers can pass
// either `doc->canvas_height()` (returns int) or `(double)` cast
// versions without ceremony.
void path_data_user_to_doc(::Curvz::PathData& pd, double canvas_h);
void path_data_doc_to_user(::Curvz::PathData& pd, double canvas_h);

// ── SVG fill-attribute pump pair (s238 m1) ───────────────────────────
//
// Bidirectional converters between an SVG `fill=` attribute string and
// the in-memory `FillStyle` value. The script-side seam for the
// `set_fill <color_token>` verb and the matching `fill` read query on
// ObjectProxy — same shape the svg-d pump pair (s236 m1) opened for
// path data, third infrastructure-pump candidate after find_by_iid and
// the svg-d pair.
//
// ── Scope of the m1 promotion ────────────────────────────────────────
//
// SvgParser.cpp's file-static `parse_fill` and SvgWriter.cpp's
// file-static `fill_attr` cover the same vocabulary plus url(#id)
// gradient references resolved via parse-time / write-time globals
// (`g_gradient_defs`, `g_grad_collector`). The gradient branches are
// CONTEXT-BOUND — they need a defs table or a collector pointer that
// only exists during a full document parse / write. Lifting them
// requires threading state the script-side seam doesn't have.
//
// The m1 pump pair covers the four context-free types: None,
// CurrentColor, Solid (hex / named), and falls back to CurrentColor
// for url(#...) and unknown tokens (the same safe-default the file-
// static parser uses). Gradient round-trip via script is deferred —
// scripts that want to apply a gradient go through the swatch/style
// surface, which already lives in a context where the gradient defs
// are resolvable. The legacy file-statics in SvgParser / SvgWriter
// stay put; a future SvgParser / SvgWriter sweep (already on backlog
// from the s236 m1 narrative) will collapse to one canonical pair
// that handles both context-bound and context-free cases.
//
// ── Vocabulary ───────────────────────────────────────────────────────
//
// fill_attr_to_fill_style accepts:
//   - ""             → CurrentColor (the SVG default-when-omitted)
//   - "currentColor" → CurrentColor
//   - "none"         → None
//   - "#RRGGBB"      → Solid
//   - "#RGB"         → Solid (short-form, expanded to RRGGBB)
//   - "rgb(r,g,b)"   → Solid (integer 0-255 channels)
//   - "rgba(r,g,b,a)"→ Solid (integer rgb, float a — alpha currently
//                            captured in FillStyle.a)
//   - named colour   → Solid (the 19-name common subset SvgParser
//                            supports — black/white/red/green/blue/
//                            yellow/cyan/magenta/orange/purple/grey/
//                            gray/silver/navy/teal/maroon/lime/aqua/
//                            fuchsia/olive)
//   - anything else  → CurrentColor (safe default, matches parser)
//
// fill_style_to_fill_attr emits:
//   - None         → "none"
//   - CurrentColor → "currentColor"
//   - Solid        → "#RRGGBB" (lowercase hex, snprintf "%02x" per
//                              channel; matches SvgWriter's fill_attr)
//   - Gradient     → "#RRGGBB" of the first stop (or remembered solid
//                              colour if stops is empty). Same fallback
//                              shape SvgWriter uses when its
//                              g_grad_collector is absent — degrades
//                              to a visible colour rather than emitting
//                              an unresolvable url(#...) reference.
//
// ── Contract ─────────────────────────────────────────────────────────
//
// Round-trip on the m1 vocabulary is byte-clean for None, CurrentColor,
// and lowercase #RRGGBB. Other inputs normalise (named → hex,
// uppercase hex → lowercase, short hex → long, rgb()/rgba() → hex).
// Pure functions. No GTK, no Cairo, no I/O. Safe to call from any
// thread.
::Curvz::FillStyle fill_attr_to_fill_style(const std::string& token);
std::string        fill_style_to_fill_attr(const ::Curvz::FillStyle& fs);

// ── Welcome SVG resolver (s294 m5a) ──────────────────────────────────
//
// Picks the SVG to use for the startup welcome animation. Two-tier
// resolution:
//
//   1. Scan ~/.config/curvz/welcome/*.svg. If non-empty, return one
//      entry at random. This is the user's own surface — drop SVGs
//      in (file manager, command line, or a script that exports
//      then copies) and they cycle through on subsequent launches.
//      Filename order is filesystem-dependent and not promised; the
//      random pick guarantees variety regardless.
//
//   2. If the folder is empty or doesn't exist, return the path to
//      the installed bundled scott-bug.svg. This path is set at
//      build time via the CMake install rule
//      (share/curvz/welcome/scott-bug.svg under CMAKE_INSTALL_PREFIX),
//      mirroring the user's folder shape one level up the tree.
//
//   3. If neither path is usable (gresource read fails, install
//      tree is malformed, etc.), return the empty string. The
//      caller treats empty as "skip autoplay silently" — LOG_INFO
//      the skip but never error out. Welcome autoplay is a courtesy,
//      not load-bearing.
//
// Pure with respect to side effects: the function reads the
// filesystem and uses std::rand for the pick. No GTK, no Cairo,
// no logging of selection (the caller logs once after resolution
// succeeds, before kicking off playback).
std::string resolve_welcome_svg_path();

} // namespace curvz::utils
