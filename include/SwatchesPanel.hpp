#pragma once
//
// SwatchesPanel.hpp — Phase 4 Swatches section inside the Content group.
//
// Phase 4 milestones:
//   M2 — skeleton + empty state.
//   M3 — add button (opens CurvzColorPicker popover) + recents strip with
//        clickable chips. Click → apply to selection fill; Alt-click →
//        stroke. Match S66 eyedropper semantics.
//   M4 — palette selector (DropDown + kebab menu) + grid of active
//        palette's swatches. Implicit "Default" palette auto-created on
//        first add. New swatches always go into the active palette.
//        Palette management (new/rename/delete/duplicate) via kebab.
//   M5 — right-click context menu on chips (edit colour, rename, duplicate,
//        remove from palette, delete swatch). Active-paint ring on chip
//        whose colour matches current selection's fill.
//
// Ownership / dependencies:
//   * Non-owning pointer to the SwatchLibrary (lives on CurvzProject).
//   * Non-owning pointer to the Canvas (for apply_fill/stroke_to_selection).
//   * Holds a ColorPickerPopover as a member (caller-singleton pattern
//     per S65 popover-lifetime rules). Attached to `this` at set_library
//     time — we need the panel in the widget tree first.
//
// Refresh model:
//   refresh() tears down and rebuilds the body. Called on every library
//   mutation (add swatch, apply-and-touch-recent, palette change, etc).
//   Small enough that full rebuild is cheaper than tracking partial diffs.
//

#include "color/SwatchLibrary.hpp"
#include "color/PaletteIO.hpp"
#include "ColorPickerPopover.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/flowbox.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/searchentry.h>
#include <giomm/menu.h>
#include <giomm/simpleactiongroup.h>
#include <sigc++/sigc++.h>

#include <unordered_map>
#include <vector>

// s208 m5 — forward-declare substrate DropDown for the per-refresh
// palette dropdown member. Full include in SwatchesPanel.cpp.
namespace curvz::widgets { class DropDown; }

namespace Curvz {

class Canvas;  // fwd — the panel calls apply_fill/stroke_to_selection
class CommandHistory;  // s220 m1a — fwd; m_history* passes through set_history

class SwatchesPanel : public Gtk::Box {
public:
    SwatchesPanel();

    // Non-owning. Caller guarantees the library outlives the panel or
    // calls set_library(nullptr) before destroying it.
    void set_library(color::SwatchLibrary* lib);

    // Non-owning. Panel calls apply_fill_to_selection / apply_stroke_to_
    // selection on this canvas when the user clicks a chip, and hooks the
    // selection-changed signal to maintain the active-paint ring. Null =
    // panel still renders, but clicks silently do nothing beyond touching
    // recents and no ring is drawn.
    void set_canvas(Canvas* canvas);

    // s220 m1a — Non-owning. CommandHistory used by the panel's CRUD
    // paths (add / delete / rename / duplicate) so swatch mutations
    // land in the undo stack. Null = panel still functions but
    // mutations are direct-write and un-undoable; the StylesPanel
    // precedent (S80 m4c-2) is the same shape.
    void set_history(CommandHistory* history) { m_history = history; }

    // Rebuild the panel body from scratch.
    void refresh();

    // Emitted whenever the library mutates in a way that needs persisting
    // (new swatch, recents touched, swatch updated). MainWindow wires
    // this to schedule_save().
    using LibraryChangedSignal = sigc::signal<void()>;
    LibraryChangedSignal& signal_library_changed() { return m_sig_library_changed; }

    // S83 m4h v3 → v4: emitted whenever a panel-driven library
    // mutation could leave the inspector stale. Sources: bind-click
    // (on_click), rename (on_ctx_rename_swatch), recolour (edit
    // popover apply paths), delete (on_ctx_delete_swatch). MainWindow
    // wires this to refresh the inspector — without it, the
    // Appearance widgets and binding annotation go stale because
    // Canvas's signal_document_changed only triggers the lightweight
    // sync_selection (bbox spinners) path. Mirrors the toolbar's
    // signal_defaults_changed → refresh_inspector wiring.
    //
    // Renamed from v3's signal_paint_applied: the original name only
    // covered bind-click; the v4 sites are library mutations broadly.
    // Inspector refresh is the unifying semantic.
    using InspectorRefreshSignal = sigc::signal<void()>;
    InspectorRefreshSignal& signal_inspector_refresh_needed() {
        return m_sig_inspector_refresh_needed;
    }

private:
    color::SwatchLibrary* m_library = nullptr;
    Canvas*               m_canvas  = nullptr;
    CommandHistory*       m_history = nullptr;  // s220 m1a: undo for CRUD

