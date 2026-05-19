// widgets/RefPointPicker.cpp ───────────────────────────────────────────

#include "widgets/RefPointPicker.hpp"

#include <cmath>
#include <gtkmm/gestureclick.h>

namespace curvz::widgets {

using namespace curvz::scripting;

// ── ctor ────────────────────────────────────────────────────────────────────
//
// Layout (horizontal, 8px spacing):
//
//   [ 36x36 grid ]  [ ✓ chk ]  [ X spin + unit ]  [ Y spin + unit ]
//
// CurvzSpinButton owns its companion unit label (get_unit_label) — we
// append it directly after the spinner so "1.500 in" reads as one
// visual group. The X/Y short labels go above each spinner in a small
// vertical sub-box.
RefPointPicker::RefPointPicker(std::string_view name,
                               const Curvz::CanvasModel *canvas_model,
                               double ruler_origin_x, double ruler_origin_y)
    : ScriptableWidget<Gtk::Box>(name, Gtk::Orientation::HORIZONTAL, 8) {

  // ── Grid area (the 9-point picker) ──────────────────────────────────
  m_grid_area.set_content_width(36);
  m_grid_area.set_content_height(36);
  m_grid_area.set_valign(Gtk::Align::CENTER);
  // s204 m4 tweak: tag with a CSS class so theme rules can target the
  // grid's `color:` property. on_grid_draw reads get_color() at paint
  // time, so any CSS override at .refpoint-picker-grid (or via the
  // motif's curvz-light/curvz-dark cascade) flows directly into the
  // Cairo strokes. The composite itself also carries a class so CSS
  // can target the whole picker if needed.
  add_css_class("refpoint-picker");
  m_grid_area.add_css_class("refpoint-picker-grid");
  m_grid_area.set_draw_func([this](const Cairo::RefPtr<Cairo::Context> &cr,
                                   int w, int h) { on_grid_draw(cr, w, h); });
  {
    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    click->signal_pressed().connect(
        [this](int /*n_press*/, double x, double y) {
          if (m_mode != Mode::Preset)
            return;
          Preset p;
          if (!pixel_to_preset(x, y, &p))
            return;
          set_preset(p);
        });
    m_grid_area.add_controller(click);
  }
  append(m_grid_area);

  // ── Mode checkbox ───────────────────────────────────────────────────
  m_arbitrary_chk.set_valign(Gtk::Align::CENTER);
  m_arbitrary_chk.set_tooltip_text(
      "Use arbitrary X/Y instead of a preset bbox point");
  m_arbitrary_chk.signal_toggled().connect([this]() {
    if (m_loading)
      return;
    set_mode(m_arbitrary_chk.get_active() ? Mode::Arbitrary : Mode::Preset);
  });
  append(m_arbitrary_chk);

  // ── X column (label above, spinner+unit below) ──────────────────────
  {
    auto *x_col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    m_x_lbl.set_text("X");
    m_x_lbl.set_xalign(0.5f);
    // s205 m2: mimic the inspector's prop-row styling so this widget
    // blends naturally into the Selection / Refpt sections it embeds
    // in. prop-lbl gives the small muted-FG header look used by X /
    // Y / W / H / SCALE / ROTATE / SKEW above; the spinner gets the
    // matching prop-width-entry + node-spin classes below. The
    // companion unit label inside CurvzSpinButton already auto-applies
    // prop-width-unit (see CurvzSpinButton.cpp ctor), so the full
    // [number | unit] visual pair matches the surrounding rows.
    // Substrate-level: same classes work in the popover context too,
    // both share the same global stylesheet.
    m_x_lbl.add_css_class("prop-lbl");
    x_col->append(m_x_lbl);

    auto *x_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
    m_sp_x = Gtk::make_managed<Curvz::CurvzSpinButton>(
        Curvz::SpinType::PositionX, canvas_model, ruler_origin_x);
    m_sp_x->with_width_chars(8);
    m_sp_x->add_css_class("prop-width-entry");
    m_sp_x->add_css_class("node-spin");
    m_sp_x->signal_internal_changed().connect([this](double v) {
      if (m_loading)
        return;
      if (m_mode != Mode::Arbitrary)
        return; // preset mode ignores edits
      m_arb_x = v;
      m_sig_point_changed.emit(m_arb_x, m_arb_y);
      emit("point_changed", ScriptValue::real(m_arb_x));
    });
    x_row->append(*m_sp_x);
    m_x_unit_lbl = m_sp_x->get_unit_label();
    if (m_x_unit_lbl)
      x_row->append(*m_x_unit_lbl);
    x_col->append(*x_row);
    append(*x_col);
  }

  // ── Y column ────────────────────────────────────────────────────────
  {
    auto *y_col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 1);
    m_y_lbl.set_text("Y");
    m_y_lbl.set_xalign(0.5f);
    m_y_lbl.add_css_class("prop-lbl");
    y_col->append(m_y_lbl);

    auto *y_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
    m_sp_y = Gtk::make_managed<Curvz::CurvzSpinButton>(
        Curvz::SpinType::PositionY, canvas_model, ruler_origin_y);
    m_sp_y->with_width_chars(8);
    m_sp_y->add_css_class("prop-width-entry");
    m_sp_y->add_css_class("node-spin");
    m_sp_y->signal_internal_changed().connect([this](double v) {
      if (m_loading)
        return;
      if (m_mode != Mode::Arbitrary)
        return;
      m_arb_y = v;
      m_sig_point_changed.emit(m_arb_x, m_arb_y);
      emit("point_changed", ScriptValue::real(m_arb_y));
    });
    y_row->append(*m_sp_y);
    m_y_unit_lbl = m_sp_y->get_unit_label();
    if (m_y_unit_lbl)
      y_row->append(*m_y_unit_lbl);
    y_col->append(*y_row);
    append(*y_col);
  }

