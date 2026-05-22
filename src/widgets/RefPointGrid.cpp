// widgets/RefPointGrid.cpp ────────────────────────────────────────────

#include "widgets/RefPointGrid.hpp"

#include <gtkmm/gestureclick.h>

namespace curvz::widgets {

RefPointGrid::RefPointGrid() {
  set_content_width(36);
  set_content_height(36);
  set_valign(Gtk::Align::CENTER);
  add_css_class("refpoint-grid");
  // s290 back-compat: the old inline-DrawingArea inside RefPointPicker
  // carried this class. Existing CSS targets (include/css.hpp) still
  // bind to it; preserve until the theme is migrated to the new class.
  add_css_class("refpoint-picker-grid");

  set_draw_func([this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
    on_draw_grid(cr, w, h);
  });

  auto click = Gtk::GestureClick::create();
  click->set_button(GDK_BUTTON_PRIMARY);
  click->signal_pressed().connect(
      [this](int /*n_press*/, double x, double y) {
        Preset p;
        if (!pixel_to_preset(x, y, &p))
          return;
        set_preset(p);
      });
  add_controller(click);
}

void RefPointGrid::set_preset(Preset p) {
  if (p == m_preset)
    return;
  m_preset = p;
  queue_draw();
  m_sig_preset_changed.emit(p);
}

void RefPointGrid::set_greyed(bool grey) {
  if (grey == m_greyed)
    return;
  m_greyed = grey;
  queue_draw();
}

bool RefPointGrid::pixel_to_preset(double px, double py, Preset *out_p) const {
  const int W = get_content_width();
  const int H = get_content_height();
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

void RefPointGrid::on_draw_grid(const Cairo::RefPtr<Cairo::Context> &cr,
                                int width, int height) {
  const double margin = 4.0;
  const double iw = width - 2 * margin;
  const double ih = height - 2 * margin;
  if (iw <= 0 || ih <= 0)
    return;

  auto fg = get_color();
  const double fr = fg.get_red();
  const double fg_g = fg.get_green();
  const double fb = fg.get_blue();
  const bool fg_is_light = (fr + fg_g + fb) > 1.5;
  const double bg_r = fg_is_light ? 0.1 : 1.0;
  const double bg_g = fg_is_light ? 0.1 : 1.0;
  const double bg_b = fg_is_light ? 0.1 : 1.0;

  const double a = m_greyed ? 0.4 : 1.0;

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

  // Bisecting cross at 0.5px so dots paint over the intersection.
  cr->save();
  cr->set_line_width(0.5);
  cr->move_to(margin + iw * 0.5, margin);
  cr->line_to(margin + iw * 0.5, margin + ih);
  cr->stroke();
  cr->move_to(margin, margin + ih * 0.5);
  cr->line_to(margin + iw, margin + ih * 0.5);
  cr->stroke();
  cr->restore();

  // 9 dots.
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
      const bool selected = (!m_greyed && p == m_preset);
      cr->rectangle(x - dot, y - dot, dot * 2, dot * 2);
      if (selected) {
        set_fg();
        cr->fill_preserve();
        set_fg();
        cr->stroke();
      } else {
        set_bg();
        cr->fill_preserve();
        set_fg();
        cr->stroke();
      }
    }
  }
}

const char *RefPointGrid::name_of(Preset p) {
  switch (p) {
  case Preset::NW: return "NW";
  case Preset::N:  return "N";
  case Preset::NE: return "NE";
  case Preset::W:  return "W";
  case Preset::C:  return "C";
  case Preset::E:  return "E";
  case Preset::SW: return "SW";
  case Preset::S:  return "S";
  case Preset::SE: return "SE";
  }
  return "C";
}

bool RefPointGrid::from_name(std::string_view name, Preset *out) {
  if (name == "NW") { *out = Preset::NW; return true; }
  if (name == "N")  { *out = Preset::N;  return true; }
  if (name == "NE") { *out = Preset::NE; return true; }
  if (name == "W")  { *out = Preset::W;  return true; }
  if (name == "C")  { *out = Preset::C;  return true; }
  if (name == "E")  { *out = Preset::E;  return true; }
  if (name == "SW") { *out = Preset::SW; return true; }
  if (name == "S")  { *out = Preset::S;  return true; }
  if (name == "SE") { *out = Preset::SE; return true; }
  return false;
}

void RefPointGrid::preset_to_fractions(Preset p, double *frac_x,
                                       double *frac_y) {
  switch (p) {
  case Preset::NW: *frac_x = 0.0; *frac_y = 0.0; return;
  case Preset::N:  *frac_x = 0.5; *frac_y = 0.0; return;
  case Preset::NE: *frac_x = 1.0; *frac_y = 0.0; return;
  case Preset::W:  *frac_x = 0.0; *frac_y = 0.5; return;
  case Preset::C:  *frac_x = 0.5; *frac_y = 0.5; return;
  case Preset::E:  *frac_x = 1.0; *frac_y = 0.5; return;
  case Preset::SW: *frac_x = 0.0; *frac_y = 1.0; return;
  case Preset::S:  *frac_x = 0.5; *frac_y = 1.0; return;
  case Preset::SE: *frac_x = 1.0; *frac_y = 1.0; return;
  }
  *frac_x = 0.5;
  *frac_y = 0.5;
}

} // namespace curvz::widgets
