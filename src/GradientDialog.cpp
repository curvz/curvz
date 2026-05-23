#include "GradientDialog.hpp"
#include "CurvzLog.hpp"
#include "color/Color.hpp"
#include "curvz_utils.hpp" // s117 m18 v2

#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/popovermenu.h>

#include <algorithm>
#include <cmath>

namespace Curvz {

// ── Helpers ──────────────────────────────────────────────────────────────────

// Stop-square geometry constants. Centralised so draw_track and hit_stop_square
// agree pixel-perfectly. SQUARE_W is the width of the swatch handle; GUTTER is
// the inset on each side of the track so end-stops aren't clipped at the edge.
static constexpr int GUTTER = 10;
static constexpr int SQUARE_W = 14;
static constexpr int SQUARE_H = 22;
// Track band is split into two horizontal strips:
//   [0 .. RAMP_BOT]                    — gradient ramp preview
//   [RAMP_BOT + RAMP_GAP .. height-2]  — stop squares row
static constexpr int RAMP_BOT = 30;
static constexpr int RAMP_GAP = 6;

// Angle convention for linear gradients: degrees clockwise from East
// (matches SVG / Affinity / Illustrator). The endpoints span the bbox
// centred at (0.5, 0.5) along the unit vector at angle θ:
//   x1 = 0.5 - 0.5·cos(θ)   y1 = 0.5 - 0.5·sin(θ)
//   x2 = 0.5 + 0.5·cos(θ)   y2 = 0.5 + 0.5·sin(θ)
// So 0° = horizontal L→R, 90° = top→bottom, 180° = R→L, 270° = bottom→top.
//
// Inverse: given any pair of endpoints, project the vector (x2-x1, y2-y1)
// to an angle. The endpoint distances aren't constrained — this just reads
// the direction. Returned angle is in [0, 360).
static double angle_from_endpoints(double x1, double y1, double x2, double y2) {
  double dx = x2 - x1;
  double dy = y2 - y1;
  if (dx == 0.0 && dy == 0.0)
    return 0.0;
  double rad = std::atan2(dy, dx);
  double deg = rad * (180.0 / M_PI);
  if (deg < 0.0)
    deg += 360.0;
  return deg;
}

static void endpoints_from_angle(double angle_deg, double &x1, double &y1,
                                 double &x2, double &y2) {
  double rad = angle_deg * (M_PI / 180.0);
  double cx = 0.5, cy = 0.5;
  double dx = 0.5 * std::cos(rad);
  double dy = 0.5 * std::sin(rad);
  x1 = cx - dx;
  y1 = cy - dy;
  x2 = cx + dx;
  y2 = cy + dy;
}

// ── ctor ─────────────────────────────────────────────────────────────────────

// s295 m1 — all eight spins are unregistered substrates. GradientDialog
// is held both as a MainWindow member (m_gradient_dialog) and as a
// StyleEditorDialog member (m_gradient_dialog) — two instances coexist
// concurrently. Registering with hardcoded abbrevs (dlg_gr_pos, dlg_gr_op,
// dlg_gr_ang, dlg_gr_r) collides on the second instance's construction;
// the registry enforces process-wide uniqueness, set_name does not.
// Substrate semantics (signal_internal_changed, value/min/max, rich
// parser) still apply; the spins just aren't addressable by script
// abbrev. The set_name calls below stay in place for widget_names.db
// harvest and GTK inspector debugging.
GradientDialog::GradientDialog()
    : m_pos_spin(curvz::scripting::unregistered, SpinType::Percentage),
      m_opacity_spin(curvz::scripting::unregistered, SpinType::Percentage),
      m_angle_spin(curvz::scripting::unregistered, SpinType::Angle),
      m_x1_spin(curvz::scripting::unregistered, SpinType::PositionX),
      m_y1_spin(curvz::scripting::unregistered, SpinType::PositionY),
      m_x2_spin(curvz::scripting::unregistered, SpinType::PositionX),
      m_y2_spin(curvz::scripting::unregistered, SpinType::PositionY),
      m_r_spin(curvz::scripting::unregistered, SpinType::Distance) {
  curvz::utils::set_name(*this, "dlg_gr", "gradient_dialog_root");
  set_title("Gradient editor");
  set_modal(true);
  set_resizable(false);
  set_default_size(420, -1);

  m_root.set_margin(12);
  m_root.set_spacing(8);

  build_type_row();
  build_stop_track();
  build_stop_props();
  build_geom_section();
  build_action_row();

  // ── S92 m1 — Right-click context menu actions ─────────────────────────
  //
  // Action group lives at window level so the muxer walk from the
  // popover (parented to m_ctx_button, which sits inside m_root) lands
  // on it. Action names are referenced from the menu model as
  // "gradient.<verb>".
  m_actions = Gio::SimpleActionGroup::create();
  m_actions->add_action(
      "ctx-delete-stop",
      sigc::mem_fun(*this, &GradientDialog::on_ctx_delete_stop));
  m_actions->add_action(
      "ctx-duplicate-stop",
      sigc::mem_fun(*this, &GradientDialog::on_ctx_duplicate_stop));
  m_actions->add_action(
      "ctx-set-offset",
      sigc::mem_fun(*this, &GradientDialog::on_ctx_set_offset));
  insert_action_group("gradient", m_actions);

  // ── Hidden context-menu helper ────────────────────────────────────────
  //
  // Per the SwatchesPanel S72 fix: a bare PopoverMenu set_parent +
  // popup() renders fine but item clicks don't dispatch — popovers
  // parented to plain widgets need gtk_popover_present() called during
  // the parent's size_allocate to fully wire the action muxer, and
  // MenuButton does that as a side effect of holding a popover. So
  // every right-click builds a fresh PopoverMenu, sets it on this
  // hidden MenuButton, and calls MenuButton::popup().
  //
  // Visible (so it gets allocated and translate_coordinates works) but
  // render-invisible: zero-sized, opacity 0, no frame, no focus.
  m_ctx_button.set_size_request(0, 0);
  m_ctx_button.set_opacity(0.0);
  m_ctx_button.set_can_focus(false);
  m_ctx_button.set_focus_on_click(false);
  m_ctx_button.set_has_frame(false);
  m_root.append(m_ctx_button);

  wire_signals();

  set_child(m_root);
}

// ── Build helpers ────────────────────────────────────────────────────────────

void GradientDialog::build_type_row() {
  m_type_row.set_spacing(6);
  m_btn_linear.set_hexpand(true);
  m_btn_radial.set_hexpand(true);
  curvz::utils::set_name(m_btn_linear, "dlg_gr_lin",
                         "gradient_dialog_linear_toggle");
  curvz::utils::set_name(m_btn_radial, "dlg_gr_rad",
                         "gradient_dialog_radial_toggle");
  // Group radio-style: linking the toggle buttons makes them mutually
  // exclusive without needing radio-button visual styling.
  m_btn_radial.set_group(m_btn_linear);
  m_type_row.append(m_btn_linear);
  m_type_row.append(m_btn_radial);
  m_root.append(m_type_row);
}

void GradientDialog::build_stop_track() {
  // Track layout: gradient ramp on top, stop squares on the bottom row.
  // Total height balances visibility of both strips against not eating
  // the dialog. 64px is a comfortable read at typical zoom.
  m_track_area.set_content_height(64);
  m_track_area.set_hexpand(true);
  curvz::utils::set_name(m_track_area, "dlg_gr_trk",
                         "gradient_dialog_track_da");
  m_track_area.set_draw_func(sigc::mem_fun(*this, &GradientDialog::draw_track));
  m_root.append(m_track_area);

  m_stop_btn_row.set_spacing(6);
  curvz::utils::set_name(m_btn_add_stop, "dlg_gr_as",
                         "gradient_dialog_add_stop_btn");
  curvz::utils::set_name(m_btn_remove_stop, "dlg_gr_rs",
                         "gradient_dialog_remove_stop_btn");
  m_stop_btn_row.append(m_btn_add_stop);
  m_stop_btn_row.append(m_btn_remove_stop);
  // S92 m1 — Reverse flips the gradient direction; Distribute spreads
  // stops uniformly. They sit to the right of Add/Remove so the
  // additive/destructive pair stays grouped on the left and the bulk
  // operations live separately. Tooltips clarify behaviour because the
  // glyph + label pair is brief.
  m_btn_reverse.set_tooltip_text(
      "Reverse stop order (flip gradient direction)");
  m_btn_distribute.set_tooltip_text(
      "Distribute stops evenly along the gradient");
  curvz::utils::set_name(m_btn_reverse, "dlg_gr_rv",
                         "gradient_dialog_reverse_btn");
  curvz::utils::set_name(m_btn_distribute, "dlg_gr_ds",
                         "gradient_dialog_distribute_btn");
  m_stop_btn_row.append(m_btn_reverse);
  m_stop_btn_row.append(m_btn_distribute);
  m_root.append(m_stop_btn_row);
}

void GradientDialog::build_stop_props() {
  m_stop_props.set_row_spacing(6);
  m_stop_props.set_column_spacing(8);
  m_stop_props.set_margin_top(4);

  // Position spin: 0..1, step 0.01.
  m_pos_spin.set_range(0.0, 1.0);
  m_pos_spin.set_increments(0.01, 0.1);
  m_pos_spin.set_digits(3);
  m_pos_spin.set_value(0.0);
  curvz::utils::set_name(m_pos_spin, "dlg_gr_pos", "gradient_dialog_pos_spn");

  // Opacity spin: 0..1, step 0.05.
  m_opacity_spin.set_range(0.0, 1.0);
  m_opacity_spin.set_increments(0.05, 0.1);
  m_opacity_spin.set_digits(2);
  m_opacity_spin.set_value(1.0);
  curvz::utils::set_name(m_opacity_spin, "dlg_gr_op",
                         "gradient_dialog_opacity_spn");

  auto *lbl_pos = Gtk::make_managed<Gtk::Label>("Position");
  lbl_pos->set_halign(Gtk::Align::START);
  auto *lbl_op = Gtk::make_managed<Gtk::Label>("Opacity");
  lbl_op->set_halign(Gtk::Align::START);

  m_stop_props.attach(*lbl_pos, 0, 0);
  m_stop_props.attach(m_pos_spin, 1, 0);
  m_stop_props.attach(*lbl_op, 2, 0);
  m_stop_props.attach(m_opacity_spin, 3, 0);

  // Colour picker — the meaty bit. Spans the full width on its own row.
  m_color_picker.set_with_alpha(false); // alpha is stop-opacity, separate
  curvz::utils::set_name(m_color_picker, "dlg_gr_cp",
                         "gradient_dialog_color_picker");
  m_stop_props.attach(m_color_picker, 0, 1, 4, 1);

  m_root.append(m_stop_props);
}

void GradientDialog::build_geom_section() {
  m_sep_geom.set_margin_top(6);
  m_root.append(m_sep_geom);
  m_geom_header.set_halign(Gtk::Align::START);
  m_geom_header.add_css_class("dim-label");
  m_root.append(m_geom_header);

  m_geom_grid.set_row_spacing(6);
  m_geom_grid.set_column_spacing(8);

  // Endpoint spins (m_x1/y1/x2/y2_spin) and the radial r-spin (m_r_spin)
  // are NOT shown to the user. They're internal state, derived from the
  // friendly controls below — exposing 0..1 fractional coordinates is a
  // confusion engine. The spins still exist as members so the wire-up
  // signals (on_geom_changed) still work for back-computation if any
  // future surface needs them; for now nothing emits into them.

  // Angle: full 0..360°, 1° steps, 1 decimal. Scope is linear-only.
  m_angle_spin.set_range(0.0, 360.0);
  m_angle_spin.set_increments(1.0, 15.0);
  m_angle_spin.set_digits(1);
  m_angle_spin.set_wrap(true); // wrap 360 → 0 so dragging past works
  // Shape: wide enough to read the full "999.9" comfortably + buttons,
  // and centred vertically beside the 64px knob rather than stretching
  // to fill the row's height.
  m_angle_spin.set_width_chars(8);
  m_angle_spin.set_max_width_chars(8);
  m_angle_spin.set_hexpand(false);
  m_angle_spin.set_vexpand(false);
  m_angle_spin.set_valign(Gtk::Align::CENTER);
  m_angle_spin.set_halign(Gtk::Align::START);
  curvz::utils::set_name(m_angle_spin, "dlg_gr_ang",
                         "gradient_dialog_angle_spn");

  // Knob — circular drawing area, drag to rotate.
  m_angle_knob.set_content_width(64);
  m_angle_knob.set_content_height(64);
  curvz::utils::set_name(m_angle_knob, "dlg_gr_akb",
                         "gradient_dialog_angle_knob_da");
  m_angle_knob.set_draw_func(
      sigc::mem_fun(*this, &GradientDialog::draw_angle_knob));

  m_lbl_angle = Gtk::make_managed<Gtk::Label>("Angle°");
  m_lbl_angle->set_halign(Gtk::Align::START);

  // Radius: 0..100%. Internally stored as 0..1 in g_r, displayed as a
  // percentage so users think in friendly units.
  m_r_spin.set_range(0.0, 100.0);
  m_r_spin.set_increments(1.0, 10.0);
  m_r_spin.set_digits(0);
  curvz::utils::set_name(m_r_spin, "dlg_gr_r", "gradient_dialog_radius_spn");
  m_lbl_r = Gtk::make_managed<Gtk::Label>("Radius %");
  m_lbl_r->set_halign(Gtk::Align::START);

  // Linear row: label, knob, spin (knob drives spin via drag, spin
  // drives knob via redraw).
  m_geom_grid.attach(*m_lbl_angle, 0, 0);
  m_geom_grid.attach(m_angle_knob, 1, 0);
  m_geom_grid.attach(m_angle_spin, 2, 0, 2, 1);
  // Radial row (visibility flips with type).
  m_geom_grid.attach(*m_lbl_r, 0, 1);
  m_geom_grid.attach(m_r_spin, 1, 1, 3, 1);

  m_root.append(m_geom_grid);
}

void GradientDialog::build_action_row() {
  m_sep_actions.set_margin_top(8);
  m_root.append(m_sep_actions);

  m_action_row.set_spacing(8);
  m_action_row.set_halign(Gtk::Align::END);
  curvz::utils::set_name(m_btn_cancel, "dlg_gr_cnc",
                         "gradient_dialog_cancel_btn");
  curvz::utils::set_name(m_btn_apply, "dlg_gr_app",
                         "gradient_dialog_apply_btn");
  m_action_row.append(m_btn_cancel);
  m_action_row.append(m_btn_apply);
  m_btn_apply.add_css_class("suggested-action");
  m_root.append(m_action_row);
}

// ── Wiring ───────────────────────────────────────────────────────────────────

void GradientDialog::wire_signals() {
  m_btn_linear.signal_toggled().connect(
      sigc::mem_fun(*this, &GradientDialog::on_type_changed));
  m_btn_radial.signal_toggled().connect(
      sigc::mem_fun(*this, &GradientDialog::on_type_changed));

  // Stop track gestures.
  //   - GestureClick (button 1): single-click selects, double-click adds.
  //   - GestureDrag: smooth retune of selected stop's offset.
  auto click = Gtk::GestureClick::create();
  click->set_button(1);
  click->signal_pressed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_track_press));
  m_track_area.add_controller(click);

  auto drag = Gtk::GestureDrag::create();
  drag->set_button(1);
  drag->signal_drag_update().connect(
      sigc::mem_fun(*this, &GradientDialog::on_track_drag_update));
  drag->signal_drag_end().connect(
      [this](double, double) { on_track_drag_end(); });
  m_track_area.add_controller(drag);

  m_btn_add_stop.signal_clicked().connect(
      sigc::mem_fun(*this, &GradientDialog::on_add_stop));
  m_btn_remove_stop.signal_clicked().connect(
      sigc::mem_fun(*this, &GradientDialog::on_remove_stop));
  // S92 m1 — Reverse + Distribute buttons.
  m_btn_reverse.signal_clicked().connect(
      sigc::mem_fun(*this, &GradientDialog::on_reverse_stops));
  m_btn_distribute.signal_clicked().connect(
      sigc::mem_fun(*this, &GradientDialog::on_distribute_stops));

  // S92 m1 — Right-click on the track. Separate GestureClick on
  // button 3 so the existing left-click controller is untouched.
  // Wired on signal_pressed so it fires on press regardless of
  // release geometry (matches platform conventions).
  auto rc_click = Gtk::GestureClick::create();
  rc_click->set_button(3);
  rc_click->signal_pressed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_track_secondary_press));
  m_track_area.add_controller(rc_click);

  m_pos_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_pos_changed));
  m_opacity_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_opacity_changed));

  // CurvzColorPicker emits live changes — ride them rather than waiting
  // for commit so the visible preview (stop list row label) updates as
  // the user drags the spectrum.
  m_color_picker.signal_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_color_changed));

  m_x1_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_geom_changed));
  m_y1_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_geom_changed));
  m_x2_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_geom_changed));
  m_y2_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_geom_changed));
  m_r_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_geom_changed));
  m_angle_spin.signal_value_changed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_angle_changed));

  // Knob gestures: click+drag rotates around centre. Click anywhere on
  // the knob also commits the angle that pointer position represents,
  // so a single click jumps directly to e.g. the 12-o'clock position.
  auto knob_click = Gtk::GestureClick::create();
  knob_click->set_button(1);
  knob_click->signal_pressed().connect(
      sigc::mem_fun(*this, &GradientDialog::on_knob_press));
  m_angle_knob.add_controller(knob_click);

  auto knob_drag = Gtk::GestureDrag::create();
  knob_drag->set_button(1);
  knob_drag->signal_drag_update().connect(
      sigc::mem_fun(*this, &GradientDialog::on_knob_drag_update));
  m_angle_knob.add_controller(knob_drag);

  m_btn_cancel.signal_clicked().connect(
      sigc::mem_fun(*this, &GradientDialog::on_cancel));
  m_btn_apply.signal_clicked().connect(
      sigc::mem_fun(*this, &GradientDialog::on_apply));
}