    // --- Header row --------------------------------------------------------
    Gtk::Box        m_header { Gtk::Orientation::HORIZONTAL };
    Gtk::Label      m_title;
    // S83 m4h v8: hamburger MenuButton consolidating the previous
    // separate "+" button (swatch creation) AND the palette-options
    // kebab in the selector row. Single panel-level menu with
    // New swatch + palette management items. Mirrors StylesPanel's
    // m_btn_add.
    Gtk::MenuButton m_btn_add;

    // --- Body container ----------------------------------------------------
    //
    // Torn down and rebuilt by refresh(). Layout when populated (v13+):
    //   [ Recents strip (FlowBox of chips) ]
    //   [ Palette grid (FlowBox of chips for active palette) ]
    //   (empty-state label when everything is empty)
    //
    // The palette dropdown chooser lives in m_header above (v13).
    // Pre-v13 the dropdown was the second body row inside a
    // selector_row; v13 promoted it to share the header row with
    // the kebab.
    Gtk::Box m_body { Gtk::Orientation::VERTICAL };

    // Held as members so refresh() can reuse them rather than reallocating.
    Gtk::Label m_empty_state;
    Gtk::Label m_recents_label;

    // --- Search (S101 m4) -------------------------------------------------
    //
    // Filters swatch chips visible in the recents strip and the active
    // palette grid. Match is case-insensitive substring against three
    // axes per swatch: name, hex (e.g. "#ff8000"), and "r,g,b" decimal
    // string. Empty query → all chips visible. Filter applies to both
    // flows uniformly so a search query reads as "find this swatch in
    // what I currently see."
    //
    // Scope: just-this-panel, not cross-palette. If a user wants to
    // find a hex they remember but don't know which palette holds, the
    // current m4 makes them switch palettes. Cross-palette flat search
    // is a possible m5+ enhancement (option 2 from the design notes).
    Gtk::SearchEntry m_search;

    // FlowBoxes built in the current refresh() pass. Cleared at the top
    // of refresh, populated by build_chip_flow. The search-change
    // handler iterates these to call invalidate_filter() so the
    // filter_func re-runs against the new query.
    std::vector<Gtk::FlowBox*> m_active_flows;

    // Per-FlowBoxChild → SwatchId mapping populated during chip build.
    // Read by the FlowBox filter_func to know which swatch a child
    // represents without storing the id on the widget itself. Cleared
    // at the top of refresh — the widgets don't outlive a refresh
    // cycle, so dangling pointers in the map are a non-issue as long
    // as we clear before reuse.
    std::unordered_map<Gtk::FlowBoxChild*, color::SwatchId> m_chip_swatch;

    // s207 m1: ColorPickerPopover collapsed to an app-wide singleton.
    // The `+` and double-click-to-edit flows reach it via
    // ColorPickerPopover::shared(). No instance member here; the
    // previous `ColorPickerPopover m_color_popover` and its
    // `m_color_popover_attached` lazy-attach flag are gone — the
    // singleton's ensure_attached() handles attach idempotently on
    // first use.

    // Tracks the swatch id currently being edited. Empty string = the
    // popover session is adding a new swatch (first change creates it,
    // subsequent changes update in place). Non-empty = editing an existing
    // swatch, each callback overwrites.
    std::string m_editing_swatch_id;

    // Per-popover-session state for create-flow Esc cleanup (S73).
    //
    // Captured at popover-open time so on_closed can identify a
    // freshly-created swatch and remove it on Esc.
    //
    //   m_session_created_id — populated only in create flow, after
    //     the first colour-change callback adds the swatch. Esc in
    //     this state removes it via on_closed(committed=false). Empty
    //     means no swatch was created yet (Esc-before-any-drag) — also
    //     a no-op in on_closed.
    //
    // (The edit flow doesn't need a session-state member: the original
    // colour is captured by the on_closed lambda's by-value capture,
    // and the picker self-reverts on Esc — see ColorPickerPopover.hpp
    // dismissal-model docs.)
    std::string  m_session_created_id;

    // --- Palette management (M4) ------------------------------------------
    //
    // Action group scoped to the kebab menu ("swatches.new-palette",
    // "swatches.rename-palette", etc). The actions live as long as the
    // panel does; building once is enough.
    Glib::RefPtr<Gio::SimpleActionGroup> m_palette_actions;

    // Dropdown populated from m_library->all_palette_ids(). Index i in
    // the DropDown's model corresponds to palette_ids[i] stored alongside
    // so we can translate selection back to an id. StringList model is
    // rebuilt on every refresh().
    // s208 m5: substrate. Forward-declared above; full include in .cpp.
    // refresh() now calls force_unregister_subtree before unparenting the
    // previous dropdown so the new registration under `sw_pal` doesn't
    // collide with GTK4's deferred destruction.
    curvz::widgets::DropDown*     m_palette_dropdown = nullptr;  // owned by m_body hierarchy
    std::vector<color::PaletteId> m_palette_order;               // shadow vector matching dropdown

