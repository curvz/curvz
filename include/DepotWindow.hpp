// ── DepotWindow ─────────────────────────────────────────────────────────────
//
// s313 m1 — Overflow depot as a custom Gtk::Window (replaces the s311/s312
// Gtk::Popover host).
//
// Why a Window and not a Popover. Through s312 we tried three different
// gesture wirings to get drag-to-resize working on the depot popover; all
// three landed at the same wall — Gtk::Popover in GTK4 does not tolerate
// post-popup size mutations. Mutating set_size_request mid-life triggers
// a surface re-allocation that drops the popover's WM grab, and the grab
// loss fires signal_closed → autohide. The pathology is structural to
// popovers (they're popup-class surfaces with modal grabs), not a wiring
// bug we could fix with a different gesture phase or widget wrapping.
//
// Migrating to a top-level Gtk::Window subclass sidesteps the whole class
// of problem: Windows resize natively via set_default_size with no grab
// to lose, no autohide machinery, no mid-life surface-rebuild. Standard
// pattern in GTK4 for "custom popup-flavored float" (cf. the GNOME forum
// thread on porting GTK3 tooltip-popup-windows to GTK4 — same shape).
//
// The trade is initial positioning. Popovers got pixel-precise anchoring
// for free via set_pointing_to because they're popup-class surfaces with
// a parent-relative position request in the windowing protocol. Top-level
// windows on GTK4 — especially under Wayland — leave first-show position
// to the WM. We hint via set_transient_for(MainWindow) so the WM places
// the window near the main window, and we offer the user a drag-to-move
// gesture (same pattern as s312 m2.1.fix) so once placed the user can
// shove it where they want. Position then sticks across hide/show within
// the session because the widget is long-lived in the per-Mgr cache.
//
// Lifetime + ownership.
// Each TextBoxMgr owns exactly one DepotWindow, built lazily on first
// indicator click / cross-into-depot. Canvas holds them in
// m_depot_window_by_mgr keyed by Mgr iid. set_hide_on_close(true) so the
// X-out (Escape, click-outside dismiss, programmatic close()) hides the
// widget without destroying it — the cache keeps the widget alive across
// hide/show cycles, preserving scroll position, edit state, position-on-
// screen, and the drag-resized size.
//
// GC: when an entry's Mgr is no longer in the doc tree, Canvas walks the
// map at each show and destroys vanished windows (lazy GC, same shape as
// the s311 m2.1.v2 popover map).
//
// Contents.
//   1. Top row — stats label ("N words total, M overflowed (B bytes)")
//      and link-to verb button (placeholder; wired in m3+). The top row
//      doubles as the drag-to-move handle (GestureDrag attached); clicks
//      below threshold reach the link button as normal.
//   2. Scrollable Gtk::TextView showing the depot bytes (the tail of the
//      Mgr's buffer past the last canvas view's bytes_consumed). The
//      TextView is editable; signal_changed routes user edits back into
//      the Mgr's buffer through Canvas (m_depot_suppress_changed guards
//      programmatic writes).
//   3. Bottom-right overlay grip — drag-to-resize via set_default_size.
//      Floor 240x140.
//
// Cross-boundary key controller.
//   Left at TextView offset 0  → Canvas::cross_back_to_canvas
//   Up   on TextView line 0    → Canvas::cross_back_to_canvas
//   Other keys fall through to TextView's stock handling.
//
// Dismiss paths.
//   - Escape inside the window  → close()
//   - Click outside the window  → Canvas's controller calls close()
//     (the s311/s312 popover got this for free via autohide; we wire
//     it explicitly in Canvas now)
//   - Programmatic close from Canvas (cross_back, auto-close on empty
//     depot)
//   All paths go through signal_hide → Canvas::resume_text_cursor_blink.
//
// Custom chrome — DEFERRED to a follow-on milestone.
//   m1 ships with set_decorated(false) and lets the default Window CSS
//   provide the background + border. m2 (or a later session) will add a
//   custom Cairo paint for the rounded frame + arrow inset from the top-
//   left corner, transparent window background. Splitting the work this
//   way means m1 proves the structural migration in isolation; if the
//   chrome work needs iteration, m1 has already landed.

#pragma once

#include <gtkmm.h>
#include <string>

#include "SceneNode.hpp"

namespace Curvz {

class Canvas;  // Canvas calls into DepotWindow; DepotWindow calls back into
               // Canvas for edit routing + cross-back. Forward decl avoids
               // the header cycle.

class DepotWindow : public Gtk::Window {
public:
  // Constructor takes the canvas (owner of the per-Mgr cache + edit
  // routing target) and the Mgr (the data source — buffer, iid, child
  // views). The window builds its internal layout once at construction;
  // subsequent shows reuse the same widgets.
  //
  // The Mgr pointer is captured by value (raw); DepotWindow's lifetime
  // is bounded by Canvas's per-Mgr cache, which GCs entries whose Mgr
  // is no longer in the doc tree before any code path touches them.
  DepotWindow(Canvas* canvas, SceneNode* mgr);
  ~DepotWindow() override = default;

  // Accessors for Canvas. The TextView is what the edit-routing signal
  // and cross-back key controller need to address; the stats label gets
  // refreshed on every show.
  Gtk::TextView* text_view() { return m_text_view; }
  Gtk::Label*    stats_label() { return m_stats_label; }

  // Mgr iid this depot belongs to. Stored at construction; survives the
  // Mgr pointer going stale (which shouldn't happen pre-GC anyway).
  const std::string& mgr_iid() const { return m_mgr_iid; }

  // Update the stats label text. Caller computes the count; this just
  // pushes the string into the label.
  void set_stats(const std::string& text);

  // Seed the TextView buffer with overflow_text. Used on fresh build
  // (after first construction) and could be used by Canvas in future
  // re-sync scenarios. Caller is responsible for setting Canvas's
  // m_depot_suppress_changed flag around this call if needed.
  void seed_text(const std::string& overflow_text);

  // Show the window in its current size and position. If the WM has
  // never placed this window before, it lands wherever the WM decides
  // (we set_transient_for(MainWindow), so usually near the main
  // window); if the user has drag-moved it during the session, the
  // window remembers its position.
  void show();

private:
  // ── References ──────────────────────────────────────────────────────────
  Canvas*     m_canvas;
  SceneNode*  m_mgr;
  std::string m_mgr_iid;  // copy at construction; outlives m_mgr stale

  // ── Inner widgets (owned by GTK widget tree via the root child) ────────
  Gtk::Box*       m_root        = nullptr;  // VERTICAL
  Gtk::Box*       m_top_row     = nullptr;  // HORIZONTAL — stats + link btn
  Gtk::Label*     m_stats_label = nullptr;
  Gtk::Button*    m_link_btn    = nullptr;
  Gtk::ScrolledWindow* m_scroll = nullptr;
  Gtk::TextView*  m_text_view   = nullptr;
  Gtk::Overlay*   m_overlay     = nullptr;  // hosts grip at bottom-right
  Gtk::Button*    m_grip        = nullptr;  // drag-to-resize handle

  // ── Build helpers ───────────────────────────────────────────────────────
  void build_chrome();        // window-level setup (decorated/transient/etc.)
  void build_layout();        // root vbox + children
  void wire_edit_routing();   // TextView signal_changed → Canvas
  void wire_cross_boundary(); // TextView key controller for Left/Up cross-back
  void wire_drag_to_move();   // top_row GestureDrag → window move
  void wire_drag_to_resize(); // grip GestureDrag → set_default_size
  void wire_dismiss();        // Escape + signal_hide → resume blink
};

} // namespace Curvz