  apply_mode_appearance();
  refresh_xy_display();

  init_scriptable();
}

// ── bind_canonical ──────────────────────────────────────────────────────────
//
// A composite has no single canonical GTK signal — we're a Gtk::Box at
// the GTK level; Box has no semantic signal of its own. So this is a
// no-op: the widget emits its own well-typed `point_changed` and
// `mode_changed` events from the internal handlers above, which IS the
// canonical channel for this composite. The ScriptableWidget base's
// m_initialised assertion in ~ScriptableWidget is satisfied by
// init_scriptable() at the end of the ctor.
void RefPointPicker::bind_canonical() {}

// ── Public C++ API ──────────────────────────────────────────────────────────

void RefPointPicker::set_bbox(double x, double y, double w, double h) {
  m_bbox_x = x;
  m_bbox_y = y;
  m_bbox_w = w;
  m_bbox_h = h;
  if (m_mode == Mode::Preset) {
    refresh_xy_display();
    auto [px, py] = point();
    m_sig_point_changed.emit(px, py);
    // Don't fire a script point_changed event on every bbox tick —
    // would flood the trace during a 60Hz drag. Script subscribers
    // can poll via `get refpoint_picker.* x/y`.
  }
  m_grid_area.queue_draw();
}

void RefPointPicker::set_mode(Mode m) {
  if (m == m_mode) {
    m_loading = true;
    m_arbitrary_chk.set_active(m == Mode::Arbitrary);
    m_loading = false;
    return;
  }
  // Caveat: Preset → Arbitrary shows whatever m_arb_x/y currently
  // hold, which is (0,0) until the caller sets them via
  // set_arbitrary_xy or a script set_arbitrary. The pivot-popover
  // caller seeds m_arb_x/y to the current pivot position before
  // showing the picker, so this only manifests in the sandbox /
  // out-of-context standalone uses. We deliberately do NOT seed arb
  // from the preset on flip — that would overwrite a user's typed
  // values on a second flip and break the mode-memory contract.
  m_mode = m;
  m_loading = true;
  m_arbitrary_chk.set_active(m == Mode::Arbitrary);
  m_loading = false;
  apply_mode_appearance();
  refresh_xy_display();
  m_sig_mode_changed.emit(m_mode);
  emit("mode_changed",
       ScriptValue::text(m_mode == Mode::Arbitrary ? "arbitrary" : "preset"));
  auto [px, py] = point();
  m_sig_point_changed.emit(px, py);
  m_grid_area.queue_draw();
}

void RefPointPicker::set_preset(Preset p) {
  if (m_mode == Mode::Arbitrary) {
    set_mode(Mode::Preset);
  }
  m_last_preset = p;
  refresh_xy_display();
  auto [px, py] = point();
  m_sig_point_changed.emit(px, py);
  emit("point_changed", ScriptValue::text(preset_name(p)));
  m_grid_area.queue_draw();
}

