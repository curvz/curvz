#pragma once
//
// ThemesDialog — apply / manage / import / export themes (S103 m3).
//
// One dialog, two surfaces stacked vertically:
//
//   ── APPLY pane (top) ─────────────────────────────────────────────
//     Source mode:  (•) Saved theme   ( ) Live document
//                   [ dropdown: <saved themes> ]   ← sensitive when (•) Saved
//                   [ dropdown: <documents> ]      ← sensitive when (•) Live
//
//     Targets:      [✓] Doc A      ← active source's row is disabled
//                   [✓] Doc B
//                   [✓] Doc C
//                   [Select all] [Select none]
//
//     Hint:         This action is not undoable.
//
//                   [Save current as theme…]  [Apply]
//
//   ── LIBRARY pane (bottom) ────────────────────────────────────────
//     Saved themes
//     ┌──────────────────────────────────────────┐
//     │  My Default                  [✎] [⎘] [✕] │
//     │  Print Setup                 [✎] [⎘] [✕] │
//     │  Web Sketch                  [✎] [⎘] [✕] │
//     └──────────────────────────────────────────┘
//                                          [⋮ Import / Export ▾]
//
//   ── Footer ───────────────────────────────────────────────────────
//                                                            [Close]
//
// Why one dialog with two panes (vs tabs, vs two dialogs):
//   * Themes are infrequent — opening a dialog at all is the rare
//     event, so we don't want users to chase a second dialog or tab
//     to do "save current and apply to other docs" (which is the
//     common combined flow). Having both surfaces visible at once
//     means the apply dropdown's choices update visibly when the
//     library changes (post-import, post-rename, post-delete) without
//     a tab switch.
//   * The dialog is bounded — sub-100 themes is the practical ceiling,
//     same as styles. A single scrollable listbox handles it without
//     paging.
//
// Why the dual-source dropdown is realised as two radio-toggled
// dropdowns rather than a single composite dropdown with section
// headers:
//   * GTK4's Gtk::DropDown does not natively render section headers
//     between items. Faking them via a custom factory works but
//     introduces ad-hoc widget plumbing and accessibility caveats.
//   * Two dropdowns + a radio toggle is the established GTK idiom for
//     "two sources, pick which kind first". Each dropdown has a
//     simple StringList; the un-active one is set_sensitive(false).
//   * The "Guest in GTK's home" principle (s99 carryover): when a
//     widget doesn't natively express our intent, default to using
//     two widgets that do, not to bending one into shape.
//
// Apply contract (per Theme.hpp top-of-file):
//   * Atomic — the whole bundle writes per target.
//   * One-shot — no bindings; once applied, the doc has those values.
//   * NOT undoable in v1 — the hint label warns the user. Apply walks
//     the targets calling apply_theme_to_doc(theme, *doc) for each
//     and queues a redraw + inspector refresh.
//
// Library mutations (rename / duplicate / delete / import) ARE
// undoable via UpdateThemeCommand / AddThemeCommand / RemoveThemeCommand
// pushed onto the project's CommandHistory. Import is atomic via
// CompositeCommand wrapping every successful AddThemeCommand
// (mirrors S102 m1 styles import).
//
// Lifetime:
//   Self-managed. The host (MainWindow) constructs with `new
//   ThemesDialog(...)`; the dialog holds an internal Gtk::Window
//   (managed) and self-deletes via signal_close_request → idle
//   delete-this. Same pattern as StyleEditorDialog.
//

#include "theme/ThemeLibrary.hpp"

#include <functional>
#include <optional>
#include <vector>
#include <string>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>
#include <giomm/simpleactiongroup.h>
#include <sigc++/connection.h>

namespace Curvz {

class CurvzProject;
class CommandHistory;

class ThemesDialog {
public:
    // Called after any apply or library mutation so the host can
    // refresh canvas + inspector. The dialog itself rebuilds its own
    // panes after every mutation (debounced on signal_idle to coalesce
    // the per-row-button bursts).
    using OnChangedCb = std::function<void()>;

    // Construct + present.
    //
    // The dialog owns itself: present() is called at the end of the
    // ctor, and the dialog destroys itself on close (Close button,
    // window close).
    //
    // Parameters:
    //   parent   — transient-for. Required.
    //   project  — borrowed; must outlive the dialog. We mutate
    //              project->themes (the library) and project->documents
    //              via apply_theme_to_doc on selected targets, plus
    //              project->snap mirror after apply if the active doc
    //              was a target (matches the existing snap-edit paths
    //              in PropertiesPanel and MainWindow).
    //   history  — borrowed. Library mutations push commands here so
    //              Ctrl+Z reverts them. Apply does NOT push commands
    //              (non-undoable in v1 — the hint label warns).
    //   on_changed — fires after any successful mutation so the host
    //              can queue_draw / refresh_inspector. Optional;
    //              dialog is self-sufficient if null (it just rebuilds
    //              its own panes).
    ThemesDialog(Gtk::Window&    parent,
                 CurvzProject*   project,
                 CommandHistory* history,
                 OnChangedCb     on_changed);

private:
    // ── Build helpers (called once from ctor) ──────────────────────
    void build();
    void build_apply_pane(Gtk::Box& root);
    void build_library_pane(Gtk::Box& root);
    void build_footer(Gtk::Box& root);

