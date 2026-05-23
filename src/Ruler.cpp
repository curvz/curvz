#include "Ruler.hpp"
#include "CurvzLog.hpp"
#include "UnitSystem.hpp"
#include "curvz_utils.hpp"    // s208 m5 — curvz::utils::set_name
#include "widgets/Button.hpp" // s208 m5 — substrate origin-popover btn
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <gdk/gdkkeysyms.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <string>
#include <vector>

namespace Curvz {

// ── Tick helpers
// ──────────────────────────────────────────────────────────────

// Convert a doc-space position to user-space, accounting for ruler origin.
// user = CoordSpace_flip(doc) - origin
// For X: user_x = doc_x - origin_x   (X doesn't flip)
// For Y: user_y = (canvas_h - doc_y) - origin_y  (Y flips)
static double doc_to_user_x(double doc_x, const RulerState &s) {
  return doc_x - s.ruler_origin_x;
}
static double doc_to_user_y(double doc_y, const RulerState &s) {
  return (s.canvas_h - doc_y) - s.ruler_origin_y;
}

// Convert user-space value to screen pixel position on the ruler.
// For H ruler: screen_x = (doc_x * zoom) + pan_x + (widget_w -
// canvas_w*zoom)*0.5 The canvas is centred in the widget by doc_origin_x/y
// logic in Canvas.cpp
static double user_x_to_screen(double user_x, const RulerState &s) {
  double doc_x = user_x + s.ruler_origin_x;
  double origin_screen_x = (s.widget_w - s.canvas_w * s.zoom) * 0.5 + s.pan_x;
  return doc_x * s.zoom + origin_screen_x;
}
static double user_y_to_screen(double user_y, const RulerState &s) {
  // user_y = (canvas_h - doc_y) - origin_y
  // doc_y  = canvas_h - (user_y + origin_y)
  double doc_y = s.canvas_h - (user_y + s.ruler_origin_y);
  double origin_screen_y = (s.widget_h - s.canvas_h * s.zoom) * 0.5 + s.pan_y;
  return doc_y * s.zoom + origin_screen_y;
}

// Choose a nice tick interval in user-space units given the current zoom.
// min_px: minimum gap in screen pixels between ticks.
struct TickScheme {
  double interval;    // user-space units between major ticks
  int subdivisions;   // minor ticks per major tick
  int decimals;       // decimal places for labels
  std::string suffix; // label suffix ("", "%", "px", "in", "mm")
};

// s265 m2: Helpers for intent-aware ruler math. When the doc has render
// intent set (intended_w/h/unit), the ruler should report in that unit
// regardless of display_mode. This mirrors DocUnits's resolution order
// and gives ruler ticks the same coordinate system every other surface
// reports.
static inline bool ruler_has_intent(const RulerState &s) {
  return s.intended_w > 0.0 && s.intended_h > 0.0 && s.canvas_w > 0.0 &&
         s.canvas_h > 0.0;
}
static inline Unit ruler_intent_unit(const RulerState &s) {
  if (s.intended_unit == "in")
    return Unit::In;
  if (s.intended_unit == "mm")
    return Unit::Mm;
  if (s.intended_unit == "pt")
    return Unit::Pt;
  return Unit::Px;
}

static TickScheme choose_ticks(const RulerState &s, double min_px_major) {
  // s265 m2: intent overrides display_unit/mode. The ruler reports in the
  // unit the user typed for Size (intended_unit), and a "display unit" tick
  // = canvas_w / intended_w doc-units (so a 16-inch poster's tick spacing
  // is one doc-unit per (canvas_w / 16) = 62.5 doc-units for the 1000-wide
  // canvas, giving 16 ticks across the doc.canvas span).
  Unit u = ruler_has_intent(s) ? ruler_intent_unit(s) : s.display_unit;

  // How many screen-px per doc-unit at current zoom
  double px_per_doc = s.zoom;

  // How many doc-units per display-unit:
  //   * intent set:     canvas_w / intended_w (X) or canvas_h / intended_h
  //                     (Y).  choose_ticks doesn't know the axis, but X and
  //                     Y of the same canvas have the same px_per_unit at
  //                     the same zoom because the canvas keeps a fixed
  //                     aspect — and the intent ratio is the same on both
  //                     axes (we set intended_w/intended_h proportionally).
  //                     Using X is fine; Y would yield the same px_per_unit.
  //   * Physical mode:  1 in = quality/phys_short doc-units (legacy path).
  //   * Pixel / Ratio:  UnitSystem::to_px(1, u) (legacy path).
  double doc_per_display = 1.0;
  if (ruler_has_intent(s) && s.intended_w > 0.0) {
    doc_per_display = s.canvas_w / s.intended_w;
  } else if (s.display_mode == DisplayMode::Physical && s.quality > 0 &&
             s.phys_short > 0) {
    doc_per_display = (double)s.quality / s.phys_short;
  } else {
    doc_per_display = UnitSystem::to_px(1.0, u);
  }
  double px_per_unit = px_per_doc * doc_per_display;

  // Nice intervals in display units
  static const double nice_px[] = {1,   2,   5,   10,   20,   50,
                                   100, 200, 500, 1000, 2000, 5000};
  static const double nice_in[] = {0.0625, 0.125, 0.25, 0.5, 1,
                                   2,      5,     10,   20,  50};
  static const double nice_mm[] = {0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500};
  static const double nice_pt[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};

  const double *table = nice_px;
  int table_len = 12;
  if (u == Unit::In) {
    table = nice_in;
    table_len = 10;
  } else if (u == Unit::Mm) {
    table = nice_mm;
    table_len = 10;
  } else if (u == Unit::Pt) {
    table = nice_pt;
    table_len = 10;
  }

  double interval_unit = table[table_len - 1];
  for (int i = 0; i < table_len; ++i) {
    if (table[i] * px_per_unit >= min_px_major) {
      interval_unit = table[i];
      break;
    }
  }

  // Convert interval from display units → doc units for drawing
  double interval_doc = interval_unit * doc_per_display;

  // Decimal places
  int dec = 0;
  if (u == Unit::In)
    dec = (interval_unit < 0.25) ? 3 : (interval_unit < 1.0) ? 2 : 0;
  else if (u == Unit::Mm)
    dec = (interval_unit < 1.0) ? 1 : 0;
  else if (u == Unit::Pt)
    dec = (interval_unit < 1.0) ? 1 : 0;

  return {interval_doc, 5, dec, UnitSystem::label(u)};
}

// Format a display-space value as a label string
static std::string fmt_tick(double val, const TickScheme &ts) {
  char buf[32];
  if (ts.decimals == 0)
    snprintf(buf, sizeof(buf), "%d", (int)std::round(val));
  else
    snprintf(buf, sizeof(buf), "%.*f", ts.decimals, val);
  return buf;
}

// Convert a doc-space tick position to the display unit value for labelling
static double doc_tick_to_display(double doc_tick, const RulerState &s,
                                  bool is_y) {
  double user_val =
      is_y ? doc_to_user_y(doc_tick, s) : doc_to_user_x(doc_tick, s);

  // s265 m2: when intent is set, scale by (intended / canvas) so the label
  // is in the user's typed unit. This is the same rule as DocUnits.
  if (ruler_has_intent(s)) {
    double span_doc = is_y ? s.canvas_h : s.canvas_w;
    double span_user = is_y ? s.intended_h : s.intended_w;
    if (span_doc > 0.0)
      return user_val * (span_user / span_doc);
    return user_val;
  }

  if (s.display_mode == DisplayMode::RatioQuality && s.quality > 0) {
    return user_val; // show raw doc units, same as inspector/status bar
  }
  if (s.display_mode == DisplayMode::Physical && s.quality > 0 &&
      s.phys_short > 0) {
    return user_val / s.quality * s.phys_short;
  }
  return user_val; // Pixel: 1:1
}

// ── Ruler drawing palette ────────────────────────────────────────────────────
// S117 m3: ruler is Cairo-painted, so it can't read CSS tokens. Instead we
// keep two palettes — dark and light — picked at draw time via the per-
// widget m_motif member. Single struct + single accessor; ruler classes
// just call palette_for(m_motif) at the top of their draw and read off it.
//
// Colour mapping mirrors the css.hpp tokens so dark+light rulers blend with
// the rest of the chrome:
//   bg     ← --bg-surface  (matches headerbar / toolbar band)
//   tick   ← --fg-muted    (secondary fg, reads as soft tick lines)
//   text   ← --fg-muted    (same — labels share tick weight)
//   origin ← --accent      (motif-invariant; brand blue reads on both)
//   cursor ← --accent      (same)
struct RulerPalette {
  double bg[3];
  double tick[3];
  double text[3];
  double origin[3];
  double cursor[3];
};
static const RulerPalette kPaletteDark = {
    {0.145, 0.145, 0.145}, // bg     #252525  (--bg-surface dark)
    {0.55, 0.55, 0.55},    // tick   #999999  (--fg-muted dark, eyeballed)
    {0.56, 0.56, 0.56},    // text   #8f8f8f  (--fg-muted dark, eyeballed)
    {0.31, 0.60, 0.94},    // origin #3584e4  (--accent)
    {0.31, 0.60, 0.94},    // cursor #3584e4  (--accent)
};
static const RulerPalette kPaletteLight = {
    {0.866, 0.866, 0.866}, // bg     #dddddd  (--bg-surface light)
    {0.290, 0.290, 0.290}, // tick   #4a4a4a  (--fg-muted light, s117 m4)
    {0.290, 0.290, 0.290}, // text   #4a4a4a  (--fg-muted light, s117 m4)
    {0.31, 0.60, 0.94},    // origin #3584e4  (--accent, motif-invariant)
    {0.31, 0.60, 0.94},    // cursor #3584e4  (--accent, motif-invariant)
};
static const RulerPalette &palette_for(Motif m) {
  return m == Motif::Light ? kPaletteLight : kPaletteDark;
}

// ── Shared unit popover builder
// ─────────────────────────────────────────────── Builds a small popover with
// px/in/mm/pt buttons, attaches it to `widget`, emits `on_unit` when a button
// is clicked. `position` controls which edge of the anchor rect the popover
// pops out from — Bottom suits horizontal rulers, Right suits vertical.
static void build_ruler_unit_popover(
    Gtk::Widget &widget, Gtk::Popover &pop, std::function<void(Unit)> on_unit,
    Gtk::PositionType position = Gtk::PositionType::BOTTOM) {
  static const std::pair<const char *, Unit> k_units[] = {
      {"px", Unit::Px}, {"in", Unit::In}, {"mm", Unit::Mm}, {"pt", Unit::Pt}};

  auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  box->set_spacing(2);
  box->set_margin_top(6);
  box->set_margin_bottom(6);
  box->set_margin_start(6);
  box->set_margin_end(6);

  for (auto &[lbl, unit] : k_units) {
    // s209 m3 — substrate Button with the unregistered tag. The helper
    // is called twice per launch (once per ruler in HRuler/VRuler
    // ctors) and constructs 4 buttons each from the unit-list loop,
    // so a registered abbrev would collide on the second-ruler call
    // (and on every subsequent rebuild). Per-instance script
    // addressability is meaningless here — the unit choice is the
    // affordance, not the individual button. Force-multiplier
    // instance of the s209 m1 pattern: one signature change, eight
    // substrate widgets per app launch.
    auto *btn = Gtk::make_managed<curvz::widgets::Button>(
        curvz::scripting::unregistered, lbl);
    btn->add_css_class("flat");
    btn->add_css_class("tb-type-btn");
    btn->signal_clicked().connect([&pop, on_unit, unit = unit]() {
      pop.popdown();
      on_unit(unit);
    });
    box->append(*btn);
  }

  pop.set_child(*box);
  pop.set_has_arrow(true);
  pop.set_position(position);
  pop.set_parent(widget);
}

// ── HRuler
// ────────────────────────────────────────────────────────────────────
HRuler::HRuler() {
  set_size_request(-1, RULER_SIZE);
  set_draw_func(sigc::mem_fun(*this, &HRuler::on_draw));

  // Drag downward from H ruler → create horizontal guide at that Y position.
  // The guide Y is computed from how far the cursor has dragged into the
  // canvas.
  auto drag = Gtk::GestureDrag::create();
  drag->set_button(1);
  drag->signal_drag_begin().connect([this](double, double) {
    m_dragging = true;
    set_cursor("row-resize");
  });
  drag->signal_drag_update().connect([this, drag](double, double offset_y) {
    if (!m_dragging || offset_y <= 0)
      return;
    // Current cursor Y in ruler-local coords = press_y + offset_y.
    // The canvas widget sits directly below the ruler; its local-Y 0
    // corresponds to ruler-local-Y RULER_SIZE.  So canvas-local Y is
    // (press_y + offset_y) - RULER_SIZE.
    double press_x, press_y;
    drag->get_start_point(press_x, press_y);
    double canvas_local_y = (press_y + offset_y) - RULER_SIZE;
    double oy = (m_state.widget_h - m_state.canvas_h * m_state.zoom) * 0.5 +
                m_state.pan_y;
    double doc_y = (canvas_local_y - oy) / m_state.zoom;
    m_drag_doc_x = doc_y; // reuse field for the guide position
    m_sig_guide_dragging.emit(doc_y);
  });
  drag->signal_drag_end().connect([this, drag](double, double offset_y) {
    m_dragging = false;
    set_cursor("default");
    if (offset_y > 0) {
      double press_x, press_y;
      drag->get_start_point(press_x, press_y);
      double canvas_local_y = (press_y + offset_y) - RULER_SIZE;
      double oy = (m_state.widget_h - m_state.canvas_h * m_state.zoom) * 0.5 +
                  m_state.pan_y;
      double doc_y = (canvas_local_y - oy) / m_state.zoom;
      m_sig_guide_created.emit(doc_y);
    } else {
      m_sig_guide_cancel.emit();
    }
  });
  add_controller(drag);

  // Right-click → unit selector popover. World UI convention is right-click
  // for context menus; the rest of the app (Toolbar tool-button popovers,
  // ruler corner square) uses the same idiom. Right-click coexists cleanly
  // with the button-1 GestureDrag above — distinct mouse buttons, distinct
  // gesture streams, no conflict.
  build_ruler_unit_popover(*this, m_pop,
                           [this](Unit u) { m_sig_unit.emit(u); });
  auto cclick = Gtk::GestureClick::create();
  cclick->set_button(3); // S89: was Ctrl+left, now plain right-click
  cclick->signal_pressed().connect([this](int, double x, double y) {
    // Anchor the popover at the click point so it rises from where the
    // user clicked, not the widget's default bounding-rect anchor.
    Gdk::Rectangle r{(int)x, (int)y, 1, 1};
    m_pop.set_pointing_to(r);
    m_pop.popup();
  });
  add_controller(cclick);
}
void HRuler::set_state(const RulerState &s) {
  m_state = s;
  queue_draw();
}

void HRuler::on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
  const RulerPalette &pal = palette_for(m_motif);
  // Background
  cr->set_source_rgb(pal.bg[0], pal.bg[1], pal.bg[2]);
  cr->paint();

