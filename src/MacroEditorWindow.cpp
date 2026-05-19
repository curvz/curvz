#include "MacroEditorWindow.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include "widgets/SpinButton.hpp"  // s211 m2 — unregistered substrate SpinButton for add_spin lambda
#include "widgets/Entry.hpp"       // s211 m2 — unregistered substrate Entry for add_entry lambda
#include <gtkmm/gestureclick.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/adjustment.h>
#include <gdk/gdkkeysyms.h>
#include <sstream>
#include <iomanip>

namespace Curvz {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

MacroEditorWindow::MacroEditorWindow() {
    curvz::utils::set_name(*this, "win_me", "macro_editor_window_root");
    set_title("Macro Editor");
    set_resizable(true);
    set_hide_on_close(true);
    set_default_size(380, 520);

    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    outer->set_spacing(0);

    // ── Header ────────────────────────────────────────────────────────────
    m_macro_name_lbl.set_text("No macro loaded");
    m_macro_name_lbl.set_margin(10);
    m_macro_name_lbl.add_css_class("heading");
    m_macro_name_lbl.set_xalign(0.0f);
    curvz::utils::set_name(m_macro_name_lbl, "win_me_nm", "macro_editor_window_name_lbl");
    outer->append(m_macro_name_lbl);
    outer->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    // ── Step list ─────────────────────────────────────────────────────────
    m_step_scroll.set_vexpand(true);
    m_step_scroll.set_hexpand(true);
    m_step_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_step_scroll.set_min_content_height(180);
    m_step_list.set_spacing(0);
    m_step_scroll.set_child(m_step_list);
    outer->append(m_step_scroll);

    outer->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    // ── Property editor frame ─────────────────────────────────────────────
    m_prop_frame.set_label("Step Properties");
    m_prop_frame.set_margin(8);
    m_prop_grid.set_row_spacing(6);
    m_prop_grid.set_column_spacing(10);
    m_prop_grid.set_margin(8);
    m_prop_frame.set_child(m_prop_grid);
    outer->append(m_prop_frame);

    outer->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

    // ── Footer ────────────────────────────────────────────────────────────
    m_footer.set_spacing(4);
    m_footer.set_margin(8);

    m_btn_up.set_label("▲");
    m_btn_up.set_tooltip_text("Move step up");
    m_btn_up.set_sensitive(false);
    curvz::utils::set_name(m_btn_up, "win_me_up", "macro_editor_window_up_btn");
    m_btn_up.signal_clicked().connect(sigc::mem_fun(*this, &MacroEditorWindow::on_move_up));
    m_footer.append(m_btn_up);

    m_btn_down.set_label("▼");
    m_btn_down.set_tooltip_text("Move step down");
    m_btn_down.set_sensitive(false);
    curvz::utils::set_name(m_btn_down, "win_me_dn", "macro_editor_window_down_btn");
    m_btn_down.signal_clicked().connect(sigc::mem_fun(*this, &MacroEditorWindow::on_move_down));
    m_footer.append(m_btn_down);

    m_btn_delete.set_label("Delete Step");
    m_btn_delete.add_css_class("destructive-action");
    m_btn_delete.set_sensitive(false);
    curvz::utils::set_name(m_btn_delete, "win_me_del", "macro_editor_window_delete_btn");
    m_btn_delete.signal_clicked().connect(sigc::mem_fun(*this, &MacroEditorWindow::on_delete_step));
    m_footer.append(m_btn_delete);

    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    m_footer.append(*spacer);

    m_btn_run_from.set_label("▶ Run from here");
    m_btn_run_from.set_tooltip_text("Run macro starting from selected step");
    m_btn_run_from.add_css_class("suggested-action");
    m_btn_run_from.set_sensitive(false);
    curvz::utils::set_name(m_btn_run_from, "win_me_run", "macro_editor_window_run_from_btn");
    m_btn_run_from.signal_clicked().connect(sigc::mem_fun(*this, &MacroEditorWindow::on_run_from));
    m_footer.append(m_btn_run_from);

    outer->append(m_footer);
    set_child(*outer);

    // Escape closes
    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [this](guint kv, guint, Gdk::ModifierType) -> bool {
            if (kv == GDK_KEY_Escape) { hide(); return true; }
            return false;
        }, true);
    add_controller(key);