// ── show ─────────────────────────────────────────────────────────────────────

void GradientDialog::show(Gtk::Window &parent, FillStyle initial, Callback cb) {
  m_callback = std::move(cb);
  // Sanity: if a non-gradient slipped in, promote to LinearGradient with
  // two basic stops so the user has something to edit. The hosting context
  // (right-click → Add gradient) is responsible for seeding correctly,
  // but defensiveness is free.
  if (!initial.is_gradient()) {
    initial.type = FillStyle::Type::LinearGradient;
    if (initial.stops.empty()) {
      GradientStop s0;
      s0.offset = 0.0;
      s0.r = 0;
      s0.g = 0;
      s0.b = 0;
      s0.a = 1;
      GradientStop s1;
      s1.offset = 1.0;
      s1.r = 1;
      s1.g = 1;
      s1.b = 1;
      s1.a = 1;
      initial.stops = {s0, s1};
    }
    initial.g_x1 = 0.0;
    initial.g_y1 = 0.5;
    initial.g_x2 = 1.0;
    initial.g_y2 = 0.5;
    initial.g_r = 0.5;
  }
  m_working = std::move(initial);
  m_selected_stop = 0;
  sync_from_state();
  set_transient_for(parent);
  curvz::utils::apply_motif_class_from_parent(*this, parent); // s117 m18 v2
  present();
}