  // Bottom border
  cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
  cr->set_line_width(0.5);
  cr->move_to(0, h - 0.5);
  cr->line_to(w, h - 0.5);
  cr->stroke();

  const double min_major_px = 60.0;
  TickScheme ts = choose_ticks(m_state, min_major_px);
  if (ts.interval <= 0)
    return;

  double ox = (m_state.widget_w - m_state.canvas_w * m_state.zoom) * 0.5 +
              m_state.pan_x;

  // Work in user space so ticks align with origin
  double doc_left = (0.0 - ox) / m_state.zoom;
  double doc_right = ((double)w - ox) / m_state.zoom;
  double user_left = doc_left - m_state.ruler_origin_x;
  double user_right = doc_right - m_state.ruler_origin_x;

  double first_user = std::floor(user_left / ts.interval) * ts.interval;

  // ── HRULER diag (s266 m1 followup) ─────────────────────────────────────
  // Throttled log of the state on_draw is actually using and what it
  // computes from that state. Compare against the RULER diag from
  // MainWindow_helpers::update_rulers to find any divergence between
  // pushed state and consumed state.
  {
    static uint64_t s_last_log_ms = 0;
    auto now_ms = (uint64_t)g_get_monotonic_time() / 1000;
    if (now_ms - s_last_log_ms > 500) {
      s_last_log_ms = now_ms;
      LOG_INFO(
          "HRULER diag: on_draw widget=({}x{}) m_state zoom={:.4g} "
          "pan=({:.1f},{:.1f}) canvas=({:.0f}x{:.0f}) intent=({:.4g}x{:.4g}) "
          "intended_unit='{}' display_unit={} origin={:.1f} "
          "→ ts.interval={:.4g} suffix='{}' user_left={:.4g} user_right={:.4g} "
          "first_user={:.4g}",
          w, h, m_state.zoom, m_state.pan_x, m_state.pan_y, m_state.canvas_w,
          m_state.canvas_h, m_state.intended_w, m_state.intended_h,
          m_state.intended_unit, (int)m_state.display_unit,
          m_state.ruler_origin_x, ts.interval, ts.suffix, user_left, user_right,
          first_user);
    }
  }