    // Signal connection for the DropDown's selection, disconnected before
    // the widget is destroyed in the next rebuild to avoid spurious fires
    // from programmatic selection changes during rebuild.
    sigc::connection m_dropdown_sel_conn;

    // Guard flag: when true, DropDown selection-change handlers no-op.
    // Set during refresh() while we programmatically re-select the active
    // palette; prevents selection-change from re-triggering refresh
    // recursively.
    bool m_syncing_dropdown = false;

    // --- Active-paint ring (M5) -------------------------------------------
    //
    // Id of the swatch whose solid colour matches the primary selected
    // object's fill, or empty if no match (no selection, multi-select
    // with mixed fills, non-solid fill, or no matching swatch in library).
    // Computed at the top of refresh() by reading m_canvas->selection().
    // Used by make_chip() to decide whether to draw the ring.
    color::SwatchId m_active_paint_swatch_id;

    // Connection for canvas selection-changed signal. Hooked in
    // set_canvas() and disconnected before the panel is destroyed.
    sigc::connection m_canvas_selection_conn;

    // Connection for library paint-changed signal (Phase 5 M3+). Fires
    // whenever set_paint writes through the choke point; listener
    // refreshes the active-paint ring so it stays correct for swatch-
    // driven writes. Hooked in set_library(), disconnected on
    // set_library(nullptr) and before destruction.
    //
    // Note: this covers set_paint writes only. Toolbar / inspector /
    // eyedropper paths still write raw colour via apply_fill_to_selection
    // and don't fire this signal; the ring will go stale for those
    // until later milestones reroute them through set_paint too.
    sigc::connection m_library_paint_changed_conn;

    // Connection for library swatch-changed signal (S72). Fires
    // whenever a swatch's contents are replaced via update_swatch.
    // Listener calls refresh() so the chip grid reflects the new
    // colour — necessary for live-recolour ripples driven from
    // outside the panel (e.g. EditSwatchCommand undo/redo) where
    // the chip view would otherwise show the stale colour.
    sigc::connection m_library_swatch_changed_conn;

    // s220 m1a hotfix — connections for the new add/remove signals on
    // the library. Without these, undo/redo of AddSwatchCommand or
    // RemoveSwatchCommand mutates the library correctly but the panel
    // grid stays stale (the original design assumed the panel was
    // always the originator of add/remove and self-refreshed). These
    // hookups close that gap.
    sigc::connection m_library_swatch_added_conn;
    sigc::connection m_library_swatch_removed_conn;

    // --- Right-click context (M5) -----------------------------------------
    //
    // When the user right-clicks a chip, we record which swatch + which
    // palette context the menu is acting on, then open the popover.
    // Context actions ("swatches.ctx-edit", "swatches.ctx-rename", etc.)
    // read these when fired. Palette id is empty for recents-chip menus.
    color::SwatchId  m_ctx_swatch_id;
    color::PaletteId m_ctx_palette_id;

    // Hidden MenuButton that owns the right-click context popover.
    //
    // S72 fix for the M5 dispatch bug: a manually-popped PopoverMenu
    // parented to the panel via set_parent() + popup() does NOT wire
    // the action muxer fully — gtk_popover_present() must be called
    // during the parent's size_allocate cycle, which happens
    // automatically inside MenuButton (and PopoverMenuBar) but nowhere
    // else. Per GTK maintainer (GNOME Discourse 2023-10-11) and
    // confirmed empirically with the s72_m1/m2 repros: bare
    // set_parent + popup pops the menu but action-muxer wiring is
    // incomplete, so item clicks don't dispatch. The kebab menu in
    // this same panel works because it goes through MenuButton.
    //
    // Solution: route the right-click popover through a hidden
    // MenuButton helper that lives in the widget tree. The MenuButton
    // is sized 0x0 with opacity 0 (visible to the layout system so
    // it gets allocated and translate_coordinates works, but rendering
    // nothing). Each right-click sets a fresh popover on it, positions
    // the popover via set_pointing_to, and calls MenuButton::popup()
    // programmatically. This is the kebab pattern applied to context
    // menus — same dispatch path GTK explicitly supports.
    Gtk::MenuButton m_ctx_button;

    LibraryChangedSignal     m_sig_library_changed;
    InspectorRefreshSignal   m_sig_inspector_refresh_needed;

    // --- Handlers ----------------------------------------------------------
    void on_add_clicked();