    // ── Refresh ────────────────────────────────────────────────────
    //
    // rebuild() throws away both pane contents and re-populates from
    // current project state. Called on construct, on every library
    // signal (added / changed / removed), and after apply (so a doc
    // rename via apply — wait, we don't rename docs on apply — so
    // really only library signals).
    void rebuild();
    void rebuild_apply_pane();
    void rebuild_library_pane();

    // ── Apply pane wiring ──────────────────────────────────────────
    //
    // Update the targets-list rows' enabled state based on the
    // current source mode + selected source doc. Called when source
    // mode changes or the doc dropdown selection changes.
    void update_targets_for_source();

    // Resolve the current source into a Theme value. For "Saved
    // theme" mode, looks up the dropdown's selected ThemeId in the
    // library. For "Live document" mode, calls
    // capture_theme_from_doc() on the selected doc.
    //
    // Returns nullopt on no-active-source (e.g. Saved-theme mode
    // selected but library is empty).
    std::optional<theme::Theme> current_source_theme() const;

    // Action handlers
    void on_apply();
    void on_save_current_as_theme();
    void on_select_all_targets();
    void on_select_no_targets();

    // ── Library pane wiring ─────────────────────────────────────────
    //
    // Per-row icon button handlers. They look up the theme by ThemeId
    // (captured into the lambda) so a rebuild that invalidates the
    // row pointer is safe.
    void on_rename_theme(const theme::ThemeId& id);
    void on_duplicate_theme(const theme::ThemeId& id);
    void on_delete_theme(const theme::ThemeId& id);

    // Import / Export — match StylesPanel's pattern. Import opens a
    // file dialog, decodes via theme_io::load_path, calls
    // import_themes_into_library (atomic-undo). Export snapshots the
    // user tier and writes it via theme_io::write_path.
    void on_import_themes();
    void on_export_themes();

    // ── Helpers ─────────────────────────────────────────────────────
    //
    // Display name for a CurvzDocument — falls back to "Untitled" if
    // filename is empty (defensive; on-disk docs always have one).
    static std::string doc_display_name(const class CurvzDocument& doc,
                                        std::size_t fallback_index);

    // Active-doc index check — the dialog sets this on construct so
    // the source-doc dropdown and the targets list can flag "active"
    // visually. Refreshed in rebuild(). The mismatch case (active
    // index out of range) just clears it; defensive.
    int active_doc_index() const;

    // Notify host. Always also rebuilds locally.
    void notify_changed();

    // Detach signal connections from the project's library on
    // destruction. Held as sigc::connection so we can disconnect
    // cleanly — the dialog can be destroyed before the project, in
    // which case the connections must not fire on a deleted dialog.
    void connect_library_signals();
    void disconnect_library_signals();

    // ── State ───────────────────────────────────────────────────────
    Gtk::Window*    m_window  = nullptr;       // self-managed
    CurvzProject*   m_project = nullptr;       // borrowed
    CommandHistory* m_history = nullptr;       // borrowed
    OnChangedCb     m_on_changed;

    // Library signal connections — disconnected in close handler.
    sigc::connection m_conn_added;
    sigc::connection m_conn_changed;
    sigc::connection m_conn_removed;

    // True while rebuild() is in progress so dropdown signal handlers
    // skip their write-back path. Same idiom as PropertiesPanel's
    // m_loading guard.
    bool m_loading = false;

    // ── Apply pane widgets ──────────────────────────────────────────
    Gtk::CheckButton* m_radio_saved   = nullptr;  // "Saved theme"
    Gtk::CheckButton* m_radio_doc     = nullptr;  // "Live document"
    Gtk::DropDown*    m_drop_saved    = nullptr;  // saved-theme picker
    Gtk::DropDown*    m_drop_doc      = nullptr;  // doc picker

    // Parallel id arrays. Index into the dropdowns' StringList rows
    // gives the corresponding ThemeId / doc index. Empty list → empty
    // arrays.
    std::vector<theme::ThemeId> m_drop_saved_ids;
    std::vector<int>            m_drop_doc_indices;

    // Targets pane
    Gtk::Box*  m_targets_list   = nullptr;        // vertical box of CheckButtons
    std::vector<Gtk::CheckButton*> m_target_checks;  // parallel to project->documents
    Gtk::Button* m_btn_apply    = nullptr;
    Gtk::Button* m_btn_save_cur = nullptr;

    // ── Library pane widgets ────────────────────────────────────────
    Gtk::Box*           m_library_list  = nullptr;  // vertical box of theme rows
    Gtk::Label*         m_library_empty = nullptr;  // shown when user tier empty
    Gtk::MenuButton*    m_btn_io_kebab  = nullptr;  // import/export popover

    Glib::RefPtr<Gio::SimpleActionGroup> m_actions;
};

} // namespace Curvz