  cr->set_font_size(8.0);
  cr->select_font_face("monospace", Cairo::ToyFontFace::Slant::NORMAL,
                       Cairo::ToyFontFace::Weight::NORMAL);

  for (double user_x = first_user; user_x <= user_right + ts.interval;
       user_x += ts.interval) {
    double doc_x = user_x + m_state.ruler_origin_x;
    double sx = doc_x * m_state.zoom + ox;
    if (sx < -2 || sx > w + 2)
      continue;

    // Major tick
    cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
    cr->set_line_width(0.5);
    cr->move_to(sx + 0.5, h * 0.4);
    cr->line_to(sx + 0.5, h);
    cr->stroke();

    // Label — convert doc-space tick to display value (handles physical
    // mode and s265 m2 intent). When intent is set, doc_tick_to_display
    // already returns the right value; only the legacy non-intent
    // non-Physical path needs the extra from_px conversion.
    double display_val = doc_tick_to_display(doc_x, m_state, false);
    if (!ruler_has_intent(m_state) &&
        m_state.display_mode != DisplayMode::Physical)
      display_val = UnitSystem::from_px(user_x, m_state.display_unit);

    std::string lbl = fmt_tick(display_val, ts);
    cr->set_source_rgb(pal.text[0], pal.text[1], pal.text[2]);
    cr->move_to(sx + 2.0, h * 0.75);
    cr->show_text(lbl);

    // Minor ticks
    if (ts.subdivisions > 1) {
      double sub_interval = ts.interval / ts.subdivisions;
      for (int si = 1; si < ts.subdivisions; ++si) {
        double sub_doc_x =
            (user_x + si * sub_interval) + m_state.ruler_origin_x;
        double sx2 = sub_doc_x * m_state.zoom + ox;
        if (sx2 < 0 || sx2 > w)
          continue;
        cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
        cr->set_line_width(0.5);
        cr->move_to(sx2 + 0.5, h * 0.7);
        cr->line_to(sx2 + 0.5, h);
        cr->stroke();
      }
    }
  }