// ── State sync ───────────────────────────────────────────────────────────────

void GradientDialog::sync_from_state() {
  m_loading = true;

  // Type radio toggles. We deactivate the "wrong" button before activating
  // the right one so the group lands in the correct state regardless of
  // prior dialog use. Without the explicit deactivate, GTK4's toggle-group
  // semantics can leave the previously-active button stuck if the new
  // active-set call is a no-op — manifesting as "dialog reopens with the
  // last-used type instead of the seeded one."
  if (m_working.type == FillStyle::Type::LinearGradient) {
    m_btn_radial.set_active(false);
    m_btn_linear.set_active(true);
  } else {
    m_btn_linear.set_active(false);
    m_btn_radial.set_active(true);
  }
  apply_type_visibility();

  // Stop list
  redraw_track();

  // Geometry
  m_x1_spin.set_value(m_working.g_x1);
  m_y1_spin.set_value(m_working.g_y1);
  m_x2_spin.set_value(m_working.g_x2);
  m_y2_spin.set_value(m_working.g_y2);
  m_r_spin.set_value(m_working.g_r * 100.0); // displayed as percent
  m_angle_spin.set_value(angle_from_endpoints(m_working.g_x1, m_working.g_y1,
                                              m_working.g_x2, m_working.g_y2));

  m_loading = false;
}

