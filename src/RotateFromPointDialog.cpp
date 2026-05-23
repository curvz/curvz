// RotateFromPointDialog.cpp ───────────────────────────────────────────────────
//
// s210 m2 — implementation of the rotate-from-point dialog. See
// RotateFromPointDialog.hpp for the rationale and lifetime model.
//
// All visual decisions (260px non-resizable, 8px grid row spacing,
// 12px column spacing, 16px margins, unit-label column 2, separator
// between pivot and rotation blocks, right-aligned Cancel/Apply with
// 8px gap, Apply carries `suggested-action`) are copied verbatim from
// the pre-conversion inline form in `Canvas_input.cpp::on_pivot_dialog`.
//
#include "RotateFromPointDialog.hpp"

#include "CurvzDocument.hpp"   // CanvasModel (full definition)
#include "curvz_utils.hpp"     // set_name, apply_motif_class_from_parent
#include "CurvzLog.hpp"

namespace Curvz {

// ── ctor ──────────────────────────────────────────────────────────────────
//
// Default-constructed once as a MainWindow member; show() is what
// callers invoke per R-key rotate session. The widget tree builds
// lazily on the first show() via the m_built latch and stays in the
// tree for the app's lifetime.
RotateFromPointDialog::RotateFromPointDialog() {
    set_title("Rotate from Point");
    // Pre-conversion behaviour was modal=true. The five-Curvz-precedent
    // (MacroManagerWindow, ManageTemplatesDialog, NewDocumentDialog,
    // PrintManager, SaveAsTemplateDialog) shows modal+hide-on-close is
    // a well-trodden combination; keep modal-true to preserve the
    // R-key rotate's "no other input until you finish this" feel.
    set_modal(true);
    set_resizable(false);
    set_default_size(260, -1);
    // s210 m2 — the singleton shape Curvz uses for long-lived dialogs.
    // close() now hides the window; the next show() re-presents it
    // populated from the new caller state.
    set_hide_on_close(true);
    // Window name + long-name annotation kept for CSS hooks and
    // widget_names_sync ingestion (substrate widgets register
    // themselves separately on construction — this is the GTK-side
    // name only).
    curvz::utils::set_name(*this, "dlg_rfp", "rotate_from_point_dialog_root");

    // s210 m2 — close-request handler. Cancel semantics on the X:
    // clear the commit callback so a stale closure can't fire after
    // a subsequent show() supplies a new one. The return-false lets
    // the default close-action proceed (hide, not destroy).
    signal_close_request().connect(
        [this]() -> bool {
            LOG_DEBUG("RotateFromPointDialog: close-request — clearing "
                      "commit callback");
            m_on_committed = nullptr;
            return false;
        }, /*after=*/false);
}

// ── show ──────────────────────────────────────────────────────────────────
void RotateFromPointDialog::show(Gtk::Window& parent,
                                 double initial_pivot_x,
                                 double initial_pivot_y,
                                 const CanvasModel* unit_model,
                                 CommittedFn on_committed) {
    m_initial_pivot_x = initial_pivot_x;
    m_initial_pivot_y = initial_pivot_y;
    m_unit_model      = unit_model;
    m_on_committed    = std::move(on_committed);

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);

    if (!m_built) {
        m_built = true;
        build();
    }
    sync_from_state();

    present();

    // Initial focus goes to the X pivot spinner — matches the prior
    // inline form's `spin_x->grab_focus()`. Selecting the field on
    // open lets the user immediately type-replace if they want a
    // different pivot.
    if (m_spin_x) m_spin_x->grab_focus();
}

