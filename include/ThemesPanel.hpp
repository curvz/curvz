#pragma once
//
// ThemesPanel — Content panel surface for the project's theme library
// and apply flow. s147 m3 replacement for ThemesDialog under the
// "discovery beats commit" / "in your face" reframe.
//
// Sits in the right pane's Content group between Styles and Documents.
// Always visible, no menu entry to open it, no modal-dialog wrapper
// around the experience. Mirrors the shape of LibraryPanel / SwatchesPanel
// / StylesPanel — non-owning project pointer, library-signal subscription,
// rebuild on change.
//
// Pane layout (top to bottom inside the section body):
//
//   ── Library list ─────────────────────────────────────────────────
//     Saved themes (one row per user theme)
//     ┌────────────────────────────────────────────┐
//     │  My Default                  [✎] [⎘] [✕]   │
//     │  Print Setup                 [✎] [⎘] [✕]   │
//     │  Web Sketch                  [✎] [⎘] [✕]   │
//     └────────────────────────────────────────────┘
//                                          [+] [⋮]
//     [+]      = save active doc as new theme (name prompt)
//     [⋮]      = import / export popover
//
//   ── Apply targets ────────────────────────────────────────────────
//     Apply to:
//       [✓] Doc A
//       [✓] Doc B
//       [✓] Doc C
//
//   ── Footer ───────────────────────────────────────────────────────
//     Click a theme above to use it as the source.
//     Hint label: "Applying a theme is not undoable."
//                                                       [Apply]
//
// Click model:
//   * Click a library row → that row is the SOURCE for the next apply.
//     Visually marked (CSS class on the selected row); the Apply button
//     becomes sensitive.
//   * Apply button → confirmation modal listing source + target docs.
//     Confirm runs apply_theme_to_doc(theme, doc) for each ticked
//     target. Cancel aborts.
//   * Library row icon buttons (rename / duplicate / delete) act on
//     that row's theme; selection state for "apply source" is
//     orthogonal.
//
// What's gone vs the old dialog:
//   * No "Live document" source mode. Source is always a saved theme.
//     The rare "apply doc A's settings to docs B/C" flow is dropped;
//     it can return as a per-row context-menu sub-affordance later
//     if real demand materialises.
//   * No "Save current as theme…" button next to Apply — the [+] at
//     the library footer is the canonical "new theme from active doc"
//     action, matching StylesPanel's [+] idiom.
//   * No top-level Close button — the panel never closes.
//
// What stays:
//   * Library mutations (rename / dup / del / import / export) push
//     UpdateThemeCommand / AddThemeCommand / RemoveThemeCommand onto
//     the project's CommandHistory. Import is atomic via
//     CompositeCommand — same as the dialog and StylesPanel import.
//   * Apply is NOT undoable. The hint label warns. The confirmation
//     modal (new in this milestone) is the safety net replacing the
//     "the dialog is in front of you and you have to mean it" psychology
//     of the old design.
//   * "Always non-empty name" rule. Empty rename is a no-op; auto-name
//     "Theme N" walks up against existing names.
//

#include "theme/ThemeLibrary.hpp"

#include <giomm/simpleactiongroup.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <sigc++/connection.h>

#include <functional>
#include <string>
#include <vector>

namespace Curvz {

class CurvzProject;
class CommandHistory;
class CurvzDocument;

class ThemesPanel : public Gtk::Box {
public:
    ThemesPanel();
    ~ThemesPanel();

    // Called after any library mutation or apply so the host can
    // queue_draw + refresh_inspector. Optional; panel is self-sufficient
    // (it rebuilds its own contents) if null.
    using OnChangedCb = std::function<void()>;

    // Non-owning. Caller must outlive the panel or call set_project(nullptr)
    // before destruction. Hooks the theme library's three mutation
    // signals (added / changed / removed) and triggers a refresh on
    // each. Also re-reads the document list so the targets section
    // stays in sync as docs are added/removed/renamed.
    void set_project(CurvzProject* project);

    // Non-owning. Library mutations push commands here for Ctrl+Z
    // round-trip. Apply does NOT push commands (non-undoable in v1).
    void set_history(CommandHistory* history) { m_history = history; }

    // Notify the host after any apply or library mutation. The panel
    // also rebuilds its own content unconditionally — the callback is
    // strictly for canvas redraw / inspector refresh / save-schedule.
    void set_on_changed(OnChangedCb cb) { m_on_changed = std::move(cb); }

    // Rebuild from scratch — both the library list and the targets
    // section. Idempotent. Called from library signal handlers and
    // from set_project().
    void refresh();

    // Called by MainWindow when the active document changes (or the
    // doc list gains/loses a doc). Rebuilds the targets section so
    // "Doc A / Doc B / Doc C" rows match the current project state
    // and the active doc is visually marked.
    void on_documents_changed();

private:
    // ── Build helpers ──────────────────────────────────────────────
    void build_ui();                  // called once from ctor
    void rebuild_library_list();
    void rebuild_targets_list();
    void update_apply_button_state();

    // Mark a library row as the active source (for the next Apply).
    // Pass empty id to deselect. Updates row CSS class and apply button.
    void set_selected_source(const theme::ThemeId& id);

    // ── Library mutation handlers — lifted near-verbatim from
    // ThemesDialog. The behaviour is identical; only the parent
    // window for transient prompts changes (panel finds it via
    // get_root() instead of holding it directly).
    void on_rename_theme(const theme::ThemeId& id);
    void on_duplicate_theme(const theme::ThemeId& id);
    void on_delete_theme(const theme::ThemeId& id);
    void on_save_current_as_theme();
    void on_import_themes();
    void on_export_themes();

    // ── Apply flow ────────────────────────────────────────────────
    // Apply button click — pops confirmation modal, on confirm
    // walks the targets list and applies. Mirrors the dialog's
    // on_apply but adds the confirmation step.
    void on_apply_clicked();

    // Targets-list helpers — lifted from ThemesDialog.
    void on_select_all_targets();
    void on_select_no_targets();

    // ── Helpers ───────────────────────────────────────────────────
    Gtk::Window* root_window();
    static std::string doc_display_name(const CurvzDocument& doc,
                                        std::size_t fallback_index);
    int active_doc_index() const;
    void notify_changed();

    void connect_library_signals();
    void disconnect_library_signals();

    // ── State ─────────────────────────────────────────────────────
    CurvzProject*   m_project = nullptr;
    CommandHistory* m_history = nullptr;
    OnChangedCb     m_on_changed;

    sigc::connection m_conn_added;
    sigc::connection m_conn_changed;
    sigc::connection m_conn_removed;

    // The currently-selected library row's theme id (empty when none).
    theme::ThemeId m_selected_id;

    // ── Library section widgets ────────────────────────────────────
    Gtk::Box*        m_library_list  = nullptr;  // VERTICAL box of theme rows
    Gtk::Label*      m_library_empty = nullptr;  // shown when user tier empty
    Gtk::Button*     m_btn_save      = nullptr;  // "+" — save active as theme
    Gtk::MenuButton* m_btn_io_kebab  = nullptr;  // import/export popover

    // ── Targets section widgets ────────────────────────────────────
    Gtk::Box*    m_targets_list = nullptr;       // VERTICAL box of CheckButtons
    std::vector<Gtk::CheckButton*> m_target_checks;  // parallel to project->documents
    Gtk::Button* m_btn_apply    = nullptr;
    Gtk::Label*  m_hint_label   = nullptr;       // "Not undoable" warning

    Glib::RefPtr<Gio::SimpleActionGroup> m_actions;
};

} // namespace Curvz
