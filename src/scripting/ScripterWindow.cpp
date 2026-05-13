// scripting/ScripterWindow.cpp ───────────────────────────────────────────────
//
// s193 m1 redesign: top toolbar (workspace cluster + playback cluster),
// collapsible-category sidebar, two-tab Notebook (Script, Output),
// run-set semantics via per-row checkboxes, Save + dirty tracking.
//
// Theme integration: Curvz's MainWindow::apply_motif_to_window() walks
// every top-level GTK window via gtk_window_get_toplevels() and stamps
// the curvz-light CSS class, so this window picks up Dark/Light
// automatically. Application triggers an apply_motif_to_window() call
// once after construction so first-present is correct.

#include "scripting/ScripterWindow.hpp"

#include <giomm/application.h>        // s193 m2: Gio::Application::get_default()
#include <giomm/file.h>               // s195 m1: Gio::File::create_for_path for save-as initial folder
#include <gdkmm/clipboard.h>          // s186 close-out: copy-output button
#include <gdkmm/contentprovider.h>    // s186 close-out: copy-output button
#include <gtkmm/alertdialog.h>        // s193 m1: dirty-discard prompt
#include <gtkmm/filedialog.h>
#include <gtkmm/paned.h>
#include <gtkmm/separator.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>

namespace curvz::scripting {

namespace fs = std::filesystem;

// ── small helpers ───────────────────────────────────────────────────────────

namespace {

// Format an HH:MM:SS wall-clock stamp for the per-script banner.
std::string wall_time_now() {
    auto t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%H:%M:%S");
    return os.str();
}

// Format a duration in seconds with 3-decimal precision.
std::string format_elapsed(std::chrono::steady_clock::duration d) {
    using namespace std::chrono;
    double s = duration_cast<microseconds>(d).count() / 1.0e6;
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << s << "s";
    return os.str();
}

// Read a file's contents into a string. Returns empty on failure.
std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ── construction ────────────────────────────────────────────────────────────

ScripterWindow::ScripterWindow(const std::string& initial_folder)
    : m_folder(initial_folder) {
    set_title("Curvz Scripter (diagnostic)");
    set_default_size(1100, 700);

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

// ── UI build ────────────────────────────────────────────────────────────────

void ScripterWindow::build_ui() {
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    set_child(*root);

    // ── Top toolbar — playback only ───────────────────────────────────────
    // The toolbar is the playback transport. Three controls: Run, Step,
    // step-delay. Nothing else lives here. Output-management actions
    // (Clear, Copy) live in the Output tab's content header; Save lives
    // in the Script tab's content header; Reload lives in the sidebar
    // header; Change-folder lives in the statusbar. Every action sits
    // next to its target.
    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    bar->set_margin(8);

    // Run: suggested-action when idle, destructive-action while running
    // (handled by update_run_button_label).
    m_btn_run.add_css_class("suggested-action");
    m_btn_run.set_tooltip_text(
        "Run the checked scripts in order. If no scripts are checked, "
        "runs the editor's current contents.");
    m_btn_run.signal_clicked().connect([this]() { on_run_or_stop(); });
    bar->append(m_btn_run);

    // Step: real CheckButton so on/off reads at a glance.
    m_btn_step.set_tooltip_text(
        "Step through the script one line at a time. Press SPACE in the "
        "Scripter window to advance. The step-delay knob is ignored "
        "while Step is checked.");
    m_btn_step.set_margin_start(12);
    bar->append(m_btn_step);

    // s193 m2: Auto-lower. Drops the Scripter behind MainWindow at
    // Run start so the user can watch Curvz drive itself without
    // window-stacking management. Brings it back at end-of-run.
    // Ignored in step mode (where Scripter needs focus to receive
    // spacebar).
    m_btn_lower.set_tooltip_text(
        "Lower the Scripter behind the main window during timed Runs "
        "so you can watch Curvz drive itself. The Scripter returns "
        "when the Run finishes. Ignored when Step is checked.");
    m_btn_lower.set_margin_start(6);
    bar->append(m_btn_lower);

    // s187 m4 pacing knob.
    m_spn_delay.set_range(0.0, 5000.0);
    m_spn_delay.set_increments(1.0, 50.0);
    m_spn_delay.set_value(5.0);
    m_spn_delay.set_width_chars(6);
    m_spn_delay.set_tooltip_text(
        "Step delay between script lines (timed mode). 0 = run as fast "
        "as the scheduler allows; higher values pace the script so each "
        "dispatch is visible. Ignored when Step is checked.");
    m_lbl_delay.set_margin_start(16);
    bar->append(m_lbl_delay);
    bar->append(m_spn_delay);

    // Trailing spacer so the cluster hugs the left edge.
    auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    spacer->set_hexpand(true);
    bar->append(*spacer);

    root->append(*bar);
    root->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

    // ── Key controllers for spacebar advance ──
    // s193 m1: belt-and-braces — install on the window AND on the
    // editor TextView directly. CAPTURE phase so the key reaches our
    // handler before any descendant consumes it.
    auto step_key = Gtk::EventControllerKey::create();
    step_key->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    step_key->signal_key_pressed().connect(
        [this](guint keyval, guint /*kc*/, Gdk::ModifierType state) -> bool {
            if (keyval != GDK_KEY_space) return false;
            const auto mods = state & (Gdk::ModifierType::CONTROL_MASK
                                       | Gdk::ModifierType::ALT_MASK
                                       | Gdk::ModifierType::SHIFT_MASK);
            if (mods != Gdk::ModifierType{}) return false;
            if (!m_running) return false;
            if (!m_btn_step.get_active()) return false;
            if (!m_step_advance) return false;
            auto advance = m_step_advance;
            m_step_advance = nullptr;
            advance();
            return true;
        },
        false);
    add_controller(step_key);

    auto editor_key = Gtk::EventControllerKey::create();
    editor_key->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    editor_key->signal_key_pressed().connect(
        [this](guint keyval, guint /*kc*/, Gdk::ModifierType state) -> bool {
            if (keyval != GDK_KEY_space) return false;
            const auto mods = state & (Gdk::ModifierType::CONTROL_MASK
                                       | Gdk::ModifierType::ALT_MASK
                                       | Gdk::ModifierType::SHIFT_MASK);
            if (mods != Gdk::ModifierType{}) return false;
            if (!m_running) return false;
            if (!m_btn_step.get_active()) return false;
            if (!m_step_advance) return false;
            auto advance = m_step_advance;
            m_step_advance = nullptr;
            advance();
            return true;
        },
        false);
    m_editor.add_controller(editor_key);

    // s193 m1: unchecking Step mid-Run releases any parked step into
    // timed mode immediately.
    m_btn_step.signal_toggled().connect([this]() {
        if (m_btn_step.get_active()) return;
        if (!m_step_advance) return;
        auto advance = m_step_advance;
        m_step_advance = nullptr;
        Glib::signal_idle().connect_once([advance]() { advance(); });
    });

    // ── Body: sidebar | notebook ──────────────────────────────────────────
    auto* outer = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);

    // -- Sidebar with header row --
    // Header: "Library" dim label + reload icon button on the right.
    // Reload acts on the sidebar's content, so it lives next to that
    // content.
    auto* sidebar_root =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    auto* sidebar_header =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    sidebar_header->set_margin_start(8);
    sidebar_header->set_margin_end(4);
    sidebar_header->set_margin_top(6);
    sidebar_header->set_margin_bottom(4);

    auto* lib_label = Gtk::make_managed<Gtk::Label>("Library");
    lib_label->set_xalign(0.0f);
    lib_label->set_hexpand(true);
    lib_label->add_css_class("dim-label");
    sidebar_header->append(*lib_label);

    auto* reload_btn = Gtk::make_managed<Gtk::Button>();
    reload_btn->set_icon_name("view-refresh-symbolic");
    reload_btn->add_css_class("flat");
    reload_btn->set_tooltip_text("Re-scan the workspace for *.curvzs files");
    reload_btn->signal_clicked().connect([this]() { rescan_library(); });
    sidebar_header->append(*reload_btn);

    sidebar_root->append(*sidebar_header);
    sidebar_root->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

    m_sidebar.set_margin(4);
    m_sidebar_scroll.set_child(m_sidebar);
    m_sidebar_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                                 Gtk::PolicyType::AUTOMATIC);
    m_sidebar_scroll.set_vexpand(true);
    sidebar_root->append(m_sidebar_scroll);

