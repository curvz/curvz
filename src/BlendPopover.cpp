#include "BlendPopover.hpp"
#include "CurvzDocument.hpp"  // CanvasModel
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <gtkmm/adjustment.h>

namespace Curvz {

BlendPopover::BlendPopover()
    : m_steps(Gtk::Adjustment::create(4, 1, 50, 1, 5), 0.0, 0)
{
    curvz::utils::set_name(*this, "pop_bld", "blend_popover_root");
    // s154 m3: Blend has no canvas-interactive component (unlike SnR's
    // pivot drag), so autohide stays ON — outside-click dismisses the
    // popover normally. Equalize is an in-popover button; clicking it
    // doesn't dismiss because the click is inside.
    set_position(Gtk::PositionType::RIGHT);

    m_outer.set_spacing(0);

    // ── Warning banner ───────────────────────────────────────────────────
    // Hidden when node counts match. Message is set per-show from a_count
    // vs b_count. The Equalize button is disabled in M3 — M4 implements
    // the De Casteljau midpoint insertion that makes it work.
    m_warn_row.set_spacing(8);
    m_warn_row.set_margin(12);
    m_warn_row.set_margin_bottom(0);
    m_warn_row.add_css_class("inspector-section");
    m_warn_lbl.set_xalign(0.0f);
    m_warn_lbl.set_hexpand(true);
    m_warn_lbl.set_wrap(true);
    m_warn_lbl.set_max_width_chars(38);
    curvz::utils::set_name(m_warn_lbl, "dlg_bld_warn", "blend_dialog_warn_lbl");
    m_warn_row.append(m_warn_lbl);
    m_btn_equalize.set_valign(Gtk::Align::CENTER);
    m_btn_equalize.set_tooltip_text("Insert nodes into the shorter path so "
                                    "both have the same count. The path "
                                    "shape is preserved (De Casteljau "
                                    "subdivision).");
    curvz::utils::set_name(m_btn_equalize, "dlg_bld_eq", "blend_dialog_equalize_btn");
    m_btn_equalize.signal_clicked().connect([this]() {
        if (!m_equalize_cb) return;
        int new_count = m_equalize_cb();
        if (new_count <= 0) return;  // equalize failed — leave banner as is
        // Update seed counts and hide the banner. OK becomes enabled.
        m_seed_a_count = new_count;
        m_seed_b_count = new_count;
        m_warn_row.set_visible(false);
        m_btn_ok.set_sensitive(true);
    });
    m_warn_row.append(m_btn_equalize);
    m_warn_row.set_visible(false);
    m_outer.append(m_warn_row);

    // ── Form grid ────────────────────────────────────────────────────────
    m_grid.set_row_spacing(8);
    m_grid.set_column_spacing(12);
    m_grid.set_margin(16);

    auto make_label = [](const char* text) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(text);
        lbl->set_xalign(1.0f);
        lbl->set_hexpand(false);
        return lbl;
    };

    int row = 0;

    // Steps
    m_grid.attach(*make_label("Steps:"), 0, row);
    m_steps.set_hexpand(true);
    m_steps.set_width_chars(6);
    m_steps.set_tooltip_text("Number of intermediate paths (1..50)");
    curvz::utils::set_name(m_steps, "dlg_bld_st", "blend_dialog_steps_spn");
    m_grid.attach(m_steps, 1, row);
    ++row;

    // Reverse
    curvz::utils::set_name(m_reverse, "dlg_bld_rv", "blend_dialog_reverse_check");
    m_grid.attach(m_reverse, 0, row, 2, 1);
    ++row;

    // Separator
    {
        auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(4);
        sep->set_margin_bottom(4);
        m_grid.attach(*sep, 0, row, 2, 1);
        ++row;
    }

    // Stroke-width override
    curvz::utils::set_name(m_stroke_override, "dlg_bld_so", "blend_dialog_stroke_override_check");
    m_grid.attach(m_stroke_override, 0, row, 2, 1);
    ++row;

    // Stroke-width start
    m_stroke_start_lbl.set_text("Start width:");
    m_stroke_start_lbl.set_xalign(1.0f);
    m_grid.attach(m_stroke_start_lbl, 0, row);
    m_stroke_start_row.set_spacing(4);
    m_stroke_start_row.set_hexpand(true);
    m_grid.attach(m_stroke_start_row, 1, row);
    ++row;

