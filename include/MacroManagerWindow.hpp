#pragma once
#include "MacroSystem.hpp"
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/entry.h>
#include <gtkmm/revealer.h>
#include <gtkmm/popover.h>
#include <functional>
#include <unordered_map>
#include <string>

namespace Curvz {

// ── MacroManagerWindow ────────────────────────────────────────────────────────
// Floating (non-modal) window that is the authoritative control centre for all
// macros.  One instance lives in MainWindow; shown/hidden by the toolbar button.
//
// Layout (top→bottom):
//   [Record ●] [Stop ■]   [Run ▶ Ctrl+M]       ← action bar
//   ──────────────────────────────────────────
//   Scrolled folder/macro tree                  ← content area
//   ──────────────────────────────────────────
//   [+ Folder]  [+ Macro]  [Delete]             ← footer bar
//
// Each macro row has:
//   ★ (layer-panel toggle)  |  name label  |  [▶ Run]  [✎ Edit]
//
// Emits signal_run_macro(macro_id) → MainWindow calls Canvas::run_macro()
// Emits signal_edit_macro(macro_id) → MainWindow opens MacroEditorWindow
// ─────────────────────────────────────────────────────────────────────────────

class MacroManagerWindow : public Gtk::Window {
public:
    MacroManagerWindow();

    // Show anchored to a parent window
    void show(Gtk::Window& parent);

    // Called by MainWindow after recording stops to refresh display
    void refresh();

    // Signals out to MainWindow
    using RunSignal  = sigc::signal<void(std::string /*macro_id*/)>;
    using EditSignal = sigc::signal<void(std::string /*macro_id*/)>;
    RunSignal&  signal_run_macro()  { return m_sig_run;  }
    EditSignal& signal_edit_macro() { return m_sig_edit; }

private:
    void rebuild();

    // Action bar
    void on_record_clicked();
    void on_stop_clicked();
    void on_run_clicked();

    // Footer
    void on_add_folder();
    void on_add_macro();
    void on_delete_selected();

    // Row interactions
    void on_macro_star_toggled(const std::string& macro_id, bool starred);
    void on_macro_run(const std::string& macro_id);
    void on_macro_edit(const std::string& macro_id);
    void set_selected(const std::string& macro_id);

    // Rename in-place (double-click label)
    void begin_rename_macro(const std::string& macro_id);
    void begin_rename_folder(const std::string& folder_id, Gtk::Label* lbl);

    // Recording indicator
    void update_record_ui();

    // ── Widgets ───────────────────────────────────────────────────────────
    // Action bar
    Gtk::Box    m_action_bar   { Gtk::Orientation::HORIZONTAL };
    Gtk::Button m_btn_record;
    Gtk::Button m_btn_stop;
    Gtk::Button m_btn_run;
    Gtk::Label  m_current_label;   // shows name of current macro

    // Content
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_content  { Gtk::Orientation::VERTICAL };

    // Footer
    Gtk::Box    m_footer       { Gtk::Orientation::HORIZONTAL };
    Gtk::Button m_btn_add_folder;
    Gtk::Button m_btn_add_macro;
    Gtk::Button m_btn_delete;

    // ── State ─────────────────────────────────────────────────────────────
    std::string m_selected_macro_id;   // highlighted row
    std::string m_selected_folder_id;  // for add_macro / delete

    // Track which folder rows are expanded (folder_id → bool)
    std::unordered_map<std::string, bool> m_folder_expanded;

    // Per-rebuild widget maps (s108 m6) — keyed by macro internal_id.
    // Populated during rebuild(); used by the row click handler to
    // swap the .macro-row-selected and .accent CSS classes directly,
    // since a full rebuild on click would tear down the widget tree
    // before the double-click press count can be reset.
    // Cleared at the top of rebuild() so stale pointers never leak
    // into the next pass.
    std::unordered_map<std::string, Gtk::Box*>   m_macro_rows;
    std::unordered_map<std::string, Gtk::Label*> m_macro_name_labels;

    RunSignal  m_sig_run;
    EditSignal m_sig_edit;

    // MacroManager changed connection
    sigc::connection m_changed_conn;
};

} // namespace Curvz