    // s195 m2 — compute initial sidebar width once at construction
    // from the longest filename in the workspace. Floor at 200 so a
    // short-filename workspace doesn't make the sidebar feel cramped;
    // ceiling at 420 so a long-filename workspace doesn't swallow the
    // editor. Users can still drag the Paned divider; we just pick a
    // better default than a single hardcoded 240.
    const int sidebar_w = compute_initial_sidebar_width();
    sidebar_root->set_size_request(sidebar_w, -1);

    // -- Notebook with per-tab content headers --
    // Each tab's content is wrapped in a vertical Box: a thin header
    // row at the top with tab-specific actions, then the TextView in
    // a ScrolledWindow below. The tab strip itself stays clean — just
    // labels.

    // Script tab content.
    auto* script_pane =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    auto* script_header =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    script_header->set_margin_start(8);
    script_header->set_margin_end(6);
    script_header->set_margin_top(4);
    script_header->set_margin_bottom(4);

    m_lbl_script_state.set_xalign(0.0f);
    m_lbl_script_state.set_hexpand(true);
    m_lbl_script_state.add_css_class("dim-label");
    m_lbl_script_state.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    script_header->append(m_lbl_script_state);

    m_btn_save.set_label("Save");
    m_btn_save.add_css_class("flat");
    m_btn_save.set_tooltip_text(
        "Save the editor contents back to the loaded script file");
    m_btn_save.set_sensitive(false);
    m_btn_save.set_visible(false);  // hidden until dirty + loaded
    m_btn_save.signal_clicked().connect(
        [this]() { save_editor_to_loaded_file(); });
    script_header->append(m_btn_save);

    // s195 m1 — Save As… is always visible. It works on both the
    // scratchpad (where Save is hidden because there's no loaded path)
    // and on loaded files (where it branches off a copy under a new
    // name). After a successful Save As, m_loaded_path is adopted so
    // subsequent Save writes back to the new file.
    m_btn_save_as.set_label("Save As…");
    m_btn_save_as.add_css_class("flat");
    m_btn_save_as.set_tooltip_text(
        "Write the editor contents to a new file in the workspace");
    m_btn_save_as.signal_clicked().connect(
        [this]() { on_save_as(); });
    script_header->append(m_btn_save_as);

    script_pane->append(*script_header);
    script_pane->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

    m_editor.set_monospace(true);
    m_editor.set_hexpand(true);
    m_editor.set_vexpand(true);
    m_editor.set_top_margin(8);
    m_editor.set_bottom_margin(8);
    m_editor.set_left_margin(8);
    m_editor.set_right_margin(8);
    m_editor_scroll.set_child(m_editor);
    m_editor_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                                Gtk::PolicyType::AUTOMATIC);
    m_editor_scroll.set_hexpand(true);
    m_editor_scroll.set_vexpand(true);
    script_pane->append(m_editor_scroll);

    // Output tab content.
    auto* output_pane =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    auto* output_header =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    output_header->set_margin_start(8);
    output_header->set_margin_end(6);
    output_header->set_margin_top(4);
    output_header->set_margin_bottom(4);

    auto* output_header_spacer =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    output_header_spacer->set_hexpand(true);
    output_header->append(*output_header_spacer);

