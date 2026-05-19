// scripting/ScripterWindow.hpp ───────────────────────────────────────────────
//
// Second top-level window. Drives the main Curvz window via the
// ScriptableRegistry — same process, same address space, no wire
// protocol. Watch Curvz and the scripter side by side as the script
// runs.
//
// Layout (s193 m1 redesign):
//
//   ┌──────────────────────────────────────────────────────────────────┐
//   │  📁 …/tests/scripts/ [Change…] [Reload]  │ ▶ Run ☐ Step [delay] │
//   ├─────────────────────┬─────────────────────────────────────────────┤
//   │ ▾ All Scripts       │  ┌───────────┬───────────┐                  │
//   │   ☐ 01_node_…       │  │ Script    │ Output    │                  │
//   │   ☑ 02_node_…       │  ├───────────┴───────────┴──────────────────┤
//   │ ▸ Tutorials         │  │  (editor / output buffer)                │
//   │ ▸ Diagnostics       │  │                                          │
//   └─────────────────────┴──────────────────────────────────────────────┘
//
// Library: subfolders under the workspace become collapsible groups
// (Gtk::Expander). Loose *.curvzs at the workspace root sit in an
// "All Scripts" group. Each script row carries a checkbox + filename.
//
//   - Checkbox  = "include this script in the next Run". Multiple
//                 checks = run-in-sequence.
//   - Row body  = "load this script into the editor" (one at a time).
//
// Run-set semantics:
//   - 1+ checked → those scripts run in order. The editor's current
//                  contents are NOT used.
//   - 0 checked  → the editor's current contents run (the historical
//                  shape: "edit freely, run what's in front of you").
//
// Editor + Output sit in a Gtk::Notebook (tabs: Script, Output). Run
// auto-switches to Output. A Save button writes the editor back to
// the loaded file; the editor tab carries a `*` marker when dirty,
// and clicking a different library row while dirty prompts to discard.
//
// Theme integration: Curvz's MainWindow::apply_motif_to_window() walks
// every top-level GTK window via gtk_window_get_toplevels() and stamps
// the curvz-light CSS class, so this window picks up Dark/Light
// automatically.
//
// Lifted from scriptproto/ during s186 m2. Gated by CURVZ_DIAGNOSTIC.

#pragma once
#include "scripting/ScriptListener.hpp"

#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/expander.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/notebook.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/textbuffer.h>
#include <gtkmm/texttag.h>
#include <gtkmm/textview.h>

// s206 m1 — Scripter substrate migration. Singleton value-held members
// flipped to pointer-held substrate widgets so they register in the
// ScriptableRegistry and become scriptable. The Scripter now drives the
// Scripter (recursion of the self-running sort).
#include "widgets/Button.hpp"
#include "widgets/CheckButton.hpp"
#include "widgets/SpinButton.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace curvz::scripting {

class ScripterWindow : public Gtk::Window {
public:
    explicit ScripterWindow(const std::string& initial_folder);

    // s219 m1 — show pattern matching the rest of Curvz's persistent
    // floating dialogs (HelpWindow, ShortcutsDialog, BlendDialog,
    // MacroEditorWindow, MacroManagerWindow). set_transient_for(parent)
    // declares the window secondary on every show — mutter needs this
    // every time, not just once at construction, to keep the
    // titlebar decorations responsive across hide/show cycles. Apply
    // the motif class so first-show is themed correctly.
    void show(Gtk::Window& parent);

    // s191 m3 — Application bridges the listener's SubtitleCallback
    // to MainWindow's caption bar. Exposed as a raw pointer (the
    // unique_ptr keeps ownership inside ScripterWindow); null if
    // construction has not yet wired m_listener (should never be
    // observed by a caller in practice).
    ScriptListener* listener() { return m_listener.get(); }

private:
    // ── Lifecycle ───────────────────────────────────────────────────────
    void build_ui();
    void rescan_library();
    void on_folder_pick();