void GradientDialog::redraw_track() {
  // Clamp m_selected_stop to current stop count and refresh the
  // properties area; the track itself just needs queue_draw.
  int n = (int)m_working.stops.size();
  if (n == 0) {
    m_selected_stop = 0;
  } else {
    m_selected_stop = std::clamp(m_selected_stop, 0, n - 1);
  }
  select_stop(m_selected_stop);
  m_track_area.queue_draw();
}

// ── Track drawing ────────────────────────────────────────────────────────────

void GradientDialog::draw_track(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                                int h) {
  // ── Ramp strip (top) ─────────────────────────────────────────────────
  // A horizontal linear gradient using the working stops, painted into
  // [GUTTER, w-GUTTER] x [0, RAMP_BOT]. Even when the working type is
  // Radial we draw a linear preview here — a 1D ramp is the right read
  // for "ordering and colour transitions of stops"; on-canvas is where
  // the real radial render lives. Radial-aware track preview is a
  // stage 3c+ refinement.
  {
    const double x0 = GUTTER;
    const double x1 = w - GUTTER;
    if (x1 > x0) {
      auto pat = Cairo::LinearGradient::create(x0, 0, x1, 0);
      // Sort defensively so reversed stop lists still render in order.
      std::vector<GradientStop> sorted = m_working.stops;
      std::sort(sorted.begin(), sorted.end(),
                [](const GradientStop &a, const GradientStop &b) {
                  return a.offset < b.offset;
                });
      for (const auto &s : sorted) {
        pat->add_color_stop_rgba(std::clamp(s.offset, 0.0, 1.0), s.r, s.g, s.b,
                                 s.a);
      }
      cr->set_source(pat);
      cr->rectangle(x0, 0, x1 - x0, RAMP_BOT);
      cr->fill();
    }
    // Faint outline so the ramp reads as a UI element, not bleed.
    cr->set_source_rgba(0, 0, 0, 0.35);
    cr->set_line_width(1.0);
    cr->rectangle(GUTTER + 0.5, 0.5, w - 2 * GUTTER - 1, RAMP_BOT - 1);
    cr->stroke();
  }

  // ── Track line ───────────────────────────────────────────────────────
  const double track_y = RAMP_BOT + RAMP_GAP + SQUARE_H * 0.5;
  cr->set_source_rgba(0, 0, 0, 0.45);
  cr->set_line_width(1.0);
  cr->move_to(GUTTER, track_y);
  cr->line_to(w - GUTTER, track_y);
  cr->stroke();

  // ── Stop squares ─────────────────────────────────────────────────────
  // Iterate in original order so m_selected_stop indexing matches the
  // model. Each square's centre x = GUTTER + offset * (w - 2*GUTTER).
  const double inner_w = (double)(w - 2 * GUTTER);
  for (int i = 0; i < (int)m_working.stops.size(); ++i) {
    const auto &s = m_working.stops[i];
    double cx = GUTTER + std::clamp(s.offset, 0.0, 1.0) * inner_w;
    double sx = cx - SQUARE_W * 0.5;
    double sy = RAMP_BOT + RAMP_GAP;
    bool selected = (i == m_selected_stop);
    bool dragging = (i == m_track_drag_idx);

    // Slight Y-lift while dragging for tactile feedback.
    if (dragging)
      sy -= 2.0;

    // Square fill = stop colour (alpha-aware).
    cr->set_source_rgba(s.r, s.g, s.b, s.a);
    cr->rectangle(sx, sy, SQUARE_W, SQUARE_H);
    cr->fill();
    // Outline. Selected = bold black; unselected = thin grey.
    if (selected) {
      cr->set_source_rgba(0, 0, 0, 0.95);
      cr->set_line_width(2.0);
    } else {
      cr->set_source_rgba(0, 0, 0, 0.55);
      cr->set_line_width(1.0);
    }
    cr->rectangle(sx + 0.5, sy + 0.5, SQUARE_W - 1, SQUARE_H - 1);
    cr->stroke();
    // Tiny tick on the track line right under the square so the
    // mapping between square and offset reads clearly.
    cr->set_source_rgba(0, 0, 0, 0.7);
    cr->set_line_width(1.0);
    cr->move_to(cx, track_y - 3);
    cr->line_to(cx, track_y + 3);
    cr->stroke();
  }
}

int GradientDialog::hit_stop_square(double x, double y, int w,
                                    int /*h*/) const {
  double sy = RAMP_BOT + RAMP_GAP;
  if (y < sy - 3 || y > sy + SQUARE_H + 3)
    return -1;
  const double inner_w = (double)(w - 2 * GUTTER);
  // Walk in reverse so the topmost (last-drawn) overlapping square wins.
  // For 3b ordering matches model order; this is forward-compatible for
  // when we add stop-overlap visual stacking.
  for (int i = (int)m_working.stops.size() - 1; i >= 0; --i) {
    const auto &s = m_working.stops[i];
    double cx = GUTTER + std::clamp(s.offset, 0.0, 1.0) * inner_w;
    double sx = cx - SQUARE_W * 0.5;
    if (x >= sx - 2 && x <= sx + SQUARE_W + 2)
      return i;
  }
  return -1;
}

double GradientDialog::x_to_offset(double x, int w) const {
  const double inner_w = (double)(w - 2 * GUTTER);
  if (inner_w <= 0)
    return 0.0;
  return std::clamp((x - GUTTER) / inner_w, 0.0, 1.0);
}