    m_btn_clear.set_label("Clear");
    m_btn_clear.add_css_class("flat");
    m_btn_clear.set_tooltip_text("Clear the output buffer");
    m_btn_clear.signal_clicked().connect([this]() { on_clear_output(); });
    output_header->append(m_btn_clear);

    m_btn_copy.set_label("Copy");
    m_btn_copy.add_css_class("flat");
    m_btn_copy.set_tooltip_text("Copy the output buffer to the clipboard");
    m_btn_copy.signal_clicked().connect([this]() { on_copy_output(); });
    output_header->append(m_btn_copy);

    output_pane->append(*output_header);
    output_pane->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

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
    m_output_scroll.set_hexpand(true);
    m_output_scroll.set_vexpand(true);
    output_pane->append(m_output_scroll);

    m_notebook.append_page(*script_pane, m_editor_tab_label);
    m_notebook.append_page(*output_pane, m_output_tab_label);
    m_notebook.set_hexpand(true);
    m_notebook.set_vexpand(true);

    // Editor: dirty tracking + step-highlight tag.
    m_editor_changed_conn = m_editor.get_buffer()->signal_changed().connect(
        [this]() { mark_dirty(true); });

    m_step_tag = m_editor.get_buffer()->create_tag("curvz_step");
    m_step_tag->property_background() = "#FFE896";
    m_step_tag->property_foreground() = "#222222";

    outer->set_start_child(*sidebar_root);
    outer->set_end_child(m_notebook);
    outer->set_position(sidebar_w);  // s195 m2 — match computed sidebar width
    outer->set_hexpand(true);
    outer->set_vexpand(true);
    root->append(*outer);

    // ── Statusbar — clickable folder strip ────────────────────────────────
    // Whole row is the change-folder affordance. A flat-styled Button
    // with a Box child (icon + path label) so the click target spans
    // the strip. Reads as a status row, behaves as a button.
    root->append(*Gtk::make_managed<Gtk::Separator>(
        Gtk::Orientation::HORIZONTAL));

    m_btn_statusbar.add_css_class("flat");
    m_btn_statusbar.set_tooltip_text(
        "Click to change the script workspace folder");
    m_btn_statusbar.set_hexpand(true);
    m_btn_statusbar.signal_clicked().connect([this]() { on_folder_pick(); });

    auto* status_row =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    status_row->set_margin_start(8);
    status_row->set_margin_end(8);
    status_row->set_margin_top(2);
    status_row->set_margin_bottom(2);

    auto* folder_icon = Gtk::make_managed<Gtk::Image>();
    folder_icon->set_from_icon_name("folder-symbolic");
    status_row->append(*folder_icon);

    m_lbl_folder.set_text(m_folder.string());
    m_lbl_folder.set_xalign(0.0f);
    m_lbl_folder.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    m_lbl_folder.set_hexpand(true);
    m_lbl_folder.add_css_class("dim-label");
    m_lbl_folder.set_tooltip_text(m_folder.string());
    status_row->append(m_lbl_folder);

    m_btn_statusbar.set_child(*status_row);
    root->append(m_btn_statusbar);

    // Initial Script-tab state label so it doesn't show empty at launch.
    update_editor_tab_title();
}

// ── Library / categories ────────────────────────────────────────────────────

