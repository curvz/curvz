#include "StepRepeatPopover.hpp"
#include "CurvzDocument.hpp"    // CanvasModel
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"      // s117 m18 v2
#include <cmath>
#include <gdk/gdkkeysyms.h>
#include <glibmm/ustring.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/stringlist.h>

namespace Curvz {

// ── ctor: layout built once, model-dependent spins populated per-show ────────
StepRepeatPopover::StepRepeatPopover()
    : m_copies("pop_sr_cp", SpinType::Integer),
      m_rotate_enable("Rotate around pivot")
{
    curvz::utils::set_name(*this, "pop_sr", "step_repeat_popover_root");
    // s154 m2: autohide OFF so canvas clicks (used to drag the SnR
    // pivot crosshair) don't dismiss the popover. Cancel/Repeat
    // buttons are the only dismissal surface — same shape as the
    // dialog era (which was a non-modal Window).
    set_autohide(false);
    set_position(Gtk::PositionType::RIGHT);

    m_outer.set_spacing(0);

    // ── Form grid ─────────────────────────────────────────────────────────
    m_grid.set_row_spacing(8);
    m_grid.set_column_spacing(12);
    m_grid.set_margin(16);
    m_grid.set_margin_bottom(8);

    auto make_label = [](const char* text) {
        auto* lbl = Gtk::make_managed<Gtk::Label>(text);
        lbl->set_xalign(1.0f);
        lbl->set_hexpand(false);
        return lbl;
    };

    int row = 0;

    // Copies
    m_grid.attach(*make_label("Copies:"), 0, row);
    m_copies.set_hexpand(true);
    m_copies.set_width_chars(6);
    curvz::utils::set_name(m_copies, "pop_sr_cp", "step_repeat_popover_copies_spn");
    m_grid.attach(m_copies, 1, row);
    ++row;

    // --- translate section separator ---
    {
        auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(4);
        sep->set_margin_bottom(4);
        m_grid.attach(*sep, 0, row, 2, 1);
        ++row;
    }

    // Offset X
    m_grid.attach(*make_label("Offset X:"), 0, row);
    m_off_x_row.set_spacing(4);
    m_off_x_row.set_hexpand(true);
    m_grid.attach(m_off_x_row, 1, row);
    ++row;

    // Offset Y
    m_grid.attach(*make_label("Offset Y:"), 0, row);
    m_off_y_row.set_spacing(4);
    m_off_y_row.set_hexpand(true);
    m_grid.attach(m_off_y_row, 1, row);
    ++row;

    // --- rotate section separator ---
    {
        auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(4);
        sep->set_margin_bottom(4);
        m_grid.attach(*sep, 0, row, 2, 1);
        ++row;
    }

    // Rotate enable + mode dropdown on the same row
    m_rotate_enable.set_active(false);
    curvz::utils::set_name(m_rotate_enable, "pop_sr_re", "step_repeat_popover_rotate_enable_check");
    m_grid.attach(m_rotate_enable, 0, row, 1, 1);

    {
        auto items = Gtk::StringList::create({"Auto (360/copies)", "Fixed"});
        m_angle_mode.set_model(items);
        m_angle_mode.set_selected(0);
        m_angle_mode.set_hexpand(true);
        curvz::utils::set_name(m_angle_mode, "pop_sr_am", "step_repeat_popover_angle_mode_dd");
        m_grid.attach(m_angle_mode, 1, row);
    }
    ++row;

    // Angle row
    m_grid.attach(*make_label("Angle:"), 0, row);
    m_angle_row.set_spacing(4);
    m_angle_row.set_hexpand(true);
    m_grid.attach(m_angle_row, 1, row);
    ++row;

    // Pivot X
    m_grid.attach(*make_label("Pivot X:"), 0, row);
    m_pivot_x_row.set_spacing(4);
    m_pivot_x_row.set_hexpand(true);
    m_grid.attach(m_pivot_x_row, 1, row);
    ++row;

    // Pivot Y
    m_grid.attach(*make_label("Pivot Y:"), 0, row);
    m_pivot_y_row.set_spacing(4);
    m_pivot_y_row.set_hexpand(true);
    m_grid.attach(m_pivot_y_row, 1, row);
    ++row;

    // Preview (labelless row, centered)
    m_preview.set_content_width(140);
    m_preview.set_content_height(100);
    m_preview.set_halign(Gtk::Align::CENTER);
    m_preview.set_margin_top(4);
    curvz::utils::set_name(m_preview, "pop_sr_pv", "step_repeat_popover_preview_da");
    m_preview.set_draw_func(sigc::mem_fun(*this,
        &StepRepeatPopover::on_preview_draw));

    // Click = jump pivot to that point; drag = continuous.
    auto gclick = Gtk::GestureClick::create();
    gclick->set_button(1);
    gclick->signal_pressed().connect(
        sigc::mem_fun(*this, &StepRepeatPopover::on_preview_press));
    m_preview.add_controller(gclick);

    auto gdrag = Gtk::GestureDrag::create();
    gdrag->set_button(1);
    gdrag->signal_drag_update().connect(
        sigc::mem_fun(*this, &StepRepeatPopover::on_preview_drag_update));
    gdrag->signal_drag_end().connect(
        [this](double, double) { m_preview_dragging = false; });
    m_preview.add_controller(gdrag);

    m_grid.attach(m_preview, 0, row, 2, 1);
    ++row;

    m_outer.append(m_grid);

    // ── Separator + button bar ────────────────────────────────────────────
    auto* sep_btn = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    m_outer.append(*sep_btn);

    auto* btn_bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_bar->set_spacing(8);
    btn_bar->set_margin(12);
    btn_bar->set_halign(Gtk::Align::END);

    m_btn_cancel.set_has_frame(true);
    curvz::utils::set_name(m_btn_cancel, "pop_sr_cnc", "step_repeat_popover_cancel_btn");
    m_btn_cancel.signal_clicked().connect(
        sigc::mem_fun(*this, &StepRepeatPopover::on_cancel));
    btn_bar->append(m_btn_cancel);

    m_btn_ok.set_has_frame(true);
    m_btn_ok.add_css_class("suggested-action");
    curvz::utils::set_name(m_btn_ok, "pop_sr_ok", "step_repeat_popover_ok_btn");
    m_btn_ok.signal_clicked().connect(
        sigc::mem_fun(*this, &StepRepeatPopover::on_ok));
    btn_bar->append(m_btn_ok);

    m_outer.append(*btn_bar);
    set_child(m_outer);

    // s154 m2: popdown() (called from on_ok/on_cancel, or programmatic
    // close) fires signal_closed. Hook it to hide the canvas crosshair.
    // signal_hide existed on the dialog-era Window; popovers expose
    // signal_closed as the equivalent dismissal hook.
    signal_closed().connect([this]() {
        if (m_pivot_cb) {
            m_pivot_cb(m_pivot_dx, m_pivot_dy, false);
        }
    });

    // Enter triggers OK; Esc cancels.
    auto key = Gtk::EventControllerKey::create();
    key->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
                on_ok();
                return true;
            }
            if (keyval == GDK_KEY_Escape) {
                on_cancel();
                return true;
            }
            return false;
        }, true);
    add_controller(key);

    // Rotate-enable wiring — toggles sensitivity of rotate widgets AND
    // dims the offset spins (dx/dy are unused in orbit mode).  Also
    // (dis)appears the canvas crosshair because refpt is meaningless when
    // rotate is off.
    m_rotate_enable.signal_toggled().connect([this]() {
        const bool on = m_rotate_enable.get_active();
        m_angle_mode.set_sensitive(on);
        if (m_angle_spin) m_angle_spin->set_sensitive(on);
        if (m_pivot_x)    m_pivot_x->set_sensitive(on);
        if (m_pivot_y)    m_pivot_y->set_sensitive(on);
        m_preview.set_sensitive(on);
        // Offsets are meaningless when rotating (UI spec: dimmed when on).
        const bool off_on = !on;
        if (m_offset_x) m_offset_x->set_sensitive(off_on);
        if (m_offset_y) m_offset_y->set_sensitive(off_on);
        m_off_x_row.set_sensitive(off_on);   // dims unit label too
        m_off_y_row.set_sensitive(off_on);
        m_preview.queue_draw();
        if (on) refresh_angle_ui();
        // Sync canvas crosshair visibility with rotate state.
        emit_pivot_change(true);
    });

    m_angle_mode.property_selected().signal_changed().connect([this]() {
        if (m_loading) return;
        refresh_angle_ui();
    });

    m_copies.signal_value_changed().connect([this]() {
        if (m_loading) return;
        // Auto mode follows copies count live.
        if (m_angle_mode.get_selected() == 0) refresh_angle_ui();
    });
}