  // Origin marker (blue tick at ruler_origin_x) — always drawn
  {
    double doc_origin_x = m_state.ruler_origin_x;
    double sx = doc_origin_x * m_state.zoom + ox;
    cr->set_source_rgb(pal.origin[0], pal.origin[1], pal.origin[2]);
    cr->set_line_width(1.0);
    cr->move_to(sx + 0.5, 0);
    cr->line_to(sx + 0.5, h);
    cr->stroke();
  }

  // Preview origin marker — dashed, shown during drag
  if (m_state.preview_active) {
    double doc_prev_x = m_state.preview_origin_x;
    double sx = doc_prev_x * m_state.zoom + ox;
    if (sx >= 0 && sx <= w) {
      std::vector<double> dash = {3.0, 3.0};
      cr->set_dash(dash, 0);
      cr->set_source_rgba(1.0, 0.65, 0.0, 0.85);
      cr->set_line_width(1.0);
      cr->move_to(sx + 0.5, 0);
      cr->line_to(sx + 0.5, h);
      cr->stroke();
      cr->set_dash(std::vector<double>{}, 0);
    }
  }

  // Cursor marker line
  {
    double sx = m_state.cursor_doc_x * m_state.zoom + ox;
    if (sx >= 0 && sx <= w) {
      cr->set_source_rgba(pal.cursor[0], pal.cursor[1], pal.cursor[2], 0.6);
      cr->set_line_width(1.0);
      cr->move_to(sx + 0.5, h * 0.5);
      cr->line_to(sx + 0.5, h);
      cr->stroke();
    }
  }
}