void GradientDialog::on_track_press(int n_press, double x, double y) {
  if (m_loading)
    return;
  int w = m_track_area.get_width();
  int h = m_track_area.get_height();
  int idx = hit_stop_square(x, y, w, h);

  if (n_press >= 2 && idx < 0) {
    // Double-click on empty track → add a stop at the click offset,
    // colour sampled from the nearest existing stop.
    double off = x_to_offset(x, w);
    GradientStop ns;
    ns.offset = off;
    if (!m_working.stops.empty()) {
      const GradientStop *nearest = &m_working.stops.front();
      double best_d = std::abs(nearest->offset - off);
      for (const auto &st : m_working.stops) {
        double d = std::abs(st.offset - off);
        if (d < best_d) {
          best_d = d;
          nearest = &st;
        }
      }
      ns.r = nearest->r;
      ns.g = nearest->g;
      ns.b = nearest->b;
      ns.a = nearest->a;
    } else {
      ns.r = 0.5;
      ns.g = 0.5;
      ns.b = 0.5;
      ns.a = 1.0;
    }
    m_working.stops.push_back(ns);
    std::sort(m_working.stops.begin(), m_working.stops.end(),
              [](const GradientStop &a, const GradientStop &b) {
                return a.offset < b.offset;
              });
    // Find the inserted stop's new index for selection.
    for (int i = 0; i < (int)m_working.stops.size(); ++i) {
      const auto &t = m_working.stops[i];
      if (t.offset == ns.offset && t.r == ns.r && t.g == ns.g && t.b == ns.b &&
          t.a == ns.a) {
        m_selected_stop = i;
        break;
      }
    }
    redraw_track();
    return;
  }

  if (idx < 0)
    return; // single click on empty track = no-op

  // Single click on a square: select + arm drag.
  m_selected_stop = idx;
  m_track_drag_idx = idx;
  m_track_dragged = false;
  m_track_press_x = x;
  select_stop(idx);
  m_track_area.queue_draw();
}

void GradientDialog::on_track_drag_update(double dx, double /*dy*/) {
  if (m_loading)
    return;
  if (m_track_drag_idx < 0)
    return;
  if (m_track_drag_idx >= (int)m_working.stops.size())
    return;
  if (std::abs(dx) > 1.0)
    m_track_dragged = true;

  int w = m_track_area.get_width();
  double cur_x = m_track_press_x + dx;
  double off = x_to_offset(cur_x, w);
  m_working.stops[m_track_drag_idx].offset = off;
  // Mirror to the position spinbox so the numeric reflects live.
  bool was_loading = m_loading;
  m_loading = true;
  m_pos_spin.set_value(off);
  m_loading = was_loading;
  m_track_area.queue_draw();
}

void GradientDialog::on_track_drag_end() {
  if (m_track_drag_idx < 0)
    return;
  if (m_track_drag_idx < (int)m_working.stops.size() && m_track_dragged) {
    // Resort and track the dragged stop's new index — same dance as
    // the position spin's commit handler.
    GradientStop sel = m_working.stops[m_track_drag_idx];
    std::sort(m_working.stops.begin(), m_working.stops.end(),
              [](const GradientStop &a, const GradientStop &b) {
                return a.offset < b.offset;
              });
    for (int i = 0; i < (int)m_working.stops.size(); ++i) {
      const auto &t = m_working.stops[i];
      if (t.offset == sel.offset && t.r == sel.r && t.g == sel.g &&
          t.b == sel.b && t.a == sel.a) {
        m_selected_stop = i;
        break;
      }
    }
  }
  m_track_drag_idx = -1;
  m_track_dragged = false;
  redraw_track();
}

// ── Angle knob drawing + interaction ─────────────────────────────────────────
//
// A small clock-face widget. Filled circle, faint tick at 0° (East, SVG
// convention), and a thicker line from centre to circumference at the
// current angle. Click-drag rotates; click-only jumps to the click's
// implied angle. Spin is the source of truth for the value — knob just
// reads it on draw and writes it on interaction (with m_loading guard).

void GradientDialog::draw_angle_knob(const Cairo::RefPtr<Cairo::Context> &cr,
                                     int w, int h) {
  const double cx = w * 0.5;
  const double cy = h * 0.5;
  const double R = std::min(w, h) * 0.5 - 2.5;

  // Filled circle background — pale grey so the knob silhouette reads
  // against Curvz's dark dialog theme. (Previous black-with-low-alpha
  // washed into the background.)
  cr->arc(cx, cy, R, 0, 2 * M_PI);
  cr->set_source_rgba(0.62, 0.62, 0.62, 1.0);
  cr->fill_preserve();
  cr->set_source_rgba(0.30, 0.30, 0.30, 1.0);
  cr->set_line_width(1.5);
  cr->stroke();

  // East tick at 0° — short mark on the circumference.
  cr->set_source_rgba(0.30, 0.30, 0.30, 1.0);
  cr->set_line_width(1.5);
  cr->move_to(cx + R - 6, cy);
  cr->line_to(cx + R + 1, cy);
  cr->stroke();

  // Indicator line — from centre to circumference at the current angle.
  // Dark colour for contrast against the pale knob body.
  double ang_deg = m_angle_spin.get_value();
  double ang_rad = ang_deg * (M_PI / 180.0);
  double ex = cx + std::cos(ang_rad) * (R - 2.0);
  double ey = cy + std::sin(ang_rad) * (R - 2.0);

  cr->set_source_rgba(0.10, 0.10, 0.10, 1.0);
  cr->set_line_width(3.0);
  cr->set_line_cap(Cairo::Context::LineCap::ROUND);
  cr->move_to(cx, cy);
  cr->line_to(ex, ey);
  cr->stroke();

  // Hub dot at centre + tip dot at line end so the indicator reads from
  // a glance without depending on the line stroke alone.
  cr->set_source_rgba(0.10, 0.10, 0.10, 1.0);
  cr->arc(cx, cy, 3.5, 0, 2 * M_PI);
  cr->fill();
  cr->arc(ex, ey, 4.5, 0, 2 * M_PI);
  cr->fill();
}

void GradientDialog::on_knob_press(int /*n_press*/, double x, double y) {
  if (m_loading)
    return;
  if (m_working.type != FillStyle::Type::LinearGradient)
    return;
  // Convert click to angle and commit. Drag updates will then use the
  // raw delta to recompute angle from the (anchored) press position.
  m_knob_press_x = x;
  m_knob_press_y = y;
  int w = m_angle_knob.get_width();
  int h = m_angle_knob.get_height();
  double cx = w * 0.5;
  double cy = h * 0.5;
  double dx = x - cx;
  double dy = y - cy;
  if (dx == 0.0 && dy == 0.0)
    return;
  double deg = std::atan2(dy, dx) * (180.0 / M_PI);
  if (deg < 0.0)
    deg += 360.0;
  m_angle_spin.set_value(
      deg); // triggers on_angle_changed which writes endpoints
}