    // Subscribe to MacroManager changes so we stay in sync
    m_changed_conn = MacroManager::instance().signal_changed().connect(
        [this]() {
            if (!m_macro_id.empty()) rebuild_step_list();
        });
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void MacroEditorWindow::load_macro(const std::string& macro_id) {
    m_macro_id      = macro_id;
    m_selected_step = -1;

    Macro* m = MacroManager::instance().find_macro(macro_id);
    if (m) {
        set_title("Macro Editor — " + m->name);
        m_macro_name_lbl.set_text(m->name +
            "  (" + std::to_string(m->steps.size()) + " steps)");
    }

    rebuild_step_list();
    rebuild_property_editor();
}

void MacroEditorWindow::show(Gtk::Window& parent) {
    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    set_modal(false);
    present();
}

// ─────────────────────────────────────────────────────────────────────────────
// Step list
// ─────────────────────────────────────────────────────────────────────────────

void MacroEditorWindow::rebuild_step_list() {
    // Remove all children
    while (auto* c = m_step_list.get_first_child())
        m_step_list.remove(*c);

    Macro* m = MacroManager::instance().find_macro(m_macro_id);
    if (!m) {
        auto* lbl = Gtk::make_managed<Gtk::Label>("No macro loaded.");
        lbl->set_margin(16);
        lbl->add_css_class("dim-label");
        m_step_list.append(*lbl);
        return;
    }

    if (m->steps.empty()) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(
            "No steps recorded yet.\nUse  ● Record  in the Macro Manager.");
        lbl->set_margin(16);
        lbl->add_css_class("dim-label");
        lbl->set_justify(Gtk::Justification::CENTER);
        m_step_list.append(*lbl);
        return;
    }

    // Update header
    m_macro_name_lbl.set_text(m->name +
        "  (" + std::to_string(m->steps.size()) + " steps)");

