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
//   [ 36x36 grid ]  [ X spin + unit ]  [ Y spin + unit ]
//
// s286 — checkbox removed. Mode is implicit: clicking a preset on the
// grid → Preset; typing into X or Y → Arbitrary. The grid's selected-
// dot rendering already keys on m_mode so deselection on mode flip is
// automatic.
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
  // s290: the visual grid is now a RefPointGrid child widget. The grid
  // emits signal_preset_changed when the user clicks; we route that
  // through our own set_preset() to keep mode + arbitrary state in sync.
  add_css_class("refpoint-picker");
  // Seed the grid with the current preset and wire the click callback.
  m_grid.set_preset(m_last_preset);
  m_grid.signal_preset_changed().connect([this](RefPointGrid::Preset p) {
    // s290 — any grid click selects that preset. set_preset flips Arbitrary
    // back to Preset internally; the gate that used to live here was a
    // vestige of the s286-removed checkbox.
    set_preset(p);
  });
  append(m_grid);

  // s286 — Arbitrary checkbox removed. Mode is inferred from user
  // action via the spinner on_changed handlers below.

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
      // s286 — user edit drives the mode flip. Seed BOTH m_arb_x and
      // m_arb_y BEFORE set_mode runs so that set_mode's
      // refresh_xy_display call paints back the correct values (the
      // freshly-typed X and the preset Y the user could see on screen
      // before typing). Without the m_arb_y seed, set_mode flips and
      // refresh paints stale m_arb_y (initial 0) over what the user
      // thought was their refpt's Y.
      auto [px, py] = point(); // current preset xy, evaluated pre-flip
      (void)px;
      m_arb_x = v;
      m_arb_y = py;
      if (m_mode != Mode::Arbitrary)
        set_mode(Mode::Arbitrary);
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
      // s286 — see X handler. Seed both from current preset before flip.
      auto [px, py] = point();
      (void)py;
      m_arb_x = px;
      m_arb_y = v;
      if (m_mode != Mode::Arbitrary)
        set_mode(Mode::Arbitrary);
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

  // s286 — apply_mode_appearance removed; spinners are always editable.
  // s290 — the embedded RefPointGrid greys out / deselects on mode flip
  // via set_greyed() in set_mode below.
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
  m_grid.queue_draw();
}

void RefPointPicker::set_mode(Mode m) {
  if (m == m_mode) {
    return;
  }
  // s286 — checkbox removed; no UI widget to sync. The grid redraw at
  // the bottom handles the visual mode change (greyout + deselect).
  //
  // Caveat: Preset → Arbitrary shows whatever m_arb_x/y currently
  // hold, which is (0,0) until the caller sets them via
  // set_arbitrary_xy or a script set_arbitrary. The pivot-popover
  // caller seeds m_arb_x/y to the current pivot position before
  // showing the picker; the user-edit-driven flip seeds them from
  // the current preset point. We deliberately do NOT seed arb
  // from the preset INSIDE set_mode — that would overwrite a user's
  // typed values on a second flip and break the mode-memory contract.
  m_mode = m;
  refresh_xy_display();
  m_sig_mode_changed.emit(m_mode);
  emit("mode_changed",
       ScriptValue::text(m_mode == Mode::Arbitrary ? "arbitrary" : "preset"));
  auto [px, py] = point();
  m_sig_point_changed.emit(px, py);
  m_grid.set_greyed(m_mode == Mode::Arbitrary);
  m_grid.queue_draw();
}

void RefPointPicker::set_preset(Preset p) {
  if (m_mode == Mode::Arbitrary) {
    set_mode(Mode::Preset);
  }
  m_last_preset = p;
  refresh_xy_display();
  auto [px, py] = point();
  m_sig_point_changed.emit(px, py);
  emit("point_changed", ScriptValue::text(RefPointGrid::name_of(p)));
  m_grid.set_preset(p);
  m_grid.queue_draw();
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
    if (!RefPointGrid::from_name(args[0].s, &p))
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
    return ScriptValue::text(RefPointGrid::name_of(m_last_preset));
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

void RefPointPicker::refresh_xy_display() {
  auto [px, py] = point();
  m_loading = true;
  if (m_sp_x)
    m_sp_x->set_internal_value(px);
  if (m_sp_y)
    m_sp_y->set_internal_value(py);
  m_loading = false;
}

} // namespace curvz::widgets
