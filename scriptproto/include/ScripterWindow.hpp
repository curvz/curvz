// ScripterWindow.hpp ─────────────────────────────────────────────────────────
//
// Second top-level window. Drives the base window (DemoWindow) via the
// ScriptableRegistry — same process, same address space, no wire
// protocol. Watch base and scripter side by side as the script runs.
//
// Layout:
//
//   ┌──────────────────────────────────────────────────────────────────┐
//   │  [Folder…]  [Reload]                    [Clear output]  [Run]    │
//   ├───────────┬──────────────────────────────┬───────────────────────┤
//   │           │                              │                       │
//   │ Script    │  Script editor (TextView)    │  Output (TextView,    │
//   │ library   │                              │   read-only,          │
//   │ (ListBox) │                              │   monospace)          │
//   │           │                              │                       │
//   │           │                              │                       │
//   └───────────┴──────────────────────────────┴───────────────────────┘
//
// Library: lists *.curvzs files in the current folder. Click a row,
// the script body loads into the editor. Edit freely in the editor;
// Run dispatches the editor's current body (NOT a re-read of the file).
//
// Output: streams the listener's output through the OutputCallback.
// Cleared on each Run start.

#pragma once
#include "ScriptListener.hpp"

#include <gtkmm/applicationwindow.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/textview.h>
#include <gtkmm/label.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace scriptproto {

class ScripterWindow : public Gtk::ApplicationWindow {
public:
    explicit ScripterWindow(const std::string& initial_folder);

private:
    void build_ui();
    void rescan_library();
    void load_script_into_editor(const std::filesystem::path& p);
    void on_run();
    void on_clear_output();
    void on_folder_pick();
    void append_output(const std::string& s);

    std::filesystem::path m_folder;

    // Top bar
    Gtk::Button m_btn_folder      { "Folder…" };
    Gtk::Button m_btn_reload      { "Reload" };
    Gtk::Button m_btn_clear       { "Clear output" };
    Gtk::Button m_btn_run         { "Run" };
    Gtk::Label  m_lbl_folder;

    // Library list
    Gtk::ListBox          m_library;
    Gtk::ScrolledWindow   m_library_scroll;

    // Editor pane
    Gtk::TextView         m_editor;
    Gtk::ScrolledWindow   m_editor_scroll;

    // Output pane
    Gtk::TextView         m_output;
    Gtk::ScrolledWindow   m_output_scroll;

    // Listener — one instance, reused across runs via reset() +
    // load_text() each click of Run.
    std::unique_ptr<ScriptListener> m_listener;

    // Cached script paths in the same order as the ListBox rows.
    std::vector<std::filesystem::path> m_scripts;

    // Re-entrancy guard — prevents stacking multiple idle pumps if
    // Run is mashed while a run is in progress.
    bool m_running = false;
};

} // namespace scriptproto