    // s195 m2 — initial sidebar width tuned to the longest filename in
    // the workspace. Pre-realization Pango isn't available, so we use a
    // per-char heuristic against the longest filename across loose
    // scripts and one level of subfolder, plus fixed overhead for the
    // checkbox + margins + scrollbar. Result clamped to a sensible
    // range so a workspace with only short filenames doesn't shrink the
    // sidebar absurdly, and one with monster filenames doesn't grow it
    // beyond half the window.
    int compute_initial_sidebar_width() const;

    // ── Library / editor ────────────────────────────────────────────────
    // s193 m1: a row in the library is one of these. Created on
    // rescan_library() and pinned for the lifetime of the scan; the
    // managed widgets get re-parented by GTK on the next rescan.
    // s195 m4: rows gain a flat trash button at the right end.
    struct ScriptRow {
        std::filesystem::path  path;
        Gtk::CheckButton*      check  = nullptr;  // managed
        Gtk::Button*           label  = nullptr;  // managed (flat-styled)
        Gtk::Button*           del    = nullptr;  // managed (flat-styled, s195 m4)
    };

    void load_script_into_editor(const std::filesystem::path& p);
    void save_editor_to_loaded_file();
    // s195 m1 — Save As… for the scratchpad (and for branching off a
    // loaded file under a new name). Opens a FileDialog rooted at the
    // workspace, writes the editor contents, adopts the new path as
    // m_loaded_path, and triggers a library rescan so the new file
    // shows up in the sidebar immediately.
    void on_save_as();
    // s195 m4 — Delete from the sidebar row. Confirms with an
    // AlertDialog, sends the file to the system trash (Gio::File::trash
    // — recoverable, matches what Nautilus does), rescans the library,
    // and if the deleted file was the currently loaded one, clears
    // m_loaded_path and marks the editor dirty (the text is still in
    // memory but no longer has a backing file).
    void confirm_and_delete(const std::filesystem::path& p);
    void try_load_with_dirty_check(const std::filesystem::path& p);
    void mark_dirty(bool dirty);
    void update_editor_tab_title();

    // s193 m1 — step-feedback. While parked waiting for spacebar in
    // step mode, the editor highlights the line that the next press
    // will dispatch. Pass -1 to clear the highlight. The editor also
    // scrolls so the highlighted line is visible.
    void highlight_step_line(int line_index);

    // ── Run plumbing ────────────────────────────────────────────────────
    void on_run();
    void on_run_or_stop();              // s193 m1: dual-mode Run/Stop
    void abort_run();                   // s193 m1: clean abort path
    void update_run_button_label();     // s193 m1: "Run" vs "Stop"
    void run_next_in_queue();           // s193 m1: pump the multi-script queue
    void start_single_script(const std::string& body,
                              const std::string& display_name);

    // ── Output helpers ──────────────────────────────────────────────────
    void on_clear_output();
    void on_copy_output();
    void append_output(const std::string& s);
    void show_output_tab();              // s193 m1: auto-switch on Run

    // ── State ──────────────────────────────────────────────────────────
    std::filesystem::path m_folder;

    // s193 m1 redesign — top toolbar is playback-only. Output buffer
    // actions (Clear, Copy) live in the Output tab's content header;
    // Save lives in the Script tab's content header; Reload lives in
    // the sidebar header; Change-folder lives in the statusbar at the
    // window bottom. Every action sits next to its target.

    // Playback cluster.
    // s206 m1 — flipped value→pointer for substrate migration. Members
    // are constructed in build_ui() with their abbrevs; labels formerly
    // in the brace-init move to the substrate ctor's label arg (Button)
    // or to set_label() after construction (CheckButton lays out the
    // label internally — same shape).
    curvz::widgets::Button*       m_btn_run    = nullptr;

    // s187 m4 — pacing knob. Step delay (ms) between script lines.
    Gtk::Label                    m_lbl_delay  { "Step delay (ms):" };
    curvz::widgets::SpinButton*   m_spn_delay  = nullptr;