    for (int i = 0; i < (int)m->steps.size(); ++i) {
        const MacroStep& step = m->steps[i];

        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        row->set_spacing(8);
        row->set_margin_start(8);
        row->set_margin_end(8);
        row->set_margin_top(3);
        row->set_margin_bottom(3);

        // Index badge
        auto* idx_lbl = Gtk::make_managed<Gtk::Label>(std::to_string(i + 1) + ".");
        idx_lbl->add_css_class("dim-label");
        idx_lbl->set_width_chars(3);
        idx_lbl->set_xalign(1.0f);
        row->append(*idx_lbl);

        // Step label (use stored label if present, else auto-generate)
        std::string display = step.label.empty() ? step.auto_label() : step.label;
        auto* lbl = Gtk::make_managed<Gtk::Label>(display);
        lbl->set_hexpand(true);
        lbl->set_xalign(0.0f);
        row->append(*lbl);

        // Highlight selected
        if (i == m_selected_step)
            row->add_css_class("macro-row-selected");

        // Click to select
        auto g = Gtk::GestureClick::create();
        g->set_button(1);
        g->signal_pressed().connect(
            [this, i](int, double, double) { set_selected_step(i); });
        row->add_controller(g);

        m_step_list.append(*row);

        // Separator between steps
        if (i < (int)m->steps.size() - 1)
            m_step_list.append(
                *Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    }

    // Update footer button sensitivity
    bool has_sel = m_selected_step >= 0 && m && m_selected_step < (int)m->steps.size();
    m_btn_delete.set_sensitive(has_sel);
    m_btn_run_from.set_sensitive(has_sel);
    m_btn_up.set_sensitive(has_sel && m_selected_step > 0);
    m_btn_down.set_sensitive(has_sel && m_selected_step < (int)m->steps.size() - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property editor
// ─────────────────────────────────────────────────────────────────────────────

void MacroEditorWindow::rebuild_property_editor() {
    // Clear existing grid children
    while (auto* c = m_prop_grid.get_first_child())
        m_prop_grid.remove(*c);
    m_adjs.clear();

    Macro* m = MacroManager::instance().find_macro(m_macro_id);
    if (!m || m_selected_step < 0 || m_selected_step >= (int)m->steps.size()) {
        auto* hint = Gtk::make_managed<Gtk::Label>("Select a step to edit its parameters.");
        hint->add_css_class("dim-label");
        m_prop_grid.attach(*hint, 0, 0, 2, 1);
        return;
    }

    const MacroStep& step = m->steps[m_selected_step];
    int row = 0;

    // Helper to add a labelled spin row
    auto add_spin = [&](const char* label_text, double val,
                        double lo, double hi, double step_inc, int digits,
                        const std::string& field) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(label_text);
        lbl->set_xalign(1.0f);
        m_prop_grid.attach(*lbl, 0, row);

        auto adj = Gtk::Adjustment::create(val, lo, hi, step_inc, step_inc * 10);
        m_adjs.push_back(adj);
        // s211 m2 — unregistered substrate SpinButton (Adjustment-taking
        // form). Per-property transient built inside the `add_spin`
        // lambda inside `rebuild_property_editor`; the lambda is called
        // multiple times per rebuild (once per macro field) with no
        // field-specific abbrev. The `adj->signal_value_changed`
        // connection below is the interaction surface — no per-spin
        // script addressability needed. The Adjustment overload was
        // added to the substrate for this site.
        auto* spin = Gtk::make_managed<curvz::widgets::SpinButton>(
                          curvz::scripting::unregistered,
                          adj, step_inc, digits);
        spin->set_hexpand(true);
        // s219: substrate default is set_numeric(false) for math input.
        m_prop_grid.attach(*spin, 1, row);

        std::string f = field;
        adj->signal_value_changed().connect([this, f, adj]() {
            commit_property(f, adj->get_value());
        });
        ++row;
    };

    // Helper to add a labelled text entry row
    auto add_entry = [&](const char* label_text, const std::string& val,
                         const std::string& field) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(label_text);
        lbl->set_xalign(1.0f);
        m_prop_grid.attach(*lbl, 0, row);

        // s211 m2 — unregistered substrate Entry. Per-property
        // transient built inside the `add_entry` lambda inside
        // `rebuild_property_editor`; same shape as the `add_spin`
        // lambda above. Signal-activate handler is the only
        // interaction surface — no per-entry script addressability
        // needed. Mirrors DocumentGallery's s211 m1 application of
        // the Entry tagged ctor.
        auto* entry = Gtk::make_managed<curvz::widgets::Entry>(
                          curvz::scripting::unregistered);
        entry->set_text(val);
        entry->set_hexpand(true);
        m_prop_grid.attach(*entry, 1, row);

        std::string f = field;
        entry->signal_activate().connect([this, f, entry]() {
            commit_property_str(f, entry->get_text());
        });
        ++row;
    };

    switch (step.op) {
        case MacroStep::Op::Move:
        case MacroStep::Op::Duplicate:
            add_spin("dx:", step.dx, -10000, 10000, 1.0, 2, "dx");
            add_spin("dy:", step.dy, -10000, 10000, 1.0, 2, "dy");
            break;

        case MacroStep::Op::Scale:
            add_spin("Scale X:", step.scale_x, 0.001, 100.0, 0.01, 3, "scale_x");
            add_spin("Scale Y:", step.scale_y, 0.001, 100.0, 0.01, 3, "scale_y");
            break;

        case MacroStep::Op::Rotate:
            add_spin("Angle °:", step.angle_deg, -360.0, 360.0, 0.1, 1, "angle_deg");
            if (step.pivot_is_explicit) {
                add_spin("Pivot X:", step.pivot_x, -10000, 10000, 1.0, 2, "pivot_x");
                add_spin("Pivot Y:", step.pivot_y, -10000, 10000, 1.0, 2, "pivot_y");
            }
            break;

        case MacroStep::Op::SetFill:
        case MacroStep::Op::SetStroke:
            add_entry("Color (#hex):", step.color_hex, "color_hex");
            break;

        case MacroStep::Op::SetStrokeWidth:
            add_spin("Width:", step.value, 0.0, 1000.0, 0.5, 2, "value");
            break;

        case MacroStep::Op::SetOpacity:
            add_spin("Opacity %:", step.value * 100.0, 0.0, 100.0, 1.0, 0, "value_pct");
            break;

        case MacroStep::Op::OffsetPath:
            add_spin("Distance:", step.value, -1000.0, 1000.0, 1.0, 2, "value");
            break;

        default: {
            // Op has no editable parameters
            auto* lbl = Gtk::make_managed<Gtk::Label>(
                "No editable parameters for this step type.");
            lbl->add_css_class("dim-label");
            m_prop_grid.attach(*lbl, 0, 0, 2, 1);
            break;
        }
    }

    // Always show/edit the step label
    ++row;
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4); sep->set_margin_bottom(4);
    m_prop_grid.attach(*sep, 0, row, 2, 1); ++row;
    add_entry("Label:", step.label.empty() ? step.auto_label() : step.label, "label");
}

