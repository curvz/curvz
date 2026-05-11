// scripting/ScripterWindow.cpp ───────────────────────────────────────────────
//
// Lifted from scriptproto/ during s186 m2 — namespace renamed,
// otherwise identical. Theme integration: Curvz's
// MainWindow::apply_motif_to_window() walks every top-level GTK window
// via gtk_window_get_toplevels() and stamps the curvz-light CSS class,
// so this window picks up Dark/Light automatically. Application
// triggers an apply_motif_to_window() call once after construction so
// first-present is correct.

#include "scripting/ScripterWindow.hpp"

#include <gdkmm/clipboard.h>          // s186 close-out: copy-output button
#include <gdkmm/contentprovider.h>    // s186 close-out: copy-output button
#include <gtkmm/filedialog.h>
#include <gtkmm/paned.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

#include <fstream>
#include <functional>   // s186 m2: send/wait/gather step lambda
#include <memory>       // s186 m2: make_shared for step lifetime
#include <sstream>

namespace curvz::scripting {

namespace fs = std::filesystem;

ScripterWindow::ScripterWindow(const std::string& initial_folder)
    : m_folder(initial_folder) {
    set_title("Curvz Scripter (diagnostic)");
    set_default_size(900, 600);

    // s190 m2 — hide-instead-of-destroy on close. The Scripter is a
    // single persistent instance owned by Application; closing it via
    // the X button (or any close path) hides the window so the next
    // present() call from MainWindow's headerbar Scripter button can
    // bring it back with editor state intact. Codebase idiom — same
    // call used by HelpWindow, ShortcutsDialog, BlendDialog,
    // MacroEditorWindow, MacroManagerWindow.
    set_hide_on_close(true);

    m_listener = std::make_unique<ScriptListener>();
    m_listener->set_output_callback([this](const std::string& s) {
        append_output(s);
    });

    build_ui();
    rescan_library();
}

void ScripterWindow::build_ui() {
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    set_child(*root);

    // ── Top bar ──────────────────────────────────────────────────────────
    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    bar->set_margin(8);
    bar->append(m_btn_folder);
    bar->append(m_btn_reload);
    m_lbl_folder.set_hexpand(true);
    m_lbl_folder.set_xalign(0.0);
    m_lbl_folder.set_margin_start(8);
    m_lbl_folder.set_text(m_folder.string());
    bar->append(m_lbl_folder);
    bar->append(m_btn_clear);
    bar->append(m_btn_copy);

    // s187 m4: pacing knob. 0 = no extra delay, 5000 = noticeable pace.
    // Default 5 preserves the pre-m4 timing exactly so existing scripts
    // behave identically.
    m_spn_delay.set_range(0.0, 5000.0);
    m_spn_delay.set_increments(1.0, 50.0);
    m_spn_delay.set_value(5.0);
    m_spn_delay.set_width_chars(6);
    m_spn_delay.set_tooltip_text(
        "Step delay between script lines. 0 = run as fast as the "
        "scheduler allows; higher values pace the script so each "
        "dispatch is visible (handy for demos and debugging).");
    m_lbl_delay.set_margin_start(8);
    bar->append(m_lbl_delay);
    bar->append(m_spn_delay);

    bar->append(m_btn_run);
    root->append(*bar);

    m_btn_folder.signal_clicked().connect([this]() { on_folder_pick(); });
    m_btn_reload.signal_clicked().connect([this]() { rescan_library(); });
    m_btn_clear .signal_clicked().connect([this]() { on_clear_output(); });
    m_btn_copy  .signal_clicked().connect([this]() { on_copy_output(); });
    m_btn_run   .signal_clicked().connect([this]() { on_run(); });

    // ── Three-pane body ──────────────────────────────────────────────────
    auto* outer = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    auto* inner = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);