void RefPointPicker::set_arbitrary_xy(double doc_x, double doc_y) {
  if (m_mode == Mode::Preset) {
    set_mode(Mode::Arbitrary);
  }
  m_arb_x = doc_x;
  m_arb_y = doc_y;
  m_loading = true;
  if (m_sp_x)
    m_sp_x->set_internal_value(doc_x);
  if (m_sp_y)
    m_sp_y->set_internal_value(doc_y);
  m_loading = false;
  m_sig_point_changed.emit(m_arb_x, m_arb_y);
  emit("point_changed", ScriptValue::real(m_arb_x));
}

// s205 m2 — silent variant of set_arbitrary_xy. No mode flip, no emit.
// See header banner for rationale (external sync hijack avoidance).
// The spinner internal-value writes go under m_loading=true so the
// CurvzSpinButton's signal_internal_changed handler (which would
// otherwise re-fire signal_point_changed via the user-edit path) early-
// returns on the loading guard. This mirrors the standard inspector
// live-sync idiom — programmatic display update without round-trip.
void RefPointPicker::update_arbitrary_xy_silent(double doc_x, double doc_y) {
  m_arb_x = doc_x;
  m_arb_y = doc_y;
  m_loading = true;
  if (m_sp_x)
    m_sp_x->set_internal_value(doc_x);
  if (m_sp_y)
    m_sp_y->set_internal_value(doc_y);
  m_loading = false;
}

std::pair<double, double> RefPointPicker::point() const {
  if (m_mode == Mode::Arbitrary)
    return {m_arb_x, m_arb_y};
  return {preset_doc_x(m_last_preset), preset_doc_y(m_last_preset)};
}

// ── Scriptable verb dispatch ────────────────────────────────────────────────

ScriptValue RefPointPicker::invoke_leaf(std::string_view verb,
                                        const ScriptArgs &args) {
  if (verb == "set_checkbox") {
    if (args.size() != 1 || args[0].kind != ValueKind::Bool)
      throw std::runtime_error(
          "RefPointPicker.set_checkbox expects one bool arg");
    set_mode(args[0].b ? Mode::Arbitrary : Mode::Preset);
    return ScriptValue::null();
  }
  if (verb == "set_preset") {
    if (args.size() != 1 || args[0].kind != ValueKind::String)
      throw std::runtime_error(
          "RefPointPicker.set_preset expects one preset-name arg "
          "(NW N NE W C E SW S SE)");
    Preset p;
    if (!preset_from_name(args[0].s, &p))
      throw std::runtime_error("RefPointPicker.set_preset: unknown preset '" +
                               args[0].s + "'");
    set_preset(p);
    return ScriptValue::null();
  }
  auto as_d = [](const ScriptValue &v) -> double {
    if (v.kind == ValueKind::Double)
      return v.d;
    if (v.kind == ValueKind::Int)
      return static_cast<double>(v.i);
    throw std::runtime_error("expected a number");
  };
  if (verb == "set_arbitrary") {
    if (args.size() != 2)
      throw std::runtime_error(
          "RefPointPicker.set_arbitrary expects two numeric args (x y)");
    set_arbitrary_xy(as_d(args[0]), as_d(args[1]));
    return ScriptValue::null();
  }
  if (verb == "set_bbox") {
    if (args.size() != 4)
      throw std::runtime_error(
          "RefPointPicker.set_bbox expects four numeric args (x y w h)");
    set_bbox(as_d(args[0]), as_d(args[1]), as_d(args[2]), as_d(args[3]));
    return ScriptValue::null();
  }
  throw std::runtime_error("RefPointPicker: unknown verb '" +
                           std::string(verb) + "'");
}

ScriptValue RefPointPicker::query_leaf(std::string_view property) const {
  if (property == "mode")
    return ScriptValue::text(m_mode == Mode::Arbitrary ? "arbitrary"
                                                       : "preset");
  if (property == "preset")
    return ScriptValue::text(preset_name(m_last_preset));
  if (property == "x") {
    auto [px, py] = point();
    (void)py;
    return ScriptValue::real(px);
  }
  if (property == "y") {
    auto [px, py] = point();
    (void)px;
    return ScriptValue::real(py);
  }
  if (property == "bbox_x")
    return ScriptValue::real(m_bbox_x);
  if (property == "bbox_y")
    return ScriptValue::real(m_bbox_y);
  if (property == "bbox_w")
    return ScriptValue::real(m_bbox_w);
  if (property == "bbox_h")
    return ScriptValue::real(m_bbox_h);
  return ScriptValue::null();
}