// ── build_model_spins ─────────────────────────────────────────────────────────
// Offset X/Y, Angle, Pivot X/Y — all (re)created per show() so CanvasModel
// changes (doc switches) don't leave stale pointers.
//
// s295 m1 — CurvzSpinButton is now a Scriptable substrate; the per-row
// teardown needs force_unregister_subtree to synchronously clear the
// previous spins' registry entries before GTK's idle-priority
// destruction runs. Without this, the second show() would collide on
// the registered abbrevs (`pop_sr_ox` / `pop_sr_oy` / `pop_sr_ang` /
// `pop_sr_px` / `pop_sr_py`). Same s199 m1 idiom as
// PropertiesPanel::do_clear and TranslateDialog::rebuild_picker.
void StepRepeatPopover::build_model_spins(const CanvasModel* model) {
    // Tear down previous
    auto clear_row = [](Gtk::Box& row) {
        curvz::utils::force_unregister_subtree(&row);
        while (auto* child = row.get_first_child()) row.remove(*child);
    };
    clear_row(m_off_x_row);
    clear_row(m_off_y_row);
    clear_row(m_angle_row);
    clear_row(m_pivot_x_row);
    clear_row(m_pivot_y_row);
    m_offset_x = nullptr;
    m_offset_y = nullptr;
    m_angle_spin = nullptr;
    m_pivot_x = nullptr;
    m_pivot_y = nullptr;

    // Offset X/Y — Distance (signed, doc units)
    m_offset_x = Gtk::make_managed<CurvzSpinButton>(
        "pop_sr_ox", SpinType::Distance, model);
    curvz::utils::set_name(m_offset_x, "pop_sr_ox", "step_repeat_popover_offset_x_spn");
    m_offset_x->with_value(m_last_dx)
               ->with_tooltip("Horizontal offset between copies")
               ->with_width_chars(8);
    m_offset_x->set_hexpand(true);
    m_offset_x->refresh_units();
    m_off_x_row.append(*m_offset_x);
    if (auto* ul = m_offset_x->get_unit_label()) m_off_x_row.append(*ul);

    m_offset_y = Gtk::make_managed<CurvzSpinButton>(
        "pop_sr_oy", SpinType::Distance, model);
    curvz::utils::set_name(m_offset_y, "pop_sr_oy", "step_repeat_popover_offset_y_spn");
    m_offset_y->with_value(m_last_dy)
               ->with_tooltip("Vertical offset between copies")
               ->with_width_chars(8);
    m_offset_y->set_hexpand(true);
    m_offset_y->refresh_units();
    m_off_y_row.append(*m_offset_y);
    if (auto* ul = m_offset_y->get_unit_label()) m_off_y_row.append(*ul);

    // Angle — degrees
    m_angle_spin = Gtk::make_managed<CurvzSpinButton>(
        "pop_sr_ang", SpinType::Angle, model);
    curvz::utils::set_name(m_angle_spin, "pop_sr_ang", "step_repeat_popover_angle_spn");
    m_angle_spin->with_value(m_last_angle)
                ->with_tooltip("Rotation applied per copy")
                ->with_width_chars(8);
    m_angle_spin->set_hexpand(true);
    m_angle_spin->refresh_units();
    m_angle_row.append(*m_angle_spin);
    if (auto* ul = m_angle_spin->get_unit_label()) m_angle_row.append(*ul);

    m_angle_spin->on_changed([this](double v) {
        if (m_loading) return;
        // Only remember when in Fixed mode — Auto is derived, not entered.
        if (m_angle_mode.get_selected() == 1) m_last_angle = v;
    });

    // Pivot X/Y — PositionX / PositionY (ruler_origin 0 so pivot is raw doc
    // coords, matching Canvas::on_pivot_dialog).
    m_pivot_x = Gtk::make_managed<CurvzSpinButton>(
        "pop_sr_px", SpinType::PositionX, model, 0.0);
    curvz::utils::set_name(m_pivot_x, "pop_sr_px", "step_repeat_popover_pivot_x_spn");
    m_pivot_x->with_value(m_pivot_dx)
             ->with_tooltip("Pivot X (doc coords)")
             ->with_width_chars(8);
    m_pivot_x->set_hexpand(true);
    m_pivot_x->refresh_units();
    m_pivot_x_row.append(*m_pivot_x);
    if (auto* ul = m_pivot_x->get_unit_label()) m_pivot_x_row.append(*ul);

    m_pivot_y = Gtk::make_managed<CurvzSpinButton>(
        "pop_sr_py", SpinType::PositionY, model, 0.0);
    curvz::utils::set_name(m_pivot_y, "pop_sr_py", "step_repeat_popover_pivot_y_spn");
    m_pivot_y->with_value(m_pivot_dy)
             ->with_tooltip("Pivot Y (doc coords)")
             ->with_width_chars(8);
    m_pivot_y->set_hexpand(true);
    m_pivot_y->refresh_units();
    m_pivot_y_row.append(*m_pivot_y);
    if (auto* ul = m_pivot_y->get_unit_label()) m_pivot_y_row.append(*ul);

    m_pivot_x->on_changed([this](double v) {
        if (m_loading) return;
        m_pivot_dx = v;
        m_preview.queue_draw();
        emit_pivot_change(true);
    });
    m_pivot_y->on_changed([this](double v) {
        if (m_loading) return;
        m_pivot_dy = v;  // csb PositionY stores doc-Y-down internally
        m_preview.queue_draw();
        emit_pivot_change(true);
    });
}

