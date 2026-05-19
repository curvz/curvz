#include "MacroManagerWindow.hpp"
#include "CurvzLog.hpp"
#include "widgets/Button.hpp"
#include "widgets/Entry.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <gtkmm/gestureclick.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/checkbutton.h>
#include <pangomm/layout.h>
#include <glibmm/main.h>
#include <gdk/gdkkeysyms.h>

namespace Curvz {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

MacroManagerWindow::MacroManagerWindow() {
    curvz::utils::set_name(*this, "win_mm", "macro_manager_window_root");
    set_title("Macros");
    set_resizable(true);
    set_hide_on_close(true);
    set_default_size(320, 480);

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    outer->set_spacing(0);

    // ── Action bar ────────────────────────────────────────────────────────
    m_action_bar.set_spacing(4);
    m_action_bar.set_margin(8);

    // s213 m3 — registered substrate Buttons (value→pointer flip from
    // the value-held members in the s108-era MacroManagerWindow.hpp).
    // Construction now happens here, not implicitly at MacroManagerWindow
    // construction. set_name calls retained for widget_names_sync.
    m_btn_record = Gtk::make_managed<curvz::widgets::Button>(
        "win_mm_rec", "● Record");
    m_btn_record->set_tooltip_text("Start recording a new macro step sequence");
    curvz::utils::set_name(*m_btn_record, "win_mm_rec", "macro_manager_window_record_btn");
    m_btn_record->signal_clicked().connect(
        sigc::mem_fun(*this, &MacroManagerWindow::on_record_clicked));
    m_action_bar.append(*m_btn_record);

    m_btn_stop = Gtk::make_managed<curvz::widgets::Button>(
        "win_mm_stp", "■ Stop");
    m_btn_stop->set_tooltip_text("Stop recording");
    m_btn_stop->set_sensitive(false);
    curvz::utils::set_name(*m_btn_stop, "win_mm_stp", "macro_manager_window_stop_btn");
    m_btn_stop->signal_clicked().connect(
        sigc::mem_fun(*this, &MacroManagerWindow::on_stop_clicked));
    m_action_bar.append(*m_btn_stop);

    // Spacer
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    m_action_bar.append(*spacer);

    m_current_label.set_text("No macro selected");
    m_current_label.set_ellipsize(Pango::EllipsizeMode::END);
    m_current_label.set_hexpand(true);
    m_current_label.set_xalign(1.0f);
    m_current_label.add_css_class("dim-label");
    curvz::utils::set_name(m_current_label, "win_mm_cur", "macro_manager_window_current_lbl");
    m_action_bar.append(m_current_label);

    m_btn_run = Gtk::make_managed<curvz::widgets::Button>(
        "win_mm_run", "▶ Run");
    m_btn_run->set_tooltip_text("Run current macro  (Ctrl+M)");
    m_btn_run->add_css_class("suggested-action");
    m_btn_run->set_sensitive(false);
    curvz::utils::set_name(*m_btn_run, "win_mm_run", "macro_manager_window_run_btn");
    m_btn_run->signal_clicked().connect(
        sigc::mem_fun(*this, &MacroManagerWindow::on_run_clicked));
    m_action_bar.append(*m_btn_run);

    outer->append(m_action_bar);
    outer->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    // ── Scrolled content ──────────────────────────────────────────────────
    m_scroll.set_vexpand(true);
    m_scroll.set_hexpand(true);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_content.set_spacing(0);
    m_scroll.set_child(m_content);
    outer->append(m_scroll);

    outer->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    // ── Footer ────────────────────────────────────────────────────────────
    m_footer.set_spacing(4);
    m_footer.set_margin(8);

    m_btn_add_folder = Gtk::make_managed<curvz::widgets::Button>(
        "win_mm_af", "+ Folder");
    m_btn_add_folder->set_tooltip_text("Add a new macro folder");
    curvz::utils::set_name(*m_btn_add_folder, "win_mm_af", "macro_manager_window_add_folder_btn");
    m_btn_add_folder->signal_clicked().connect(
        sigc::mem_fun(*this, &MacroManagerWindow::on_add_folder));
    m_footer.append(*m_btn_add_folder);

    m_btn_add_macro = Gtk::make_managed<curvz::widgets::Button>(
        "win_mm_am", "+ Macro");
    m_btn_add_macro->set_tooltip_text("Add a new macro to the selected folder");
    m_btn_add_macro->set_sensitive(false);
    curvz::utils::set_name(*m_btn_add_macro, "win_mm_am", "macro_manager_window_add_macro_btn");
    m_btn_add_macro->signal_clicked().connect(
        sigc::mem_fun(*this, &MacroManagerWindow::on_add_macro));
    m_footer.append(*m_btn_add_macro);

    auto* del_spacer = Gtk::make_managed<Gtk::Box>();
    del_spacer->set_hexpand(true);
    m_footer.append(*del_spacer);

    m_btn_delete = Gtk::make_managed<curvz::widgets::Button>(
        "win_mm_del", "Delete");
    m_btn_delete->set_tooltip_text("Delete selected macro or folder");
    m_btn_delete->add_css_class("destructive-action");
    m_btn_delete->set_sensitive(false);
    curvz::utils::set_name(*m_btn_delete, "win_mm_del", "macro_manager_window_delete_btn");
    m_btn_delete->signal_clicked().connect(
        sigc::mem_fun(*this, &MacroManagerWindow::on_delete_selected));
    m_footer.append(*m_btn_delete);

    outer->append(m_footer);
    set_child(*outer);

    // Escape closes; Ctrl+M runs current macro even when this window has focus
    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [this](guint kv, guint, Gdk::ModifierType mod) -> bool {
            if (kv == GDK_KEY_Escape) { hide(); return true; }
            bool ctrl = (mod & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
            if (ctrl && (kv == GDK_KEY_m || kv == GDK_KEY_M)) {
                on_run_clicked();
                return true;
            }
            return false;
        }, true);
    add_controller(key);

    // Subscribe to MacroManager changes
    m_changed_conn = MacroManager::instance().signal_changed().connect(
        sigc::mem_fun(*this, &MacroManagerWindow::refresh));

    rebuild();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void MacroManagerWindow::show(Gtk::Window& parent) {
    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    set_modal(false);   // floating, not modal
    present();
}

void MacroManagerWindow::refresh() {
    rebuild();
    update_record_ui();
}

// ─────────────────────────────────────────────────────────────────────────────
// Rebuild content tree
// ─────────────────────────────────────────────────────────────────────────────

void MacroManagerWindow::rebuild() {
    // Remove all existing children
    while (auto* child = m_content.get_first_child())
        m_content.remove(*child);

    // Clear per-rebuild widget maps — pointers held in here would be
    // stale once Gtk::make_managed children are torn down by the loop
    // above. Repopulated as rows are created below.
    m_macro_rows.clear();
    m_macro_name_labels.clear();

    auto& mgr = MacroManager::instance();

    if (mgr.folders().empty()) {
        auto* lbl = Gtk::make_managed<Gtk::Label>("No macros yet.\nClick  + Folder  to begin.");
        lbl->set_margin(24);
        lbl->add_css_class("dim-label");
        m_content.append(*lbl);
        return;
    }

    for (const auto& folder : mgr.folders()) {
        // ── Folder header row ─────────────────────────────────────────────
        auto* frow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        frow->set_spacing(4);
        frow->add_css_class("macro-folder-row");
        frow->set_margin_start(4);
        frow->set_margin_end(4);
        frow->set_margin_top(4);

        // Expand/collapse arrow
        bool expanded = true;
        auto exp_it = m_folder_expanded.find(folder.internal_id);
        if (exp_it != m_folder_expanded.end()) expanded = exp_it->second;
        else m_folder_expanded[folder.internal_id] = true;

        // s213 m3 — unregistered substrate Button (per-folder loop in
        // rebuild path; no per-folder abbrev possible). First-costume
        // use case for the s209 m1 unregistered_t pattern.
        auto* arrow_btn = Gtk::make_managed<curvz::widgets::Button>(
            curvz::scripting::unregistered_t{}, expanded ? "▾" : "▸");
        arrow_btn->add_css_class("flat");
        arrow_btn->set_has_frame(false);
        frow->append(*arrow_btn);

        auto* flbl = Gtk::make_managed<Gtk::Label>(folder.name);
        flbl->set_hexpand(true);
        flbl->set_xalign(0.0f);
        flbl->add_css_class("macro-folder-label");
        frow->append(*flbl);

        m_content.append(*frow);

        // ── Macro rows (collapsible) ──────────────────────────────────────
        auto* rev = Gtk::make_managed<Gtk::Revealer>();
        rev->set_reveal_child(expanded);
        rev->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);

        auto* macro_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        macro_box->set_spacing(0);

        const std::string fid = folder.internal_id;

        // Folder header click → select folder (for + Macro)
        auto fclick = Gtk::GestureClick::create();
        fclick->signal_pressed().connect(
            [this, fid](int, double, double) {
                m_selected_folder_id = fid;
                m_selected_macro_id  = "";
                m_btn_add_macro->set_sensitive(true);
                m_btn_delete->set_sensitive(true);
                m_btn_run->set_sensitive(false);
                m_current_label.set_text("Folder selected");
            });
        frow->add_controller(fclick);

        // Double-click folder label → rename
        auto fdbl = Gtk::GestureClick::create();
        fdbl->set_button(1);
        fdbl->signal_pressed().connect(
            [this, fid, flbl](int n_press, double, double) {
                if (n_press == 2) begin_rename_folder(fid, flbl);
            });
        flbl->add_controller(fdbl);

        // Arrow toggle expand/collapse
        arrow_btn->signal_clicked().connect(
            [this, fid, rev, arrow_btn]() {
                bool& exp = m_folder_expanded[fid];
                exp = !exp;
                rev->set_reveal_child(exp);
                arrow_btn->set_label(exp ? "▾" : "▸");
            });

        // ── One row per macro ──────────────────────────────────────────────
        for (const auto& mid : folder.macro_ids) {
            Macro* mac = MacroManager::instance().find_macro(mid);
            if (!mac) continue;

            auto* mrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
            mrow->set_spacing(4);
            mrow->set_margin_start(20);
            mrow->set_margin_end(4);
            mrow->set_margin_top(2);
            mrow->set_margin_bottom(2);

            // ★ Layer-panel toggle
            // Must use a CAPTURE-phase GestureClick that claims the sequence
            // before the row-level mclick gesture can consume it.
            //
            // s213 m3 — unregistered substrate Button (per-macro inner
            // loop in rebuild path).
            auto* star_btn = Gtk::make_managed<curvz::widgets::Button>(
                curvz::scripting::unregistered_t{},
                mac->in_layer_panel ? "★" : "☆");
            star_btn->add_css_class("flat");
            star_btn->set_has_frame(false);
            star_btn->set_tooltip_text("Show in Layers panel macro area");
            const std::string cmid = mid;

            auto star_gesture = Gtk::GestureClick::create();
            star_gesture->set_button(1);
            star_gesture->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
            star_gesture->signal_pressed().connect(
                [this, cmid, star_gesture](int, double, double) {
                    // Claim immediately so mrow gesture never sees this press
                    star_gesture->set_state(Gtk::EventSequenceState::CLAIMED);
                    Macro* m = MacroManager::instance().find_macro(cmid);
                    if (!m) { LOG_WARN("MacroManager: star click — macro {} not found", cmid); return; }
                    m->in_layer_panel = !m->in_layer_panel;
                    LOG_INFO("MacroManager: star toggled → in_layer_panel={} for '{}'",
                             m->in_layer_panel, m->name);
                    m_changed_conn.block();
                    MacroManager::instance().save();
                    m_changed_conn.unblock();
                    Glib::signal_idle().connect_once([this]() { rebuild(); });
                });
            star_btn->add_controller(star_gesture);
            mrow->append(*star_btn);

            // Name label
            auto* nlbl = Gtk::make_managed<Gtk::Label>(mac->name);
            nlbl->set_hexpand(true);
            nlbl->set_xalign(0.0f);
            nlbl->set_ellipsize(Pango::EllipsizeMode::END);

            // Highlight if current
            Macro* cur = MacroManager::instance().current_macro();
            if (cur && cur->internal_id == mid)
                nlbl->add_css_class("accent");

            mrow->append(*nlbl);

            // Register row + label for direct CSS-class swap on click.
            // Without this, only rebuild() can move the highlight, and
            // we deliberately suppress rebuild on single-click to keep
            // the GestureClick double-click counter alive.
            m_macro_rows[mid]        = mrow;
            m_macro_name_labels[mid] = nlbl;

            // Step count badge
            std::string badge = std::to_string(mac->steps.size()) + " steps";
            auto* badge_lbl = Gtk::make_managed<Gtk::Label>(badge);
            badge_lbl->add_css_class("dim-label");
            badge_lbl->set_margin_end(4);
            mrow->append(*badge_lbl);

            // Run button
            // s213 m3 — unregistered substrate Button (per-macro inner loop).
            auto* run_btn = Gtk::make_managed<curvz::widgets::Button>(
                curvz::scripting::unregistered_t{}, "▶");
            run_btn->add_css_class("flat");
            run_btn->set_has_frame(false);
            run_btn->set_tooltip_text("Run this macro");
            {
                auto g = Gtk::GestureClick::create();
                g->set_button(1);
                g->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
                g->signal_pressed().connect(
                    [this, cmid, g](int, double, double) {
                        g->set_state(Gtk::EventSequenceState::CLAIMED);
                        on_macro_run(cmid);
                    });
                run_btn->add_controller(g);
            }
            mrow->append(*run_btn);

            // Edit button
            // s213 m3 — unregistered substrate Button (per-macro inner loop).
            auto* edit_btn = Gtk::make_managed<curvz::widgets::Button>(
                curvz::scripting::unregistered_t{}, "✎");
            edit_btn->add_css_class("flat");
            edit_btn->set_has_frame(false);
            edit_btn->set_tooltip_text("Edit macro steps");
            {
                auto g = Gtk::GestureClick::create();
                g->set_button(1);
                g->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
                g->signal_pressed().connect(
                    [this, cmid, g](int, double, double) {
                        g->set_state(Gtk::EventSequenceState::CLAIMED);
                        on_macro_edit(cmid);
                    });
                edit_btn->add_controller(g);
            }
            mrow->append(*edit_btn);

            // Single click → select; double-click → rename
            auto mclick = Gtk::GestureClick::create();
            mclick->set_button(1);
            mclick->signal_pressed().connect(
                [this, cmid](int n_press, double, double) {
                    if (n_press == 1) {
                        // Update model state only — no rebuild, so double-click
                        // press count is not reset by a widget tree teardown.
                        // The CSS classes that mark "selected row" and
                        // "active macro" therefore need to be moved by
                        // hand here, since rebuild() is what would
                        // normally re-emit them per-row.
                        const std::string old_id = m_selected_macro_id;
                        if (old_id != cmid) {
                            // Drop the row-background highlight from the
                            // previously-selected row.
                            if (!old_id.empty()) {
                                auto rit = m_macro_rows.find(old_id);
                                if (rit != m_macro_rows.end() && rit->second)
                                    rit->second->remove_css_class("macro-row-selected");
                            }
                            // Drop the accent (active-macro) class from
                            // the previously-active label. set_current()
                            // below will move "current" to cmid, so the
                            // old current's accent must come off too.
                            // current_macro() reflects the pre-update
                            // state at this point.
                            if (auto* cur = MacroManager::instance().current_macro()) {
                                auto lit = m_macro_name_labels.find(cur->internal_id);
                                if (lit != m_macro_name_labels.end() && lit->second)
                                    lit->second->remove_css_class("accent");
                            }
                        }

                        m_selected_macro_id = cmid;
                        MacroManager::instance().set_current(cmid);
                        Macro* m = MacroManager::instance().find_macro(cmid);
                        if (m) {
                            m_current_label.set_text(m->name);
                            m_selected_folder_id = m->folder_id;
                        }

                        // Apply the row-background highlight + accent to
                        // the freshly-selected row. Idempotent — adding
                        // a class already present is a no-op.
                        {
                            auto rit = m_macro_rows.find(cmid);
                            if (rit != m_macro_rows.end() && rit->second)
                                rit->second->add_css_class("macro-row-selected");
                            auto lit = m_macro_name_labels.find(cmid);
                            if (lit != m_macro_name_labels.end() && lit->second)
                                lit->second->add_css_class("accent");
                        }

                        m_btn_run->set_sensitive(true);
                        m_btn_add_macro->set_sensitive(true);
                        m_btn_delete->set_sensitive(true);
                    } else if (n_press == 2) {
                        begin_rename_macro(cmid);
                    }
                });
            mrow->add_controller(mclick);

            // Highlight selected row
            if (mid == m_selected_macro_id)
                mrow->add_css_class("macro-row-selected");

            macro_box->append(*mrow);
        }

        rev->set_child(*macro_box);
        m_content.append(*rev);
    }

    // Update footer sensitivity
    bool has_folder = !mgr.folders().empty();
    bool has_macro  = !m_selected_macro_id.empty();
    m_btn_add_macro->set_sensitive(!m_selected_folder_id.empty());
    m_btn_delete->set_sensitive(has_folder || has_macro);
}

// ─────────────────────────────────────────────────────────────────────────────
// Recording UI
// ─────────────────────────────────────────────────────────────────────────────

void MacroManagerWindow::update_record_ui() {
    auto& mgr = MacroManager::instance();
    bool rec  = mgr.is_recording();
    m_btn_record->set_sensitive(!rec);
    m_btn_stop->set_sensitive(rec);
    if (rec) {
        m_btn_record->add_css_class("destructive-action");
    } else {
        m_btn_record->remove_css_class("destructive-action");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Action bar handlers
// ─────────────────────────────────────────────────────────────────────────────

void MacroManagerWindow::on_record_clicked() {
    auto& mgr = MacroManager::instance();

    // Need a current macro to record into
    if (m_selected_macro_id.empty()) {
        LOG_WARN("MacroManager: no macro selected for recording");
        return;
    }
    mgr.start_recording(m_selected_macro_id);
    update_record_ui();
}

void MacroManagerWindow::on_stop_clicked() {
    MacroManager::instance().stop_recording();
    update_record_ui();
    rebuild();
}

void MacroManagerWindow::on_run_clicked() {
    if (m_selected_macro_id.empty()) {
        // No macro selected — present manager so user can pick one
        present();
        return;
    }
    m_sig_run.emit(m_selected_macro_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Footer handlers
// ─────────────────────────────────────────────────────────────────────────────

void MacroManagerWindow::on_add_folder() {
    auto& f = MacroManager::instance().add_folder("New Folder");
    m_selected_folder_id = f.internal_id;
    m_folder_expanded[f.internal_id] = true;
    rebuild();
    // TODO: immediately begin rename of the new folder label
}

void MacroManagerWindow::on_add_macro() {
    if (m_selected_folder_id.empty()) return;
    auto& m = MacroManager::instance().add_macro(m_selected_folder_id, "New Macro");
    set_selected(m.internal_id);
    rebuild();
    // TODO: immediately begin rename
}

void MacroManagerWindow::on_delete_selected() {
    auto& mgr = MacroManager::instance();
    if (!m_selected_macro_id.empty()) {
        mgr.delete_macro(m_selected_macro_id);
        m_selected_macro_id = "";
    } else if (!m_selected_folder_id.empty()) {
        mgr.delete_folder(m_selected_folder_id);
        m_selected_folder_id = "";
    }
    rebuild();
}

// ─────────────────────────────────────────────────────────────────────────────
// Row interactions
// ─────────────────────────────────────────────────────────────────────────────

void MacroManagerWindow::set_selected(const std::string& macro_id) {
    m_selected_macro_id = macro_id;
    MacroManager::instance().set_current(macro_id);

    Macro* m = MacroManager::instance().find_macro(macro_id);
    if (m) {
        m_current_label.set_text(m->name);
        m_selected_folder_id = m->folder_id;
    }

    m_btn_run->set_sensitive(true);
    m_btn_add_macro->set_sensitive(true);
    m_btn_delete->set_sensitive(true);
    // Defer highlight redraw to idle so any in-progress gesture completes first
    Glib::signal_idle().connect_once([this]() { rebuild(); });
}

void MacroManagerWindow::on_macro_run(const std::string& macro_id) {
    set_selected(macro_id);
    m_sig_run.emit(macro_id);
}

void MacroManagerWindow::on_macro_edit(const std::string& macro_id) {
    set_selected(macro_id);
    m_sig_edit.emit(macro_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// Inline rename
// ─────────────────────────────────────────────────────────────────────────────

void MacroManagerWindow::begin_rename_macro(const std::string& macro_id) {
    Macro* m = MacroManager::instance().find_macro(macro_id);
    if (!m) return;

    // Use a simple dialog-style window instead of a popover to avoid
    // parenting issues with managed widgets in a rebuilt tree.
    auto* dlg = new Gtk::Window();
    dlg->set_title("Rename Macro");
    dlg->set_transient_for(*this);
    curvz::utils::apply_motif_class_from_parent(*dlg, *this);  // s117 m18 v2
    dlg->set_modal(true);
    dlg->set_resizable(false);
    dlg->set_default_size(240, -1);
    dlg->set_hide_on_close(true);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_spacing(8);
    box->set_margin(12);

    // s213 m3 — unregistered substrate widgets for the per-show
    // transient rename prompt. Per-show heap-allocated dialog
    // (set_hide_on_close + delete-on-idle) — different lifetime
    // shape than PrintManager/TranslateDialog/ThemesPanel hosts
    // (which are MainWindow-owned singletons), but the substrate
    // sweep is the same: every show rebuilds, no per-call abbrev
    // is possible, so the unregistered_t tag is the right call.
    auto* entry = Gtk::make_managed<curvz::widgets::Entry>(
        curvz::scripting::unregistered_t{});
    entry->set_text(m->name);
    entry->select_region(0, -1);
    box->append(*entry);

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_row->set_spacing(8);
    btn_row->set_halign(Gtk::Align::END);
    auto* ok_btn = Gtk::make_managed<curvz::widgets::Button>(
        curvz::scripting::unregistered_t{}, "Rename");
    ok_btn->add_css_class("suggested-action");
    btn_row->append(*ok_btn);
    box->append(*btn_row);

    dlg->set_child(*box);

    auto commit = [this, macro_id, entry, dlg]() {
        std::string new_name = entry->get_text();
        if (new_name.empty()) new_name = "Untitled";
        dlg->hide();
        MacroManager::instance().rename_macro(macro_id, new_name);
    };

    ok_btn->signal_clicked().connect(commit);
    entry->signal_activate().connect(commit);

    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [dlg, commit](guint kv, guint, Gdk::ModifierType) -> bool {
            if (kv == GDK_KEY_Escape) { dlg->hide(); return true; }
            if (kv == GDK_KEY_Return) { commit(); return true; }
            return false;
        }, true);
    dlg->add_controller(key);

    dlg->signal_hide().connect([dlg]() {
        Glib::signal_idle().connect_once([dlg]() { delete dlg; });
    });

    dlg->present();
    entry->grab_focus();
}

void MacroManagerWindow::begin_rename_folder(const std::string& folder_id,
                                              Gtk::Label* lbl) {
    MacroFolder* f = MacroManager::instance().find_folder(folder_id);
    if (!f) return;

    auto* dlg = new Gtk::Window();
    dlg->set_title("Rename Folder");
    dlg->set_transient_for(*this);
    curvz::utils::apply_motif_class_from_parent(*dlg, *this);  // s117 m18 v2
    dlg->set_modal(true);
    dlg->set_resizable(false);
    dlg->set_default_size(240, -1);
    dlg->set_hide_on_close(true);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_spacing(8);
    box->set_margin(12);

    // s213 m3 — unregistered substrate widgets for the per-show
    // transient rename prompt (sibling of begin_rename_macro above).
    auto* entry = Gtk::make_managed<curvz::widgets::Entry>(
        curvz::scripting::unregistered_t{});
    entry->set_text(f->name);
    entry->select_region(0, -1);
    box->append(*entry);

    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_row->set_spacing(8);
    btn_row->set_halign(Gtk::Align::END);
    auto* ok_btn = Gtk::make_managed<curvz::widgets::Button>(
        curvz::scripting::unregistered_t{}, "Rename");
    ok_btn->add_css_class("suggested-action");
    btn_row->append(*ok_btn);
    box->append(*btn_row);

    dlg->set_child(*box);

    auto commit = [this, folder_id, entry, dlg]() {
        std::string new_name = entry->get_text();
        if (new_name.empty()) new_name = "Untitled";
        dlg->hide();
        MacroManager::instance().rename_folder(folder_id, new_name);
    };

    ok_btn->signal_clicked().connect(commit);
    entry->signal_activate().connect(commit);

    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [dlg, commit](guint kv, guint, Gdk::ModifierType) -> bool {
            if (kv == GDK_KEY_Escape) { dlg->hide(); return true; }
            if (kv == GDK_KEY_Return) { commit(); return true; }
            return false;
        }, true);
    dlg->add_controller(key);

    dlg->signal_hide().connect([dlg]() {
        Glib::signal_idle().connect_once([dlg]() { delete dlg; });
    });

    dlg->present();
    entry->grab_focus();
}

} // namespace Curvz