void GradientDialog::on_knob_drag_update(double dx, double dy) {
  if (m_loading)
    return;
  if (m_working.type != FillStyle::Type::LinearGradient)
    return;
  // Reconstruct current pointer position from press + drag delta, then
  // compute angle from centre. This decouples the gesture from the
  // press-to-current vector — so the indicator line tracks the cursor
  // exactly, even when the cursor strays outside the knob's bounds.
  double cur_x = m_knob_press_x + dx;
  double cur_y = m_knob_press_y + dy;
  int w = m_angle_knob.get_width();
  int h = m_angle_knob.get_height();
  double cx = w * 0.5;
  double cy = h * 0.5;
  double vx = cur_x - cx;
  double vy = cur_y - cy;
  if (vx == 0.0 && vy == 0.0)
    return;
  double deg = std::atan2(vy, vx) * (180.0 / M_PI);
  if (deg < 0.0)
    deg += 360.0;
  m_angle_spin.set_value(deg);
}

void GradientDialog::select_stop(int idx) {
  bool was_loading = m_loading;
  m_loading = true;

  int n = (int)m_working.stops.size();
  if (n == 0 || idx < 0 || idx >= n) {
    // Disable stop-prop widgets when nothing's selected. Visually
    // they remain — disabling avoids users editing an empty index.
    m_pos_spin.set_sensitive(false);
    m_opacity_spin.set_sensitive(false);
    m_color_picker.set_sensitive(false);
    m_btn_remove_stop.set_sensitive(false);
    // S92 m1 — Distribute / Reverse also senseless with no stops.
    m_btn_reverse.set_sensitive(false);
    m_btn_distribute.set_sensitive(false);
    m_loading = was_loading;
    return;
  }
  m_pos_spin.set_sensitive(true);
  m_opacity_spin.set_sensitive(true);
  m_color_picker.set_sensitive(true);
  // Don't allow removing the last stop — gradients with zero stops
  // render as transparent and there's no UX for that here.
  m_btn_remove_stop.set_sensitive(n > 1);
  // S92 m1 — Distribute / Reverse are no-ops with <2 stops; grey them
  // out to match the model state.
  m_btn_reverse.set_sensitive(n > 1);
  m_btn_distribute.set_sensitive(n > 1);

  const auto &s = m_working.stops[idx];
  m_pos_spin.set_value(s.offset);
  m_opacity_spin.set_value(s.a);
  color::Color c;
  c.r = s.r;
  c.g = s.g;
  c.b = s.b;
  c.a = 1.0;
  m_color_picker.set_initial(c);

  m_loading = was_loading;
}

void GradientDialog::apply_type_visibility() {
  bool radial = (m_working.type == FillStyle::Type::RadialGradient);
  // Linear-only widgets.
  m_angle_spin.set_visible(!radial);
  m_angle_knob.set_visible(!radial);
  if (m_lbl_angle)
    m_lbl_angle->set_visible(!radial);
  // Radial-only widgets.
  m_r_spin.set_visible(radial);
  if (m_lbl_r)
    m_lbl_r->set_visible(radial);
}

// ── Edit handlers ────────────────────────────────────────────────────────────

void GradientDialog::on_type_changed() {
  if (m_loading)
    return;
  bool was_radial = (m_working.type == FillStyle::Type::RadialGradient);
  bool now_radial = m_btn_radial.get_active();
  if (was_radial == now_radial)
    return; // toggle echo, no real change

  if (now_radial) {
    m_working.type = FillStyle::Type::RadialGradient;
    // Reset to sane radial defaults: centred, full-bbox radius, focal
    // at centre. Linear endpoint values would land in degenerate
    // territory for radial (focal outside the circle) and not render.
    // The simple-and-safe call is "user gets a working radial after
    // toggle"; if they'd tweaked the linear endpoints, that loss is
    // recoverable via Cancel.
    m_working.g_x1 = 0.5;
    m_working.g_y1 = 0.5; // focal at centre
    m_working.g_x2 = 0.5;
    m_working.g_y2 = 0.5; // centre at centre
    m_working.g_r = 0.5;  // radius half-bbox
  } else {
    m_working.type = FillStyle::Type::LinearGradient;
    // Symmetric reset on the way back: standard horizontal L→R.
    m_working.g_x1 = 0.0;
    m_working.g_y1 = 0.5;
    m_working.g_x2 = 1.0;
    m_working.g_y2 = 0.5;
  }
  // Re-sync the geometry spins so the user sees the new values.
  bool was_loading = m_loading;
  m_loading = true;
  m_x1_spin.set_value(m_working.g_x1);
  m_y1_spin.set_value(m_working.g_y1);
  m_x2_spin.set_value(m_working.g_x2);
  m_y2_spin.set_value(m_working.g_y2);
  m_r_spin.set_value(m_working.g_r * 100.0);
  m_angle_spin.set_value(angle_from_endpoints(m_working.g_x1, m_working.g_y1,
                                              m_working.g_x2, m_working.g_y2));
  m_loading = was_loading;

  apply_type_visibility();
  m_track_area.queue_draw();
  m_angle_knob.queue_draw();
}

void GradientDialog::on_pos_changed() {
  if (m_loading)
    return;
  int n = (int)m_working.stops.size();
  if (m_selected_stop < 0 || m_selected_stop >= n)
    return;
  m_working.stops[m_selected_stop].offset = m_pos_spin.get_value();
  // Keep stops sorted by offset so the gradient renders correctly. The
  // user's "currently selected" stop tracks its position in the list;
  // we re-sort and find where it landed, then update m_selected_stop.
  GradientStop sel = m_working.stops[m_selected_stop];
  std::sort(m_working.stops.begin(), m_working.stops.end(),
            [](const GradientStop &a, const GradientStop &b) {
              return a.offset < b.offset;
            });
  // Find sel's new index. Match by offset+colour+alpha (offset alone may
  // collide with another stop at the same position).
  for (int i = 0; i < (int)m_working.stops.size(); ++i) {
    const auto &t = m_working.stops[i];
    if (t.offset == sel.offset && t.r == sel.r && t.g == sel.g &&
        t.b == sel.b && t.a == sel.a) {
      m_selected_stop = i;
      break;
    }
  }
  redraw_track();
}

void GradientDialog::on_opacity_changed() {
  if (m_loading)
    return;
  int n = (int)m_working.stops.size();
  if (m_selected_stop < 0 || m_selected_stop >= n)
    return;
  m_working.stops[m_selected_stop].a = m_opacity_spin.get_value();
  // Refresh the stop row label so the α suffix updates.
  redraw_track();
}

