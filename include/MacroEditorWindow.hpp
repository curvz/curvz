#pragma once
#include "MacroSystem.hpp"
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/grid.h>
#include <gtkmm/entry.h>
#include <gtkmm/frame.h>
#include <string>
#include <unordered_map>

namespace Curvz {

// ── MacroEditorWindow ─────────────────────────────────────────────────────────
// Floating (non-modal) window for inspecting and editing macro steps.
// One instance per open macro — MainWindow creates on demand.
//
// Layout (top → bottom):
//   Macro name label                           ← header
//   ─────────────────────────────────────────
//   Scrolled step list                         ← left ~60%
//   ─────────────────────────────────────────
//   Step property editor (right/bottom)        ← parameter grid
//   ─────────────────────────────────────────
//   [▲ Up] [▼ Down] [Delete] … [▶ Run from here]  ← footer
//
// Emits signal_run_from(macro_id, step_index) → MainWindow routes to Canvas
// ─────────────────────────────────────────────────────────────────────────────

class MacroEditorWindow : public Gtk::Window {
public:
    MacroEditorWindow();

    // Load a macro into the editor (replaces any previous content)
    void load_macro(const std::string& macro_id);

    // Show anchored to parent
    void show(Gtk::Window& parent);

    // Emitted when user clicks "Run from here"
    using RunFromSignal = sigc::signal<void(std::string /*macro_id*/, int /*step_idx*/)>;
    RunFromSignal& signal_run_from() { return m_sig_run_from; }

private:
    void rebuild_step_list();
    void rebuild_property_editor();
    void set_selected_step(int idx);

    // Footer actions
    void on_move_up();
    void on_move_down();
    void on_delete_step();
    void on_run_from();

    // Property editor commit — called when a spin/entry value changes
    void commit_property(const std::string& field, double value);
    void commit_property_str(const std::string& field, const std::string& value);

    // ── Widgets ───────────────────────────────────────────────────────────
    Gtk::Label          m_macro_name_lbl;

    Gtk::ScrolledWindow m_step_scroll;
    Gtk::Box            m_step_list   { Gtk::Orientation::VERTICAL };

    Gtk::Frame          m_prop_frame;
    Gtk::Grid           m_prop_grid;

    Gtk::Box            m_footer      { Gtk::Orientation::HORIZONTAL };
    Gtk::Button         m_btn_up;
    Gtk::Button         m_btn_down;
    Gtk::Button         m_btn_delete;
    Gtk::Button         m_btn_run_from;

    // ── State ─────────────────────────────────────────────────────────────
    std::string m_macro_id;
    int         m_selected_step = -1;   // index into macro.steps

    // Spin buttons kept alive for property editor (rebuilt per step)
    std::vector<Glib::RefPtr<Gtk::Adjustment>> m_adjs;

    // MacroManager changed connection
    sigc::connection m_changed_conn;

    RunFromSignal m_sig_run_from;
};

} // namespace Curvz