// ── show ──────────────────────────────────────────────────────────────────────
void StepRepeatPopover::show(Gtk::Widget&       anchor,
                             const CanvasModel* model,
                             double             bbox_cx,
                             double             bbox_cy,
                             double             bbox_w,
                             double             bbox_h,
                             PivotChangeCb      pivot_cb,
                             ApplyCb            apply_cb)
{
    m_pivot_cb = std::move(pivot_cb);
    m_apply_cb = std::move(apply_cb);

    m_bbox_cx = bbox_cx;
    m_bbox_cy = bbox_cy;
    m_bbox_w  = (bbox_w  > 1e-6) ? bbox_w  : 100.0;
    m_bbox_h  = (bbox_h  > 1e-6) ? bbox_h  : 100.0;

    // Seed pivot = bbox center (this is always fresh — sticky pivot doesn't
    // really make sense when selection changes between invocations).
    m_pivot_dx = m_bbox_cx;
    m_pivot_dy = m_bbox_cy;

    m_loading = true;
    build_model_spins(model);

    // Restore rotation prefs
    m_rotate_enable.set_active(m_last_rotate);
    m_angle_mode.set_selected(m_last_mode);

    const bool on = m_last_rotate;
    m_angle_mode.set_sensitive(on);
    if (m_angle_spin) m_angle_spin->set_sensitive(on);
    if (m_pivot_x)    m_pivot_x->set_sensitive(on);
    if (m_pivot_y)    m_pivot_y->set_sensitive(on);
    m_preview.set_sensitive(on);
    // Offsets dim when rotate is active.
    const bool off_on = !on;
    if (m_offset_x) m_offset_x->set_sensitive(off_on);
    if (m_offset_y) m_offset_y->set_sensitive(off_on);
    m_off_x_row.set_sensitive(off_on);
    m_off_y_row.set_sensitive(off_on);

    refresh_angle_ui();
    m_loading = false;

    m_preview.queue_draw();

    // s154 m2: re-anchor if needed. Toolbar always passes the same
    // SnR button, but if some other entry point invokes show() with
    // a different anchor we accept it. unparent() before set_parent()
    // is the GTK4-idiomatic way to repoint.
    if (get_parent() != &anchor) {
        if (get_parent()) unparent();
        set_parent(anchor);
    }
    popup();

    // Emit initial pivot so canvas crosshair shows immediately.
    emit_pivot_change(true);
}