void GradientDialog::on_color_changed(color::Color c) {
  if (m_loading)
    return;
  int n = (int)m_working.stops.size();
  if (m_selected_stop < 0 || m_selected_stop >= n)
    return;
  m_working.stops[m_selected_stop].r = c.r;
  m_working.stops[m_selected_stop].g = c.g;
  m_working.stops[m_selected_stop].b = c.b;
  // Don't pull alpha from the picker — it has alpha disabled. Alpha is
  // edited via m_opacity_spin so they don't fight.
  //
  // S92 m2: do NOT call redraw_track() here. redraw_track() funnels
  // through select_stop(), which calls m_color_picker.set_initial() —
  // and set_initial overwrites m_spectrum_nx/ny by round-tripping the
  // incoming colour through to_oklch(). The picker's documented "the
  // crosshair will draw at exactly (nx, ny) regardless of any gamut
  // clamping" guarantee only holds if nothing else stomps those
  // members between drag-update frames; set_initial does. Symptom:
  // dragging the spectrum dot into out-of-gamut territory snaps the
  // dot back onto the gamut boundary every frame, so the user can't
  // travel past the diagonal-ish shoulder curve.
  //
  // We still need the track ramp to repaint with the new colour. Just
  // queue a draw on the track area directly — that doesn't touch the
  // picker.
  m_track_area.queue_draw();
}

void GradientDialog::on_add_stop() {
  if (m_loading)
    return;
  GradientStop ns;
  // Insert at the midpoint of the current stops by default — most
  // common user gesture is "I want a stop in the middle".
  if (m_working.stops.empty()) {
    ns.offset = 0.0;
  } else if (m_working.stops.size() == 1) {
    ns.offset = std::min(1.0, m_working.stops.front().offset + 0.5);
  } else {
    // Find the largest gap between adjacent stops, place the new one
    // at its centre. Matches Affinity's default "duplicate at midpoint"
    // behaviour for + clicks.
    std::vector<GradientStop> sorted = m_working.stops;
    std::sort(sorted.begin(), sorted.end(),
              [](const GradientStop &a, const GradientStop &b) {
                return a.offset < b.offset;
              });
    double best_gap = 0.0;
    double best_mid = 0.5;
    for (size_t i = 0; i + 1 < sorted.size(); ++i) {
      double g = sorted[i + 1].offset - sorted[i].offset;
      if (g > best_gap) {
        best_gap = g;
        best_mid = (sorted[i].offset + sorted[i + 1].offset) * 0.5;
      }
    }
    ns.offset = best_mid;
  }
  // Sample colour from the existing gradient at the new offset — visually
  // appending a stop "where it already passes through" is less jarring
  // than a hard-coded black or grey. For 3a we just take the colour of
  // the nearest-by-offset existing stop; a true linear sample is a
  // 3b polish.
  if (!m_working.stops.empty()) {
    const GradientStop *nearest = &m_working.stops.front();
    double best_d = std::abs(nearest->offset - ns.offset);
    for (const auto &s : m_working.stops) {
      double d = std::abs(s.offset - ns.offset);
      if (d < best_d) {
        best_d = d;
        nearest = &s;
      }
    }
    ns.r = nearest->r;
    ns.g = nearest->g;
    ns.b = nearest->b;
    ns.a = nearest->a;
  } else {
    ns.r = 0.5;
    ns.g = 0.5;
    ns.b = 0.5;
    ns.a = 1.0;
  }
  m_working.stops.push_back(ns);
  std::sort(m_working.stops.begin(), m_working.stops.end(),
            [](const GradientStop &a, const GradientStop &b) {
              return a.offset < b.offset;
            });
  // Find the inserted stop's new index and select it.
  for (int i = 0; i < (int)m_working.stops.size(); ++i) {
    const auto &t = m_working.stops[i];
    if (t.offset == ns.offset && t.r == ns.r && t.g == ns.g && t.b == ns.b &&
        t.a == ns.a) {
      m_selected_stop = i;
      break;
    }
  }
  redraw_track();
}

void GradientDialog::on_remove_stop() {
  if (m_loading)
    return;
  int n = (int)m_working.stops.size();
  if (n <= 1)
    return; // disabled in select_stop, defensive
  if (m_selected_stop < 0 || m_selected_stop >= n)
    return;
  m_working.stops.erase(m_working.stops.begin() + m_selected_stop);
  if (m_selected_stop >= (int)m_working.stops.size()) {
    m_selected_stop = (int)m_working.stops.size() - 1;
  }
  redraw_track();
}

// ── S92 m1 — Stage 5 polish
// ───────────────────────────────────────────────────

void GradientDialog::on_distribute_stops() {
  if (m_loading)
    return;
  int n = (int)m_working.stops.size();
  if (n < 2)
    return; // single stop has nowhere to go
  // Walk in current array order and rewrite offsets to k/(n-1). Stops
  // are kept sorted ascending on read/write so the array order matches
  // the visual order; assigning monotonically increasing offsets in
  // the same order preserves both. Selection index doesn't move
  // (the same physical stop sits at the same array slot, just with
  // a new offset).
  for (int k = 0; k < n; ++k) {
    m_working.stops[k].offset =
        static_cast<double>(k) / static_cast<double>(n - 1);
  }
  redraw_track();
}

void GradientDialog::on_reverse_stops() {
  if (m_loading)
    return;
  int n = (int)m_working.stops.size();
  if (n < 2)
    return; // nothing to reverse
  // Reverse the gradient: stop at array slot k swaps with slot (n-1-k),
  // and each offset becomes (1 - offset). Both transformations together
  // preserve the ascending-offset invariant — the largest old offset
  // becomes the smallest new offset and ends up at slot 0 by the
  // reverse, matching the new offset value.
  std::reverse(m_working.stops.begin(), m_working.stops.end());
  for (auto &s : m_working.stops) {
    s.offset = 1.0 - s.offset;
  }
  // Selected stop's new index = (n-1) - old index — same physical
  // colour, mirrored position.
  if (m_selected_stop >= 0 && m_selected_stop < n) {
    m_selected_stop = (n - 1) - m_selected_stop;
  }
  redraw_track();
}

// ── S92 m1 — Right-click context menu
// ─────────────────────────────────────────