    // s193 m1 — step-through playback. When checked, Run pauses
    // between script lines and advances on spacebar.
    curvz::widgets::CheckButton*  m_btn_step   = nullptr;

    // s193 m2 — auto-lower. When checked + timed playback, Run lowers
    // the Scripter window behind MainWindow at start and presents it
    // back at end. Lets the user keep eyes on Curvz during demos
    // without manually managing window stacking. Ignored in step mode
    // (where the Scripter needs focus to receive spacebar).
    curvz::widgets::CheckButton*  m_btn_lower  = nullptr;

    // Contextual action buttons — each lives next to its target.
    curvz::widgets::Button*       m_btn_save     = nullptr;  // Script tab content header
    curvz::widgets::Button*       m_btn_save_as  = nullptr;  // Script tab content header (s195 m1)
    curvz::widgets::Button*       m_btn_clear    = nullptr;  // Output tab content header
    curvz::widgets::Button*       m_btn_copy     = nullptr;  // Output tab content header

    // s193 m1 — statusbar at bottom: whole strip is the change-folder
    // affordance. Button with a Box child (icon + path label) and the
    // "flat" CSS class so the strip reads like a status row, not a
    // button, while still being fully clickable.
    curvz::widgets::Button*       m_btn_statusbar = nullptr;
    Gtk::Label                    m_lbl_folder;

    // s193 m1 — sidebar: collapsible category groups + script rows.
    Gtk::Box          m_sidebar         { Gtk::Orientation::VERTICAL, 0 };
    Gtk::ScrolledWindow m_sidebar_scroll;
    std::vector<Gtk::Expander*>          m_category_expanders;   // managed
    std::vector<std::unique_ptr<ScriptRow>> m_rows;

    // s193 m1 — center notebook with two tabs.
    Gtk::Notebook     m_notebook;
    Gtk::TextView     m_editor;
    Gtk::ScrolledWindow m_editor_scroll;
    Gtk::TextView     m_output;
    Gtk::ScrolledWindow m_output_scroll;
    Gtk::Label        m_editor_tab_label { "Script" };
    Gtk::Label        m_output_tab_label { "Output" };

    // s193 m1 — Script tab content header has a dim label showing the
    // current state (loaded filename, dirty marker, step-mode hint).
    // The Save button (m_btn_save above) hides when nothing's dirty.
    Gtk::Label        m_lbl_script_state;

    // s193 m1 — editor state: what file is loaded, is it dirty?
    // Loaded-file path is empty when nothing's been loaded (scratchpad).
    std::filesystem::path m_loaded_path;
    bool m_dirty = false;
    sigc::connection m_editor_changed_conn;

    // s193 m1 — text tag for step-mode line highlight. Created once
    // in build_ui; applied/removed by highlight_step_line.
    Glib::RefPtr<Gtk::TextTag> m_step_tag;

    std::unique_ptr<ScriptListener> m_listener;

    bool m_running = false;
    bool m_aborted = false;             // s193 m1: set by Stop click; the
                                        //   step lambda checks this and bails
    bool m_run_lowered = false;         // s193 m2: this Run called lower();
                                        //   end-of-run / abort calls present()

    // s193 m1 — run queue. on_run snapshots the checked rows into
    // this list (or fills it with one entry for editor-fallback) and
    // run_next_in_queue pumps them in order. Each entry carries the
    // body to run and the display name to show in the per-script
    // banner.
    struct QueueEntry {
        std::string body;
        std::string display_name;
    };
    std::vector<QueueEntry> m_run_queue;
    size_t                  m_run_queue_idx = 0;
    std::chrono::steady_clock::time_point m_script_start;

    // s193 m1 — step-through advance signal. The Run loop installs
    // a step lambda that, in step mode, parks itself waiting on this
    // function being called. The key controller's spacebar handler
    // calls it; calling it when no step is pending is a no-op.
    std::function<void()> m_step_advance;
};

} // namespace curvz::scripting