void ScripterWindow::rescan_library() {
    // Tear down old children. The sidebar Box owns the Expanders; each
    // Expander owns its row Box; row Boxes own CheckButton + label
    // Button. Removing from m_sidebar drops the chain.
    while (auto* w = m_sidebar.get_first_child()) m_sidebar.remove(*w);
    m_category_expanders.clear();
    m_rows.clear();

    if (!fs::exists(m_folder) || !fs::is_directory(m_folder)) {
        auto* lbl = Gtk::make_managed<Gtk::Label>("(folder not found)");
        lbl->set_xalign(0.0f);
        lbl->set_margin(8);
        m_sidebar.append(*lbl);
        m_lbl_folder.set_text(m_folder.string());
        m_lbl_folder.set_tooltip_text(m_folder.string());
        return;
    }

    // First pass: collect loose scripts + subfolder names.
    std::vector<fs::path> loose_scripts;
    std::vector<fs::path> subfolders;
    for (auto& e : fs::directory_iterator(m_folder)) {
        if (e.is_regular_file() && e.path().extension() == ".curvzs") {
            loose_scripts.push_back(e.path());
        } else if (e.is_directory()) {
            // Only include subfolders that contain at least one *.curvzs.
            bool has_any = false;
            for (auto& sub : fs::directory_iterator(e.path())) {
                if (sub.is_regular_file()
                    && sub.path().extension() == ".curvzs") {
                    has_any = true;
                    break;
                }
            }
            if (has_any) subfolders.push_back(e.path());
        }
    }
    std::sort(loose_scripts.begin(), loose_scripts.end());
    std::sort(subfolders.begin(), subfolders.end());

    // Helper to append one category Expander with a list of scripts.
    // s193 m1: each row is a horizontal Box with a CheckButton (for
    // run-set inclusion) and a Button (flat-styled, left-aligned, with
    // the filename as label, for "load into editor" semantics).
    auto add_category = [this](const std::string& title,
                                const std::vector<fs::path>& scripts,
                                bool initial_expanded) {
        auto* exp = Gtk::make_managed<Gtk::Expander>();
        exp->set_label(title + "  (" + std::to_string(scripts.size()) + ")");
        exp->set_expanded(initial_expanded);
        exp->set_margin_bottom(2);

        auto* rows_box = Gtk::make_managed<Gtk::Box>(
            Gtk::Orientation::VERTICAL, 0);
        rows_box->set_margin_start(12);
        rows_box->set_margin_top(2);
        rows_box->set_margin_bottom(4);

        for (const auto& p : scripts) {
            auto row = std::make_unique<ScriptRow>();
            row->path = p;

            auto* hb = Gtk::make_managed<Gtk::Box>(
                Gtk::Orientation::HORIZONTAL, 6);
            hb->set_margin_top(1);
            hb->set_margin_bottom(1);

            row->check = Gtk::make_managed<Gtk::CheckButton>();
            row->check->set_tooltip_text(
                "Include this script in the next Run");
            hb->append(*row->check);

            // Use a Button-with-explicit-child-Label so we have direct
            // control over xalign / ellipsize. A Button constructed with
            // a string label wraps the text in widgets we can't reliably
            // address via get_child().
            row->label = Gtk::make_managed<Gtk::Button>();
            row->label->add_css_class("flat");
            row->label->set_hexpand(true);
            row->label->set_halign(Gtk::Align::FILL);
            auto* inner_lbl = Gtk::make_managed<Gtk::Label>(
                p.filename().string());
            inner_lbl->set_xalign(0.0f);
            inner_lbl->set_ellipsize(Pango::EllipsizeMode::END);
            row->label->set_child(*inner_lbl);
            row->label->set_tooltip_text(
                "Load this script into the editor (click) — independent "
                "of the checkbox.");

            fs::path script_path = p;
            row->label->signal_clicked().connect(
                [this, script_path]() {
                    try_load_with_dirty_check(script_path);
                });

            hb->append(*row->label);

            // s195 m4 — flat trash button. Always visible at low visual
            // weight; sits next to the file it operates on. Click sends
            // through confirm_and_delete which prompts before trashing.
            row->del = Gtk::make_managed<Gtk::Button>();
            row->del->set_icon_name("edit-delete-symbolic");
            row->del->add_css_class("flat");
            row->del->set_tooltip_text(
                "Send this script to the trash (recoverable)");
            row->del->signal_clicked().connect(
                [this, script_path]() {
                    confirm_and_delete(script_path);
                });
            hb->append(*row->del);

            rows_box->append(*hb);

            m_rows.push_back(std::move(row));
        }

        exp->set_child(*rows_box);
        m_sidebar.append(*exp);
        m_category_expanders.push_back(exp);
    };

    // "All Scripts" (loose .curvzs at workspace root) first.
    if (!loose_scripts.empty()) {
        add_category("All Scripts", loose_scripts, /*expanded=*/true);
    }

    // Then one Expander per subfolder. Use folder name as title;
    // collapse by default so a deep tree doesn't blow up vertically.
    for (const auto& sub : subfolders) {
        std::vector<fs::path> hits;
        for (auto& e : fs::directory_iterator(sub)) {
            if (e.is_regular_file() && e.path().extension() == ".curvzs") {
                hits.push_back(e.path());
            }
        }
        std::sort(hits.begin(), hits.end());
        add_category(sub.filename().string(), hits, /*expanded=*/false);
    }

    if (loose_scripts.empty() && subfolders.empty()) {
        auto* lbl = Gtk::make_managed<Gtk::Label>("(no .curvzs files)");
        lbl->set_xalign(0.0f);
        lbl->set_margin(8);
        m_sidebar.append(*lbl);
    }

    m_lbl_folder.set_text(m_folder.string());
    m_lbl_folder.set_tooltip_text(m_folder.string());
}

// s195 m2 — auto-tune the sidebar's initial width to fit the workspace.
//
// Pre-realization Pango isn't available (the widget tree isn't yet
// hooked to a display), so we approximate with a per-char heuristic.
// Filenames are ASCII .curvzs files; for GTK's default UI font around
// 13–14pt sans, ~8.5 px/char is a reasonable upper bound. We round up
// to 9 px/char to leave a little breathing room and avoid the ellipsis
// for typical filenames. Fixed overhead covers checkbox (~24), row
// margins (~12 left + 6 right), and a scrollbar reservation (~16).
//
// Clamped [200, 420] so a workspace with only short filenames doesn't
// shrink the sidebar absurdly, and one with monster filenames doesn't
// grow it beyond half the default 1100×700 window. Users who want
// something else can still drag the Paned divider.
int ScripterWindow::compute_initial_sidebar_width() const {
    constexpr int kPxPerChar = 9;
    constexpr int kFixedOverhead = 58;   // 24 + 12 + 6 + 16
    constexpr int kMinWidth = 200;
    constexpr int kMaxWidth = 420;

    if (!fs::exists(m_folder) || !fs::is_directory(m_folder)) {
        return kMinWidth;
    }

    size_t longest = 0;
    auto consider = [&longest](const fs::path& p) {
        if (p.extension() == ".curvzs") {
            longest = std::max(longest, p.filename().string().size());
        }
    };

    // Loose .curvzs at workspace root.
    for (auto& e : fs::directory_iterator(m_folder)) {
        if (e.is_regular_file()) consider(e.path());
    }
    // One level of subfolder (matches rescan_library's depth contract).
    for (auto& e : fs::directory_iterator(m_folder)) {
        if (!e.is_directory()) continue;
        for (auto& sub : fs::directory_iterator(e.path())) {
            if (sub.is_regular_file()) consider(sub.path());
        }
    }

    if (longest == 0) return kMinWidth;

    int desired = static_cast<int>(longest) * kPxPerChar + kFixedOverhead;
    return std::clamp(desired, kMinWidth, kMaxWidth);
}

// ── Editor load / save / dirty ──────────────────────────────────────────────

void ScripterWindow::try_load_with_dirty_check(const fs::path& p) {
    if (!m_dirty || m_loaded_path.empty()) {
        load_script_into_editor(p);
        return;
    }

    auto dlg = Gtk::AlertDialog::create(
        "Discard unsaved changes to "
        + m_loaded_path.filename().string() + "?");
    dlg->set_detail(
        "The editor has unsaved changes. Loading a different script "
        "will discard them.");
    dlg->set_buttons({"Cancel", "Discard"});
    dlg->set_cancel_button(0);
    dlg->set_default_button(0);

    fs::path target = p;
    dlg->choose(*this,
        [this, dlg, target](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                int choice = dlg->choose_finish(result);
                if (choice == 1) load_script_into_editor(target);
            } catch (...) {
                // dismissed — keep editor state
            }
        });
}