    m_library.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_library_scroll.set_child(m_library);
    m_library_scroll.set_size_request(180, -1);
    m_library_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                                 Gtk::PolicyType::AUTOMATIC);

    m_editor.set_monospace(true);
    m_editor.set_hexpand(true);
    m_editor.set_vexpand(true);
    // s186 m2: TextView internal margins. Distinct from widget margins —
    // these pad the text inside the scrolled viewport without pushing
    // the scrollbar away from the pane edge. Affinity / VS Code shape.
    m_editor.set_top_margin(8);
    m_editor.set_bottom_margin(8);
    m_editor.set_left_margin(8);
    m_editor.set_right_margin(8);
    m_editor_scroll.set_child(m_editor);
    m_editor_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                                Gtk::PolicyType::AUTOMATIC);

    m_output.set_monospace(true);
    m_output.set_editable(false);
    m_output.set_cursor_visible(false);
    m_output.set_hexpand(true);
    m_output.set_vexpand(true);
    m_output.set_top_margin(8);
    m_output.set_bottom_margin(8);
    m_output.set_left_margin(8);
    m_output.set_right_margin(8);
    m_output_scroll.set_child(m_output);
    m_output_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                                Gtk::PolicyType::AUTOMATIC);

    inner->set_start_child(m_editor_scroll);
    inner->set_end_child(m_output_scroll);
    inner->set_position(380);

    outer->set_start_child(m_library_scroll);
    outer->set_end_child(*inner);
    outer->set_position(200);

    outer->set_hexpand(true);
    outer->set_vexpand(true);
    root->append(*outer);

    m_library.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        int idx = row->get_index();
        if (idx < 0 || idx >= static_cast<int>(m_scripts.size())) return;
        load_script_into_editor(m_scripts[idx]);
    });
}

void ScripterWindow::rescan_library() {
    while (auto* row = m_library.get_row_at_index(0)) m_library.remove(*row);
    m_scripts.clear();

    if (!fs::exists(m_folder) || !fs::is_directory(m_folder)) {
        auto* lbl = Gtk::make_managed<Gtk::Label>("(folder not found)");
        lbl->set_xalign(0.0);
        m_library.append(*lbl);
        return;
    }

    std::vector<fs::path> hits;
    for (auto& e : fs::directory_iterator(m_folder)) {
        if (e.is_regular_file() && e.path().extension() == ".curvzs") {
            hits.push_back(e.path());
        }
    }
    std::sort(hits.begin(), hits.end());

    if (hits.empty()) {
        auto* lbl = Gtk::make_managed<Gtk::Label>("(no .curvzs files)");
        lbl->set_xalign(0.0);
        m_library.append(*lbl);
        return;
    }

    for (auto& p : hits) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(p.filename().string());
        lbl->set_xalign(0.0);
        lbl->set_margin_start(8);
        lbl->set_margin_end(8);
        lbl->set_margin_top(4);
        lbl->set_margin_bottom(4);
        m_library.append(*lbl);
        m_scripts.push_back(p);
    }

    m_lbl_folder.set_text(m_folder.string());
}

void ScripterWindow::load_script_into_editor(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) {
        append_output("# could not open " + p.string() + "\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    m_editor.get_buffer()->set_text(ss.str());
    append_output("# loaded " + p.filename().string() + "\n");
}

void ScripterWindow::on_clear_output() {
    m_output.get_buffer()->set_text("");
}

void ScripterWindow::on_copy_output() {
    // s186 close-out: copy the full output buffer to the system
    // clipboard. The TextView's monospace columns make in-place
    // selection finicky for paste-into-bug-report flows; this just
    // grabs the whole buffer in one click.
    //
    // Clipboard API: use the set_content(Gdk::ContentProvider) path
    // that's already shipping elsewhere in Curvz (LayersPanel,
    // Canvas_input) — Gdk::Clipboard::set_text exists in gtkmm-4 but
    // the codebase consistently uses set_content, so we match that
    // pattern. One less surface for "works on my machine but not
    // someone's distro" version variance.
    //
    // Visual feedback: briefly change the button label so the user
    // knows the click registered. 800ms is enough to read, short
    // enough not to feel laggy. Reset via Glib::signal_timeout.
    auto text = m_output.get_buffer()->get_text();
    auto disp = get_display();
    if (disp) {
        auto clip = disp->get_clipboard();
        if (clip) {
            Glib::Value<Glib::ustring> val;
            val.init(Glib::Value<Glib::ustring>::value_type());
            val.set(Glib::ustring(text));
            clip->set_content(Gdk::ContentProvider::create(val));
        }
    }
    m_btn_copy.set_label("Copied");
    Glib::signal_timeout().connect_once(
        [this]() { m_btn_copy.set_label("Copy output"); },
        800);
}

void ScripterWindow::append_output(const std::string& s) {
    auto buf = m_output.get_buffer();
    auto end = buf->end();
    buf->insert(end, s);
    auto mark = buf->create_mark(buf->end(), false);
    m_output.scroll_to(mark);
    buf->delete_mark(mark);
}

void ScripterWindow::on_folder_pick() {
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Pick a script folder");
    dlg->select_folder(*this,
        [this, dlg](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto f = dlg->select_folder_finish(result);
                if (f) {
                    m_folder = f->get_path();
                    rescan_library();
                }
            } catch (...) {
                // user dismissed
            }
        });
}