// ── VRuler
// ────────────────────────────────────────────────────────────────────
VRuler::VRuler() {
  set_size_request(RULER_SIZE, -1);
  set_draw_func(sigc::mem_fun(*this, &VRuler::on_draw));

  // Drag rightward from V ruler → create vertical guide at that X position.
  auto drag = Gtk::GestureDrag::create();
  drag->set_button(1);
  drag->signal_drag_begin().connect([this](double, double) {
    m_dragging = true;
    set_cursor("col-resize");
  });
  drag->signal_drag_update().connect([this, drag](double offset_x, double) {
    if (!m_dragging || offset_x <= 0)
      return;
    double press_x, press_y;
    drag->get_start_point(press_x, press_y);
    double canvas_local_x = (press_x + offset_x) - RULER_SIZE;
    double ox = (m_state.widget_w - m_state.canvas_w * m_state.zoom) * 0.5 +
                m_state.pan_x;
    double doc_x = (canvas_local_x - ox) / m_state.zoom;
    m_drag_doc_y = doc_x;
    m_sig_guide_dragging.emit(doc_x);
  });
  drag->signal_drag_end().connect([this, drag](double offset_x, double) {
    m_dragging = false;
    set_cursor("default");
    if (offset_x > 0) {
      double press_x, press_y;
      drag->get_start_point(press_x, press_y);
      double canvas_local_x = (press_x + offset_x) - RULER_SIZE;
      double ox = (m_state.widget_w - m_state.canvas_w * m_state.zoom) * 0.5 +
                  m_state.pan_x;
      double doc_x = (canvas_local_x - ox) / m_state.zoom;
      m_sig_guide_created.emit(doc_x);
    } else {
      m_sig_guide_cancel.emit();
    }
  });
  add_controller(drag);

  // Right-click → unit selector popover, popping out to the RIGHT of the
  // ruler at the click's vertical position (the ruler is a tall skinny
  // strip, so BOTTOM positioning would make the popover hang off the top
  // of the ruler — which was the pre-fix behaviour).
  build_ruler_unit_popover(
      *this, m_pop, [this](Unit u) { m_sig_unit.emit(u); },
      Gtk::PositionType::RIGHT);
  auto cclick = Gtk::GestureClick::create();
  cclick->set_button(3); // S89: was Ctrl+left, now plain right-click
  cclick->signal_pressed().connect([this](int, double x, double y) {
    // Anchor at the click — popover arrow points at the click spot;
    // the popover body itself appears to the right of the ruler.
    Gdk::Rectangle r{(int)x, (int)y, 1, 1};
    m_pop.set_pointing_to(r);
    m_pop.popup();
  });
  add_controller(cclick);
}

void VRuler::set_state(const RulerState &s) {
  m_state = s;
  queue_draw();
}

