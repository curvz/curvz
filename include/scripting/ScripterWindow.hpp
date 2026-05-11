// scripting/ScripterWindow.hpp ───────────────────────────────────────────────
//
// Second top-level window. Drives the main Curvz window via the
// ScriptableRegistry — same process, same address space, no wire
// protocol. Watch Curvz and the scripter side by side as the script
// runs.
//
// Layout (same as sandbox):
//
//   ┌──────────────────────────────────────────────────────────────────┐
//   │  [Folder…]  [Reload]                    [Clear output]  [Run]    │
//   ├───────────┬──────────────────────────────┬───────────────────────┤
//   │ Script    │  Script editor (TextView)    │  Output (TextView,    │
//   │ library   │                              │   read-only,          │
//   │ (ListBox) │                              │   monospace)          │
//   └───────────┴──────────────────────────────┴───────────────────────┘
//
// Library: lists *.curvzs files in the current folder. Click a row,
// the script body loads into the editor. Edit freely in the editor;
// Run dispatches the editor's current body.
//
// Output: streams the listener's output through the OutputCallback.
// Cleared on each Run start.
//
// Theme integration: this is a regular Gtk::ApplicationWindow, so
// MainWindow::apply_motif_to_window() walks every top-level via
// gtk_window_get_toplevels() and stamps the curvz-light CSS class
// automatically. The Scripter window picks it up like every other
// surface.
//
// Lifted from scriptproto/ during s186 m2. Gated by CURVZ_DIAGNOSTIC.

#pragma once
#include "scripting/ScriptListener.hpp"

#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textview.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace curvz::scripting {

class ScripterWindow : public Gtk::ApplicationWindow {
public:
    explicit ScripterWindow(const std::string& initial_folder);

private:
    void build_ui();
    void rescan_library();
    void load_script_into_editor(const std::filesystem::path& p);
    void on_run();
    void on_clear_output();
    void on_copy_output();
    void on_folder_pick();
    void append_output(const std::string& s);

    std::filesystem::path m_folder;

    Gtk::Button m_btn_folder      { "Folder…" };
    Gtk::Button m_btn_reload      { "Reload" };
    Gtk::Button m_btn_clear       { "Clear output" };
    Gtk::Button m_btn_copy        { "Copy output" };
    Gtk::Button m_btn_run         { "Run" };
    Gtk::Label  m_lbl_folder;

    // s187 m4 — pacing knob. Step delay (ms) between script lines.
    // 0 = run as fast as the scheduler allows (signal_timeout(0) chain,
    // which still yields to the main loop between lines so signals
    // dispatch). 5+ = visible pacing for "watch Curvz drive itself"
    // demos. 1000+ = slow enough to read each line as it dispatches.
    // Range 0–5000ms. Default 5ms (matches the pre-m4 hard-coded value
    // so existing scripts behave identically out of the box).
    Gtk::Label       m_lbl_delay  { "Step delay (ms):" };
    Gtk::SpinButton  m_spn_delay;

    Gtk::ListBox          m_library;
    Gtk::ScrolledWindow   m_library_scroll;

    Gtk::TextView         m_editor;
    Gtk::ScrolledWindow   m_editor_scroll;

    Gtk::TextView         m_output;
    Gtk::ScrolledWindow   m_output_scroll;

    std::unique_ptr<ScriptListener> m_listener;

    std::vector<std::filesystem::path> m_scripts;

    bool m_running = false;
};

} // namespace curvz::scripting