void ScripterWindow::on_run() {
    if (m_running) return;
    m_running = true;

    // Output is NOT cleared between Runs. The Clear output button is
    // the user's explicit affordance for that — auto-clearing was
    // doing their work for them in a way that lost previous results
    // when running multiple scripts in sequence (e.g. testing the
    // session-warm-up matrix during s186 m2 close-out). Each Run
    // appends a banner line so the boundaries are visible.
    append_output("# --- Run ---\n");

    auto body = m_editor.get_buffer()->get_text();
    m_listener->reset();
    m_listener->load_text(body.raw());

    m_listener->set_done_callback([this]() {
        m_running = false;
    });

    // ── send / wait / gather ─────────────────────────────────────────────
    // GTK signal dispatch is main-loop bound. activate() and other write
    // verbs queue signals; they do not deliver them synchronously. If
    // we pump every script line in the same idle slot, an assert on
    // line N+1 can read state that line N's dispatch hasn't propagated
    // yet (s186 m2: observed 235ms latency between activate() and the
    // resulting signal_toggled on a busy Curvz main loop).
    //
    // The shape every script line therefore implies is three-phase:
    //   - send:   dispatch the verb (run_line inside pump_next)
    //   - wait:   yield to GTK so its signal queue drains
    //   - gather: next line reads / asserts against the new state
    //
    // The wait is just a one-shot signal_timeout between dispatches.
    // s187 m4 adds the pacing knob: the spin button's value is sampled
    // once per Run, captured into the step lambda, and used as the
    // timeout interval. Default 5ms (matches the pre-m4 value); 0 runs
    // as fast as Glib::signal_timeout(0) allows; higher values pace
    // the script visibly for demos and debugging.
    //
    // Signal-bound verbs (ToggleButton::click etc) ALSO use the
    // synchronizer internally (wait_for_signal in ScriptableWidget),
    // so they wait on their canonical signal regardless of the step
    // delay. The step delay is additive — it adds visible time on top
    // of the synchronizer's signal-bound wait. Total = synchronizer
    // duration + step delay.
    //
    // Lifetime: the step lambda needs to outlive on_run()'s stack frame
    // because each call to step schedules the next. We wrap it in a
    // shared_ptr that captures itself — the chain holds the only ref,
    // and the final pump_next returning false breaks the chain so the
    // shared_ptr count drops to zero and the lambda destructs cleanly.
    auto* lst = m_listener.get();
    int delay_ms = static_cast<int>(m_spn_delay.get_value());
    auto step = std::make_shared<std::function<void()>>();
    *step = [lst, step, delay_ms]() {
        if (!lst->pump_next()) return;   // chain ends; shared_ptr releases
        Glib::signal_timeout().connect_once(
            [step]() { (*step)(); }, delay_ms);
    };
    Glib::signal_timeout().connect_once([step]() { (*step)(); }, 0);
}

} // namespace curvz::scripting