void VRuler::on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
  const RulerPalette &pal = palette_for(m_motif);
  // Background
  cr->set_source_rgb(pal.bg[0], pal.bg[1], pal.bg[2]);
  cr->paint();

  // Right border
  cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
  cr->set_line_width(0.5);
  cr->move_to(w - 0.5, 0);
  cr->line_to(w - 0.5, h);
  cr->stroke();

  const double min_major_px = 40.0;
  TickScheme ts = choose_ticks(m_state, min_major_px);
  if (ts.interval <= 0)
    return;

  double oy = (m_state.widget_h - m_state.canvas_h * m_state.zoom) * 0.5 +
              m_state.pan_y;

  // Work in user space so ticks align with the origin regardless of pan/zoom.
  // Convert screen top/bottom to user-space Y values (Y-up).
  // screen_top → doc_y_top → user_y_top (largest user Y on screen)
  // screen_bottom → doc_y_bottom → user_y_bottom (smallest user Y on screen)
  double doc_top_val = (0.0 - oy) / m_state.zoom;
  double doc_bottom_val = ((double)h - oy) / m_state.zoom;
  double user_top = doc_to_user_y(doc_top_val, m_state); // large positive
  double user_bottom =
      doc_to_user_y(doc_bottom_val, m_state); // small or negative

  // Snap first tick to interval boundary in user space
  // user_bottom is the smallest user Y visible, user_top is largest
  double first_user = std::floor(user_bottom / ts.interval) * ts.interval;

  cr->set_font_size(8.0);
  cr->select_font_face("monospace", Cairo::ToyFontFace::Slant::NORMAL,
                       Cairo::ToyFontFace::Weight::NORMAL);

  for (double user_y = first_user; user_y <= user_top + ts.interval;
       user_y += ts.interval) {
    // Convert user Y back to doc Y, then to screen Y
    double doc_y = m_state.canvas_h - (user_y + m_state.ruler_origin_y);
    double sy = doc_y * m_state.zoom + oy;
    if (sy < -2 || sy > h + 2)
      continue;

    // Major tick
    cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
    cr->set_line_width(0.5);
    cr->move_to(w * 0.4, sy + 0.5);
    cr->line_to(w, sy + 0.5);
    cr->stroke();

    // Label — convert doc-space tick to display value (handles physical
    // mode and s265 m2 intent). When intent is set, doc_tick_to_display
    // already returns the right value; only the legacy non-intent
    // non-Physical path needs the extra from_px conversion.
    double display_val = doc_tick_to_display(doc_y, m_state, true);
    if (!ruler_has_intent(m_state) &&
        m_state.display_mode != DisplayMode::Physical)
      display_val = UnitSystem::from_px(user_y, m_state.display_unit);

    std::string lbl = fmt_tick(display_val, ts);
    cr->save();
    cr->set_source_rgb(pal.text[0], pal.text[1], pal.text[2]);
    cr->translate(w * 0.65, sy - 2.0);
    cr->rotate(-M_PI / 2.0);
    cr->move_to(0, 0);
    cr->show_text(lbl);
    cr->restore();

    // Minor ticks
    if (ts.subdivisions > 1) {
      double sub_interval = ts.interval / ts.subdivisions;
      for (int si = 1; si < ts.subdivisions; ++si) {
        double sub_user_y = user_y + si * sub_interval;
        double sub_doc_y =
            m_state.canvas_h - (sub_user_y + m_state.ruler_origin_y);
        double sy2 = sub_doc_y * m_state.zoom + oy;
        if (sy2 < 0 || sy2 > h)
          continue;
        cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
        cr->set_line_width(0.5);
        cr->move_to(w * 0.7, sy2 + 0.5);
        cr->line_to(w, sy2 + 0.5);
        cr->stroke();
      }
    }
  }

  // Origin marker (blue tick at ruler_origin_y) — always drawn
  {
    double doc_oy = m_state.canvas_h - m_state.ruler_origin_y;
    double sy = doc_oy * m_state.zoom + oy;
    cr->set_source_rgb(pal.origin[0], pal.origin[1], pal.origin[2]);
    cr->set_line_width(1.0);
    cr->move_to(0, sy + 0.5);
    cr->line_to(w - 1.0, sy + 0.5);
    cr->stroke();
  }

  // Preview origin marker — dashed, shown during drag
  if (m_state.preview_active) {
    double doc_prev_y = m_state.canvas_h - m_state.preview_origin_y;
    double sy = doc_prev_y * m_state.zoom + oy;
    if (sy >= 0 && sy <= h) {
      std::vector<double> dash = {3.0, 3.0};
      cr->set_dash(dash, 0);
      cr->set_source_rgba(1.0, 0.65, 0.0, 0.85);
      cr->set_line_width(1.0);
      cr->move_to(0, sy + 0.5);
      cr->line_to(w - 1.0, sy + 0.5);
      cr->stroke();
      cr->set_dash(std::vector<double>{}, 0);
    }
  }

  // Cursor marker line
  {
    double sy = m_state.cursor_doc_y * m_state.zoom + oy;
    if (sy >= 0 && sy <= h) {
      cr->set_source_rgba(pal.cursor[0], pal.cursor[1], pal.cursor[2], 0.6);
      cr->set_line_width(1.0);
      cr->move_to(w * 0.5, sy + 0.5);
      cr->line_to(w - 1.0, sy + 0.5);
      cr->stroke();
    }
  }
}