// ── apply_with_last ───────────────────────────────────────────────────────────
// s154 m2: toolbar left-click path. No popup, no spin rebuild, no preview,
// no pivot_change callbacks fired. Just construct a Result from the
// stored last values and forward it to apply_cb. Pivot defaults to the
// supplied bbox centre (matching show()'s "sticky pivot doesn't make
// sense across selections" reasoning); rotate / offset / angle reuse
// whatever the last popover invocation committed (or the hardcoded
// constructor defaults for a session that has never opened the popover).
void StepRepeatPopover::apply_with_last(const CanvasModel* /*model*/,
                                        double             bbox_cx,
                                        double             bbox_cy,
                                        ApplyCb            apply_cb)
{
    Result r;
    r.copies         = (int)m_copies.get_value();
    r.dx             = m_last_dx;
    r.dy             = m_last_dy;
    r.rotate_enabled = m_last_rotate;
    r.pivot_x        = bbox_cx;
    r.pivot_y        = bbox_cy;

    if (r.rotate_enabled) {
        if (m_last_mode == 0) {
            // Auto: derived from copies. +1 accounts for the original.
            int c = std::max(1, r.copies);
            r.angle_deg = 360.0 / (double)(c + 1);
        } else {
            r.angle_deg = m_last_angle;
        }
    } else {
        r.angle_deg = 0.0;
    }

    if (apply_cb) apply_cb(r);
}

