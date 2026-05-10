#pragma once
//
// ThemeEditDialog — modeless multi-section editor for a theme::Theme.
//
// s183 m2 — created. The reusable Theme Editor that pops over MainWindow
// when the user clicks the Edit (✎) button on a row in the Themes
// panel. v1 ships a single Edit mode; New and Duplicate are not
// surfaced here because the Themes panel already has dedicated
// affordances for both ([+] saves the active doc as a new theme,
// per-row Duplicate button clones an existing one).
//
// The dialog edits a single Theme in isolation — no document
// awareness, no broadcast, no coalesced undo. OK pushes one atomic
// UpdateThemeCommand; Cancel discards the working buffer. The host
// wires the command and the library-changed signal at the call site
// (see MainWindow_zones.cpp where the dialog is instantiated, mirror
// of the StyleEditorDialog wiring there).
//
// Section roster (m2 ships skeleton + identity + button row; m3-m5
// fill in the body):
//
//   ── Identity ──
//   * name           — CurvzEntry. Empty-on-OK is sanitised by the
//                      panel's existing rename rules; the dialog's
//                      OK path mirrors them.
//
//   ── Body (m3+) ──
//   * Live thumbnail — Cairo widget rendering a mock canvas
//                      (artboard, workspace, margins, grid, guides)
//                      in the working theme's settings, with a
//                      light/dark mode toggle that flips only the
//                      thumbnail (not the dialog chrome).
//   * Property controls — sub-sections per Theme sub-bundle (motif
//                      colours, units, guides, grid, margins, snap),
//                      mode-aware where motif applies.
//
//   ── Button row (m2) ──
//   * Cancel         — discard, close.
//   * Reset          — m5: mode-scoped reset to factory defaults.
//                      In m2 this is a placeholder no-op so the
//                      shape is right and the visual review is
//                      complete; m5 fills in the actual semantic.
//   * OK             — fire on_committed(working) and close.
//
// Lifetime: dialog is heap-allocated and self-deletes via
// signal_close_request → signal_idle → delete this. Same idiom as
// StyleEditorDialog and the s126-canon factory dialogs.
//
// Why no per-property reset glyphs: deferred. v1 ships one global
// mode-scoped Reset. If that proves too coarse, per-property reset
// glyphs ride alongside as a cheap extension.
//
// Why no factory-theme protection logic: the data model already
// has is_built_in() which checks the "app:" prefix; v1 has no app
// themes seeded so every theme is editable. The protection mechanism
// is in place for future built-ins; the dialog doesn't need to
// reproduce it. Caller (the Themes panel) gates which rows even
// expose the Edit button.
//

#include "theme/Theme.hpp"
#include "CurvzDocument.hpp"  // Motif, CanvasModel
#include "UnitSystem.hpp"     // Unit
#include "ColorPickerPopover.hpp"
#include "CurvzSpinButton.hpp"   // s183 m4 fix2 — unit-aware spinners

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <sigc++/connection.h>
#include <cairomm/context.h>

#include <functional>

namespace Curvz {

class CurvzEntry;

class ThemeEditDialog {
public:
    // Fires once on OK with the result Theme. Caller pushes
    // UpdateThemeCommand against m_history. The committed theme's
    // header.id matches the initial id; the dialog never mints a
    // new id (it doesn't do New).
    using CommittedFn = std::function<void(theme::Theme result)>;

    // Construct + show. Self-deletes on close.
    //
    // parent       — transient-for. Required.
    // initial      — starting Theme. header.id must be set.
    // initial_mode — m3: which motif slot the thumbnail reads first.
    //                Pass the host app's current motif so the user
    //                opens the dialog and sees their actual working
    //                mode previewed; the toggle lets them flip the
    //                thumbnail to inspect the other mode without
    //                changing app chrome.
    // on_committed — fires once on OK with the working theme.
    ThemeEditDialog(Gtk::Window& parent,
                    theme::Theme initial,
                    Motif initial_mode,
                    CommittedFn on_committed);

    ~ThemeEditDialog();

    // Non-copyable.
    ThemeEditDialog(const ThemeEditDialog&) = delete;
    ThemeEditDialog& operator=(const ThemeEditDialog&) = delete;

private:
    void build();
    void build_identity_section(Gtk::Box& root);
    void build_body_section(Gtk::Box& root);
    void build_properties_notebook(Gtk::Box& root);   // s183 m4
    void build_tab_colors_snap(Gtk::Box& page);        // s183 m4 tab 1
    void build_tab_grid(Gtk::Box& page);               // s183 m4 tab 2
    void build_tab_margins(Gtk::Box& page);            // s183 m4 tab 3
    void build_button_row(Gtk::Box& root);

    // s183 m3 — preview thumbnail.
    void draw_thumbnail(const Cairo::RefPtr<Cairo::Context>& cr,
                        int w, int h) const;
    void queue_thumbnail_redraw();

    // s183 m4 prelude — apply / clear the OK-blue suggested-action
    // highlight on whichever mode toggle is active. Called from the
    // toggle signal handlers AND once during build() for the initial
    // state.
    void refresh_mode_toggle_highlight();

    // s183 m4 — Motif color swatches are mode-aware: the same three
    // swatches edit the dark_* triple when m_preview_mode=Dark and the
    // light_* triple when m_preview_mode=Light. This helper re-reads
    // the three swatches and queue_draws them after a mode flip.
    void refresh_motif_swatches();

    void on_ok();
    void on_cancel();
    void on_reset();   // m5 fills in the semantic; m2 is placeholder