    // Stroke-width end
    m_stroke_end_lbl.set_text("End width:");
    m_stroke_end_lbl.set_xalign(1.0f);
    m_grid.attach(m_stroke_end_lbl, 0, row);
    m_stroke_end_row.set_spacing(4);
    m_stroke_end_row.set_hexpand(true);
    m_grid.attach(m_stroke_end_row, 1, row);
    ++row;

    m_outer.append(m_grid);

    // ── Button row ───────────────────────────────────────────────────────
    m_btn_row.set_spacing(8);
    m_btn_row.set_margin(12);
    m_btn_row.set_margin_top(0);
    m_btn_row.set_halign(Gtk::Align::END);
    m_btn_cancel.signal_clicked().connect([this]() { on_cancel(); });
    m_btn_ok.signal_clicked().connect([this]() { on_ok(); });
    m_btn_ok.add_css_class("suggested-action");
    curvz::utils::set_name(m_btn_cancel, "dlg_bld_cnc", "blend_dialog_cancel_btn");
    curvz::utils::set_name(m_btn_ok, "dlg_bld_ok", "blend_dialog_ok_btn");
    m_btn_row.append(m_btn_cancel);
    m_btn_row.append(m_btn_ok);
    m_outer.append(m_btn_row);

    set_child(m_outer);

    // Override toggle: enable/disable the start/end rows.
    m_stroke_override.signal_toggled().connect([this]() {
        if (m_loading) return;
        refresh_stroke_row_sensitive();
    });
}

// ── build_model_spins ────────────────────────────────────────────────────
// (Re)creates the stroke-width start/end spins against the current
// CanvasModel so unit labels reflect the active doc. Called per show().
void BlendPopover::build_model_spins(const CanvasModel* model) {
    auto clear_row = [](Gtk::Box& row) {
        while (auto* child = row.get_first_child()) row.remove(*child);
    };
    clear_row(m_stroke_start_row);
    clear_row(m_stroke_end_row);
    m_stroke_start = nullptr;
    m_stroke_end   = nullptr;

    // Seed: when override is off, use A's and B's current widths.
    // When override is on and we have remembered values, use those.
    double start_val = m_last_stroke_override ? m_last_stroke_start
                                               : m_seed_a_stroke_w;
    double end_val   = m_last_stroke_override ? m_last_stroke_end
                                               : m_seed_b_stroke_w;

    m_stroke_start = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, model);
    curvz::utils::set_name(m_stroke_start, "dlg_bld_sw", "blend_dialog_start_w_spn");
    m_stroke_start->with_value(start_val)
                   ->with_tooltip("Stroke width at A (first intermediate "
                                  "uses a value close to this)")
                   ->with_width_chars(8);
    m_stroke_start->set_hexpand(true);
    m_stroke_start->refresh_units();
    m_stroke_start_row.append(*m_stroke_start);
    if (auto* ul = m_stroke_start->get_unit_label())
        m_stroke_start_row.append(*ul);

    m_stroke_end = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, model);
    curvz::utils::set_name(m_stroke_end, "dlg_bld_ew", "blend_dialog_end_w_spn");
    m_stroke_end->with_value(end_val)
                 ->with_tooltip("Stroke width at B (last intermediate "
                                "uses a value close to this)")
                 ->with_width_chars(8);
    m_stroke_end->set_hexpand(true);
    m_stroke_end->refresh_units();
    m_stroke_end_row.append(*m_stroke_end);
    if (auto* ul = m_stroke_end->get_unit_label())
        m_stroke_end_row.append(*ul);
}

void BlendPopover::refresh_stroke_row_sensitive() {
    const bool on = m_stroke_override.get_active();
    m_stroke_start_lbl.set_sensitive(on);
    m_stroke_end_lbl.set_sensitive(on);
    m_stroke_start_row.set_sensitive(on);
    m_stroke_end_row.set_sensitive(on);
}