// ── refresh_angle_ui ──────────────────────────────────────────────────────────
// Auto mode: compute 360/copies, disable angle spin, set its display.
// Fixed mode: enable angle spin, restore m_last_angle.
void StepRepeatPopover::refresh_angle_ui() {
    if (!m_angle_spin) return;

    const bool enabled = m_rotate_enable.get_active();
    if (!enabled) {
        m_angle_spin->set_sensitive(false);
        return;
    }

    const int mode = m_angle_mode.get_selected();
    const bool save = m_loading;
    m_loading = true;

    if (mode == 0) {  // Auto
        // +1 accounts for the original item (position 0 in the ring).
        // copies=3 → 4 total positions → 90° per step.
        int copies = std::max(1, (int)m_copies.get_value());
        double a = 360.0 / (double)(copies + 1);
        m_angle_spin->with_value(a);
        m_angle_spin->set_sensitive(false);
    } else {          // Fixed
        m_angle_spin->with_value(m_last_angle);
        m_angle_spin->set_sensitive(true);
    }

    m_loading = save;
}

// ── emit_pivot_change ─────────────────────────────────────────────────────────
// 'visible' parameter is the CALLER'S intent (e.g. show/hide on dialog
// open/close).  Even when the caller asks for visible=true, we only report
// visible to the canvas if rotate is enabled — refpt is meaningless when
// rotate is off, so no crosshair should render.
void StepRepeatPopover::emit_pivot_change(bool visible) {
    const bool effective = visible && m_rotate_enable.get_active();
    if (m_pivot_cb) m_pivot_cb(m_pivot_dx, m_pivot_dy, effective);
}

// ── set_pivot_from_canvas ─────────────────────────────────────────────────────
// Called by MainWindow when the user drags the crosshair on canvas.  Pushes
// the new pivot into dialog state + spins + mini preview dot.  m_loading
// guard prevents the spin on_changed callbacks from re-emitting
// pivot_change_cb back to canvas (which is already where this data came
// from — avoids feedback loop).
void StepRepeatPopover::set_pivot_from_canvas(double px, double py) {
    m_pivot_dx = px;
    m_pivot_dy = py;

    m_loading = true;
    if (m_pivot_x) m_pivot_x->with_value(m_pivot_dx);
    if (m_pivot_y) m_pivot_y->with_value(m_pivot_dy);
    m_loading = false;

    m_preview.queue_draw();
}

// ── Mini preview rendering ────────────────────────────────────────────────────
// Layout: 140x100 drawing area.
//   - 10px margin
//   - Draws the selection bbox scaled to fit with aspect preserved.
//   - Draws pivot as orange crosshair + dot.
//   - Doc coord space is Y-DOWN (matches node storage); preview pixel Y is
//     also down so the transform is a straight scale+offset without flip.
//
// preview_frame = the scaled bbox drawn inside the preview area.
//
// Scale factor computed to fit bbox into (w - 2*margin, h - 2*margin), but we
// also clamp so pivot can be placed outside the bbox and still visible within
// the preview. Approach: use 60% of available space for the bbox so there's
// room around it for the pivot.