std::vector<std::string> RefPointPicker::leaf_verbs() const {
  return {"set_checkbox", "set_preset", "set_arbitrary", "set_bbox"};
}

std::vector<std::string> RefPointPicker::leaf_properties() const {
  return {"mode", "preset", "x", "y", "bbox_x", "bbox_y", "bbox_w", "bbox_h"};
}

// ── Internal helpers ────────────────────────────────────────────────────────

double RefPointPicker::preset_doc_x(Preset p) const {
  const double left = m_bbox_x;
  const double midx = m_bbox_x + m_bbox_w * 0.5;
  const double right = m_bbox_x + m_bbox_w;
  switch (p) {
  case Preset::NW:
  case Preset::W:
  case Preset::SW:
    return left;
  case Preset::N:
  case Preset::C:
  case Preset::S:
    return midx;
  case Preset::NE:
  case Preset::E:
  case Preset::SE:
    return right;
  }
  return midx;
}

double RefPointPicker::preset_doc_y(Preset p) const {
  const double top = m_bbox_y;
  const double midy = m_bbox_y + m_bbox_h * 0.5;
  const double bottom = m_bbox_y + m_bbox_h;
  switch (p) {
  case Preset::NW:
  case Preset::N:
  case Preset::NE:
    return top;
  case Preset::W:
  case Preset::C:
  case Preset::E:
    return midy;
  case Preset::SW:
  case Preset::S:
  case Preset::SE:
    return bottom;
  }
  return midy;
}

bool RefPointPicker::pixel_to_preset(double px, double py,
                                     Preset *out_p) const {
  const int W = m_grid_area.get_content_width();
  const int H = m_grid_area.get_content_height();
  const double margin = 4.0;
  const double inner_w = W - 2 * margin;
  const double inner_h = H - 2 * margin;
  if (inner_w <= 0 || inner_h <= 0)
    return false;
  double fx = (px - margin) / inner_w;
  double fy = (py - margin) / inner_h;
  if (fx < -0.1 || fx > 1.1 || fy < -0.1 || fy > 1.1)
    return false;
  int col = (fx < 1.0 / 3.0) ? 0 : (fx < 2.0 / 3.0 ? 1 : 2);
  int row = (fy < 1.0 / 3.0) ? 0 : (fy < 2.0 / 3.0 ? 1 : 2);
  static const Preset table[3][3] = {
      {Preset::NW, Preset::N, Preset::NE},
      {Preset::W, Preset::C, Preset::E},
      {Preset::SW, Preset::S, Preset::SE},
  };
  *out_p = table[row][col];
  return true;
}

void RefPointPicker::refresh_xy_display() {
  auto [px, py] = point();
  m_loading = true;
  if (m_sp_x)
    m_sp_x->set_internal_value(px);
  if (m_sp_y)
    m_sp_y->set_internal_value(py);
  m_loading = false;
}

void RefPointPicker::apply_mode_appearance() {
  const bool arb = (m_mode == Mode::Arbitrary);
  // Preset mode: spinners are NOT editable (read-only feel), still
  // sensitive so the numbers stay legible. Arbitrary mode: editable.
  if (m_sp_x)
    m_sp_x->set_editable(arb);
  if (m_sp_y)
    m_sp_y->set_editable(arb);
}

