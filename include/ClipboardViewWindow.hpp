// ── ClipboardViewWindow ─────────────────────────────────────────────────────
//
// s203 m1 — "View Clipboard" mini float (Edit ▸ View Clipboard…)
//
// Motivation: the Measure tool's copy buttons write a structured multi-line
// block to the system clipboard. Users who want one field from that block
// (just the distance, or just x₂) have to paste into a text editor, select,
// re-copy, then paste at the destination. That round-trip is the friction
// this window removes.
//
// Scope kept narrow on purpose. This is NOT a clipboard-history viewer, nor a
// type-aware previewer for images / native objects / Curvz scene-graph
// fragments. It's: snapshot the system clipboard, show whatever text is
// there with a small MIME-type header strip, let the user select a piece and
// recopy with Ctrl+C. For non-text clipboards, show the MIME types plus a
// `URL: (none)` line and let the rest stay empty — honest about scope rather
// than pretending to render images.
//
// Lifetime. Window is owned by MainWindow via unique_ptr; lazy-constructed
// on first action invocation. Following the m6_v2 quick-jump pattern:
// - set_decorated(false) for the mini chromeless look
// - Escape closes
// - close NOT triggered on focus-out (workflow point: user clicks away to
//   paste a piece elsewhere, then comes back for another piece)
// - hide_on_close so subsequent invocations re-show the same window
// - transient_for the MainWindow so window-managers keep it associated, but
//   NOT modal — the whole point is the user works alongside it.
//
// Clipboard read. GTK4 clipboards are async-read for content
// (read_text_async + read_text_finish), but synchronous for format
// enumeration (get_formats). The text body is filled in via callback; the
// header is filled in immediately. If the read callback fires after the
// window has been hidden or destroyed, the captured WeakPtr guard skips
// the populate.

#pragma once

#include <gtkmm.h>

namespace Curvz {

class ClipboardViewWindow : public Gtk::Window {
public:
  ClipboardViewWindow();
  ~ClipboardViewWindow() override = default;

  // Re-read the system clipboard and refresh the displayed contents. Called
  // automatically on first show and by the Refresh button; safe to call
  // repeatedly.
  void refresh();

private:
  // ── UI ────────────────────────────────────────────────────────────────────
  Gtk::Box       m_root;       // vertical
  Gtk::Label     m_type_lbl;   // MIME type header strip (dim, mono-ish)
  Gtk::Separator m_sep;
  Gtk::ScrolledWindow m_scroll;
  Gtk::TextView  m_body;       // read-only-feeling, selectable+copyable
  Gtk::Box       m_btn_row;    // horizontal — Refresh only (titlebar × closes)
  Gtk::Button    m_refresh_btn;

  // Generation counter — a refresh started before the previous callback
  // returns invalidates the older callback. Same idea as the m_build_gen
  // pattern in PropertiesPanel.
  unsigned m_gen = 0;
};

} // namespace Curvz