    // ── State ──────────────────────────────────────────────────────
    Gtk::Window* m_window = nullptr;       // owns the dialog UI
    theme::Theme m_working;                // edit-buffer; mutates as
                                           // user types/picks; OK
                                           // commits this value
    theme::Theme m_initial;                // for Cancel-discard and
                                           // for Reset's "is this a
                                           // change?" comparisons
                                           // (m5 may also use this
                                           // for diff-vs-defaults)
    CommittedFn  m_on_committed;

    // ── Identity widgets ───────────────────────────────────────────
    CurvzEntry*  m_name_entry = nullptr;

    // ── Preview thumbnail (m3) ─────────────────────────────────────
    Motif            m_preview_mode = Motif::Dark;
    Gtk::DrawingArea* m_thumbnail   = nullptr;
    Gtk::ToggleButton* m_mode_dark_btn  = nullptr;
    Gtk::ToggleButton* m_mode_light_btn = nullptr;

    // ── Property controls (m4) ─────────────────────────────────────
    //
    // Color popovers attached to m_window. One per logical colour
    // slot — keeping them separate gives each its own session state
    // (recents, hex history) and avoids the "one open popover that
    // forgets which swatch summoned it" trap. Detached in the
    // signal_close_request handler before window finalisation.
    ColorPickerPopover m_motif_artboard_popover;
    ColorPickerPopover m_motif_workspace_popover;
    ColorPickerPopover m_motif_creation_popover;
    ColorPickerPopover m_guides_popover;
    ColorPickerPopover m_grid_popover;
    ColorPickerPopover m_margin_popover;

    // Tab 1 — Colors & Snap
    Gtk::DrawingArea* m_swatch_motif_artboard  = nullptr;
    Gtk::DrawingArea* m_swatch_motif_workspace = nullptr;
    Gtk::DrawingArea* m_swatch_motif_creation  = nullptr;
    Gtk::DrawingArea* m_swatch_guides          = nullptr;
    Gtk::CheckButton* m_guides_visible_chk     = nullptr;
    Gtk::DropDown*    m_units_dd               = nullptr;
    Gtk::CheckButton* m_snap_enabled_chk       = nullptr;
    Gtk::CheckButton* m_snap_guides_chk        = nullptr;
    Gtk::CheckButton* m_snap_grid_chk          = nullptr;
    Gtk::CheckButton* m_snap_margins_chk       = nullptr;
    Gtk::CheckButton* m_snap_nodes_chk         = nullptr;
    Gtk::CheckButton* m_snap_edges_chk         = nullptr;
    Gtk::CheckButton* m_snap_centers_chk       = nullptr;

    // Tab 2 — Grid
    // s183 m4 fix2: dimensional fields (spacing, offset) use
    // CurvzSpinButton so the units dropdown can rebrand them via
    // refresh_units(). Alpha % stays plain — it's a percent, not a
    // distance.
    Gtk::CheckButton* m_grid_enabled_chk = nullptr;
    Gtk::CheckButton* m_grid_visible_chk = nullptr;
    Gtk::CheckButton* m_grid_dots_chk    = nullptr;
    CurvzSpinButton*  m_grid_spacing_x   = nullptr;
    CurvzSpinButton*  m_grid_spacing_y   = nullptr;
    CurvzSpinButton*  m_grid_offset_x    = nullptr;
    CurvzSpinButton*  m_grid_offset_y    = nullptr;
    Gtk::SpinButton*  m_grid_alpha_pct   = nullptr;
    Gtk::DrawingArea* m_swatch_grid      = nullptr;

    // Tab 3 — Margins
    // s183 m4 fix2: dimensional fields (top/bottom/left/right, gaps)
    // use CurvzSpinButton. Counts (cols, rows) and alpha % stay plain.
    Gtk::CheckButton* m_margin_enabled_chk = nullptr;
    Gtk::CheckButton* m_margin_visible_chk = nullptr;
    CurvzSpinButton*  m_margin_top    = nullptr;
    CurvzSpinButton*  m_margin_bottom = nullptr;
    CurvzSpinButton*  m_margin_left   = nullptr;
    CurvzSpinButton*  m_margin_right  = nullptr;
    Gtk::SpinButton*  m_margin_cols   = nullptr;
    CurvzSpinButton*  m_margin_col_gap = nullptr;
    Gtk::SpinButton*  m_margin_rows   = nullptr;
    CurvzSpinButton*  m_margin_row_gap = nullptr;
    Gtk::SpinButton*  m_margin_alpha_pct = nullptr;
    Gtk::DrawingArea* m_swatch_margin = nullptr;

    // s183 m4 fix2 — Dialog-local CanvasModel that owns the
    // dialog's display_unit. CurvzSpinButton reads the unit from
    // this; refreshing it after a units-dropdown change re-displays
    // every dimensional spinner in the new unit (the underlying
    // internal-px value never changes). Only display_unit is read
    // from this model; geometry fields are unused.
    CanvasModel m_unit_model;

    // s183 m4 fix2 — ScrolledWindow that wraps the notebook only.
    // Identity, thumbnail, mode toggle, and button row stay pinned;
    // only the property notebook scrolls when the dialog runs out
    // of vertical room on a small screen.
    Gtk::ScrolledWindow* m_props_scroll = nullptr;

    // ── Button row widgets ─────────────────────────────────────────
    Gtk::Button* m_btn_cancel = nullptr;
    Gtk::Button* m_btn_reset  = nullptr;
    Gtk::Button* m_btn_ok     = nullptr;

    // m_syncing: true while build() is populating widgets so the
    // signal handlers we connect don't read stale-buffer values
    // back into m_working during construction. Mirrors the
    // StyleEditorDialog idiom exactly.
    bool m_syncing = false;
};

} // namespace Curvz