    // Apply a swatch to the current selection. Reads Alt-modifier from
    // the supplied GTK modifier state; Alt → stroke, no Alt → fill.
    // Always touches the recents MRU.
    void apply_swatch(const color::SwatchId& id, unsigned gtk_modifiers);

    // Palette actions — wired to kebab menu items via m_palette_actions.
    void on_new_palette();
    void on_rename_palette();
    void on_delete_palette();
    void on_duplicate_palette();

    // Palette interchange (S101 m3). Three new entry points on the
    // kebab menu: bundled palettes (parameterised by stem), import a
    // .gpl file, and export the active palette as .gpl. All three
    // route through color::palette_io for the format ↔ library
    // translation; this class only handles the file dialog plumbing
    // and emitting library_changed afterwards.
    void on_load_bundled(const std::string& stem);
    void on_import_palette();
    void on_export_palette();

    // S101 m5: load a palette from ~/.config/curvz/palettes/<stem>.gpl
    // — the directory the export dialog defaults to. Same shape as
    // on_load_bundled but reads from disk rather than gresource. The
    // kebab populates this section dynamically from
    // color::palette_io::enumerate_user(), so user palettes appear
    // automatically once exported.
    void on_load_user(const std::string& stem);

    // Build (or rebuild) the kebab menu attached to m_btn_add.
    // Called unconditionally near the top of refresh() so the kebab
    // is reachable regardless of library state — particularly the
    // Load palette submenu, which is the primary first-use path
    // when the library is empty (S101 m3 fix2).
    //
    // Reads m_library to decide which sections to include:
    //   - Always: New swatch, New palette…, Load palette ▶
    //   - When active palette has swatches: Export current…
    //   - When at least one palette exists: Rename/Duplicate/Delete
    // A null library still produces a working menu with creation +
    // load entries — Import… is the empty-state escape hatch.
    void rebuild_kebab_menu();

    // Search predicate (S101 m4). Returns true iff the given swatch
    // should be visible for the given (already-lowercased) query. An
    // empty query always returns true. Match is case-insensitive
    // substring against three axes:
    //   - swatch.header.name
    //   - hex of swatch's solid colour, e.g. "#ff8000"
    //   - "r,g,b" decimal triple of swatch's solid colour
    // For non-solid swatches (future gradient kinds), name-only match
    // is used — there's no single hex to test against.
    bool swatch_matches_query(const color::Swatch& s,
                              const std::string& lowered_query) const;

    // Chip context-menu actions (M5) — fire on the swatch currently
    // recorded in m_ctx_swatch_id (set at right-click time).
    void on_ctx_edit_color();
    void on_ctx_rename_swatch();
    void on_ctx_duplicate_swatch();
    void on_ctx_delete_swatch();
    void on_ctx_move_to_palette(const color::PaletteId& target);

    // Open the color picker pre-loaded with the given swatch's current
    // colour. Subsequent callbacks call update_swatch on the same id.
    void open_edit_popover_for(const color::SwatchId& id, Gtk::Widget& anchor);

    // Recompute m_active_paint_swatch_id from the current canvas
    // selection. Safe to call with a null canvas or empty selection.
    void update_active_paint();

    // Ensure the library has at least one palette and an active_palette
    // set. If none exists, create "Default" and make it active. Returns
    // the active palette id (never empty after this call, as long as the
    // library is valid).
    color::PaletteId ensure_active_palette();

    // Prompt the user for a palette name via a simple modal dialog.
    // If `ok` is called, the callback runs; if cancelled, nothing happens.
    // Simple wrapper around Gtk::Dialog + Entry for M4 — may be replaced
    // by a richer widget later.
    void prompt_text(const std::string& title,
                     const std::string& initial,
                     std::function<void(const std::string&)> on_ok);

    // Render one chip into its DrawingArea. Shared between recents strip
    // and palette grid.
    void draw_chip(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h,
                   const color::Color& color, bool is_active_paint);

    // Build a chip widget (Button wrapping a DrawingArea) for the given
    // swatch id. Returns nullptr if the swatch is not in the library or
    // is a non-solid kind this build can't render yet.
    //
    // palette_context is the palette this chip is being rendered inside
    // (empty string for recents-only chips). Used by the right-click
    // menu to decide whether the move/add submenu reads "Add to
    // Palette" (recents-only) or "Move to Palette" (palette grid),
    // and by Duplicate to place the copy in the same palette.
    Gtk::Widget* make_chip(const color::SwatchId& id,
                           const color::PaletteId& palette_context);

    // Build a FlowBox of chips for a list of swatch ids. Shared between
    // recents strip (takes library->recents()) and palette grid (takes
    // active palette's ordered list).
    Gtk::FlowBox* build_chip_flow(const std::vector<color::SwatchId>& ids,
                                  const color::PaletteId& palette_context);
};

} // namespace Curvz