void StepRepeatPopover::on_preview_draw(
    const Cairo::RefPtr<Cairo::Context>& cr,
    int width, int height)
{
    // Background
    cr->set_source_rgba(0.12, 0.12, 0.12, 1.0);
    cr->rectangle(0, 0, width, height);
    cr->fill();

    // Border
    cr->set_source_rgba(0.35, 0.35, 0.35, 1.0);
    cr->set_line_width(1.0);
    cr->rectangle(0.5, 0.5, width - 1.0, height - 1.0);
    cr->stroke();

    // Compute bbox placement within preview.
    // Scale chosen so bbox fills 60% of min(w-20, h-20).
    const double margin = 10.0;
    const double avail_w = std::max(1.0, width  - 2 * margin);
    const double avail_h = std::max(1.0, height - 2 * margin);
    const double target  = 0.60 * std::min(avail_w, avail_h);
    const double sx = target / m_bbox_w;
    const double sy = target / m_bbox_h;
    const double s  = std::min(sx, sy);
    const double fw = m_bbox_w * s;
    const double fh = m_bbox_h * s;
    const double fx = (width  - fw) * 0.5;   // preview pixel of bbox top-left
    const double fy = (height - fh) * 0.5;

    // Selection bbox rectangle
    const bool enabled = m_rotate_enable.get_active();
    const double alpha = enabled ? 1.0 : 0.35;
    cr->set_source_rgba(0.55, 0.75, 1.0, alpha);
    cr->set_line_width(1.0);
    cr->rectangle(fx + 0.5, fy + 0.5, fw, fh);
    cr->stroke();

    // Bbox center cross (faint, as reference)
    cr->set_source_rgba(0.55, 0.75, 1.0, alpha * 0.45);
    const double cx = fx + fw * 0.5;
    const double cy = fy + fh * 0.5;
    cr->move_to(cx - 3, cy); cr->line_to(cx + 3, cy); cr->stroke();
    cr->move_to(cx, cy - 3); cr->line_to(cx, cy + 3); cr->stroke();

    // Pivot position in preview pixels
    double px = 0.0, py = 0.0;
    doc_to_preview_px(m_pivot_dx, m_pivot_dy, px, py);

    // Clamp into preview bounds for drawing (still show even if dragged off).
    px = std::max(2.0, std::min((double)width  - 2.0, px));
    py = std::max(2.0, std::min((double)height - 2.0, py));

    // Orange crosshair + dot (same colour family as canvas pivot)
    const double arm = 7.0;
    // Shadow
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.6);
    cr->set_line_width(3.0);
    cr->move_to(px - arm, py); cr->line_to(px + arm, py); cr->stroke();
    cr->move_to(px, py - arm); cr->line_to(px, py + arm); cr->stroke();
    // Foreground
    cr->set_source_rgba(1.0, 0.55, 0.0, alpha);
    cr->set_line_width(1.5);
    cr->move_to(px - arm, py); cr->line_to(px + arm, py); cr->stroke();
    cr->move_to(px, py - arm); cr->line_to(px, py + arm); cr->stroke();
    cr->arc(px, py, 2.5, 0, 2 * M_PI);
    cr->fill();
}

// ── preview coord transforms ──────────────────────────────────────────────────
// The transform matches on_preview_draw exactly — keep these in lockstep.
void StepRepeatPopover::doc_to_preview_px(
    double dx, double dy, double& px, double& py) const
{
    int width  = m_preview.get_width();
    int height = m_preview.get_height();
    if (width <= 0) width = 140;
    if (height <= 0) height = 100;

    const double margin = 10.0;
    const double avail_w = std::max(1.0, width  - 2 * margin);
    const double avail_h = std::max(1.0, height - 2 * margin);
    const double target  = 0.60 * std::min(avail_w, avail_h);
    const double s = std::min(target / m_bbox_w, target / m_bbox_h);
    const double fw = m_bbox_w * s;
    const double fh = m_bbox_h * s;
    const double fx = (width  - fw) * 0.5;
    const double fy = (height - fh) * 0.5;
    const double cx = fx + fw * 0.5;  // preview px of bbox center
    const double cy = fy + fh * 0.5;

    // Doc offset from bbox center → pixel offset from preview bbox center.
    px = cx + (dx - m_bbox_cx) * s;
    py = cy + (dy - m_bbox_cy) * s;   // doc Y-down + pixel Y-down = no flip
}