// ── CornerSquare
// ──────────────────────────────────────────────────────────────
CornerSquare::CornerSquare()
    : m_x_spin(curvz::scripting::unregistered, SpinType::Distance)
    , m_y_spin(curvz::scripting::unregistered, SpinType::Distance)
{
  set_size_request(RULER_SIZE, RULER_SIZE);
  set_draw_func(sigc::mem_fun(*this, &CornerSquare::on_draw));

  build_origin_popover();
  m_pop.set_parent(*this);

  // Drag gesture — drag from corner to set origin approximately
  auto drag = Gtk::GestureDrag::create();
  drag->set_button(1);
  drag->signal_drag_begin().connect([this](double, double) {
    m_dragging = true;
    set_cursor("crosshair");
  });
  drag->signal_drag_update().connect([this](double offset_x, double offset_y) {
    // Emit live preview coords so rulers show dashed line
    double canvas_x = offset_x - RULER_SIZE;
    double canvas_y = offset_y - RULER_SIZE;
    auto [ux, uy] = screen_to_user(canvas_x, canvas_y);
    m_sig_preview.emit(ux, uy);
    queue_draw();
  });
  drag->signal_drag_end().connect([this](double offset_x, double offset_y) {
    m_dragging = false;
    set_cursor("default");
    m_sig_preview_stop.emit();
    // Ignore trivial drags (< 4px) — these are click/double-click artefacts
    double dist = std::sqrt(offset_x * offset_x + offset_y * offset_y);
    if (dist < 4.0) {
      queue_draw();
      return;
    }
    double canvas_x = offset_x - RULER_SIZE;
    double canvas_y = offset_y - RULER_SIZE;
    auto [ux, uy] = screen_to_user(canvas_x, canvas_y);
    m_sig_origin.emit(ux, uy);
    queue_draw();
  });
  add_controller(drag);

  // Click gestures — double-click resets origin, right-click opens dialog.
  // Left-click double-press = reset ruler origin to (0,0). Single-click does
  // nothing (avoids accidental clicks tripping anything).
  auto click = Gtk::GestureClick::create();
  click->set_button(1);
  click->signal_pressed().connect([this](int n_press, double, double) {
    if (n_press == 2) {
      m_sig_reset.emit();
    }
  });
  add_controller(click);

  // Right-click = open ruler origin popover.
  // S89: was Ctrl+left-click on the same gesture; split out into its own
  // right-click gesture for world-consistent context-menu convention.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  rclick->signal_pressed().connect([this](int, double, double) {
    // s290: sync the popover's ephemeral CanvasModel from current ruler state
    // so the spinners convert against the live document. Then prefill the
    // spinners with the current ruler origin (doc-px in / user-Y-doc-px in).
    m_pop_model.quality      = m_state.quality;
    // canvas_width/canvas_height come from ratio_w/ratio_h × quality.
    // m_state has canvas_w/canvas_h directly; derive ratios.
    if (m_state.quality > 0) {
      m_pop_model.ratio_w = m_state.canvas_w / (double)m_state.quality;
      m_pop_model.ratio_h = m_state.canvas_h / (double)m_state.quality;
    }
    m_pop_model.display_mode = m_state.display_mode;
    m_pop_model.display_unit = m_state.display_unit;
    m_pop_model.phys_width   = m_state.phys_short;
    m_pop_model.phys_height  = m_state.phys_short;
    m_pop_model.phys_unit    = m_state.phys_unit;
    m_pop_model.intended_w   = m_state.intended_w;
    m_pop_model.intended_h   = m_state.intended_h;
    m_pop_model.intended_unit = m_state.intended_unit;

    if (m_unit_lbl) {
      Unit u = (m_state.display_mode == DisplayMode::Physical)
                   ? UnitSystem::parse_unit(m_state.phys_unit)
                   : m_state.display_unit;
      m_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    }
    m_x_spin.set_internal_value(m_state.ruler_origin_x);
    m_y_spin.set_internal_value(m_state.ruler_origin_y);
    m_pop.popup();
  });
  add_controller(rclick);
}