void ScripterWindow::load_script_into_editor(const fs::path& p) {
    std::string body = read_file(p);
    if (body.empty() && !fs::exists(p)) {
        append_output("# could not open " + p.string() + "\n");
        return;
    }
    // Guard the dirty signal during the synthetic load.
    m_editor_changed_conn.block();
    m_editor.get_buffer()->set_text(body);
    m_editor_changed_conn.unblock();

    // Clear any leftover step-highlight from a prior script.
    highlight_step_line(-1);

    m_loaded_path = p;
    mark_dirty(false);
    update_editor_tab_title();

    // Switch focus to the Script tab so the user sees what just loaded.
    m_notebook.set_current_page(0);
}

void ScripterWindow::save_editor_to_loaded_file() {
    if (m_loaded_path.empty()) return;
    std::ofstream f(m_loaded_path);
    if (!f.is_open()) {
        append_output("# save failed: " + m_loaded_path.string() + "\n");
        return;
    }
    f << m_editor.get_buffer()->get_text().raw();
    f.close();
    mark_dirty(false);
    append_output("# saved " + m_loaded_path.filename().string() + "\n");
}

// s195 m1 — Save As… for the scratchpad (and for branching off a loaded
// file under a new name). Opens a save FileDialog rooted at the
// workspace folder, writes the editor contents to whatever path comes
// back, then adopts that path as the loaded file so subsequent Save
// writes there. A rescan_library() picks up the new file in the
// sidebar.
//
// Filename hygiene: if the user names a file without an extension we
// append .curvzs. If they name one with a different extension we
// respect it (they may be saving a derivative — README, notes, etc).
void ScripterWindow::on_save_as() {
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Save script as…");

    // Root the dialog at the workspace folder so the new file lands
    // somewhere that rescan_library() will pick up.
    if (fs::exists(m_folder) && fs::is_directory(m_folder)) {
        dlg->set_initial_folder(Gio::File::create_for_path(m_folder.string()));
    }

    // Pre-fill the name field. If we already have a loaded path, suggest
    // a sibling — strip extension, append "_copy". Otherwise default to
    // "untitled.curvzs".
    std::string suggested_name;
    if (!m_loaded_path.empty()) {
        suggested_name = m_loaded_path.stem().string() + "_copy.curvzs";
    } else {
        suggested_name = "untitled.curvzs";
    }
    dlg->set_initial_name(suggested_name);

    dlg->save(*this,
        [this, dlg](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto file = dlg->save_finish(result);
                if (!file) return;

                fs::path target = file->get_path();

                // Friendly extension default: if no extension at all,
                // append .curvzs. Respect any extension the user
                // explicitly typed (including weird ones).
                if (target.extension().empty()) {
                    target += ".curvzs";
                }

                std::ofstream f(target);
                if (!f.is_open()) {
                    append_output(
                        "# save-as failed: " + target.string() + "\n");
                    return;
                }
                f << m_editor.get_buffer()->get_text().raw();
                f.close();

                // Adopt the new path. From now on Save writes here.
                m_loaded_path = target;
                mark_dirty(false);
                append_output(
                    "# saved " + target.filename().string() + "\n");

                // Refresh the sidebar so the new file shows up under
                // its category (loose at root → "All Scripts"; in a
                // subfolder → that subfolder's expander).
                rescan_library();
            } catch (...) {
                // User dismissed, or filesystem-level error. Either way,
                // editor state and m_loaded_path stay as they were.
            }
        });
}

// s195 m4 — Delete from the sidebar row. Confirms via AlertDialog,
// then sends the file to the system trash (Gio::File::trash —
// recoverable, matches Nautilus / the file manager). On Linux this
// lands in ~/.local/share/Trash; users can restore from there. After
// trashing, the sidebar rescans so the row disappears.
//
// Side effect on the loaded file: if the deleted file is the one
// currently in the editor, we clear m_loaded_path and mark the editor
// dirty. The user's text stays in memory — they can recover it via
// Save As… to a new file. Save (which writes to m_loaded_path) becomes
// a no-op since there's no longer a backing path, which is the right
// thing: writing to a freshly-trashed path would re-create it and the
// trash entry would dangle.
void ScripterWindow::confirm_and_delete(const fs::path& p) {
    auto dlg = Gtk::AlertDialog::create(
        "Delete " + p.filename().string() + "?");
    dlg->set_detail(
        "The script will be moved to the trash. You can restore it "
        "from your file manager if you change your mind.");
    dlg->set_buttons({"Cancel", "Delete"});
    dlg->set_cancel_button(0);
    dlg->set_default_button(0);

    fs::path target = p;
    dlg->choose(*this,
        [this, dlg, target](Glib::RefPtr<Gio::AsyncResult>& result) {
            int choice = 0;
            try {
                choice = dlg->choose_finish(result);
            } catch (...) {
                return;  // dismissed
            }
            if (choice != 1) return;  // Cancel

            auto gio_file = Gio::File::create_for_path(target.string());
            try {
                gio_file->trash();
            } catch (const Glib::Error& err) {
                append_output(
                    "# delete failed: " + target.string()
                    + " (" + err.what() + ")\n");
                return;
            }

            append_output(
                "# trashed " + target.filename().string() + "\n");

            // If we just trashed the loaded file, the editor's backing
            // path is gone. Keep the text in memory but detach from
            // disk — Save becomes a no-op, Save As… is the recovery
            // path. Mark dirty so the user sees the work is unattached.
            if (m_loaded_path == target) {
                m_loaded_path.clear();
                mark_dirty(true);
            }

            rescan_library();
        });
}

