// ScripterWindow.cpp ─────────────────────────────────────────────────────────

#include "ScripterWindow.hpp"

#include <gtkmm/filedialog.h>
#include <gtkmm/paned.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

#include <fstream>
#include <sstream>

namespace scriptproto {

namespace fs = std::filesystem;

ScripterWindow::ScripterWindow(const std::string& initial_folder)
    : m_folder(initial_folder) {
    set_title("scriptproto — scripter");
    set_default_size(900, 600);

    // Single listener instance for the window's lifetime. Each Run
    // resets it and feeds it the editor body. The output callback is
    // wired once and forwards every line to the output TextView.
    // Default-constructed: load_text() supplies the script body each
    // run, no source istream needed.
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
    bar->append(m_btn_run);
    root->append(*bar);

    m_btn_folder.signal_clicked().connect([this]() { on_folder_pick(); });
    m_btn_reload.signal_clicked().connect([this]() { rescan_library(); });
    m_btn_clear .signal_clicked().connect([this]() { on_clear_output(); });
    m_btn_run   .signal_clicked().connect([this]() { on_run(); });

    // ── Three-pane body ──────────────────────────────────────────────────
    // Two horizontal Paned widgets nested gives a 3-column adjustable
    // layout. Outer Paned: [library | (editor+output)]. Inner Paned:
    // [editor | output].
    auto* outer = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    auto* inner = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);

    // Library list
    m_library.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_library_scroll.set_child(m_library);
    m_library_scroll.set_size_request(180, -1);
    m_library_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                                 Gtk::PolicyType::AUTOMATIC);

    // Editor
    m_editor.set_monospace(true);
    m_editor.set_hexpand(true);
    m_editor.set_vexpand(true);
    m_editor_scroll.set_child(m_editor);
    m_editor_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                                Gtk::PolicyType::AUTOMATIC);

    // Output (read-only)
    m_output.set_monospace(true);
    m_output.set_editable(false);
    m_output.set_cursor_visible(false);
    m_output.set_hexpand(true);
    m_output.set_vexpand(true);
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

    // Library row activated → load that script.
    m_library.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        if (!row) return;
        int idx = row->get_index();
        if (idx < 0 || idx >= static_cast<int>(m_scripts.size())) return;
        load_script_into_editor(m_scripts[idx]);
    });
}

void ScripterWindow::rescan_library() {
    // Drop existing rows.
    while (auto* row = m_library.get_row_at_index(0)) m_library.remove(*row);
    m_scripts.clear();

    if (!fs::exists(m_folder) || !fs::is_directory(m_folder)) {
        auto* lbl = Gtk::make_managed<Gtk::Label>("(folder not found)");
        lbl->set_xalign(0.0);
        m_library.append(*lbl);
        return;
    }

    // Collect *.curvzs files, sorted.
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

void ScripterWindow::append_output(const std::string& s) {
    auto buf = m_output.get_buffer();
    auto end = buf->end();
    buf->insert(end, s);
    // Auto-scroll to bottom — easier than tracking a cursor mark.
    // gtkmm-4 scroll_to takes the RefPtr<TextMark> by const-ref.
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

    on_clear_output();
    append_output("# --- Run ---\n");

    auto body = m_editor.get_buffer()->get_text();
    m_listener->reset();
    m_listener->load_text(body.raw());

    // No done callback for app->quit() — scripter-window mode keeps the
    // app alive after a script finishes. Reset the running flag from
    // a done callback instead.
    m_listener->set_done_callback([this]() {
        m_running = false;
    });

    auto* lst = m_listener.get();
    Glib::signal_idle().connect([lst]() -> bool {
        return lst->pump_next();
    });
}

} // namespace scriptproto