void BlendPopover::show(Gtk::Widget&       anchor,
                        const CanvasModel* model,
                        int                a_count,
                        int                b_count,
                        double             a_stroke_w,
                        double             b_stroke_w,
                        ApplyCb            apply_cb,
                        EqualizeCb         equalize_cb)
{
    m_apply_cb    = std::move(apply_cb);
    m_equalize_cb = std::move(equalize_cb);

    m_seed_a_count    = a_count;
    m_seed_b_count    = b_count;
    m_seed_a_stroke_w = a_stroke_w;
    m_seed_b_stroke_w = b_stroke_w;

    m_loading = true;

    // Warning banner visibility and message. Shown when counts differ.
    // Equalize button enabled iff caller provided a callback.
    if (a_count != b_count) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "A has %d nodes and B has %d. They must match to blend. "
                 "Click Equalize to add %d node(s) to the shorter path, "
                 "or cancel and adjust manually.",
                 a_count, b_count, std::abs(a_count - b_count));
        m_warn_lbl.set_text(buf);
        m_warn_row.set_visible(true);
        m_btn_equalize.set_sensitive(m_equalize_cb != nullptr);
        m_btn_ok.set_sensitive(false);
    } else {
        m_warn_row.set_visible(false);
        m_btn_ok.set_sensitive(true);
    }

    // Restore sticky form state.
    m_steps.set_value(m_last_steps);
    m_reverse.set_active(m_last_reverse);
    m_stroke_override.set_active(m_last_stroke_override);

    build_model_spins(model);
    refresh_stroke_row_sensitive();

    m_loading = false;

    // s154 m3: re-anchor if needed (idempotent for the common case where
    // it's always the same toolbar button). Mirrors StepRepeatPopover's
    // approach.
    if (get_parent() != &anchor) {
        if (get_parent()) unparent();
        set_parent(anchor);
    }
    popup();
}

// ── apply_with_last ───────────────────────────────────────────────────────────
// s154 m3: Toolbar Blend left-click path. Two outcomes:
//   - Counts match → fire apply_cb with last-values. No UI.
//   - Counts mismatch → fall through to show() with the same params, so
//     the user sees the warning banner and Equalize button. Their
//     interaction either resolves the mismatch (via Equalize → click
//     Blend) or cancels.
// Note that apply-with-last only uses sticky values (steps, reverse,
// stroke_w_override, and stroke_w_start/end if override was on). The
// stroke widths from the *current* selection (a_stroke_w / b_stroke_w)
// only matter for seeding the spins when override gets toggled on
// inside the popover — they're not referenced here in the UI-less
// apply path. Override-off blend interpolates A→B natively via Canvas.
void BlendPopover::apply_with_last(Gtk::Widget&       anchor,
                                   const CanvasModel* model,
                                   int                a_count,
                                   int                b_count,
                                   double             a_stroke_w,
                                   double             b_stroke_w,
                                   ApplyCb            apply_cb,
                                   EqualizeCb         equalize_cb)
{
    if (a_count != b_count) {
        // Validation failure — fall back to the popover so the user
        // sees the warning + Equalize. Same params; show() owns the
        // re-anchor + popup.
        show(anchor, model, a_count, b_count, a_stroke_w, b_stroke_w,
             std::move(apply_cb), std::move(equalize_cb));
        return;
    }

    Result r;
    r.steps             = m_last_steps;
    r.reverse           = m_last_reverse;
    r.stroke_w_override = m_last_stroke_override;
    r.stroke_w_start    = m_last_stroke_start;
    r.stroke_w_end      = m_last_stroke_end;

    if (apply_cb) apply_cb(r);
}

void BlendPopover::on_ok() {
    Result r;
    r.steps             = std::clamp((int)m_steps.get_value(), 1, 50);
    r.reverse           = m_reverse.get_active();
    r.stroke_w_override = m_stroke_override.get_active();
    r.stroke_w_start    = m_stroke_start ? m_stroke_start->get_internal_value()
                                          : m_last_stroke_start;
    r.stroke_w_end      = m_stroke_end   ? m_stroke_end->get_internal_value()
                                          : m_last_stroke_end;

    // Remember for next invocation.
    m_last_steps           = r.steps;
    m_last_reverse         = r.reverse;
    m_last_stroke_override = r.stroke_w_override;
    if (r.stroke_w_override) {
        m_last_stroke_start = r.stroke_w_start;
        m_last_stroke_end   = r.stroke_w_end;
    }

    popdown();
    if (m_apply_cb) m_apply_cb(r);
}

void BlendPopover::on_cancel() {
    popdown();
}

} // namespace Curvz