void StepRepeatPopover::preview_px_to_doc(
    double px, double py, double& dx, double& dy) const
{
    int width  = m_preview.get_width();
    int height = m_preview.get_height();
    if (width <= 0) width = 140;
    if (height <= 0) height = 100;

    const double margin = 10.0;
    const double avail_w = std::max(1.0, width  - 2 * margin);
    const double avail_h = std::max(1.0, height - 2 * margin);
    const double target  = 0.60 * std::min(avail_w, avail_h);
    const double s = std::min(target / m_bbox_w, target / m_bbox_h);
    const double fw = m_bbox_w * s;
    const double fh = m_bbox_h * s;
    const double fx = (width  - fw) * 0.5;
    const double fy = (height - fh) * 0.5;
    const double cx = fx + fw * 0.5;
    const double cy = fy + fh * 0.5;

    if (s <= 1e-9) { dx = m_bbox_cx; dy = m_bbox_cy; return; }

    dx = m_bbox_cx + (px - cx) / s;
    dy = m_bbox_cy + (py - cy) / s;
}

// ── Preview interaction ───────────────────────────────────────────────────────
void StepRepeatPopover::on_preview_press(int, double x, double y) {
    if (!m_rotate_enable.get_active()) return;

    // Snap pivot to clicked point.
    double dx, dy;
    preview_px_to_doc(x, y, dx, dy);
    m_pivot_dx = dx;
    m_pivot_dy = dy;

    // Keep spins in sync (without re-emitting).
    m_loading = true;
    if (m_pivot_x) m_pivot_x->with_value(m_pivot_dx);
    if (m_pivot_y) m_pivot_y->with_value(m_pivot_dy);
    m_loading = false;

    m_preview_dragging = true;
    m_drag_anchor_pdx = m_pivot_dx;
    m_drag_anchor_pdy = m_pivot_dy;
    m_drag_anchor_px = x;
    m_drag_anchor_py = y;

    m_preview.queue_draw();
    emit_pivot_change(true);
}

void StepRepeatPopover::on_preview_drag_update(
    double offset_x, double offset_y)
{
    if (!m_rotate_enable.get_active()) return;

    // GestureDrag gives offset from press point in widget pixels. Convert the
    // offset to doc space using the current scale.
    int width  = m_preview.get_width();
    int height = m_preview.get_height();
    if (width <= 0) width = 140;
    if (height <= 0) height = 100;

    const double margin = 10.0;
    const double avail_w = std::max(1.0, width  - 2 * margin);
    const double avail_h = std::max(1.0, height - 2 * margin);
    const double target  = 0.60 * std::min(avail_w, avail_h);
    const double s = std::min(target / m_bbox_w, target / m_bbox_h);
    if (s <= 1e-9) return;

    m_pivot_dx = m_drag_anchor_pdx + offset_x / s;
    m_pivot_dy = m_drag_anchor_pdy + offset_y / s;

    m_loading = true;
    if (m_pivot_x) m_pivot_x->with_value(m_pivot_dx);
    if (m_pivot_y) m_pivot_y->with_value(m_pivot_dy);
    m_loading = false;

    m_preview.queue_draw();
    emit_pivot_change(true);
}

// ── OK / Cancel ───────────────────────────────────────────────────────────────
void StepRepeatPopover::on_ok() {
    Result r;
    r.copies  = (int)m_copies.get_value();
    r.dx      = m_offset_x ? m_offset_x->get_internal_value() : m_last_dx;
    r.dy      = m_offset_y ? m_offset_y->get_internal_value() : m_last_dy;
    r.rotate_enabled = m_rotate_enable.get_active();
    r.pivot_x = m_pivot_dx;
    r.pivot_y = m_pivot_dy;

    if (r.rotate_enabled) {
        if (m_angle_mode.get_selected() == 0) {
            // +1 accounts for the original (position 0). See refresh_angle_ui.
            int c = std::max(1, r.copies);
            r.angle_deg = 360.0 / (double)(c + 1);
        } else {
            r.angle_deg = m_angle_spin ? m_angle_spin->get_internal_value()
                                       : m_last_angle;
        }
    } else {
        r.angle_deg = 0.0;
    }

    // Remember for next invocation.
    m_last_dx     = r.dx;
    m_last_dy     = r.dy;
    m_last_rotate = r.rotate_enabled;
    m_last_mode   = (int)m_angle_mode.get_selected();
    if (m_last_mode == 1) m_last_angle = r.angle_deg;

    // popdown() will emit signal_closed → crosshair off (via the hook
    // wired in the ctor).
    popdown();
    if (m_apply_cb) m_apply_cb(r);
}

void StepRepeatPopover::on_cancel() {
    popdown();
}

} // namespace Curvz