void ScripterWindow::mark_dirty(bool dirty) {
    m_dirty = dirty;
    // Save is visible whenever a file is loaded — the user sees the
    // action is available even on a clean file. Enabled only when
    // there's actually something to save (dirty + loaded). This keeps
    // the Script tab's content-header row from shifting layout when
    // the file flips between clean and dirty.
    bool loaded = !m_loaded_path.empty();
    m_btn_save.set_visible(loaded);
    m_btn_save.set_sensitive(loaded && dirty);
    update_editor_tab_title();
}

void ScripterWindow::update_editor_tab_title() {
    // Tab strip label: short. Just "Script" or "*Script" so the tab
    // strip stays tidy. Detail (filename, dirty state, step-mode hint)
    // lives in the content header where there's room.
    std::string tab = m_dirty ? "*Script" : "Script";
    m_editor_tab_label.set_text(tab);

    // Content header label: state-rich, dim-styled. Tells the user
    // what they're looking at: filename, dirty marker, and (if step
    // mode is parked) which line the next spacebar press will run.
    std::string state;
    if (m_loaded_path.empty()) {
        state = "(scratchpad — type a script, or click one in the library)";
    } else {
        state = m_loaded_path.filename().string();
        if (m_dirty) state += "  •  unsaved changes";
    }
    m_lbl_script_state.set_text(state);
}

void ScripterWindow::highlight_step_line(int line_index) {
    if (!m_step_tag) return;
    auto buf = m_editor.get_buffer();

    // Clear any prior highlight first — single-line highlight, the
    // previous line drops as the new one applies.
    buf->remove_tag(m_step_tag, buf->begin(), buf->end());

    if (line_index < 0) return;
    int total = buf->get_line_count();
    if (line_index >= total) return;

    auto start = buf->get_iter_at_line(line_index);
    auto end   = start;
    if (!end.ends_line()) end.forward_to_line_end();
    buf->apply_tag(m_step_tag, start, end);

    // Scroll the editor so the line is visible. Quarter-screen
    // top-margin so the line lands a bit above center — friendlier
    // for following along than dead-center.
    auto mark = buf->create_mark(start, false);
    m_editor.scroll_to(mark, 0.25);
    buf->delete_mark(mark);

    // In step mode, bring the Script tab forward so the user sees
    // the highlight. The auto-switch-to-Output at Run start would
    // otherwise leave step mode reading at a hidden editor. In
    // timed mode, leave the tab alone — highlight still applies in
    // the background, useful if the user manually flips to Script.
    //
    // After switching, grab focus to the editor TextView. Without
    // this, GTK's tab strip keeps keyboard focus after set_current_page
    // and spacebar triggers tab cycling instead of our CAPTURE-phase
    // step-advance controller. The editor is editable but our CAPTURE
    // controller intercepts space before the TextView sees it, so the
    // user can't accidentally type into the script.
    //
    // Idle-scheduled because set_current_page queues a post-switch
    // focus shift internally; a synchronous grab_focus here gets
    // overwritten by that. signal_idle runs after the focus shuffle
    // settles, so our grab is the last word.
    if (m_btn_step.get_active()) {
        m_notebook.set_current_page(0);
        Glib::signal_idle().connect_once([this]() {
            m_editor.grab_focus();
        });
    }
}

// ── Output ─────────────────────────────────────────────────────────────────

void ScripterWindow::on_clear_output() {
    m_output.get_buffer()->set_text("");
}