void CornerSquare::build_origin_popover() {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(10);
  outer->set_margin_bottom(10);
  outer->set_margin_start(10);
  outer->set_margin_end(10);

  auto *title = Gtk::make_managed<Gtk::Label>("Ruler Origin");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // s290: spinners are CurvzSpinButton::Distance (set in ctor init list) —
  // scale only, no offset or Y-flip. ruler_origin_x is doc-px and
  // ruler_origin_y is user-Y-doc-px (page-relative, Y-up); both are scalars
  // in the same display-unit scale. The popover holds an ephemeral CanvasModel
  // synced from m_state so the spinner's intent-aware pump can convert against
  // the current document.
  m_x_spin.set_model(&m_pop_model);
  m_y_spin.set_model(&m_pop_model);
  m_x_spin.set_digits(2);
  m_y_spin.set_digits(2);
  m_x_spin.set_width_chars(8);
  m_y_spin.set_width_chars(8);

  auto make_row = [&](const std::string &lbl, CurvzSpinButton &spin) {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(6);
    auto *l = Gtk::make_managed<Gtk::Label>(lbl);
    l->set_width_chars(2);
    l->set_xalign(0.0f);
    l->add_css_class("tb-pop-label");
    row->append(*l);
    row->append(spin);
    outer->append(*row);
  };

  make_row("X", m_x_spin);
  make_row("Y", m_y_spin);

  // Commit typed value on Enter — use key controller (GTK4 SpinButton
  // signal_activate unreliable)
  auto make_spin_enter = [this](CurvzSpinButton &spin) {
    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [this, &spin](guint keyval, guint, Gdk::ModifierType) -> bool {
          if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            spin.update();
            m_pop.popdown();
            m_sig_origin.emit(m_x_spin.get_internal_value(),
                              m_y_spin.get_internal_value());
            return false;
          }
          return false;
        },
        false);
    spin.add_controller(kc);
  };
  make_spin_enter(m_x_spin);
  make_spin_enter(m_y_spin);

  m_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_unit_lbl->set_xalign(0.0f);
  m_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_unit_lbl);

  // s208 m5: substrate. CornerSquare is a single-instance member of
  // MainWindow's ruler corner; build_origin_popover runs once per app
  // lifetime, so the substrate registration is unproblematic.
  auto *ok_btn =
      Gtk::make_managed<curvz::widgets::Button>("pop_or_set", "Set Origin");
  curvz::utils::set_name(ok_btn, "pop_or_set", "popover_ruler_origin_set_btn");
  ok_btn->add_css_class("tb-type-btn");
  ok_btn->add_css_class("tb-type-btn-active");
  ok_btn->signal_clicked().connect([this]() {
    m_pop.popdown();
    m_sig_origin.emit(m_x_spin.get_internal_value(),
                      m_y_spin.get_internal_value());
  });
  outer->append(*ok_btn);

  m_pop.set_child(*outer);
  m_pop.set_has_arrow(true);
  m_pop.set_position(Gtk::PositionType::RIGHT);
}

std::pair<double, double> CornerSquare::screen_to_user(double sx,
                                                       double sy) const {
  double ox = (m_state.widget_w - m_state.canvas_w * m_state.zoom) * 0.5 +
              m_state.pan_x;
  double oy = (m_state.widget_h - m_state.canvas_h * m_state.zoom) * 0.5 +
              m_state.pan_y;
  double doc_x = (sx - ox) / m_state.zoom;
  double doc_y = (sy - oy) / m_state.zoom;
  doc_x = std::clamp(doc_x, 0.0, m_state.canvas_w);
  doc_y = std::clamp(doc_y, 0.0, m_state.canvas_h);
  double user_x = doc_x;
  double user_y = m_state.canvas_h - doc_y;
  return {user_x, user_y};
}

void CornerSquare::on_draw(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                           int h) {
  const RulerPalette &pal = palette_for(m_motif);
  // Background matches ruler background
  if (m_dragging)
    cr->set_source_rgb(0.20, 0.35, 0.55); // active — blue tint
  else
    cr->set_source_rgb(pal.bg[0], pal.bg[1], pal.bg[2]);
  cr->paint();

  // Bottom + right borders matching the rulers
  cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
  cr->set_line_width(0.5);
  cr->move_to(0, h - 0.5);
  cr->line_to(w, h - 0.5);
  cr->stroke();
  cr->move_to(w - 0.5, 0);
  cr->line_to(w - 0.5, h);
  cr->stroke();

  // Small crosshair icon in the centre
  double cx = w * 0.5, cy = h * 0.5;
  cr->set_source_rgb(pal.tick[0], pal.tick[1], pal.tick[2]);
  cr->set_line_width(0.75);
  cr->move_to(cx - 3, cy);
  cr->line_to(cx + 3, cy);
  cr->stroke();
  cr->move_to(cx, cy - 3);
  cr->line_to(cx, cy + 3);
  cr->stroke();
}

} // namespace Curvz