// ── build ─────────────────────────────────────────────────────────────────
//
// One-shot widget-tree construction. Called from the first show().
// Mirrors the pre-conversion inline form 1:1; only the lifetime story
// has changed (heap-allocated → singleton member).
void RotateFromPointDialog::build() {
    auto* vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(8);
    grid->set_column_spacing(12);
    grid->set_margin(16);
    grid->set_margin_bottom(8);

    auto make_lbl = [](const char* t) {
        auto* l = Gtk::make_managed<Gtk::Label>(t);
        l->set_halign(Gtk::Align::START);
        return l;
    };

    // Pivot X / Y — PositionX / PositionY types handle unit display
    // and the Y-up display flip internally; we hand them the doc-Y-
    // down internal value via with_value() / set_internal_value() and
    // the rendering takes care of itself. Ruler origin = 0 so pivot
    // coordinates display in raw canvas space, matching the pre-
    // conversion behaviour. The CanvasModel pointer is wired here at
    // build() (the first show()'s model) and re-bound on every
    // subsequent show() via set_model() in sync_from_state — so a
    // dialog opened in one document and re-opened in another picks
    // up the new doc's display_unit.
    // s295 m1 — unregistered substrate ctors. These spins are
    // unnamed (no `set_name` call at this site) so there's no abbrev
    // to thread through; current behaviour is "never script-
    // addressable" and we preserve that. Substrate semantics
    // (universal verbs, signal_internal_changed, parser) still apply;
    // adding script handles is a separate future opt-in.
    m_spin_x = Gtk::make_managed<CurvzSpinButton>(
            curvz::scripting::unregistered,
            SpinType::PositionX, m_unit_model, /*ruler_origin=*/0.0);
    m_spin_x->set_hexpand(true);

    m_spin_y = Gtk::make_managed<CurvzSpinButton>(
            curvz::scripting::unregistered,
            SpinType::PositionY, m_unit_model, /*ruler_origin=*/0.0);
    m_spin_y->set_hexpand(true);

    // Angle — degrees, no unit-label, always-zero-on-open.
    m_spin_a = Gtk::make_managed<CurvzSpinButton>(
            curvz::scripting::unregistered,
            SpinType::Angle, m_unit_model);
    m_spin_a->set_hexpand(true);

    grid->attach(*make_lbl("Pivot X"), 0, 0);
    grid->attach(*m_spin_x, 1, 0);
    if (auto* ul = m_spin_x->get_unit_label())
        grid->attach(*ul, 2, 0);
    grid->attach(*make_lbl("Pivot Y"), 0, 1);
    grid->attach(*m_spin_y, 1, 1);
    if (auto* ul = m_spin_y->get_unit_label())
        grid->attach(*ul, 2, 1);

    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4);
    sep->set_margin_bottom(4);
    grid->attach(*sep, 0, 2, 3, 1);

    grid->attach(*make_lbl("Rotate °"), 0, 3);
    grid->attach(*m_spin_a, 1, 3);

    // Button row — right-aligned Cancel / Apply with 8px gap, Apply
    // carries the GTK4 `suggested-action` class (blue highlight on
    // most motifs).
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    btn_row->set_halign(Gtk::Align::END);
    btn_row->set_margin_start(16);
    btn_row->set_margin_end(16);
    btn_row->set_margin_bottom(16);

    // s210 m2 — substrate-registered Cancel / Apply. Both abbrevs
    // (`dlg_rfp_cnc` / `dlg_rfp_app`) are useful test targets — same
    // reasoning as s210 m1 for `dlg_imginfo_close`. The substrate
    // wrapper itself doesn't take a long-name parameter; long-name
    // annotations stay as sibling comments for widget_names_sync.
    m_btn_cancel = Gtk::make_managed<curvz::widgets::Button>("dlg_rfp_cnc");
    // long-name: rotate_from_point_dialog_cancel_btn
    m_btn_cancel->set_label("Cancel");
    m_btn_cancel->signal_clicked().connect(
            sigc::mem_fun(*this, &RotateFromPointDialog::on_cancel));

    m_btn_apply = Gtk::make_managed<curvz::widgets::Button>("dlg_rfp_app");
    // long-name: rotate_from_point_dialog_apply_btn
    m_btn_apply->set_label("Apply");
    m_btn_apply->add_css_class("suggested-action");
    m_btn_apply->signal_clicked().connect(
            sigc::mem_fun(*this, &RotateFromPointDialog::on_apply));

    btn_row->append(*m_btn_cancel);
    btn_row->append(*m_btn_apply);

    vbox->append(*grid);
    vbox->append(*btn_row);
    set_child(*vbox);

    LOG_DEBUG("RotateFromPointDialog: built widget tree");
}

// ── sync_from_state ───────────────────────────────────────────────────────
//
// Re-bind the unit model on every show (so a dialog opened against
// doc A and then doc B picks up B's display_unit), then re-seed the
// three spinner values from m_initial_pivot_x/y and angle=0.
void RotateFromPointDialog::sync_from_state() {
    m_spin_x->set_model(m_unit_model);
    m_spin_y->set_model(m_unit_model);
    m_spin_a->set_model(m_unit_model);

    m_spin_x->set_internal_value(m_initial_pivot_x);
    m_spin_y->set_internal_value(m_initial_pivot_y);
    m_spin_a->set_internal_value(0.0);
}

// ── on_cancel ─────────────────────────────────────────────────────────────
void RotateFromPointDialog::on_cancel() {
    m_on_committed = nullptr;
    close();   // hide-on-close → hide
}

// ── on_apply ──────────────────────────────────────────────────────────────
//
// Read the three spinner values, fire the commit callback once, then
// close. The callback is moved out of m_on_committed before firing so
// the close-request handler can't double-clear or double-fire on a
// fast re-open.
void RotateFromPointDialog::on_apply() {
    // csb internal value = doc-Y-down for PositionY (the PositionY
    // CurvzSpinButton flips for display; internal coords match the
    // pre-conversion line `m_custom_pivot_y = spin_y->get_internal_value()`).
    const double pivot_x  = m_spin_x->get_internal_value();
    const double pivot_y  = m_spin_y->get_internal_value();
    const double angle    = m_spin_a->get_internal_value();

    CommittedFn cb = std::move(m_on_committed);
    m_on_committed = nullptr;

    close();   // hide-on-close → hide
    if (cb) cb(pivot_x, pivot_y, angle);
}

} // namespace Curvz