void RefPointPicker::on_grid_draw(const Cairo::RefPtr<Cairo::Context> &cr,
                                  int width, int height) {
  const double margin = 4.0;
  const double iw = width - 2 * margin;
  const double ih = height - 2 * margin;
  if (iw <= 0 || ih <= 0)
    return;

  // s204 m4 tweak: pull stroke color from the GTK theme so the widget
  // reads correctly on both Curvz motifs (and any other GTK theme that
  // hosts it later). `Gtk::Widget::get_color()` returns the CSS-resolved
  // `color` property — black-ish on light, white-ish on dark. CSS callers
  // can override via a custom class on the picker (.refpoint-picker
  // { color: ...; }) without this code needing to know about motifs.
  //
  // Unselected dot fill is derived as the strokes complement: if FG is
  // dark, fill is white; if FG is light, fill is black. Keeps the
  // "small white-filled square outlined in stroke color" look the
  // earlier hardcoded version had, but inverted on dark motif so the
  // dots show as small dark squares outlined in the light FG — same
  // visual semantics, theme-correct on both backgrounds.
  auto fg = m_grid_area.get_color();
  const double fr = fg.get_red();
  const double fg_g = fg.get_green();
  const double fb = fg.get_blue();
  const bool fg_is_light = (fr + fg_g + fb) > 1.5; // sum > 1.5 → light FG
  const double bg_r = fg_is_light ? 0.1 : 1.0;
  const double bg_g = fg_is_light ? 0.1 : 1.0;
  const double bg_b = fg_is_light ? 0.1 : 1.0;

  const bool grey = (m_mode == Mode::Arbitrary);
  const double a = grey ? 0.4 : 1.0;

  auto set_fg = [&](double alpha = 1.0) {
    cr->set_source_rgba(fr, fg_g, fb, a * alpha);
  };
  auto set_bg = [&](double alpha = 1.0) {
    cr->set_source_rgba(bg_r, bg_g, bg_b, a * alpha);
  };

  set_fg();
  cr->set_line_width(1.0);

  // Outer rect.
  cr->rectangle(margin + 0.5, margin + 0.5, iw - 1.0, ih - 1.0);
  cr->stroke();

  // s204 m4 tweak: thin bisecting cross through N–C–S and W–C–E,
  // dividing the bounding rect into 4 visual cells. Drawn at half the
  // outer stroke weight (0.5px) and BEFORE the dots so the dots paint
  // over the intersection. Visually frames the 9-point picker as a
  // 3x3 grid even when no dot is selected.
  cr->save();
  cr->set_line_width(0.5);
  cr->move_to(margin + iw * 0.5, margin);
  cr->line_to(margin + iw * 0.5, margin + ih);
  cr->stroke();
  cr->move_to(margin, margin + ih * 0.5);
  cr->line_to(margin + iw, margin + ih * 0.5);
  cr->stroke();
  cr->restore();

  // 9 dots at the inner-rect corners + edge midpoints + centre.
  const double xs[3] = {margin, margin + iw * 0.5, margin + iw};
  const double ys[3] = {margin, margin + ih * 0.5, margin + ih};
  static const Preset table[3][3] = {
      {Preset::NW, Preset::N, Preset::NE},
      {Preset::W, Preset::C, Preset::E},
      {Preset::SW, Preset::S, Preset::SE},
  };
  const double dot = 3.0;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      const double x = xs[c];
      const double y = ys[r];
      const Preset p = table[r][c];
      const bool selected = (m_mode == Mode::Preset && p == m_last_preset);
      cr->rectangle(x - dot, y - dot, dot * 2, dot * 2);
      if (selected) {
        // Filled in FG (solid black on light motif, solid white-ish on dark).
        set_fg();
        cr->fill_preserve();
        set_fg();
        cr->stroke();
      } else {
        // Filled in complement (white on light motif, dark on dark motif),
        // outlined in FG.
        set_bg();
        cr->fill_preserve();
        set_fg();
        cr->stroke();
      }
    }
  }
}

const char *RefPointPicker::preset_name(Preset p) {
  switch (p) {
  case Preset::NW:
    return "NW";
  case Preset::N:
    return "N";
  case Preset::NE:
    return "NE";
  case Preset::W:
    return "W";
  case Preset::C:
    return "C";
  case Preset::E:
    return "E";
  case Preset::SW:
    return "SW";
  case Preset::S:
    return "S";
  case Preset::SE:
    return "SE";
  }
  return "C";
}

bool RefPointPicker::preset_from_name(std::string_view name, Preset *out) {
  if (name == "NW") {
    *out = Preset::NW;
    return true;
  }
  if (name == "N") {
    *out = Preset::N;
    return true;
  }
  if (name == "NE") {
    *out = Preset::NE;
    return true;
  }
  if (name == "W") {
    *out = Preset::W;
    return true;
  }
  if (name == "C") {
    *out = Preset::C;
    return true;
  }
  if (name == "E") {
    *out = Preset::E;
    return true;
  }
  if (name == "SW") {
    *out = Preset::SW;
    return true;
  }
  if (name == "S") {
    *out = Preset::S;
    return true;
  }
  if (name == "SE") {
    *out = Preset::SE;
    return true;
  }
  return false;
}

} // namespace curvz::widgets
