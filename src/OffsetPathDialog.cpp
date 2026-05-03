#include "OffsetPathDialog.hpp"
#include "CurvzDocument.hpp"     // CanvasModel
#include "curvz_utils.hpp"       // s117 m18 v2
#include <gtkmm/stringlist.h>

namespace Curvz {

OffsetPathDialog::OffsetPathDialog() {
    curvz::utils::set_name(*this, "dlg_off", "offset_path_dialog_root");
    set_title("Offset Path");
    set_resizable(false);
    set_modal(true);
    set_default_size(280, -1);

    // ── Distance row (populated per-show) ─────────────────────────────────
    m_dist_row.set_spacing(4);
    m_dist_row.set_hexpand(true);

    // ── Side dropdown ─────────────────────────────────────────────────────
    auto sides = Gtk::StringList::create({"Outside", "Inside", "Both"});
    m_side_drop.set_model(sides);
    m_side_drop.set_selected(0);
    m_side_drop.set_hexpand(true);
    curvz::utils::set_name(m_side_drop, "dlg_off_side", "offset_path_dialog_side_dd");

    // ── Grid layout ───────────────────────────────────────────────────────
    m_grid.set_row_spacing(8);
    m_grid.set_column_spacing(12);
    m_grid.set_margin(16);

    auto* lbl_dist = Gtk::make_managed<Gtk::Label>("Distance:");
    lbl_dist->set_halign(Gtk::Align::START);
    auto* lbl_side = Gtk::make_managed<Gtk::Label>("Side:");
    lbl_side->set_halign(Gtk::Align::START);

    m_grid.attach(*lbl_dist,   0, 0);
    m_grid.attach(m_dist_row,  1, 0);
    m_grid.attach(*lbl_side,   0, 1);
    m_grid.attach(m_side_drop, 1, 1);

    m_keep_original.set_active(false);
    m_keep_original.set_margin_top(4);
    curvz::utils::set_name(m_keep_original, "dlg_off_keep", "offset_path_dialog_keep_original_check");
    m_grid.attach(m_keep_original, 0, 2, 2, 1);

    // ── Button row ────────────────────────────────────────────────────────
    m_btn_row.set_spacing(8);
    m_btn_row.set_halign(Gtk::Align::END);
    m_btn_row.set_margin_start(16);
    m_btn_row.set_margin_end(16);
    m_btn_row.set_margin_bottom(16);
    curvz::utils::set_name(m_btn_cancel, "dlg_off_cnc", "offset_path_dialog_cancel_btn");
    curvz::utils::set_name(m_btn_apply, "dlg_off_app", "offset_path_dialog_apply_btn");
    m_btn_row.append(m_btn_cancel);
    m_btn_row.append(m_btn_apply);

    m_btn_apply.add_css_class("suggested-action");

    m_btn_cancel.signal_clicked().connect(
        sigc::mem_fun(*this, &OffsetPathDialog::on_cancel));
    m_btn_apply.signal_clicked().connect(
        sigc::mem_fun(*this, &OffsetPathDialog::on_apply));

    // ── Root box ──────────────────────────────────────────────────────────
    m_sep.set_margin_top(4);
    m_root.append(m_grid);
    m_root.append(m_sep);
    m_root.append(m_btn_row);
    set_child(m_root);
}

void OffsetPathDialog::build_distance_spin(const CanvasModel* model,
                                           double initial) {
    // Tear down previous children (spin + unit label) so doc switches don't
    // leave stale widgets pointing at a dead CanvasModel.
    while (auto* child = m_dist_row.get_first_child()) {
        m_dist_row.remove(*child);
    }
    m_distance_spin = nullptr;

    m_distance_spin =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, model);
    curvz::utils::set_name(m_distance_spin, "dlg_off_dist", "offset_path_dialog_distance_spn");
    m_distance_spin->with_value(initial)
                    ->with_tooltip("Offset distance (negative = inward)")
                    ->with_width_chars(8);
    m_distance_spin->set_hexpand(true);
    // Honor active unit — no-op on fresh construction, safe to call.
    m_distance_spin->refresh_units();

    m_dist_row.append(*m_distance_spin);
    if (auto* ul = m_distance_spin->get_unit_label()) {
        m_dist_row.append(*ul);
    }
}

void OffsetPathDialog::show(Gtk::Window& parent,
                            const CanvasModel* model,
                            Callback cb) {
    m_callback = std::move(cb);
    build_distance_spin(model, m_last_distance);
    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    present();
}

void OffsetPathDialog::on_apply() {
    Options opts;
    opts.distance = m_distance_spin
                        ? m_distance_spin->get_internal_value()
                        : m_last_distance;
    m_last_distance     = opts.distance;
    opts.keep_original  = m_keep_original.get_active();
    int sel = (int)m_side_drop.get_selected();
    if      (sel == 1) opts.side = OffsetSide::Inside;
    else if (sel == 2) opts.side = OffsetSide::Both;
    else               opts.side = OffsetSide::Outside;

    hide();
    if (m_callback) m_callback(opts);
}

void OffsetPathDialog::on_cancel() {
    hide();
}

} // namespace Curvz