// ─────────────────────────────────────────────────────────────────────────────
// Selection
// ─────────────────────────────────────────────────────────────────────────────

void MacroEditorWindow::set_selected_step(int idx) {
    m_selected_step = idx;
    rebuild_step_list();
    rebuild_property_editor();
}

// ─────────────────────────────────────────────────────────────────────────────
// Footer actions
// ─────────────────────────────────────────────────────────────────────────────

void MacroEditorWindow::on_move_up() {
    Macro* m = MacroManager::instance().find_macro(m_macro_id);
    if (!m || m_selected_step <= 0) return;
    std::swap(m->steps[m_selected_step], m->steps[m_selected_step - 1]);
    --m_selected_step;
    MacroManager::instance().save();
    rebuild_step_list();
    rebuild_property_editor();
}

void MacroEditorWindow::on_move_down() {
    Macro* m = MacroManager::instance().find_macro(m_macro_id);
    if (!m || m_selected_step < 0 || m_selected_step >= (int)m->steps.size() - 1) return;
    std::swap(m->steps[m_selected_step], m->steps[m_selected_step + 1]);
    ++m_selected_step;
    MacroManager::instance().save();
    rebuild_step_list();
    rebuild_property_editor();
}

void MacroEditorWindow::on_delete_step() {
    Macro* m = MacroManager::instance().find_macro(m_macro_id);
    if (!m || m_selected_step < 0 || m_selected_step >= (int)m->steps.size()) return;
    m->steps.erase(m->steps.begin() + m_selected_step);
    // Clamp selection
    if (m_selected_step >= (int)m->steps.size())
        m_selected_step = (int)m->steps.size() - 1;
    MacroManager::instance().save();
    rebuild_step_list();
    rebuild_property_editor();
}

void MacroEditorWindow::on_run_from() {
    if (m_selected_step < 0) return;
    m_sig_run_from.emit(m_macro_id, m_selected_step);
}

// ─────────────────────────────────────────────────────────────────────────────
// Property commit
// ─────────────────────────────────────────────────────────────────────────────

void MacroEditorWindow::commit_property(const std::string& field, double value) {
    Macro* m = MacroManager::instance().find_macro(m_macro_id);
    if (!m || m_selected_step < 0 || m_selected_step >= (int)m->steps.size()) return;
    MacroStep& s = m->steps[m_selected_step];

    if      (field == "dx")        s.dx        = value;
    else if (field == "dy")        s.dy        = value;
    else if (field == "scale_x")   s.scale_x   = value;
    else if (field == "scale_y")   s.scale_y   = value;
    else if (field == "angle_deg") s.angle_deg = value;
    else if (field == "pivot_x")   s.pivot_x   = value;
    else if (field == "pivot_y")   s.pivot_y   = value;
    else if (field == "value")     s.value     = value;
    else if (field == "value_pct") s.value     = value / 100.0;

    // Regenerate auto-label if user hasn't customised it
    if (s.label.empty() || s.label == s.auto_label())
        s.label = s.auto_label();

    MacroManager::instance().save();
    // Refresh the step list row label without full rebuild
    rebuild_step_list();
}

void MacroEditorWindow::commit_property_str(const std::string& field,
                                             const std::string& value) {
    Macro* m = MacroManager::instance().find_macro(m_macro_id);
    if (!m || m_selected_step < 0 || m_selected_step >= (int)m->steps.size()) return;
    MacroStep& s = m->steps[m_selected_step];

    if      (field == "color_hex") s.color_hex = value;
    else if (field == "label")     s.label     = value;

    MacroManager::instance().save();
    rebuild_step_list();
}

} // namespace Curvz