void ScripterWindow::on_copy_output() {
    // s186 close-out: copy the full output buffer to the system
    // clipboard. The TextView's monospace columns make in-place
    // selection finicky; this just grabs the whole buffer in one
    // click. Same set_content(ContentProvider) path used in
    // LayersPanel and Canvas_input — match the codebase idiom.
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

void ScripterWindow::show_output_tab() {
    // Page 1 is Output (page 0 is Script). See build_ui append order.
    m_notebook.set_current_page(1);
}

// ── Folder picker ──────────────────────────────────────────────────────────

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

// ── Run ────────────────────────────────────────────────────────────────────

void ScripterWindow::on_run_or_stop() {
    if (m_running) {
        abort_run();
    } else {
        on_run();
    }
}

void ScripterWindow::update_run_button_label() {
    if (m_running) {
        m_btn_run.set_label("Stop");
        // Re-style as destructive while running so the user reads the
        // shift: green/blue suggested-action -> red destructive-action.
        // Standard GTK CSS classes.
        m_btn_run.remove_css_class("suggested-action");
        m_btn_run.add_css_class("destructive-action");
        m_btn_run.set_tooltip_text("Stop the current Run");
    } else {
        m_btn_run.set_label("Run");
        m_btn_run.remove_css_class("destructive-action");
        m_btn_run.add_css_class("suggested-action");
        m_btn_run.set_tooltip_text(
            "Run the checked scripts in order. If no scripts are checked, "
            "runs the editor's current contents.");
    }
}

void ScripterWindow::abort_run() {
    // Set the abort flag — the step lambda checks it before pump_next,
    // so any pending signal_timeout that fires will break the chain.
    // Also clear m_step_advance so the parked closure (if any) is
    // collected without firing the next line. Last, clear the run
    // queue so the listener's done_callback doesn't auto-start the
    // next script.
    m_aborted = true;
    m_step_advance = nullptr;
    // Truncate the queue so the post-finish dispatch sees idx >= size.
    m_run_queue.clear();
    m_run_queue_idx = 0;
    // Output marker so the user sees the abort.
    append_output("--- Stopped by user ---\n");
    // Drop running state immediately so update_run_button_label flips
    // back to Run. The in-flight step lambda may still be live for one
    // more dispatch but it'll see m_aborted and bail.
    m_running = false;
    update_run_button_label();
    // s193 m2: if Auto-lower had lowered us, bring the window back so
    // the user can see the Stop took effect. Without this the user
    // would click Stop in a flash visit to the Scripter, the window
    // would stay lowered, and the next attention shift would be
    // confusing.
    if (m_run_lowered) {
        m_run_lowered = false;
        Glib::signal_idle().connect_once([this]() { present(); });
    }
}

void ScripterWindow::on_run() {
    if (m_running) return;

    // Build the run queue: checked rows first, in sidebar order. If
    // none are checked, fall back to the editor's current text as a
    // single anonymous entry.
    m_run_queue.clear();
    m_run_queue_idx = 0;
    for (const auto& row : m_rows) {
        if (row->check && row->check->get_active()) {
            std::string body = read_file(row->path);
            m_run_queue.push_back({ body, row->path.filename().string() });
        }
    }
    if (m_run_queue.empty()) {
        std::string body = m_editor.get_buffer()->get_text().raw();
        std::string name = m_loaded_path.empty()
            ? std::string("(editor scratchpad)")
            : m_loaded_path.filename().string();
        m_run_queue.push_back({ body, name });
    }

    m_aborted = false;
    m_running = true;
    m_run_lowered = false;
    update_run_button_label();

    // s193 m1: in step mode the user reads the script, so park them on
    // the Script tab from the moment Run is clicked — and grab focus
    // to the editor so the key controller picks up spacebar without
    // the tab strip eating it. The grab is idle-scheduled because
    // set_current_page queues GTK's own post-switch focus shift
    // internally; a synchronous grab here gets overwritten by that.
    //
    // In timed mode the user watches Output stream, so auto-switch
    // to Output instead. The step lambda's highlight_step_line will
    // also call set_current_page(0) at each park as a safety net.
    if (m_btn_step.get_active()) {
        m_notebook.set_current_page(0);
        Glib::signal_idle().connect_once([this]() {
            m_editor.grab_focus();
        });
    } else {
        show_output_tab();
    }

    // s193 m2: in timed mode, if Auto-lower is checked, raise
    // MainWindow above the Scripter so the user can watch Curvz
    // react. GTK4 deliberately deprecated programmatic window
    // lowering (Wayland compositors don't honor it); the supported
    // direction is "raise the other window," which produces the
    // same UX outcome. Step mode is incompatible (Scripter needs
    // focus for spacebar) and is silently the winner — the option
    // pairing is documented in the Auto-lower tooltip. Idle-
    // scheduled so the focus/tab shuffling above settles first.
    if (m_btn_lower.get_active() && !m_btn_step.get_active()) {
        m_run_lowered = true;
        Glib::signal_idle().connect_once([this]() {
            // Find a non-Scripter application window and present it.
            //
            // Path notes: get_application() returns null when called
            // from this idle callback (cause TBD), so we fetch the
            // running app via Gio::Application::get_default() and
            // cast it via std::dynamic_pointer_cast (the codebase's
            // gtkmm aliases Glib::RefPtr to std::shared_ptr, so
            // dynamic_pointer_cast is the standard-library cast, not
            // a gtkmm-specific one).
            auto app_base = Gio::Application::get_default();
            auto app_ref = std::dynamic_pointer_cast<Gtk::Application>(
                app_base);
            if (!app_ref) return;
            for (auto* w : app_ref->get_windows()) {
                if (w && w != this) w->present();
            }
        });
    }

    // Output is NOT cleared between Runs. The Clear output button is
    // the user's explicit affordance for that — auto-clearing was
    // doing their work for them in a way that lost previous results
    // when running multiple scripts in sequence. Each script appends
    // its own banner so the boundaries are visible.
    run_next_in_queue();
}

void ScripterWindow::run_next_in_queue() {
    if (m_aborted) {
        // User clicked Stop between scripts. Don't start the next one.
        return;
    }
    if (m_run_queue_idx >= m_run_queue.size()) {
        m_running = false;
        update_run_button_label();
        // s193 m2: if we raised MainWindow at Run start, bring the
        // Scripter back. Idle-scheduled so any final Output append
        // finishes paint before the window comes forward.
        if (m_run_lowered) {
            m_run_lowered = false;
            Glib::signal_idle().connect_once([this]() { present(); });
        }
        return;
    }
    const auto& entry = m_run_queue[m_run_queue_idx];

    // Per-script banner: double-rule above + filename + start time
    // + double-rule below. Matches the m1 design contract:
    // "prominent line and filename, time start; time elapsed at end."
    std::string banner =
        "\n"
        "===================================================\n"
        "  " + entry.display_name + "\n"
        "  Started " + wall_time_now() + "\n"
        "===================================================\n";
    append_output(banner);

    m_script_start = std::chrono::steady_clock::now();
    start_single_script(entry.body, entry.display_name);
}

void ScripterWindow::start_single_script(const std::string& body,
                                          const std::string& /*display_name*/) {
    m_listener->reset();
    m_listener->load_text(body);

    m_listener->set_done_callback([this]() {
        auto elapsed = std::chrono::steady_clock::now() - m_script_start;
        append_output("--- Finished in " + format_elapsed(elapsed) + " ---\n");
        // Advance the queue: schedule the next script on idle so the
        // current done-callback fully unwinds before we re-enter the
        // listener with new content.
        m_run_queue_idx++;
        Glib::signal_idle().connect_once([this]() { run_next_in_queue(); });
    });

    // ── send / wait / gather ─────────────────────────────────────────
    // GTK signal dispatch is main-loop bound. activate() and other
    // write verbs queue signals; they do not deliver them synchronously.
    // If we pump every script line in the same idle slot, an assert
    // on line N+1 can read state that line N's dispatch hasn't
    // propagated yet (s186 m2: observed 235ms latency between
    // activate() and the resulting signal_toggled on a busy Curvz
    // main loop).
    //
    // The shape every script line therefore implies is three-phase:
    //   - send:   dispatch the verb (run_line inside pump_next)
    //   - wait:   yield to GTK so its signal queue drains
    //   - gather: next line reads / asserts against the new state
    //
    // The wait is a one-shot signal_timeout between dispatches. The
    // spin button's value is sampled once per script start and used
    // as the timeout interval. Default 5ms; 0 runs as fast as
    // Glib::signal_timeout(0) allows; higher values pace visibly
    // for demos and debugging.
    //
    // Signal-bound verbs (ToggleButton::click etc) ALSO use the
    // synchronizer internally (wait_for_signal in ScriptableWidget),
    // so they wait on their canonical signal regardless of the step
    // delay. The step delay is additive — it adds visible time on top
    // of the synchronizer's signal-bound wait.
    //
    // s193 m1 — step-through. When m_btn_step is checked, the
    // timed-wait is replaced with a user-paced wait: each step parks
    // m_step_advance as a closure that calls the next step, and the
    // spacebar key controller fires it. The step is the *line*, not
    // the underlying timeout, so a verb mid-signal-sync still
    // completes its synchronizer wait before the next step runs; we
    // only gate the inter-line transition. The toggle is read per-
    // iteration, so the user can flip mid-Run.
    //
    // Lifetime: the step lambda needs to outlive on_run()'s stack
    // frame because each call schedules the next. We wrap it in a
    // shared_ptr that captures itself — the chain holds the only ref,
    // and the final pump_next returning false breaks the chain.
    auto* lst = m_listener.get();
    int delay_ms = static_cast<int>(m_spn_delay.get_value());

    // s193 m1 — the step lambda has a different shape in step mode vs
    // timed mode:
    //
    //   timed mode (default):
    //     enter -> pump_next -> if true, schedule timeout to self
    //     The first call dispatches line 0 immediately; the
    //     dispatch-wait-dispatch chain advances on its own.
    //
    //   step mode:
    //     enter -> highlight current line, park advance closure
    //     User presses space -> advance fires -> pump_next dispatches
    //     the highlighted line -> highlight new current -> park again.
    //     The first call parks at line 0 before dispatching, so the
    //     user sees what's about to run before they press space.
    //
    // The toggle is read each iteration so flipping mid-Run works.
    // Highlight clears on natural end (pump_next returns false) and
    // on abort.
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, lst, step, delay_ms]() {
        if (m_aborted) {
            highlight_step_line(-1);
            m_step_advance = nullptr;
            return;
        }

        // Step mode path: park BEFORE dispatching, on a runnable line.
        // The closure that spacebar fires actually does the dispatch +
        // re-park.
        if (m_btn_step.get_active()) {
            // s193 m1 — fast-forward past comment-only lines so the
            // user only stops on lines that DO something (verbs,
            // get/assert/sleep/quit, #[sub] markers). The fast-forward
            // pumps those skip-lines silently — they emit nothing, the
            // cursor advances, and we land on the next runnable line
            // (or end-of-script). Comment headers that used to require
            // 20 spacebar presses to walk past now scroll by in one.
            size_t runnable = lst->next_runnable_index_from(
                lst->next_line_index());
            while (lst->next_line_index() < runnable) {
                lst->pump_next();
            }
            // Are we at end-of-script? If so, drain so done_callback
            // can fire and close out the script.
            if (lst->next_line_index() >= lst->lines_count()) {
                highlight_step_line(-1);
                lst->pump_next();
                m_step_advance = nullptr;
                return;
            }
            highlight_step_line(static_cast<int>(lst->next_line_index()));
            m_step_advance = [step, lst]() {
                // Spacebar handler clears m_step_advance and calls us.
                // Dispatch one line, then re-enter the step lambda
                // (which fast-forwards comments + re-parks).
                lst->pump_next();
                (*step)();
            };
            return;
        }

        // Timed mode path: dispatch then schedule.
        if (!lst->pump_next()) {
            highlight_step_line(-1);
            m_step_advance = nullptr;
            return;
        }

        // s202 m3 — fast-forward past non-runnable lines in timed
        // mode. Mirrors the step-mode fast-forward above (line ~1256).
        //
        // Pre-m3: every line, including plain `#` comments, blank
        // lines, and unknown `#[tag]` markers, ate one full step
        // delay before the next pump_next. That was cheap when scripts
        // were short and comment-light, but the s202 visual-narration
        // foundation lands `#[sub]` lines that come bundled with
        // explanatory `#` headers — a typical narrated script now
        // doubles or triples in line count, with most of the new
        // lines being comments that emit nothing. Test 24 went from
        // ~12s to ~57s post-m2 prologue for exactly this reason;
        // every comment line was paying delay_ms even though
        // pump_next did nothing visible with it.
        //
        // Fix: after dispatching the current line, fast-forward the
        // cursor past any non-runnable lines that follow before
        // scheduling the next timeout. The dispatched-line wait
        // (which is what `delay_ms` actually exists for — letting
        // GTK's signal queue drain after a verb fires) still happens
        // once per runnable line; comments fly through silently.
        //
        // `#[sub]` lines are runnable per is_runnable_line, so they
        // still get the delay-multiplier treatment (their caption
        // bumps the next runnable line's wait so the subtitle sits
        // readable). The bump is sampled AFTER the fast-forward so
        // the last skipped line's effect — if it was a `#[sub]` —
        // wouldn't actually be possible: `#[sub]` lines aren't
        // skipped because they ARE runnable. The fast-forward only
        // chews silent lines.
        size_t runnable = lst->next_runnable_index_from(
            lst->next_line_index());
        while (lst->next_line_index() < runnable) {
            if (!lst->pump_next()) {
                // End-of-script during fast-forward (a trailing
                // comment block). Same cleanup as the pump_next
                // returning false above.
                highlight_step_line(-1);
                m_step_advance = nullptr;
                return;
            }
        }

        // s191 m3 — `#[sub]` lines bump the next delay by their
        // multiplier so captions sit visible long enough to read.
        int mult = lst->take_delay_multiplier();
        Glib::signal_timeout().connect_once(
            [step]() { (*step)(); }, delay_ms * mult);
    };
    Glib::signal_timeout().connect_once([step]() { (*step)(); }, 0);
}

} // namespace curvz::scripting
