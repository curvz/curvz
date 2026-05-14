#pragma once
#include "MacroSystem.hpp"
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/revealer.h>
#include <gtkmm/popover.h>
#include <functional>
#include <unordered_map>
#include <string>

// s213 m3 — forward-declare the substrate widget types used by
// pointer-held members below. Full includes live in the .cpp (the
// s208 m5 / s212 m3 / s213 m2 header-coupling discipline).
namespace curvz::widgets {
class Button;
}  // namespace curvz::widgets

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
    // s213 m3 — value→pointer flips for the action-bar + footer Buttons.
    // Substrate ctors take the abbrev as a build-time arg, so value-held
    // members can't carry the substrate type — they flip to pointer-held
    // with construction moved into the ctor body. Same shape as
    // TranslateDialog s212 m3, PrintManager s213 m2, and the s198 m1
    // Toolbar precedent. All six are persistent for app lifetime
    // (MacroManagerWindow is MainWindow-owned and hide-on-close), so
    // registered substrate is the right choice — no rebuild-path
    // collision risk.
    curvz::widgets::Button* m_btn_record = nullptr;
    curvz::widgets::Button* m_btn_stop   = nullptr;
    curvz::widgets::Button* m_btn_run    = nullptr;
    Gtk::Label  m_current_label;   // shows name of current macro

    // Content
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_content  { Gtk::Orientation::VERTICAL };

    // Footer
    Gtk::Box    m_footer       { Gtk::Orientation::HORIZONTAL };
    curvz::widgets::Button* m_btn_add_folder = nullptr;
    curvz::widgets::Button* m_btn_add_macro  = nullptr;
    curvz::widgets::Button* m_btn_delete     = nullptr;

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