void GradientDialog::on_track_secondary_press(int /*n_press*/, double x,
                                              double y) {
  if (m_loading)
    return;
  int w = m_track_area.get_width();
  int h = m_track_area.get_height();
  int idx = hit_stop_square(x, y, w, h);
  if (idx < 0)
    return; // right-click on empty track = no-op (for now)

  // Stash the target index for the action handlers. Also select the
  // stop visually so the context the menu acts on matches what the
  // user sees highlighted.
  m_ctx_stop_idx = idx;
  m_selected_stop = idx;
  select_stop(idx);
  m_track_area.queue_draw();

  // Build the menu fresh per click. n<=1 disables Delete (deleting
  // the last stop would leave the gradient unrenderable).
  auto menu = Gio::Menu::create();
  int n = (int)m_working.stops.size();
  if (n > 1) {
    menu->append("Delete stop", "gradient.ctx-delete-stop");
  }
  menu->append("Duplicate stop", "gradient.ctx-duplicate-stop");
  menu->append("Set offset…", "gradient.ctx-set-offset");

  auto *pop = Gtk::make_managed<Gtk::PopoverMenu>(menu);
  pop->set_has_arrow(false);
  m_ctx_button.set_popover(*pop);

  // Anchor the popover at the click point. set_pointing_to takes a
  // rect in the popover's parent (m_ctx_button) coord space, so
  // translate from track-local to button-local.
  double bx = x, by = y;
  if (!m_track_area.translate_coordinates(m_ctx_button, x, y, bx, by)) {
    bx = 0;
    by = 0; // defensive — shouldn't trigger in practice
  }
  Gdk::Rectangle rect(static_cast<int>(bx), static_cast<int>(by), 1, 1);
  pop->set_pointing_to(rect);

  m_ctx_button.popup();
}

void GradientDialog::on_ctx_delete_stop() {
  int n = (int)m_working.stops.size();
  if (n <= 1)
    return;
  if (m_ctx_stop_idx < 0 || m_ctx_stop_idx >= n)
    return;
  m_working.stops.erase(m_working.stops.begin() + m_ctx_stop_idx);
  // Selection follows: clamp into the new range. If the deleted stop
  // was the selected one, fall onto its predecessor (or 0 if it was
  // already at 0).
  if (m_selected_stop >= (int)m_working.stops.size()) {
    m_selected_stop = (int)m_working.stops.size() - 1;
  }
  m_ctx_stop_idx = -1;
  redraw_track();
}

void GradientDialog::on_ctx_duplicate_stop() {
  int n = (int)m_working.stops.size();
  if (m_ctx_stop_idx < 0 || m_ctx_stop_idx >= n)
    return;
  GradientStop src = m_working.stops[m_ctx_stop_idx];
  // Place the duplicate at the midpoint between the source stop and
  // its right neighbour; if it's already the last stop, place it
  // halfway between the source and 1.0. This mirrors the Add Stop
  // "fill the largest gap" idiom locally — duplicate is "I want a
  // copy of THIS one near it" and the sensible "near" is the
  // immediate-rightward gap.
  double new_offset;
  if (m_ctx_stop_idx + 1 < n) {
    new_offset =
        (src.offset + m_working.stops[m_ctx_stop_idx + 1].offset) * 0.5;
  } else {
    new_offset = (src.offset + 1.0) * 0.5;
  }
  // Clamp to valid range.
  new_offset = std::clamp(new_offset, 0.0, 1.0);
  GradientStop dup = src;
  dup.offset = new_offset;
  m_working.stops.push_back(dup);
  std::sort(m_working.stops.begin(), m_working.stops.end(),
            [](const GradientStop &a, const GradientStop &b) {
              return a.offset < b.offset;
            });
  // Find + select the inserted duplicate. Same identity-match dance
  // as on_add_stop. When dup.offset == src.offset (degenerate case
  // where src was at 1.0), the two stops are indistinguishable —
  // selecting either one is visually identical, so first-match is
  // fine.
  for (int i = 0; i < (int)m_working.stops.size(); ++i) {
    const auto &t = m_working.stops[i];
    if (t.offset == dup.offset && t.r == dup.r && t.g == dup.g &&
        t.b == dup.b && t.a == dup.a) {
      m_selected_stop = i;
      break;
    }
  }
  m_ctx_stop_idx = -1;
  redraw_track();
}

void GradientDialog::on_ctx_set_offset() {
  // No sub-dialog — the position spin already exists for keyboard
  // precision. Selecting the stop and focusing the spin gives the
  // user numeric editing without the heavy modal "enter offset"
  // dance Affinity also avoids. The user types a number, hits Enter,
  // done.
  int n = (int)m_working.stops.size();
  if (m_ctx_stop_idx < 0 || m_ctx_stop_idx >= n)
    return;
  m_selected_stop = m_ctx_stop_idx;
  select_stop(m_selected_stop);
  m_pos_spin.grab_focus();
  // Pre-select the spin's text so the user can overtype directly.
  m_pos_spin.select_region(0, -1);
  m_ctx_stop_idx = -1;
}

void GradientDialog::on_geom_changed() {
  if (m_loading)
    return;
  m_working.g_x1 = m_x1_spin.get_value();
  m_working.g_y1 = m_y1_spin.get_value();
  m_working.g_x2 = m_x2_spin.get_value();
  m_working.g_y2 = m_y2_spin.get_value();
  m_working.g_r = m_r_spin.get_value() / 100.0; // percent → fraction
  // Reflect the new endpoints into the angle spin (linear only). The
  // m_loading guard prevents the spin's value-changed signal from
  // looping back to on_angle_changed and rewriting the endpoints.
  if (m_working.type == FillStyle::Type::LinearGradient) {
    bool was_loading = m_loading;
    m_loading = true;
    m_angle_spin.set_value(angle_from_endpoints(
        m_working.g_x1, m_working.g_y1, m_working.g_x2, m_working.g_y2));
    m_loading = was_loading;
  }
  m_track_area.queue_draw();
  m_angle_knob.queue_draw();
}

void GradientDialog::on_angle_changed() {
  if (m_loading)
    return;
  if (m_working.type != FillStyle::Type::LinearGradient)
    return;
  double ang = m_angle_spin.get_value();
  double x1, y1, x2, y2;
  endpoints_from_angle(ang, x1, y1, x2, y2);
  m_working.g_x1 = x1;
  m_working.g_y1 = y1;
  m_working.g_x2 = x2;
  m_working.g_y2 = y2;
  // Reflect into the endpoint spins. Same loading guard so they don't
  // call on_geom_changed and recompute the angle we just set.
  bool was_loading = m_loading;
  m_loading = true;
  m_x1_spin.set_value(x1);
  m_y1_spin.set_value(y1);
  m_x2_spin.set_value(x2);
  m_y2_spin.set_value(y2);
  m_loading = was_loading;
  m_track_area.queue_draw();
  m_angle_knob.queue_draw();
}

// ── Dialog actions ───────────────────────────────────────────────────────────

void GradientDialog::on_apply() {
  hide();
  if (m_callback)
    m_callback(m_working);
}

void GradientDialog::on_cancel() { hide(); }

} // namespace Curvz
