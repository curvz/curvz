#include "Toolbar.hpp"
#include "CurvzLog.hpp"
#include "CurvzSwitch.hpp"
#include "color/SwatchLibrary.hpp"  // S87 m1 — picker section in popovers
#include "color/Swatch.hpp"         // S87 m1 — SolidSwatch resolve
#include "curvz_utils.hpp"          // s118 — curvz::utils::set_name
#include <algorithm>
#include <cairomm/context.h>
#include <cmath>
#include <gdk/gdkkeysyms.h>
#include <glibmm/main.h>
#include <gtkmm/cellrenderer.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/styleprovider.h>
#include <gtkmm/window.h>
#include <iomanip>
#include <iostream>
#include <pango/pangocairo.h>
#include <sstream>

namespace Curvz {

// ── Helper: canvas width/height in display units ──────────────────────────────
// Returns {w, h} in the document's display unit, correctly handling physical
// mode (where the canvas is defined in physical dimensions, not px).
static std::pair<double,double> canvas_display_size(const CurvzDocument* doc) {
    if (!doc) return {1000.0, 1000.0};
    const CanvasModel& cm = doc->canvas;
    if (cm.display_mode == DisplayMode::Physical) {
        // Return physical dimensions directly — popover values are in phys_unit
        return { cm.phys_width, cm.phys_height };
    }
    // Pixel / RatioQuality
    return { UnitSystem::from_px(doc->canvas_width(),  cm.display_unit),
             UnitSystem::from_px(doc->canvas_height(), cm.display_unit) };
}

// ── Hex helpers
// ───────────────────────────────────────────────────────────────
static std::string color_to_hex(double r, double g, double b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", (int)std::round(r * 255),
           (int)std::round(g * 255), (int)std::round(b * 255));
  return buf;
}

static bool hex_to_color(const std::string &hex, double &r, double &g,
                         double &b) {
  std::string s = hex;
  if (!s.empty() && s[0] == '#')
    s = s.substr(1);
  if (s.size() != 6)
    return false;
  try {
    r = std::stoi(s.substr(0, 2), nullptr, 16) / 255.0;
    g = std::stoi(s.substr(2, 2), nullptr, 16) / 255.0;
    b = std::stoi(s.substr(4, 2), nullptr, 16) / 255.0;
    return true;
  } catch (...) {
    return false;
  }
}

// ── Constructor
// ───────────────────────────────────────────────────────────────
Toolbar::Toolbar() : Gtk::Box(Gtk::Orientation::VERTICAL) {
  curvz::utils::set_name(*this, "tb", "main_toolbar_root");
  set_margin_top(4);
  set_margin_bottom(4);
  set_margin_start(2);
  set_margin_end(2);
  set_spacing(2);
  set_size_request(48,
                   -1); // fixed column width — children must fit, not set it
  add_css_class("toolbar-panel");

  // Shared colour-picker popover for fill + stroke wells.
  //
  // IMPORTANT: we attach to the root Gtk::Window, not to `*this`. The
  // Toolbar is a 48px-wide vertical strip on the left edge; GTK
  // constrains popover geometry to its stable-parent's bounds, so
  // parenting to `*this` produced a popover that got squashed or pushed
  // vertically. Attaching to the window gives the popover the whole
  // window as geometry space, while m_well remains the visual anchor
  // (via the anchor rect computed in open()).
  //
  // Deferred to signal_realize because get_root() isn't valid during
  // construction.
  signal_realize().connect([this]() {
    if (auto* w = dynamic_cast<Gtk::Window*>(get_root())) {
      m_color_popover.attach(*w);
    }
  });

  add_tool_button("curvz-select-symbolic", "Select (S)", ActiveTool::Selection);
  curvz::utils::set_name(m_buttons.back(), "tb_sel", "main_toolbar_select_tool_btn");
  add_tool_button("curvz-node-symbolic", "Nodes (N)", ActiveTool::Node);
  curvz::utils::set_name(m_buttons.back(), "tb_nod", "main_toolbar_node_tool_btn");

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(4);
  sep->set_margin_bottom(4);
  append(*sep);

  add_tool_button("curvz-pen-symbolic", "Pen (P)", ActiveTool::Pen);
  curvz::utils::set_name(m_buttons.back(), "tb_pen", "main_toolbar_pen_tool_btn");
  add_tool_button("curvz-rect-symbolic",
                  "Rectangle (R)  —  Right-click to place precisely",
                  ActiveTool::Rect);
  Gtk::ToggleButton *rect_tool_btn = m_buttons.back();
  curvz::utils::set_name(rect_tool_btn, "tb_rec", "main_toolbar_rect_tool_btn");
  add_tool_button("curvz-oval-symbolic",
                  "Ellipse (E)  —  Right-click to place precisely",
                  ActiveTool::Ellipse);
  Gtk::ToggleButton *ellipse_tool_btn = m_buttons.back();
  curvz::utils::set_name(ellipse_tool_btn, "tb_ova", "main_toolbar_ellipse_tool_btn");
  add_tool_button("curvz-line-symbolic",
                  "Line (L)  —  Right-click to place precisely",
                  ActiveTool::Line);
  Gtk::ToggleButton *line_tool_btn = m_buttons.back();
  curvz::utils::set_name(line_tool_btn, "tb_lin", "main_toolbar_line_tool_btn");
  add_tool_button("curvz-ref-symbolic",
                  "Reference Point (F)  —  Right-click to place precisely",
                  ActiveTool::Ref);
  Gtk::ToggleButton *ref_tool_btn = m_buttons.back();
  curvz::utils::set_name(ref_tool_btn, "tb_ref", "main_toolbar_ref_tool_btn");
  add_tool_button("curvz-text-symbolic", "Text (T)  —  Right-click for options",
                  ActiveTool::Text);
  Gtk::ToggleButton *text_tool_btn = m_buttons.back();
  curvz::utils::set_name(text_tool_btn, "tb_txt", "main_toolbar_text_tool_btn");
  add_tool_button("curvz-text-on-path-symbolic",
                  "Text on Path (U)  —  Click text then path to link",
                  ActiveTool::TextOnPath);
  curvz::utils::set_name(m_buttons.back(), "tb_top", "main_toolbar_text_on_path_tool_btn");

  auto *sep2 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep2->set_margin_top(4);
  sep2->set_margin_bottom(4);
  append(*sep2);

  add_tool_button("curvz-eyedropper-symbolic", "Eyedropper (I)",
                  ActiveTool::Eyedropper);
  curvz::utils::set_name(m_buttons.back(), "tb_eyd", "main_toolbar_eyedropper_tool_btn");
  add_tool_button("curvz-corner-symbolic", "Corner Treatment (K)",
                  ActiveTool::Corner);
  m_corner_tool_btn = m_buttons.back();
  curvz::utils::set_name(m_corner_tool_btn, "tb_cor", "main_toolbar_corner_tool_btn");
  add_tool_button("curvz-polygon-symbolic",
                  "Polygon / Star (G)  —  Right-click to configure",
                  ActiveTool::Polygon);
  Gtk::ToggleButton *polygon_tool_btn = m_buttons.back();
  curvz::utils::set_name(polygon_tool_btn, "tb_pol", "main_toolbar_polygon_tool_btn");
  add_tool_button("curvz-spiral-symbolic",
                  "Spiral (W)  —  Right-click to configure", ActiveTool::Spiral);
  Gtk::ToggleButton *spiral_tool_btn = m_buttons.back();
  curvz::utils::set_name(spiral_tool_btn, "tb_spi", "main_toolbar_spiral_tool_btn");
  // Icon stays curvz-ruler-symbolic — the visual glyph is a ruler, fine
  // as an icon. Tool name is Measure; canvas-edge ruler strips are
  // separately still rulers.
  add_tool_button("curvz-ruler-symbolic",
                  "Measure (M)  —  Measure between two nodes; "
                  "right-click to configure",
                  ActiveTool::Measure);
  Gtk::ToggleButton *measure_tool_btn = m_buttons.back();
  curvz::utils::set_name(measure_tool_btn, "tb_meas", "main_toolbar_measure_tool_btn");
  add_tool_button("curvz-zoom-symbolic", "Zoom (Z)  —  Right-click to set level",
                  ActiveTool::Zoom);
  // Zoom tool button is always the last added — capture for popover anchor
  Gtk::ToggleButton *zoom_tool_btn = m_buttons.back();
  curvz::utils::set_name(zoom_tool_btn, "tb_zom", "main_toolbar_zoom_tool_btn");

  // Spacer — everything below floats to the bottom
  auto *spacer = Gtk::make_managed<Gtk::Box>();
  spacer->set_vexpand(true);
  append(*spacer);

  // ── Bottom section: well only ─────────────────────────────────────────
  auto *sep3 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep3->set_margin_top(4);
  sep3->set_margin_bottom(4);
  append(*sep3);

  build_zoom_popover(zoom_tool_btn);
  build_ref_popover(ref_tool_btn);
  build_rect_popover(rect_tool_btn);
  build_ellipse_popover(ellipse_tool_btn);
  build_line_popover(line_tool_btn);
  build_text_popover(text_tool_btn);
  build_polygon_popover(polygon_tool_btn);
  build_spiral_popover(spiral_tool_btn);
  build_measure_popover(measure_tool_btn);   // s150

  if (!m_buttons.empty())
    m_buttons[0]->set_active(true);

  // ── Snap toggle switch — very bottom of toolbar ───────────────────────
  {
    // auto *sep =
    // Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    // sep->set_margin_top(4);
    // sep->set_margin_bottom(2);
    // append(*sep);

    // auto *snap_label = Gtk::make_managed<Gtk::Label>("Snap");
    // snap_label->add_css_class("dim-label");
    // snap_label->set_margin_bottom(2);
    // append(*snap_label);

    curvz::utils::set_name(m_snap_switch, "tb_ss", "main_toolbar_snap_switch");
    m_snap_switch.set_state(true);
    m_snap_switch.set_hexpand(false);
    m_snap_switch.set_vexpand(false);
    m_snap_switch.signal_toggled().connect(
        [this](bool on) { m_sig_snap.emit(on); });
    append(m_snap_switch);
    build_snap_popover(&m_snap_switch);
  }

  build_align_button(); // align & distribute — above snap

  // ── Macro manager button ───────────────────────────────────────────────
  auto *macro_sep =
      Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  macro_sep->set_margin_top(4);
  macro_sep->set_margin_bottom(4);
  append(*macro_sep);

  auto *macro_btn = Gtk::make_managed<Gtk::Button>();
  curvz::utils::set_name(macro_btn, "tb_mb", "main_toolbar_macro_btn");
  macro_btn->set_icon_name("media-record-symbolic");
  // Left-click runs the current macro (Ctrl+M); right-click opens the
  // Macro Manager (Ctrl+Shift+M). MainWindow handles the empty/no-current
  // fallback by opening the manager.
  macro_btn->set_tooltip_text(
      "Run current macro  (Ctrl+M)\nRight-click: Macro Manager  (Ctrl+Shift+M)");
  macro_btn->set_has_frame(false);
  macro_btn->set_halign(Gtk::Align::CENTER);
  macro_btn->set_size_request(40, 40);
  macro_btn->add_css_class("flat");
  macro_btn->signal_clicked().connect([this]() { m_sig_macro_run.emit(); });

  // Right-click → Macro Manager dialog (matches Ctrl+Shift+M).
  auto macro_rclick = Gtk::GestureClick::create();
  macro_rclick->set_button(3);
  macro_rclick->signal_pressed().connect(
      [this](int, double, double) { m_sig_macro.emit(); });
  macro_btn->add_controller(macro_rclick);

  append(*macro_btn);

  auto *sep4 = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep4->set_margin_top(4);
  sep4->set_margin_bottom(4);
  append(*sep4);

  build_defaults_well(); // fill/stroke well

  LOG_DEBUG("Toolbar created with {} tools", m_buttons.size());
  select_tool(ActiveTool::Selection); // apply tool-active CSS class on startup
}

// ── Tool buttons
// ──────────────────────────────────────────────────────────────
void Toolbar::add_tool_button(const char *icon, const char *tooltip,
                              ActiveTool tool) {
  auto *btn = Gtk::make_managed<Gtk::ToggleButton>();
  btn->set_icon_name(icon);
  btn->set_tooltip_text(tooltip);
  btn->set_has_frame(false);
  btn->add_css_class("tool-btn");
  btn->set_halign(Gtk::Align::FILL);
  btn->set_hexpand(false);
  btn->set_size_request(40, 40); // force square — icon centres inside
  btn->signal_clicked().connect([this, btn, tool]() {
    if (btn->get_active()) {
      select_tool(tool);
    } else if (m_active == tool) {
      btn->set_active(true);
      select_tool(tool); // re-apply tool-active class
    }
  });
  append(*btn);
  m_buttons.push_back(btn);
  m_button_tools.push_back(tool);
}

void Toolbar::add_tool_button(Gtk::Picture *pic, const char *tooltip,
                              ActiveTool tool) {
  auto *btn = Gtk::make_managed<Gtk::ToggleButton>();
  btn->set_child(*pic);
  btn->set_tooltip_text(tooltip);
  btn->set_has_frame(true);
  btn->set_halign(Gtk::Align::FILL);
  btn->set_hexpand(false);
  btn->set_size_request(40, 40);
  btn->signal_clicked().connect([this, btn, tool]() {
    if (btn->get_active())
      select_tool(tool);
    else if (m_active == tool)
      btn->set_active(true);
  });
  append(*btn);
  m_buttons.push_back(btn);
  m_button_tools.push_back(tool);
}

void Toolbar::set_active_tool_icon(ActiveTool tool, const char *icon_name) {
  for (size_t i = 0; i < m_button_tools.size(); ++i)
    if (m_button_tools[i] == tool) {
      m_buttons[i]->set_icon_name(icon_name);
      return;
    }
}

void Toolbar::select_tool(ActiveTool tool) {
  m_active = tool;
  for (size_t i = 0; i < m_buttons.size(); ++i) {
    bool active = m_button_tools[i] == tool;
    m_buttons[i]->set_active(active);
    if (active)
      m_buttons[i]->add_css_class("tool-active");
    else
      m_buttons[i]->remove_css_class("tool-active");
    // Apply inline style directly — bypasses Adwaita specificity entirely
    auto provider = Gtk::CssProvider::create();
    if (active) {
      provider->load_from_data(
          "button { background: #15539e; color: white; border-radius: 4px; }"
          "button image { color: white; }"
          "button:hover { background: #1e6abf; color: white; }");
    } else {
      provider->load_from_data(
          "button { background: transparent; color: #7c7c7c; }"
          "button:hover { background: #5e5e5e; color: #cecece; }"
          "button:hover image { color: #cecece; }");
    }
    m_buttons[i]->get_style_context()->add_provider(
        provider, GTK_STYLE_PROVIDER_PRIORITY_USER);
  }
  m_signal_tool_changed.emit(tool);
  LOG_DEBUG("Tool selected: {}", (int)tool);
}

void Toolbar::cycle_tool(int dir) {
  if (m_button_tools.empty()) return;
  int n = (int)m_button_tools.size();
  int cur = 0;
  for (int i = 0; i < n; ++i)
    if (m_button_tools[i] == m_active) { cur = i; break; }
  int next = (cur + dir + n) % n;
  select_tool(m_button_tools[next]);
}

// ── Defaults well — only widget in toolbar column
// ─────────────────────────────
void Toolbar::build_defaults_well() {
  // 30×26 drawing area, centred in the 40px toolbar column.
  // Fill square 14×14 at (0,0), stroke square 14×14 at (8,8).
  // Click top-left region → fill popover; click bottom-right → stroke popover.
  curvz::utils::set_name(m_well, "tb_well", "main_toolbar_defaults_well");
  m_well.set_size_request(30, 26);
  m_well.set_can_target(true);
  m_well.set_halign(Gtk::Align::CENTER);
  m_well.set_tooltip_text("Click fill or stroke square to edit");
  m_well.add_css_class("tb-well");
  redraw_well();

  auto well_click = Gtk::GestureClick::create();
  well_click->set_button(1);
  well_click->signal_pressed().connect(
      [this, &well_click = *well_click](int, double x, double y) {
        // Ctrl+click anywhere → reset to defaults
        Gdk::ModifierType mods = well_click.get_current_event_state();
        if ((mods & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{}) {
          reset_to_defaults();
          return;
        }
        // Top-left region → fill popover; bottom-right → stroke popover
        bool stroke_region = (x >= 16 || y >= 16);
        if (stroke_region)
          m_stroke_pop.popup();
        else
          m_fill_pop.popup();
      });
  m_well.add_controller(well_click);

  build_fill_popover();
  build_stroke_popover();

  m_fill_pop.set_parent(m_well);
  m_stroke_pop.set_parent(m_well);

  // S87 m1 fix5: refresh on every popover open. Replaces the prior
  // child-realize-hooked refresh which fired re-entrantly when the
  // colour row went from hidden→visible during a type-button click.
  // signal_show fires once per popup cycle, before the popover is
  // interactive — refresh lands cleanly with no event in flight.
  m_fill_pop.signal_show().connect([this]() {
    refresh_fill_popover();
  });
  m_stroke_pop.signal_show().connect([this]() {
    refresh_stroke_popover();
    update_cap_buttons();
    update_join_buttons();
  });

  append(m_well);
}

// ── Fill popover
// ──────────────────────────────────────────────────────────────
void Toolbar::build_fill_popover() {
  curvz::utils::set_name(m_fill_pop, "pop_tb_fill", "popover_toolbar_fill_root");
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(10);
  outer->set_margin_bottom(10);
  outer->set_margin_start(10);
  outer->set_margin_end(10);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Fill");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Type row: Solid / None / currentColor / Swatch / Gradient
  // (S87 m1 v2 added Swatch as 4th; S91 added Gradient as 5th)
  auto *type_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  type_row->set_spacing(4);

  curvz::utils::set_name(m_fill_type_solid_btn, "pop_tb_fill_sol", "popover_toolbar_fill_type_solid_toggle");
  m_fill_type_solid_btn.set_label("Solid");
  m_fill_type_solid_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(m_fill_type_none_btn, "pop_tb_fill_non", "popover_toolbar_fill_type_none_toggle");
  m_fill_type_none_btn.set_label("None");
  m_fill_type_none_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(m_fill_type_cc_btn, "pop_tb_fill_cc", "popover_toolbar_fill_type_currentcolor_toggle");
  m_fill_type_cc_btn.set_label("currentColor");
  m_fill_type_cc_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(m_fill_type_swatch_btn, "pop_tb_fill_sw", "popover_toolbar_fill_type_swatch_toggle");
  m_fill_type_swatch_btn.set_label("Swatch");
  m_fill_type_swatch_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(m_fill_type_gradient_btn, "pop_tb_fill_grd", "popover_toolbar_fill_type_gradient_toggle");
  m_fill_type_gradient_btn.set_label("Gradient");
  m_fill_type_gradient_btn.add_css_class("tb-type-btn");

  // S87 m1 fix6: ToggleButton + set_group, mirroring PaintEditor's idiom.
  // The previous Gtk::Button approach hit a GTK4 autohide trap: a click
  // on a plain Button inside an autohide popover bubbles through to the
  // popover's outside-click-dismiss handler and triggers popdown ~3-5 ms
  // later. ToggleButton consumes the click via its toggled-state change
  // in a way GTK doesn't interpret as outside-click. set_group ties the
  // five into a radio: only one is active at a time, GTK auto-deactivates
  // the previous one when a new one activates.
  m_fill_type_none_btn.set_group(m_fill_type_solid_btn);
  m_fill_type_cc_btn.set_group(m_fill_type_solid_btn);
  m_fill_type_swatch_btn.set_group(m_fill_type_solid_btn);
  m_fill_type_gradient_btn.set_group(m_fill_type_solid_btn);

  // signal_toggled fires twice on group changes (deactivate + activate).
  // Guard with get_active() so each handler only runs on the activation
  // edge, not the deactivation. m_syncing guards against re-entry from
  // refresh_fill_popover's programmatic set_active calls. Same idiom
  // as PaintEditor.
  m_fill_type_solid_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_fill_type_solid_btn.get_active()) return;
    m_def_fill.type = FillStyle::Type::Solid;
    m_fill_picker_open = false;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  });
  m_fill_type_none_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_fill_type_none_btn.get_active()) return;
    m_def_fill.type = FillStyle::Type::None;
    m_fill_picker_open = false;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  });
  m_fill_type_cc_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_fill_type_cc_btn.get_active()) return;
    m_def_fill.type = FillStyle::Type::CurrentColor;
    m_fill_picker_open = false;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  });
  // S87 m1 v2: Swatch toggle reveals the chip picker. Doesn't change
  // m_def_fill.type — the toolbar has no Swatch FillStyle case (it's
  // session-only state, and binding wouldn't survive next session
  // anyway, per the copy-only design). When the user clicks a chip,
  // apply_swatch_pick_to_fill snaps the type back to Solid + the
  // chip's RGB, equivalent to a hex paste.
  m_fill_type_swatch_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_fill_type_swatch_btn.get_active()) return;
    m_fill_picker_open = true;
    refresh_fill_popover();
  });

  // S91 Gradient toggle. Snaps m_def_fill into a 2-stop linear gradient
  // when promoting from a non-gradient state; preserves existing stops
  // on a programmatic re-activate. The Edit… button below is what
  // actually opens the editor; toggling Gradient just switches the
  // *kind* of paint that gets used on next placement, with sensible
  // black→white defaults so the well and ramp preview render
  // immediately.
  //
  // No re-emit guard for "already a gradient" — the early-return below
  // covers it. Symmetric with the inspector's signal_type_changed
  // handler that seeds defaults on a fresh promotion.
  m_fill_type_gradient_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_fill_type_gradient_btn.get_active()) return;
    m_fill_picker_open = false;
    if (!m_def_fill.is_gradient()) {
      m_def_fill.type = FillStyle::Type::LinearGradient;
      if (m_def_fill.stops.empty()) {
        GradientStop s0; s0.offset = 0.0;
        s0.r = 0; s0.g = 0; s0.b = 0; s0.a = 1;
        GradientStop s1; s1.offset = 1.0;
        s1.r = 1; s1.g = 1; s1.b = 1; s1.a = 1;
        m_def_fill.stops = { s0, s1 };
      }
      m_def_fill.g_x1 = 0.0; m_def_fill.g_y1 = 0.5;
      m_def_fill.g_x2 = 1.0; m_def_fill.g_y2 = 0.5;
      m_def_fill.g_r  = 0.5;
    }
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  });

  // S91 Edit gradient button. Bubbles up via signal_gradient_edit_
  // requested with an apply_cb that writes the dialog's edit result
  // back into m_def_fill. MainWindow connects the signal to its
  // m_gradient_dialog.show — same dialog the inspector uses.
  //
  // Popdown the fill popover before opening the dialog: GTK4 autohide
  // popovers and modal windows interact poorly otherwise (the popover's
  // grab can fight the dialog's transient parent).
  m_fill_gradient_edit_btn.signal_clicked().connect([this]() {
    if (m_syncing) return;
    FillStyle current = m_def_fill;
    m_fill_pop.popdown();
    auto apply_cb = [this](FillStyle edited) {
      m_def_fill = edited;
      refresh_fill_popover();
      redraw_well();
      emit_defaults();
    };
    m_sig_gradient_edit.emit(current, apply_cb);
  });

  type_row->append(m_fill_type_solid_btn);
  type_row->append(m_fill_type_none_btn);
  type_row->append(m_fill_type_cc_btn);
  type_row->append(m_fill_type_swatch_btn);
  type_row->append(m_fill_type_gradient_btn);
  outer->append(*type_row);

  // Swatch + hex row — promoted to a member pointer so refresh_fill_
  // popover can drive its visibility.
  m_fill_color_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  m_fill_color_row->set_spacing(6);

  curvz::utils::set_name(m_fill_swatch, "pop_tb_fill_ch", "popover_toolbar_fill_color_swatch_da");
  m_fill_swatch.set_size_request(24, 24);
  m_fill_swatch.set_can_target(true);
  m_fill_swatch.add_css_class("tb-swatch");
  m_fill_color_row->append(m_fill_swatch);

  curvz::utils::set_name(m_fill_hex_entry, "pop_tb_fill_hex", "popover_toolbar_fill_hex_entry");
  m_fill_hex_entry.set_max_length(7);
  m_fill_hex_entry.set_width_chars(8);
  m_fill_hex_entry.set_placeholder_text("#RRGGBB");
  m_fill_hex_entry.add_css_class("tb-hex-entry");
  m_fill_hex_entry.on_commit(
      [this]() { apply_hex_to_fill(m_fill_hex_entry.get_text()); });
  m_fill_color_row->append(m_fill_hex_entry);
  outer->append(*m_fill_color_row);

  // S91 Gradient row: [ramp ─────][Edit…]. Visible only when m_def_fill
  // is a gradient. Same idiom as PaintEditor's gradient row — the ramp
  // is non-interactive; the button is the affordance for opening the
  // editor.
  m_fill_gradient_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  m_fill_gradient_row->set_spacing(6);
  curvz::utils::set_name(m_fill_gradient_ramp, "pop_tb_fill_grm", "popover_toolbar_fill_gradient_ramp_da");
  m_fill_gradient_ramp.set_size_request(120, 22);
  m_fill_gradient_ramp.set_hexpand(true);
  m_fill_gradient_ramp.add_css_class("tb-swatch");
  m_fill_gradient_ramp.set_valign(Gtk::Align::CENTER);
  curvz::utils::set_name(m_fill_gradient_edit_btn, "pop_tb_fill_ged", "popover_toolbar_fill_gradient_edit_btn");
  m_fill_gradient_edit_btn.set_label("Edit…");
  m_fill_gradient_edit_btn.set_tooltip_text(
      "Edit gradient stops, type, and angle…");
  m_fill_gradient_row->append(m_fill_gradient_ramp);
  m_fill_gradient_row->append(m_fill_gradient_edit_btn);
  outer->append(*m_fill_gradient_row);
  m_fill_gradient_row->set_visible(false);  // refresh_fill_popover gates

  // Click swatch → open colour picker popover (Solid mode only).
  //
  // The popover is parented to the main window, not to the Toolbar (the
  // Toolbar is a 48px-wide strip with no room to host a picker). We
  // position the popover's arrow at a point in the canvas's lower-left
  // area so it floats over the canvas rather than squashing against the
  // toolbar. That matches the pre-Phase-2 experience where the system
  // Gtk::ColorDialog was a floating window.
  auto swatch_click = Gtk::GestureClick::create();
  swatch_click->set_button(1);
  swatch_click->signal_pressed().connect([this](int, double, double) {
    if (m_def_fill.type != FillStyle::Type::Solid)
      return;
    color::Color initial(m_def_fill.r, m_def_fill.g, m_def_fill.b, 1.0);
    m_fill_pop.popdown();

    // Position the picker with its left edge at the toolbar's right
    // edge, and its bottom near the window's bottom. We use a 1x1
    // anchor at exactly that corner and let GTK expand the popover
    // body up-and-right from there. This reliably places the picker
    // in the canvas's lower-left area without the arrow ambiguity
    // that was making it drift to x=0.
    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) return;
    const int win_h = root->get_height();
    const int tb_w  = get_width();
    Gdk::Rectangle anchor(tb_w, std::max(0, win_h - 8), 1, 1);

    m_color_popover.open(
        anchor, initial, /*with_alpha=*/false,
        [this](const color::Color& c) {
          m_def_fill.type = FillStyle::Type::Solid;
          m_def_fill.r = c.r;
          m_def_fill.g = c.g;
          m_def_fill.b = c.b;
          refresh_fill_popover();
          redraw_well();
          emit_defaults();
        },
        /*has_arrow=*/false);
  });
  m_fill_swatch.add_controller(swatch_click);

  // S87 m1: copy-only swatch picker section, appended after the colour
  // row. Section is hidden until set_swatch_library() is called.
  build_swatch_picker_section(*outer, /*is_stroke=*/false);

  // Swatch draw func
  m_fill_swatch.set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        if (m_fill_mixed) {
          // S58n: diagonal-stripe swatch for mixed multi-selection.
          cr->set_source_rgb(1.0, 1.0, 1.0);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->save();
          cr->rectangle(0, 0, w, h);
          cr->clip();
          cr->set_source_rgb(0.55, 0.55, 0.55);
          cr->set_line_width(2.0);
          const double span = (double)(w + h) * 1.5;
          const double step = 5.0;
          cr->translate(w * 0.5, h * 0.5);
          cr->rotate(M_PI / 4.0);
          cr->translate(-span * 0.5, -span * 0.5);
          for (double yy = 0; yy < span; yy += step) {
            cr->move_to(0, yy);
            cr->line_to(span, yy);
          }
          cr->stroke();
          cr->restore();
        } else if (m_def_fill.type == FillStyle::Type::Solid) {
          cr->set_source_rgb(m_def_fill.r, m_def_fill.g, m_def_fill.b);
          cr->rectangle(0, 0, w, h);
          cr->fill();
        } else if (m_def_fill.type == FillStyle::Type::None) {
          // White background + red slash — clearly visible
          cr->set_source_rgb(0.92, 0.92, 0.92);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->set_source_rgb(0.85, 0.18, 0.18);
          cr->set_line_width(2.0);
          cr->move_to(2, 2);
          cr->line_to(w - 2, h - 2);
          cr->stroke();
        } else {
          // currentColor — checkerboard (white + mid-gray), no frame.
          // Fill swatch has no border for currentColor — stroke swatch
          // is the one that gets a visible border.
          const int cs = 2;  // 2px squares
          for (int row = 0; row < h; row += cs)
            for (int col = 0; col < w; col += cs) {
              bool light = ((row / cs + col / cs) % 2 == 0);
              if (light)
                cr->set_source_rgb(1.0, 1.0, 1.0);
              else
                cr->set_source_rgb(0.55, 0.55, 0.55);
              cr->rectangle(col, row, cs, cs);
              cr->fill();
            }
        }
        cr->set_source_rgba(0, 0, 0, 0.45);
        cr->set_line_width(1.0);
        cr->rectangle(0.5, 0.5, w - 1, h - 1);
        cr->stroke();
      });

  m_fill_pop.set_child(*outer);
  m_fill_pop.set_has_arrow(true);
  m_fill_pop.set_position(Gtk::PositionType::RIGHT);

  // S87 m1 fix5: previously the initial refresh was hooked to
  // m_fill_swatch.signal_realize(). That worked in v1 (when the swatch
  // was always visible from popover creation) but broke in v2 — the
  // colour row is now hidden when type != Solid, so the swatch doesn't
  // realize until the user picks Solid; the realize handler then fires
  // a re-entrant refresh during the click event, which destabilises
  // GTK's autohide popover grab and causes the popover to dismiss in
  // the next idle cycle (~3 ms later — confirmed via diag1 logs).
  // The refresh is now driven by the popover's own signal_show hook
  // (wired further up in the ctor next to the diag log connections),
  // which fires once per popup with no re-entrancy and no realize-
  // dependent timing.
}

void Toolbar::refresh_fill_popover() {
  // S87 m1 fix6: programmatic ToggleButton state changes run inside
  // m_syncing so the signal_toggled handlers bail (they re-check
  // m_syncing at the top). Pattern lifted from PaintEditor.
  m_syncing = true;

  // Update type button styles AND active state. The CSS class drives
  // the visual highlight (with the .tb-type-btn-active CSS rule from
  // fix4); set_active drives the GTK ToggleButton state which the
  // radio group uses to enforce one-active-at-a-time. Both must be
  // kept in sync.
  auto set_active = [](Gtk::ToggleButton &btn, bool on) {
    if (on)
      btn.add_css_class("tb-type-btn-active");
    else
      btn.remove_css_class("tb-type-btn-active");
    btn.set_active(on);
  };

  // Type-button active states.
  if (m_fill_mixed) {
    set_active(m_fill_type_solid_btn,    false);
    set_active(m_fill_type_none_btn,     false);
    set_active(m_fill_type_cc_btn,       false);
    set_active(m_fill_type_swatch_btn,   false);
    set_active(m_fill_type_gradient_btn, false);
  } else if (m_fill_picker_open) {
    set_active(m_fill_type_solid_btn,    false);
    set_active(m_fill_type_none_btn,     false);
    set_active(m_fill_type_cc_btn,       false);
    set_active(m_fill_type_swatch_btn,   true);
    set_active(m_fill_type_gradient_btn, false);
  } else {
    set_active(m_fill_type_solid_btn,
               m_def_fill.type == FillStyle::Type::Solid);
    set_active(m_fill_type_none_btn,
               m_def_fill.type == FillStyle::Type::None);
    set_active(m_fill_type_cc_btn,
               m_def_fill.type == FillStyle::Type::CurrentColor);
    set_active(m_fill_type_swatch_btn,   false);
    set_active(m_fill_type_gradient_btn, m_def_fill.is_gradient());
  }

  // Colour row visibility — hidden for gradients (gradient row owns
  // the chip-area real estate, same as PaintEditor).
  const bool show_color_row =
      m_fill_mixed
      || (!m_fill_picker_open
          && m_def_fill.type == FillStyle::Type::Solid);
  if (m_fill_color_row) m_fill_color_row->set_visible(show_color_row);

  // S91 Gradient row visibility — visible iff paint is a gradient and
  // the popover isn't displaying the swatch picker. Mixed-selection
  // suppresses it (the user clicks Gradient on the type row to snap
  // everyone to a default gradient first).
  if (m_fill_gradient_row) {
    const bool show_gradient_row =
        !m_fill_mixed
        && !m_fill_picker_open
        && m_def_fill.is_gradient();
    m_fill_gradient_row->set_visible(show_gradient_row);
    if (show_gradient_row) {
      // Repaint the ramp from current stops. Capture-by-value matches
      // PaintEditor::apply_gradient_row.
      std::vector<GradientStop> stops = m_def_fill.stops;
      std::sort(stops.begin(), stops.end(),
                [](const GradientStop &a, const GradientStop &b) {
                    return a.offset < b.offset;
                });
      m_fill_gradient_ramp.set_draw_func(
          [stops](const Cairo::RefPtr<Cairo::Context> &cr,
                  int w, int h) {
            if (stops.empty()) {
              cr->set_source_rgb(0.5, 0.5, 0.5);
              cr->rectangle(0, 0, w, h);
              cr->fill();
            } else if (stops.size() == 1) {
              const auto &s0 = stops.front();
              cr->set_source_rgba(s0.r, s0.g, s0.b, s0.a);
              cr->rectangle(0, 0, w, h);
              cr->fill();
            } else {
              auto pat = Cairo::LinearGradient::create(0, 0, w, 0);
              for (const auto &st : stops) {
                pat->add_color_stop_rgba(
                    std::clamp(st.offset, 0.0, 1.0),
                    st.r, st.g, st.b, st.a);
              }
              cr->set_source(pat);
              cr->rectangle(0, 0, w, h);
              cr->fill();
            }
            cr->set_source_rgba(0, 0, 0, 0.4);
            cr->set_line_width(1.0);
            cr->rectangle(0.5, 0.5, w - 1, h - 1);
            cr->stroke();
          });
      m_fill_gradient_ramp.queue_draw();
    }
  }

  // Hex entry contents.
  if (m_fill_mixed) {
    m_fill_hex_entry.set_text("");
    m_fill_hex_entry.set_placeholder_text("mixed");
    m_fill_hex_entry.set_sensitive(true);
  } else {
    m_fill_hex_entry.set_placeholder_text("#RRGGBB");
    if (m_def_fill.type == FillStyle::Type::Solid)
      m_fill_hex_entry.set_text(
          color_to_hex(m_def_fill.r, m_def_fill.g, m_def_fill.b));
    else
      m_fill_hex_entry.set_text("");
    m_fill_hex_entry.set_sensitive(m_def_fill.type == FillStyle::Type::Solid);
  }

  // Picker section visibility.
  if (m_fill_picker_section) {
    const bool show_picker =
        m_fill_picker_open
        && (m_swatch_library != nullptr)
        && (m_swatch_library->swatch_count() > 0)
        && !m_fill_mixed;
    m_fill_picker_section->set_visible(show_picker);
  }

  // Swatch button sensitivity.
  const bool swatch_enabled =
      (m_swatch_library != nullptr)
      && (m_swatch_library->swatch_count() > 0);
  m_fill_type_swatch_btn.set_sensitive(swatch_enabled);

  if (m_fill_swatch.get_realized())
    m_fill_swatch.queue_draw();

  m_syncing = false;
}

void Toolbar::apply_hex_to_fill(const std::string &hex) {
  // S93 m4: defensive m_syncing guard — same shape as the m_width_adj
  // handler. Hex entries commit on focus-leave, which can fire during
  // a refresh that replaces the entry's text via set_text(). Pre-S93
  // a stray commit during refresh would have leaked an emit_defaults
  // call. Belt-and-suspenders since the L1074 fix already plugs the
  // observed width-spin leak.
  if (m_syncing) return;
  double r, g, b;
  if (hex_to_color(hex, r, g, b)) {
    m_def_fill.type = FillStyle::Type::Solid;
    m_def_fill.r = r;
    m_def_fill.g = g;
    m_def_fill.b = b;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
  }
}

// ── Stroke popover
// ────────────────────────────────────────────────────────────
void Toolbar::build_stroke_popover() {
  curvz::utils::set_name(m_stroke_pop, "pop_tb_strk", "popover_toolbar_stroke_root");
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(10);
  outer->set_margin_bottom(10);
  outer->set_margin_start(10);
  outer->set_margin_end(10);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Stroke");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Type row
  auto *type_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  type_row->set_spacing(4);

  curvz::utils::set_name(m_stroke_type_solid_btn, "pop_tb_strk_sol", "popover_toolbar_stroke_type_solid_toggle");
  m_stroke_type_solid_btn.set_label("Solid");
  m_stroke_type_solid_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(m_stroke_type_none_btn, "pop_tb_strk_non", "popover_toolbar_stroke_type_none_toggle");
  m_stroke_type_none_btn.set_label("None");
  m_stroke_type_none_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(m_stroke_type_cc_btn, "pop_tb_strk_cc", "popover_toolbar_stroke_type_currentcolor_toggle");
  m_stroke_type_cc_btn.set_label("currentColor");
  m_stroke_type_cc_btn.add_css_class("tb-type-btn");
  curvz::utils::set_name(m_stroke_type_swatch_btn, "pop_tb_strk_sw", "popover_toolbar_stroke_type_swatch_toggle");
  m_stroke_type_swatch_btn.set_label("Swatch");
  m_stroke_type_swatch_btn.add_css_class("tb-type-btn");

  // S87 m1 fix6: ToggleButton radio group, mirroring the fill side and
  // PaintEditor's idiom. Plain Gtk::Button inside an autohide popover
  // dismissed the popover on click — see fill-side comment for details.
  // S91: stroke deliberately has no Gradient toggle (fill-only surface).
  m_stroke_type_none_btn.set_group(m_stroke_type_solid_btn);
  m_stroke_type_cc_btn.set_group(m_stroke_type_solid_btn);
  m_stroke_type_swatch_btn.set_group(m_stroke_type_solid_btn);

  m_stroke_type_solid_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_stroke_type_solid_btn.get_active()) return;
    m_def_stroke.paint.type = FillStyle::Type::Solid;
    m_stroke_picker_open = false;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
  });
  m_stroke_type_none_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_stroke_type_none_btn.get_active()) return;
    m_def_stroke.paint.type = FillStyle::Type::None;
    m_stroke_picker_open = false;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
  });
  m_stroke_type_cc_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_stroke_type_cc_btn.get_active()) return;
    m_def_stroke.paint.type = FillStyle::Type::CurrentColor;
    m_stroke_picker_open = false;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
  });
  m_stroke_type_swatch_btn.signal_toggled().connect([this]() {
    if (m_syncing || !m_stroke_type_swatch_btn.get_active()) return;
    m_stroke_picker_open = true;
    refresh_stroke_popover();
  });

  type_row->append(m_stroke_type_solid_btn);
  type_row->append(m_stroke_type_none_btn);
  type_row->append(m_stroke_type_cc_btn);
  type_row->append(m_stroke_type_swatch_btn);
  outer->append(*type_row);

  // Swatch + hex — promoted to a member pointer so refresh_stroke_
  // popover can drive its visibility.
  m_stroke_color_row =
      Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  m_stroke_color_row->set_spacing(6);

  curvz::utils::set_name(m_stroke_swatch, "pop_tb_strk_ch", "popover_toolbar_stroke_color_swatch_da");
  m_stroke_swatch.set_size_request(24, 24);
  m_stroke_swatch.set_can_target(true);
  m_stroke_swatch.add_css_class("tb-swatch");
  m_stroke_color_row->append(m_stroke_swatch);

  curvz::utils::set_name(m_stroke_hex_entry, "pop_tb_strk_hex", "popover_toolbar_stroke_hex_entry");
  m_stroke_hex_entry.set_max_length(7);
  m_stroke_hex_entry.set_width_chars(8);
  m_stroke_hex_entry.set_placeholder_text("#RRGGBB");
  m_stroke_hex_entry.add_css_class("tb-hex-entry");
  m_stroke_hex_entry.on_commit(
      [this]() { apply_hex_to_stroke(m_stroke_hex_entry.get_text()); });
  m_stroke_color_row->append(m_stroke_hex_entry);
  outer->append(*m_stroke_color_row);

  // S91: stroke does NOT get a gradient row (fill-only surface).

  // Click swatch → open colour picker popover (Solid mode only). Same
  // canvas-lower-left positioning as the fill swatch — see that handler
  // for the reasoning.
  auto swatch_click = Gtk::GestureClick::create();
  swatch_click->set_button(1);
  swatch_click->signal_pressed().connect([this](int, double, double) {
    if (m_def_stroke.paint.type != FillStyle::Type::Solid)
      return;
    color::Color initial(m_def_stroke.paint.r, m_def_stroke.paint.g,
                         m_def_stroke.paint.b, 1.0);
    m_stroke_pop.popdown();

    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    if (!root) return;
    const int win_h = root->get_height();
    const int tb_w  = get_width();
    Gdk::Rectangle anchor(tb_w, std::max(0, win_h - 8), 1, 1);

    m_color_popover.open(
        anchor, initial, /*with_alpha=*/false,
        [this](const color::Color& c) {
          m_def_stroke.paint.type = FillStyle::Type::Solid;
          m_def_stroke.paint.r = c.r;
          m_def_stroke.paint.g = c.g;
          m_def_stroke.paint.b = c.b;
          refresh_stroke_popover();
          redraw_well();
          emit_defaults();
        },
        /*has_arrow=*/false);
  });
  m_stroke_swatch.add_controller(swatch_click);

  m_stroke_swatch.set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        const auto &p = m_def_stroke.paint;
        if (m_stroke_mixed) {
          // S58n: diagonal-stripe swatch for mixed multi-selection, plus a
          // dark inset frame to preserve the "stroke" visual signature.
          cr->set_source_rgb(1.0, 1.0, 1.0);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->save();
          cr->rectangle(0, 0, w, h);
          cr->clip();
          cr->set_source_rgb(0.55, 0.55, 0.55);
          cr->set_line_width(2.0);
          const double span = (double)(w + h) * 1.5;
          const double step = 5.0;
          cr->translate(w * 0.5, h * 0.5);
          cr->rotate(M_PI / 4.0);
          cr->translate(-span * 0.5, -span * 0.5);
          for (double yy = 0; yy < span; yy += step) {
            cr->move_to(0, yy);
            cr->line_to(span, yy);
          }
          cr->stroke();
          cr->restore();
          cr->set_source_rgb(0.102, 0.102, 0.102);
          cr->set_line_width(2.0);
          cr->rectangle(1.0, 1.0, w - 2, h - 2);
          cr->stroke();
        } else if (p.type == FillStyle::Type::Solid) {
          cr->set_source_rgb(p.r, p.g, p.b);
          cr->rectangle(0, 0, w, h);
          cr->fill();
        } else if (p.type == FillStyle::Type::None) {
          // White background + red slash
          cr->set_source_rgb(0.92, 0.92, 0.92);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          cr->set_source_rgb(0.85, 0.18, 0.18);
          cr->set_line_width(2.0);
          cr->move_to(2, 2);
          cr->line_to(w - 2, h - 2);
          cr->stroke();
        } else {
          // currentColor — checkerboard interior + visible near-black
          // border (2px, inset).  The border is the "stroke" signal;
          // the checkerboard is the "currentColor" signal.
          const int cs = 2;  // 2px squares
          for (int row = 0; row < h; row += cs)
            for (int col = 0; col < w; col += cs) {
              bool light = ((row / cs + col / cs) % 2 == 0);
              if (light)
                cr->set_source_rgb(1.0, 1.0, 1.0);
              else
                cr->set_source_rgb(0.55, 0.55, 0.55);
              cr->rectangle(col, row, cs, cs);
              cr->fill();
            }
          // Near-black stroke-ness frame (#1a1a1a)
          cr->set_source_rgb(0.102, 0.102, 0.102);
          cr->set_line_width(2.0);
          cr->rectangle(1.0, 1.0, w - 2, h - 2);
          cr->stroke();
        }
        cr->set_source_rgba(0, 0, 0, 0.45);
        cr->set_line_width(1.0);
        cr->rectangle(0.5, 0.5, w - 1, h - 1);
        cr->stroke();
      });

  // S87 m1: copy-only swatch picker section, between the colour row and
  // the thickness/cap/join area. Section is hidden until
  // set_swatch_library() is called.
  build_swatch_picker_section(*outer, /*is_stroke=*/true);

  // Separator
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer->append(*sep);

  // Thickness row
  auto *width_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  width_row->set_spacing(6);
  m_width_label.set_text("Thickness (px):");
  m_width_label.set_xalign(0.0f);
  m_width_label.add_css_class("tb-pop-label");
  m_width_label.set_hexpand(true);
  width_row->append(m_width_label);

  m_width_adj =
      Gtk::Adjustment::create(m_def_stroke.width, 0.0, 9999.0, 0.5, 5.0);
  curvz::utils::set_name(m_width_spin, "pop_tb_strk_w", "popover_toolbar_stroke_width_spn");
  m_width_spin.set_adjustment(m_width_adj);
  m_width_spin.set_digits(1);
  m_width_spin.set_width_chars(5);
  m_width_spin.add_css_class("tb-well-spin");
  m_width_adj->signal_value_changed().connect([this]() {
    // S93 m4: defensive m_syncing guard. Programmatic set_value
    // calls during a refresh fire this signal synchronously; without
    // the guard, a sync_from_selection (which calls refresh_stroke_
    // popover, which set_value's the adjustment) leaked an
    // emit_defaults call back to the host. That in turn pushed a
    // spurious 'Edit appearance' undo entry on every selection
    // change and — in multi-select — bled the primary's fill/stroke
    // onto siblings via MainWindow's defaults_changed handler. The
    // refresh function was also fixed to keep m_syncing true through
    // its width-update section; this guard is defence-in-depth.
    if (m_syncing) return;
    const CanvasModel* cm = m_doc ? &m_doc->canvas : nullptr;
    double display = m_width_adj->get_value();
    if (cm && cm->display_mode == DisplayMode::Physical) {
      double short_phys = std::min(cm->phys_width, cm->phys_height);
      int q = std::max(1, cm->quality);
      m_def_stroke.width = short_phys > 0 ? (display / short_phys) * q : display;
    } else if (cm && cm->display_mode == DisplayMode::RatioQuality) {
      int q = std::max(1, cm->quality);
      m_def_stroke.width = (display / 100.0) * q;
    } else {
      m_def_stroke.width = display;
    }
    emit_defaults();
  });
  m_width_spin.signal_activate().connect([this]() {
    m_width_spin.update();
    m_stroke_pop.popdown();
    m_sig_canvas_focus.emit();
  });
  // Key controller belt-and-suspenders for SpinButton inside popover
  {
    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
          if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            m_width_spin.update();
            m_stroke_pop.popdown();
            m_sig_canvas_focus.emit();
            return false;
          }
          return false;
        },
        false);
    m_width_spin.add_controller(kc);
  }
  width_row->append(m_width_spin);
  m_width_unit_lbl.set_text("px");
    m_width_label.set_text("Thickness (px):");
  m_width_unit_lbl.add_css_class("prop-width-unit");
  width_row->append(m_width_unit_lbl);
  outer->append(*width_row);

  // Cap row
  auto make_section = [&](const std::string &lbl_text) -> Gtk::Box * {
    auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_spacing(3);
    auto *lbl = Gtk::make_managed<Gtk::Label>(lbl_text);
    lbl->set_xalign(0.0f);
    lbl->add_css_class("tb-pop-label");
    vbox->append(*lbl);
    auto *hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hbox->set_spacing(8);
    vbox->append(*hbox);
    outer->append(*vbox);
    return hbox;
  };

  // Helper: create an icon button for cap/join
  auto make_icon_btn = [](const char* icon, const char* tip) -> Gtk::Button* {
    auto *btn = Gtk::make_managed<Gtk::Button>();
    btn->set_icon_name(icon);
    btn->set_has_frame(false);
    btn->add_css_class("tb-icon-btn");
    btn->set_tooltip_text(tip);
    return btn;
  };

  auto *cap_row = make_section("Cap");
  m_cap_butt_btn   = make_icon_btn("curvz-cap-butt-symbolic",   "Butt");
  curvz::utils::set_name(m_cap_butt_btn, "pop_tb_strk_cb", "popover_toolbar_stroke_cap_butt_btn");
  m_cap_round_btn  = make_icon_btn("curvz-cap-round-symbolic",  "Round");
  curvz::utils::set_name(m_cap_round_btn, "pop_tb_strk_cr", "popover_toolbar_stroke_cap_round_btn");
  m_cap_square_btn = make_icon_btn("curvz-cap-square-symbolic", "Square");
  curvz::utils::set_name(m_cap_square_btn, "pop_tb_strk_cs", "popover_toolbar_stroke_cap_square_btn");
  m_cap_butt_btn->signal_clicked().connect([this]() {
    m_def_stroke.cap = LineCap::Butt;
    update_cap_buttons();
    emit_defaults();
  });
  m_cap_round_btn->signal_clicked().connect([this]() {
    m_def_stroke.cap = LineCap::Round;
    update_cap_buttons();
    emit_defaults();
  });
  m_cap_square_btn->signal_clicked().connect([this]() {
    m_def_stroke.cap = LineCap::Square;
    update_cap_buttons();
    emit_defaults();
  });
  cap_row->append(*m_cap_butt_btn);
  cap_row->append(*m_cap_round_btn);
  cap_row->append(*m_cap_square_btn);

  auto *join_row = make_section("Join");
  m_join_miter_btn = make_icon_btn("curvz-join-miter-symbolic", "Miter");
  curvz::utils::set_name(m_join_miter_btn, "pop_tb_strk_jm", "popover_toolbar_stroke_join_miter_btn");
  m_join_round_btn = make_icon_btn("curvz-join-round-symbolic", "Round");
  curvz::utils::set_name(m_join_round_btn, "pop_tb_strk_jr", "popover_toolbar_stroke_join_round_btn");
  m_join_bevel_btn = make_icon_btn("curvz-join-bevel-symbolic", "Bevel");
  curvz::utils::set_name(m_join_bevel_btn, "pop_tb_strk_jb", "popover_toolbar_stroke_join_bevel_btn");
  m_join_miter_btn->signal_clicked().connect([this]() {
    m_def_stroke.join = LineJoin::Miter;
    update_join_buttons();
    emit_defaults();
  });
  m_join_round_btn->signal_clicked().connect([this]() {
    m_def_stroke.join = LineJoin::Round;
    update_join_buttons();
    emit_defaults();
  });
  m_join_bevel_btn->signal_clicked().connect([this]() {
    m_def_stroke.join = LineJoin::Bevel;
    update_join_buttons();
    emit_defaults();
  });
  join_row->append(*m_join_miter_btn);
  join_row->append(*m_join_round_btn);
  join_row->append(*m_join_bevel_btn);

  m_stroke_pop.set_child(*outer);
  m_stroke_pop.set_has_arrow(true);
  m_stroke_pop.set_position(Gtk::PositionType::RIGHT);

  // S87 m1 fix5: see fill_pop equivalent comment. Refresh is driven by
  // the popover's signal_show hook, not a child realize hook — moves
  // the refresh out of the click event's idle window and avoids the
  // re-entrant autohide-dismiss bug.
}

void Toolbar::refresh_stroke_popover() {
  // S87 m1 fix6: m_syncing wraps programmatic ToggleButton state to
  // avoid signal_toggled re-entry. See refresh_fill_popover for the
  // matching pattern.
  m_syncing = true;

  auto set_active = [](Gtk::ToggleButton &btn, bool on) {
    if (on)
      btn.add_css_class("tb-type-btn-active");
    else
      btn.remove_css_class("tb-type-btn-active");
    btn.set_active(on);
  };
  const auto &p = m_def_stroke.paint;

  // S87 m1 v2: visibility model — see refresh_fill_popover for the
  // matrix. The thickness / cap / join sub-area is independent and
  // always visible (those properties belong to the stroke regardless
  // of whether the picker tab is open).
  if (m_stroke_mixed) {
    set_active(m_stroke_type_solid_btn,  false);
    set_active(m_stroke_type_none_btn,   false);
    set_active(m_stroke_type_cc_btn,     false);
    set_active(m_stroke_type_swatch_btn, false);
  } else if (m_stroke_picker_open) {
    set_active(m_stroke_type_solid_btn,  false);
    set_active(m_stroke_type_none_btn,   false);
    set_active(m_stroke_type_cc_btn,     false);
    set_active(m_stroke_type_swatch_btn, true);
  } else {
    set_active(m_stroke_type_solid_btn,  p.type == FillStyle::Type::Solid);
    set_active(m_stroke_type_none_btn,   p.type == FillStyle::Type::None);
    set_active(m_stroke_type_cc_btn,     p.type == FillStyle::Type::CurrentColor);
    set_active(m_stroke_type_swatch_btn, false);
  }

  // Colour row visibility.
  const bool show_color_row =
      m_stroke_mixed
      || (!m_stroke_picker_open && p.type == FillStyle::Type::Solid);
  if (m_stroke_color_row) m_stroke_color_row->set_visible(show_color_row);

  // S91: no stroke gradient row to manage (fill-only gradient surface).

  // Hex entry contents.
  if (m_stroke_mixed) {
    m_stroke_hex_entry.set_text("");
    m_stroke_hex_entry.set_placeholder_text("mixed");
    m_stroke_hex_entry.set_sensitive(true);
  } else {
    m_stroke_hex_entry.set_placeholder_text("#RRGGBB");
    if (p.type == FillStyle::Type::Solid)
      m_stroke_hex_entry.set_text(color_to_hex(p.r, p.g, p.b));
    else
      m_stroke_hex_entry.set_text("");
    m_stroke_hex_entry.set_sensitive(p.type == FillStyle::Type::Solid);
  }

  // Picker section visibility.
  if (m_stroke_picker_section) {
    const bool show_picker =
        m_stroke_picker_open
        && (m_swatch_library != nullptr)
        && (m_swatch_library->swatch_count() > 0)
        && !m_stroke_mixed;
    m_stroke_picker_section->set_visible(show_picker);
  }

  // Greyed-out Swatch button when no library wired or empty.
  const bool swatch_enabled =
      (m_swatch_library != nullptr)
      && (m_swatch_library->swatch_count() > 0);
  m_stroke_type_swatch_btn.set_sensitive(swatch_enabled);

  if (m_stroke_swatch.get_realized())
    m_stroke_swatch.queue_draw();

  // S93 m4: m_syncing must remain true through the rest of the
  // function. Pre-S93 the flag flipped to false here, BEFORE the
  // m_width_adj->set_value() calls below — those set_value calls
  // fire signal_value_changed synchronously, the handler at the
  // adjustment definition (currently L1059) had no m_syncing check,
  // and so every selection-change leaked an emit_defaults() call
  // ~30ms later. That signal in turn triggered MainWindow's
  // defaults_changed handler, which broadcasts the toolbar's
  // primary-fed fill/stroke onto every selected target — bleeding
  // a Compound's fill onto its multi-select sibling, stripping the
  // Compound's bound_style, and pushing a spurious 'Edit appearance'
  // command to the undo stack on every selection change.
  //
  // The flag is now flipped at the genuine end of the function,
  // wrapping the thickness / cap / join updates too. The
  // m_width_adj signal_value_changed handler also got a defensive
  // m_syncing guard (see comment at the connect site below).

  // Thickness — convert from doc units to display units
  const CanvasModel* cm = m_doc ? &m_doc->canvas : nullptr;
  if (cm && cm->display_mode == DisplayMode::Physical) {
    double short_phys = std::min(cm->phys_width, cm->phys_height);
    int q = std::max(1, cm->quality);
    double display = short_phys > 0 ? (m_def_stroke.width / q) * short_phys : m_def_stroke.width;
    double step = (cm->phys_unit == "in") ? 0.001 : 0.01;
    m_width_adj->set_step_increment(step);
    m_width_adj->set_page_increment(step * 10.0);
    m_width_adj->set_value(display);
    m_width_spin.set_digits(3);
    m_width_unit_lbl.set_text(cm->phys_unit);
    m_width_label.set_text("Thickness (" + cm->phys_unit + "):");
  } else if (cm && cm->display_mode == DisplayMode::RatioQuality) {
    int q = std::max(1, cm->quality);
    m_width_adj->set_step_increment(0.05);
    m_width_adj->set_page_increment(0.5);
    m_width_adj->set_value((m_def_stroke.width / q) * 100.0);
    m_width_spin.set_digits(2);
    m_width_unit_lbl.set_text("%");
    m_width_label.set_text("Thickness (%):");
  } else {
    m_width_adj->set_step_increment(0.5);
    m_width_adj->set_page_increment(5.0);
    m_width_adj->set_value(m_def_stroke.width);
    m_width_spin.set_digits(1);
    m_width_unit_lbl.set_text("px");
    m_width_label.set_text("Thickness (px):");
  }
  update_cap_buttons();
  update_join_buttons();

  m_syncing = false;
}

void Toolbar::apply_hex_to_stroke(const std::string &hex) {
  // S93 m4: defensive m_syncing guard. See apply_hex_to_fill.
  if (m_syncing) return;
  double r, g, b;
  if (hex_to_color(hex, r, g, b)) {
    m_def_stroke.paint.type = FillStyle::Type::Solid;
    m_def_stroke.paint.r = r;
    m_def_stroke.paint.g = g;
    m_def_stroke.paint.b = b;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
  }
}

// ── Cap / Join icon draw funcs
// ─────────────────────────────────────────────────

void Toolbar::update_cap_buttons() {
  auto set_active = [](Gtk::Button* btn, bool active) {
    if (!btn) return;
    if (active) btn->add_css_class("tb-icon-btn-active");
    else        btn->remove_css_class("tb-icon-btn-active");
  };
  set_active(m_cap_butt_btn,   m_def_stroke.cap == LineCap::Butt);
  set_active(m_cap_round_btn,  m_def_stroke.cap == LineCap::Round);
  set_active(m_cap_square_btn, m_def_stroke.cap == LineCap::Square);
}

void Toolbar::update_join_buttons() {
  auto set_active = [](Gtk::Button* btn, bool active) {
    if (!btn) return;
    if (active) btn->add_css_class("tb-icon-btn-active");
    else        btn->remove_css_class("tb-icon-btn-active");
  };
  set_active(m_join_miter_btn, m_def_stroke.join == LineJoin::Miter);
  set_active(m_join_round_btn, m_def_stroke.join == LineJoin::Round);
  set_active(m_join_bevel_btn, m_def_stroke.join == LineJoin::Bevel);
}

// ── Well draw
// ─────────────────────────────────────────────────────────────────
void Toolbar::redraw_well() {
  FillStyle fill = m_def_fill;
  StrokeStyle stroke = m_def_stroke;
  bool fill_mixed = m_fill_mixed;
  bool stroke_mixed = m_stroke_mixed;

  m_well.set_draw_func([fill, stroke, fill_mixed, stroke_mixed]
                       (const Cairo::RefPtr<Cairo::Context> &cr,
                        int /*w*/, int /*h*/) {
    const double sq = 14.0, off = 8.0;

    // Draw a checkerboard (white + mid-gray) inside [x, y] of size sz.
    auto draw_checker = [&](double x, double y, double sz) {
      const int cs = 2;  // 2px squares
      for (int r = 0; r < (int)sz; r += cs)
        for (int c = 0; c < (int)sz; c += cs) {
          bool light = ((r / cs + c / cs) % 2 == 0);
          if (light)
            cr->set_source_rgb(1.0, 1.0, 1.0);
          else
            cr->set_source_rgb(0.55, 0.55, 0.55);
          cr->rectangle(x + c, y + r, cs, cs);
          cr->fill();
        }
    };

    // S58n: Diagonal-stripe swatch to indicate mixed paint across a
    // multi-selection. Matches the Inspector Appearance mixed renderer.
    auto draw_mixed = [&](double x, double y, double sz) {
      cr->set_source_rgb(1.0, 1.0, 1.0);
      cr->rectangle(x, y, sz, sz);
      cr->fill();
      cr->save();
      cr->rectangle(x, y, sz, sz);
      cr->clip();
      cr->set_source_rgb(0.55, 0.55, 0.55);
      cr->set_line_width(1.5);
      const double span = sz * 2.0;
      const double step = 3.0;
      cr->translate(x + sz * 0.5, y + sz * 0.5);
      cr->rotate(M_PI / 4.0);
      cr->translate(-span * 0.5, -span * 0.5);
      for (double yy = 0; yy < span; yy += step) {
        cr->move_to(0, yy);
        cr->line_to(span, yy);
      }
      cr->stroke();
      cr->restore();
    };

    // S91 helper: render a small horizontal gradient ramp inside [x,y,sz].
    // Used by both fill and stroke when the corresponding paint is a
    // linear or radial gradient. We always paint a 1D ramp here even
    // for radial — the well preview is too small to show 2D structure
    // meaningfully; stop ordering is the useful signal.
    auto draw_gradient = [&](double x, double y, double sz,
                             const FillStyle &fs) {
      // Sort defensively so reversed lists still render in canonical
      // order (mirrors PaintEditor::apply_gradient_row).
      std::vector<GradientStop> sorted = fs.stops;
      std::sort(sorted.begin(), sorted.end(),
                [](const GradientStop &a, const GradientStop &b) {
                    return a.offset < b.offset;
                });
      if (sorted.empty()) {
        cr->set_source_rgb(0.5, 0.5, 0.5);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      } else if (sorted.size() == 1) {
        const auto &s0 = sorted.front();
        cr->set_source_rgba(s0.r, s0.g, s0.b, s0.a);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      } else {
        auto pat = Cairo::LinearGradient::create(x, 0, x + sz, 0);
        for (const auto &st : sorted) {
          pat->add_color_stop_rgba(
              std::clamp(st.offset, 0.0, 1.0),
              st.r, st.g, st.b, st.a);
        }
        cr->set_source(pat);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      }
    };

    // Fill-style square at (x,y) size sz.
    auto draw_fill_square = [&](double x, double y, double sz,
                                const FillStyle &fs, bool mixed) {
      if (mixed) {
        draw_mixed(x, y, sz);
      } else if (fs.type == FillStyle::Type::CurrentColor) {
        draw_checker(x, y, sz);
      } else if (fs.type == FillStyle::Type::None) {
        cr->set_source_rgb(0.92, 0.92, 0.92);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      } else if (fs.is_gradient()) {
        draw_gradient(x, y, sz, fs);
      } else {
        cr->set_source_rgb(fs.r, fs.g, fs.b);
        cr->rectangle(x, y, sz, sz);
        cr->fill();
      }
    };

    auto draw_none_slash = [&](double x, double y, double sz) {
      cr->set_source_rgb(0.85, 0.18, 0.18);
      cr->set_line_width(1.5);
      cr->move_to(x + 2, y + 2);
      cr->line_to(x + sz - 2, y + sz - 2);
      cr->stroke();
    };

    // Stroke square — drawn first (behind fill).
    if (stroke_mixed) {
      // Mixed stroke: diagonal stripes with the dark interior flavour.
      draw_mixed(off, off, sq);
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->set_line_width(2.0);
      cr->rectangle(off + 0.5, off + 0.5, sq - 1, sq - 1);
      cr->stroke();
    } else if (stroke.paint.type == FillStyle::Type::Solid) {
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->rectangle(off, off, sq, sq);
      cr->fill();
      cr->set_source_rgb(stroke.paint.r, stroke.paint.g, stroke.paint.b);
      cr->set_line_width(2.0);
      cr->rectangle(off + 0.5, off + 0.5, sq - 1, sq - 1);
      cr->stroke();
    } else if (stroke.paint.type == FillStyle::Type::None) {
      cr->set_source_rgb(0.92, 0.92, 0.92);
      cr->rectangle(off, off, sq, sq);
      cr->fill();
      draw_none_slash(off, off, sq);
    } else if (stroke.paint.is_gradient()) {
      // S91 gradient stroke preview. Fill the square with a ramp the
      // same way fill does, then draw the dark inner background to
      // emulate the "stroke around fill" frame visual. The stroke
      // version of the well overlay is just a colour signal — using
      // the gradient as the colour is the right read.
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->rectangle(off, off, sq, sq);
      cr->fill();
      // Draw the gradient as a 2px-thick frame: render the gradient
      // into a slightly-inset square, then punch out the inner. We
      // approximate this with a fill + an inset cover-up so the visual
      // matches the Solid case's stroke-frame idiom.
      draw_gradient(off, off, sq, stroke.paint);
      cr->set_source_rgb(0.18, 0.18, 0.18);
      cr->rectangle(off + 2, off + 2, sq - 4, sq - 4);
      cr->fill();
    } else {
      // currentColor: checkerboard + near-black stroke-signal frame.
      draw_checker(off, off, sq);
      cr->set_source_rgb(0.102, 0.102, 0.102);
      cr->set_line_width(1.5);
      cr->rectangle(off + 0.75, off + 0.75, sq - 1.5, sq - 1.5);
      cr->stroke();
    }

    // Fill square — drawn on top.
    draw_fill_square(0, 0, sq, fill, fill_mixed);
    if (!fill_mixed && fill.type == FillStyle::Type::None)
      draw_none_slash(0, 0, sq);
    cr->set_source_rgba(0, 0, 0, 0.55);
    cr->set_line_width(1.0);
    cr->rectangle(0.5, 0.5, sq - 1, sq - 1);
    cr->stroke();
  });
  if (m_well.get_realized())
    m_well.queue_draw();
}

void Toolbar::set_snap_enabled(bool enabled) {
  if (m_snap_switch.get_state() != enabled)
    m_snap_switch.set_state(enabled);
}

void Toolbar::set_zoom(double zoom_rel) {
  m_zoom = zoom_rel;
  // Keep popover spin in sync so it shows current value when opened
  if (m_zoom_adj)
    m_zoom_adj->set_value(std::round(zoom_rel * 100.0));
}

void Toolbar::set_zoom_alt(bool) {
  // Zoom +/- buttons removed — nothing to update
}

void Toolbar::set_popup_unit(Unit u) {
  m_popup_unit = u;
  std::string txt = std::string("Units: ") + UnitSystem::label(u);
  if (m_rect_unit_lbl)    m_rect_unit_lbl->set_text(txt);
  if (m_ellipse_unit_lbl) m_ellipse_unit_lbl->set_text(txt);
  if (m_line_unit_lbl)    m_line_unit_lbl->set_text(txt);
  if (m_ref_unit_lbl)     m_ref_unit_lbl->set_text(txt);
  if (m_poly_unit_lbl)    m_poly_unit_lbl->set_text(txt);
  if (m_spiral_unit_lbl)  m_spiral_unit_lbl->set_text(txt);
  if (m_text_unit_lbl)    m_text_unit_lbl->set_text(txt);
}

void Toolbar::set_document(CurvzDocument* doc) {
  m_doc = doc;
  if (doc) {
    // In physical mode the effective unit for popovers is phys_unit
    if (doc->canvas.display_mode == DisplayMode::Physical) {
      m_popup_unit = UnitSystem::parse_unit(doc->canvas.phys_unit);
    } else {
      m_popup_unit = doc->canvas.display_unit;
    }
    set_popup_unit(m_popup_unit);
  }
}

void Toolbar::reset_to_defaults() {
  m_def_fill.type = FillStyle::Type::CurrentColor;
  m_def_stroke.paint.type = FillStyle::Type::None;
  m_def_stroke.width = 1.0;
  m_def_stroke.cap = LineCap::Butt;
  m_def_stroke.join = LineJoin::Miter;
  redraw_well();
  refresh_fill_popover();
  refresh_stroke_popover();
  update_cap_buttons();
  update_join_buttons();
  emit_defaults();
}

void Toolbar::emit_defaults() {
  if (m_syncing)
    return;
  m_sig_defaults.emit(m_def_fill, m_def_stroke);
}

void Toolbar::sync_from_object(const FillStyle &fill,
                               const StrokeStyle &stroke) {
  m_syncing = true;
  m_def_fill = fill;
  m_def_stroke = stroke;
  m_fill_mixed = false;
  m_stroke_mixed = false;
  redraw_well();
  refresh_fill_popover();
  refresh_stroke_popover();
  update_cap_buttons();
  update_join_buttons();
  m_syncing = false;
}

// ══════════════════════════════════════════════════════════════════════════════
// S58n: multi-select aware sync. Uniformity check mirrors the Inspector's
// rule (S58i): compare FillStyle at 8-bit hex granularity so two paint-paths
// that display the same hex are equal. Paint targets are Paths + Compounds +
// Text (matches the toolbar broadcast target set in MainWindow). Groups are
// pass-through containers per user rule and are flattened via recursion.
// ══════════════════════════════════════════════════════════════════════════════
namespace {

bool tb_fills_equal(const FillStyle& a, const FillStyle& b) {
    if (a.type != b.type) return false;
    if (a.type == FillStyle::Type::Solid) {
        auto q = [](double v) {
            return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
        };
        return q(a.r) == q(b.r) && q(a.g) == q(b.g) && q(a.b) == q(b.b);
    }
    // S91: gradient comparison — same-typed gradients are equal iff they
    // have matching stops (offset + RGBA at 8-bit granularity) and
    // matching geometry. This isn't ideal for mixed-but-similar
    // gradients (e.g. same colours, slightly different angles —
    // "mixed" still feels right) but matches the spirit of the Solid
    // 8-bit-hex bucketing.
    if (a.is_gradient()) {
        if (a.stops.size() != b.stops.size()) return false;
        auto q = [](double v) {
            return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
        };
        for (std::size_t i = 0; i < a.stops.size(); ++i) {
            const auto& sa = a.stops[i];
            const auto& sb = b.stops[i];
            if (q(sa.offset) != q(sb.offset)) return false;
            if (q(sa.r) != q(sb.r)) return false;
            if (q(sa.g) != q(sb.g)) return false;
            if (q(sa.b) != q(sb.b)) return false;
            if (q(sa.a) != q(sb.a)) return false;
        }
        // Geometry compared at the same 8-bit granularity as colours
        // (objectBoundingBox space is 0..1, so the bucket count is the
        // same). Equality on all five fields, which covers both linear
        // (uses x1/y1/x2/y2) and radial (uses x1/y1 fx,fy / x2/y2 cx,cy
        // / r).
        if (q(a.g_x1) != q(b.g_x1)) return false;
        if (q(a.g_y1) != q(b.g_y1)) return false;
        if (q(a.g_x2) != q(b.g_x2)) return false;
        if (q(a.g_y2) != q(b.g_y2)) return false;
        if (q(a.g_r)  != q(b.g_r))  return false;
    }
    return true;
}

// Walk a selection and gather its paint targets (Path / Compound / Text).
// Groups are recursed into. Mirrors the collect() lambda in MainWindow's
// signal_defaults_changed handler (S58h) so the display and the broadcast
// see the same object set.
void tb_collect_paint_targets(SceneNode* node,
                              std::vector<SceneNode*>& out) {
    if (!node) return;
    if (node->is_path() || node->is_compound() ||
        node->type == SceneNode::Type::Text) {
        out.push_back(node);
    } else if (node->is_group()) {
        for (auto &child : node->children)
            tb_collect_paint_targets(child.get(), out);
    }
}

// ── S87 m1 chip helpers ──────────────────────────────────────────────────────
//
// Mirrors PaintEditor's anonymous-namespace helpers (paint_chip,
// resolve_solid, kAllPaletteId). Duplicated rather than promoted to a
// shared header because (a) it's small, (b) promoting would leak Cairo
// out of a widget header that's currently GTK-only, and (c) the chip
// metrics are intentionally tunable per host (PaintEditor uses 18px;
// here we match StylesPanel's 24px to match the more recent visual
// convention).
//
// If a future milestone extracts a shared SwatchChipGrid widget, both
// PaintEditor and Toolbar can switch to it and these can go.

constexpr const char* kAllPaletteId = "__all__";

constexpr int    TB_CHIP_SIZE                  = 24;
constexpr double TB_CHIP_CORNER_RADIUS         = 3.0;
constexpr double TB_CHIP_BORDER_GREY           = 0.55;
constexpr double TB_CHIP_BORDER_WIDTH          = 1.0;
constexpr double TB_CHIP_BORDER_LUMA_THRESHOLD = 0.85;

void tb_paint_chip(const Cairo::RefPtr<Cairo::Context>& cr,
                   int w, int h,
                   const color::Color& c) {
    const double r = TB_CHIP_CORNER_RADIUS;
    const double x = 0.5;
    const double y = 0.5;
    const double ww = static_cast<double>(w) - 1.0;
    const double hh = static_cast<double>(h) - 1.0;

    auto rounded_rect = [&](double x0, double y0,
                            double x1, double y1, double rad) {
        cr->move_to(x0 + rad, y0);
        cr->line_to(x1 - rad, y0);
        cr->arc(x1 - rad, y0 + rad, rad, -M_PI / 2, 0);
        cr->line_to(x1, y1 - rad);
        cr->arc(x1 - rad, y1 - rad, rad, 0, M_PI / 2);
        cr->line_to(x0 + rad, y1);
        cr->arc(x0 + rad, y1 - rad, rad, M_PI / 2, M_PI);
        cr->line_to(x0, y0 + rad);
        cr->arc(x0 + rad, y0 + rad, rad, M_PI, 3 * M_PI / 2);
        cr->close_path();
    };

    rounded_rect(x, y, x + ww, y + hh, r);
    cr->set_source_rgba(c.r, c.g, c.b, c.a);
    cr->fill_preserve();

    double luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    if (luma > TB_CHIP_BORDER_LUMA_THRESHOLD || c.a < 0.95) {
        cr->set_source_rgba(TB_CHIP_BORDER_GREY, TB_CHIP_BORDER_GREY,
                            TB_CHIP_BORDER_GREY, 1.0);
        cr->set_line_width(TB_CHIP_BORDER_WIDTH);
        cr->stroke();
    } else {
        cr->begin_new_path();
    }
    // No active-binding ring on copy-only chips — toolbar has no
    // notion of "the bound swatch". Click is a copy, full stop.
}

// Resolve a swatch id to its display name + colour. Returns false
// (no fields written) if missing or non-solid. Future gradient
// swatches need their own visit case.
bool tb_resolve_solid(const color::SwatchLibrary& lib,
                      const std::string& id,
                      std::string& name_out,
                      color::Color& color_out) {
    const color::Swatch* sw = lib.find_swatch(id);
    if (!sw) return false;
    const auto* solid = std::get_if<color::SolidSwatch>(sw);
    if (!solid) return false;
    name_out  = solid->header.name;
    color_out = solid->color;
    return true;
}

} // anonymous namespace

void Toolbar::sync_from_selection(const std::vector<SceneNode*>& sel,
                                   SceneNode* primary) {
    if (!primary) {
        // Nothing selected — don't change well state (keep whatever the
        // defaults currently are) but clear the mixed flags so a stale
        // stripe render doesn't persist.
        m_fill_mixed = false;
        m_stroke_mixed = false;
        redraw_well();
        refresh_fill_popover();
        refresh_stroke_popover();
        return;
    }

    // Expand the selection to paint targets (Groups flatten; Paths /
    // Compounds / Text stay terminal).
    std::vector<SceneNode*> targets;
    for (SceneNode* n : sel) tb_collect_paint_targets(n, targets);

    // Display reads from primary (Path → itself; Compound → itself per
    // S58d rule; a Group primary → first leaf in its tree).
    SceneNode* disp = primary;
    if (primary->is_group()) {
        std::vector<SceneNode*> flat;
        tb_collect_paint_targets(primary, flat);
        if (!flat.empty()) disp = flat.front();
    }

    m_syncing = true;
    m_def_fill   = disp->fill;
    m_def_stroke = disp->stroke;

    // Uniformity check. One target or none → not mixed.
    m_fill_mixed = false;
    m_stroke_mixed = false;
    if (targets.size() >= 2) {
        const FillStyle& first_fill   = targets[0]->fill;
        const FillStyle& first_stroke = targets[0]->stroke.paint;
        for (size_t i = 1; i < targets.size(); ++i) {
            if (!m_fill_mixed &&
                !tb_fills_equal(first_fill, targets[i]->fill))
                m_fill_mixed = true;
            if (!m_stroke_mixed &&
                !tb_fills_equal(first_stroke, targets[i]->stroke.paint))
                m_stroke_mixed = true;
            if (m_fill_mixed && m_stroke_mixed) break; // both decided
        }
    }

    redraw_well();
    refresh_fill_popover();
    refresh_stroke_popover();
    update_cap_buttons();
    update_join_buttons();
    m_syncing = false;
}

// ── Align & Distribute
// ────────────────────────────────────────────────────────

// Draw a single align/distribute icon into a 32×32 area.
// Uses the same Cairo-icon pattern as cap/join buttons.
// S117 m5: align icon was hardcoded for dark motif (light icon on dark
// surface). On light motif (icon on #dddddd toolbar) the same #e0e0e0
// rects vanished. Same problem class as the ruler chrome — Cairo paint
// can't read CSS tokens, so we keep two palettes and pick at draw time.
// Mirrors RulerPalette in src/Ruler.cpp.
struct AlignIconPalette {
  double fg_enabled;   // rect fill when button is enabled
  double fg_disabled;  // rect fill when button is disabled (faded)
  double ref;          // reference line / tick colour
};
static const AlignIconPalette kAlignDark  = {0.88, 0.50, 0.38};
static const AlignIconPalette kAlignLight = {0.20, 0.55, 0.40};
static const AlignIconPalette &align_palette_for(Motif m) {
  return m == Motif::Light ? kAlignLight : kAlignDark;
}

static void draw_align_icon(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                            int h, AlignOp op, bool enabled, Motif motif) {
  const AlignIconPalette &pal = align_palette_for(motif);
  double fg = enabled ? pal.fg_enabled : pal.fg_disabled;
  double ref_col = pal.ref;

  cr->set_antialias(Cairo::Antialias::ANTIALIAS_DEFAULT);

  // Helper: filled rect
  auto rect = [&](double x, double y, double rw, double rh) {
    cr->rectangle(x, y, rw, rh);
    cr->fill();
  };

  switch (op) {
  case AlignOp::AlignLeft: {
    // Vertical reference line on left; two rects of different widths flush left
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(5.5, 4);
    cr->line_to(5.5, h - 4);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(6, 8, 16, 6);
    rect(6, 18, 10, 6);
    break;
  }
  case AlignOp::AlignCenterH: {
    // Vertical reference line in centre; rects centred on it
    double cx = w * 0.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(cx, 4);
    cr->line_to(cx, h - 4);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(cx - 8, 8, 16, 6);
    rect(cx - 5, 18, 10, 6);
    break;
  }
  case AlignOp::AlignRight: {
    double rx = w - 5.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(rx, 4);
    cr->line_to(rx, h - 4);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(rx - 16, 8, 16, 6);
    rect(rx - 10, 18, 10, 6);
    break;
  }
  case AlignOp::AlignTop: {
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(4, 5.5);
    cr->line_to(w - 4, 5.5);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(8, 6, 6, 14);
    rect(18, 6, 6, 9);
    break;
  }
  case AlignOp::AlignCenterV: {
    double cy = h * 0.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(4, cy);
    cr->line_to(w - 4, cy);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(8, cy - 7, 6, 14);
    rect(18, cy - 4, 6, 9);
    break;
  }
  case AlignOp::AlignBottom: {
    double by = h - 5.5;
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.5);
    cr->move_to(4, by);
    cr->line_to(w - 4, by);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    rect(8, by - 14, 6, 14);
    rect(18, by - 9, 6, 9);
    break;
  }
  case AlignOp::DistributeH: {
    // Three vertical bars with equal gaps indicated by arrows/bars
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.0);
    // Left + right edge ticks
    cr->move_to(4.5, 6);
    cr->line_to(4.5, h - 6);
    cr->stroke();
    cr->move_to(w - 4.5, 6);
    cr->line_to(w - 4.5, h - 6);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    // Three equal-width rects spaced evenly
    double bw = 5, gap = (w - 8 - 3 * bw) / 2.0;
    for (int i = 0; i < 3; ++i)
      rect(4 + i * (bw + gap), 8, bw, h - 16);
    break;
  }
  case AlignOp::DistributeV: {
    cr->set_source_rgb(ref_col, ref_col, ref_col);
    cr->set_line_width(1.0);
    cr->move_to(6, 4.5);
    cr->line_to(w - 6, 4.5);
    cr->stroke();
    cr->move_to(6, h - 4.5);
    cr->line_to(w - 6, h - 4.5);
    cr->stroke();
    cr->set_source_rgb(fg, fg, fg);
    double bh = 5, gap = (h - 8 - 3 * bh) / 2.0;
    for (int i = 0; i < 3; ++i)
      rect(8, 4 + i * (bh + gap), w - 16, bh);
    break;
  }
  }
}

void Toolbar::build_align_button() {
  // Separator above button
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(4);
  sep->set_margin_bottom(2);
  append(*sep);

  // The trigger button — Cairo-drawn align icon
  Gtk::DrawingArea *icon_da = Gtk::make_managed<Gtk::DrawingArea>();
  curvz::utils::set_name(icon_da, "tb_ab_icon", "main_toolbar_align_btn_icon_da");
  icon_da->set_size_request(22, 22);
  icon_da->set_can_target(false);
  icon_da->set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        bool en = m_align_btn.get_sensitive();
        draw_align_icon(cr, w, h, AlignOp::AlignCenterH, en, m_motif);
      });
  m_align_btn.set_child(*icon_da);
  curvz::utils::set_name(m_align_btn, "tb_ab", "main_toolbar_align_btn");
  // S117 m5: was set_has_frame(true). Every other toolbar button uses
  // frame=false (logo, hamburger, zoom, fit, macro, etc.); this one
  // alone wore the system button frame chrome, which paints a grey
  // background-image that sits visibly over the toolbar band in light
  // motif. Source-side fix is cleaner than a CSS background-image
  // override for one specific class — drops the inconsistency and the
  // visual bug together.
  m_align_btn.set_has_frame(false);
  m_align_btn.set_halign(Gtk::Align::CENTER);
  m_align_btn.set_size_request(40, 40);
  m_align_btn.set_tooltip_text("Align & Distribute");
  m_align_btn.set_sensitive(
      false); // enabled by MainWindow when ≥2 objects selected
  m_align_btn.add_css_class("tb-align-btn");

  build_align_popover();
  m_align_pop.set_parent(m_align_btn);

  m_align_btn.signal_clicked().connect([this]() { m_align_pop.popup(); });

  append(m_align_btn);
}

void Toolbar::build_align_popover() {
  curvz::utils::set_name(m_align_pop, "pop_tb_align", "popover_toolbar_align_root");
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(10);
  outer->set_margin_bottom(10);
  outer->set_margin_start(10);
  outer->set_margin_end(10);

  // ── Title ─────────────────────────────────────────────────────────────
  auto *title = Gtk::make_managed<Gtk::Label>("Align & Distribute");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Helper: make a labelled section with a row of icon buttons
  auto make_section = [&](const std::string &label_text) -> Gtk::Box * {
    auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_spacing(3);
    auto *lbl = Gtk::make_managed<Gtk::Label>(label_text);
    lbl->set_xalign(0.0f);
    lbl->add_css_class("tb-pop-label");
    vbox->append(*lbl);
    auto *hbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    hbox->set_spacing(3);
    vbox->append(*hbox);
    outer->append(*vbox);
    return hbox;
  };

  // Helper: wrap a DrawingArea in a button that emits an AlignOp
  auto make_btn = [&](Gtk::DrawingArea &da, AlignOp op,
                      const std::string &tip) -> Gtk::Button * {
    da.set_size_request(32, 32);
    da.set_can_target(false);
    da.set_draw_func([this, op](const Cairo::RefPtr<Cairo::Context> &cr, int w,
                                int h) { draw_align_icon(cr, w, h, op, true, m_motif); });
    auto *btn = Gtk::make_managed<Gtk::Button>();
    btn->set_child(da);
    btn->set_has_frame(false);
    btn->add_css_class("tb-icon-btn");
    btn->set_tooltip_text(tip);
    btn->signal_clicked().connect([this, op]() {
      m_align_pop.popdown();
      m_sig_align.emit(op);
    });
    return btn;
  };

  // ── Align row (6 buttons: L CH R / T CV B) ────────────────────────────
  // We lay them out as two sub-rows for clarity
  auto *align_h_row = make_section("Align");
  Gtk::Button *al_btn;
  al_btn = make_btn(m_align_da[0], AlignOp::AlignLeft, "Align left edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_l", "popover_toolbar_align_left_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(m_align_da[1], AlignOp::AlignCenterH, "Center on vertical axis");
  curvz::utils::set_name(al_btn, "pop_tb_align_ch", "popover_toolbar_align_center_h_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(m_align_da[2], AlignOp::AlignRight, "Align right edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_r", "popover_toolbar_align_right_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(m_align_da[3], AlignOp::AlignTop, "Align top edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_t", "popover_toolbar_align_top_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(m_align_da[4], AlignOp::AlignCenterV, "Center on horizontal axis");
  curvz::utils::set_name(al_btn, "pop_tb_align_cv", "popover_toolbar_align_center_v_btn");
  align_h_row->append(*al_btn);
  al_btn = make_btn(m_align_da[5], AlignOp::AlignBottom, "Align bottom edges");
  curvz::utils::set_name(al_btn, "pop_tb_align_b", "popover_toolbar_align_bottom_btn");
  align_h_row->append(*al_btn);

  // ── Distribute row ─────────────────────────────────────────────────────
  auto *dist_row = make_section("Distribute");
  al_btn = make_btn(m_align_da[6], AlignOp::DistributeH, "Distribute horizontally (equal gaps)");
  curvz::utils::set_name(al_btn, "pop_tb_align_dh", "popover_toolbar_distribute_h_btn");
  dist_row->append(*al_btn);
  al_btn = make_btn(m_align_da[7], AlignOp::DistributeV, "Distribute vertically (equal gaps)");
  curvz::utils::set_name(al_btn, "pop_tb_align_dv", "popover_toolbar_distribute_v_btn");
  dist_row->append(*al_btn);

  m_align_pop.set_child(*outer);
  m_align_pop.set_has_arrow(true);
  m_align_pop.set_position(Gtk::PositionType::RIGHT);
}

void Toolbar::set_align_enabled(bool enabled) {
  m_align_btn.set_sensitive(enabled);
  // Redraw the trigger icon so it reflects the enabled state
  if (m_align_btn.get_child()) {
    if (auto *da = dynamic_cast<Gtk::DrawingArea *>(m_align_btn.get_child()))
      da->queue_draw();
  }
}

void Toolbar::build_zoom_popover(Gtk::ToggleButton *zoom_btn) {
  // ── Right-click on zoom tool button opens popover ─────────────────────
  // S89: was Ctrl+left-click; now plain right-click for world consistency.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  rclick->signal_pressed().connect([this](int, double, double) {
    if (m_zoom_adj)
      m_zoom_adj->set_value(m_zoom * 100.0);
    m_zoom_pop.popup();
  });
  zoom_btn->add_controller(rclick);

  // ── Popover content ───────────────────────────────────────────────────
  curvz::utils::set_name(m_zoom_pop, "pop_tb_zom", "popover_toolbar_zoom_root");
  m_zoom_pop.set_parent(*zoom_btn);
  m_zoom_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(10);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Zoom Level");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Spin button row
  auto *spin_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  spin_row->set_spacing(8);
  auto *spin_lbl = Gtk::make_managed<Gtk::Label>("Zoom:");
  spin_lbl->add_css_class("tb-pop-label");
  spin_lbl->set_xalign(0.0f);

  m_zoom_adj = Gtk::Adjustment::create(100.0, 1.0, 9500.0, 1.0, 10.0);
  curvz::utils::set_name(m_zoom_spin, "pop_tb_zom_spn", "popover_toolbar_zoom_pct_spn");
  m_zoom_spin.set_adjustment(m_zoom_adj);
  m_zoom_spin.set_digits(0);
  m_zoom_spin.set_numeric(true);
  m_zoom_spin.set_hexpand(true);
  m_zoom_spin.set_width_chars(6);

  auto *pct_lbl = Gtk::make_managed<Gtk::Label>("%");
  pct_lbl->add_css_class("tb-pop-label");

  spin_row->append(*spin_lbl);
  spin_row->append(m_zoom_spin);
  spin_row->append(*pct_lbl);
  outer->append(*spin_row);

  // Preset buttons
  auto *preset_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  preset_row->set_spacing(4);
  preset_row->set_homogeneous(true);

  // Five preset buttons — unrolled from the previous loop so each call
  // to set_name carries literal string args, which is what
  // widget_names_sync's harvester regex requires (it captures quoted
  // string literals, not constructed std::string c_str pointers). One
  // local helper keeps the body compact without re-introducing the
  // dynamic-string problem.
  auto add_zoom_preset = [&](int pct, Gtk::Button* btn) {
    btn->add_css_class("tb-type-btn");
    btn->signal_clicked().connect([this, pct]() {
      if (m_zoom_adj)
        m_zoom_adj->set_value(pct);
    });
    preset_row->append(*btn);
  };
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("25%");
    curvz::utils::set_name(btn, "pop_tb_zom_p25", "popover_toolbar_zoom_preset_25pct_btn");
    add_zoom_preset(25, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("50%");
    curvz::utils::set_name(btn, "pop_tb_zom_p50", "popover_toolbar_zoom_preset_50pct_btn");
    add_zoom_preset(50, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("100%");
    curvz::utils::set_name(btn, "pop_tb_zom_p100", "popover_toolbar_zoom_preset_100pct_btn");
    add_zoom_preset(100, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("200%");
    curvz::utils::set_name(btn, "pop_tb_zom_p200", "popover_toolbar_zoom_preset_200pct_btn");
    add_zoom_preset(200, btn);
  }
  {
    auto *btn = Gtk::make_managed<Gtk::Button>("400%");
    curvz::utils::set_name(btn, "pop_tb_zom_p400", "popover_toolbar_zoom_preset_400pct_btn");
    add_zoom_preset(400, btn);
  }
  outer->append(*preset_row);

  // Separator
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(2);
  outer->append(*sep);

  // Button row: Fit | Apply
  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_spacing(8);
  btn_row->set_halign(Gtk::Align::END);

  auto *fit_btn = Gtk::make_managed<Gtk::Button>("Fit");
  curvz::utils::set_name(fit_btn, "pop_tb_zom_fit", "popover_toolbar_zoom_fit_btn");
  fit_btn->signal_clicked().connect([this]() {
    m_zoom_pop.popdown();
    m_sig_fit.emit();
    m_sig_canvas_focus.emit();
  });

  auto *apply_btn = Gtk::make_managed<Gtk::Button>("Apply");
  curvz::utils::set_name(apply_btn, "pop_tb_zom_apl", "popover_toolbar_zoom_apply_btn");
  apply_btn->add_css_class("suggested-action");
  apply_btn->signal_clicked().connect([this]() {
    double pct = m_zoom_adj ? m_zoom_adj->get_value() : 100.0;
    double target = pct / 100.0; // raw zoom factor: 100% → 1.0, 50% → 0.5
    if (target > 0.0)
      m_sig_zoom_to.emit(
          target); // MainWindow calls zoom_toward with exact factor
    m_zoom_pop.popdown();
    m_sig_canvas_focus.emit();
  });

  btn_row->append(*fit_btn);
  btn_row->append(*apply_btn);
  outer->append(*btn_row);

  m_zoom_pop.set_child(*outer);
}

// ── s150 — Measure tool right-click popover ─────────────────────────────
// Houses the two measure-behaviour settings (save_to_layer,
// destruct_after_copy) that used to live in the Inspector ▸ Document ▸
// Measure section. The section was deleted in s150 per ARC.md design
// rule 1: behaviour at the tool, not in the inspector.
//
// State source: m_doc (set via set_document). On popover open, checkbox
// states are synced from doc->measure_*. On toggle, writes go directly
// to doc->measure_*; signal_measure_settings_changed fires so MainWindow
// can schedule_save. No project-level mirror — measure prefs are per-doc.
//
// Mirrors the snap popover shape: open syncs from doc, toggles write
// back through. Mirrors the deleted Inspector Measure section's
// interaction: the two checkboxes have a sensitivity coupling
// (delete-on-copy is greyed out when save-to-layer is on).
void Toolbar::build_measure_popover(Gtk::ToggleButton *measure_btn) {
  // ── Right-click on Measure tool button opens popover ──────────────────
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  rclick->signal_pressed().connect([this](int, double, double) {
    // Sync checkboxes from current doc state on every open. Same pattern
    // as the snap popover (signal_snap_pop_open in the inspector path).
    if (m_doc && m_measure_save_chk && m_measure_del_chk && m_measure_del_lbl) {
      m_measure_save_chk->set_active(m_doc->measure_save_to_layer);
      m_measure_del_chk->set_active(m_doc->measure_destruct_after_copy);
      m_measure_del_chk->set_sensitive(!m_doc->measure_save_to_layer);
      if (m_doc->measure_save_to_layer)
        m_measure_del_lbl->add_css_class("dim-label");
      else
        m_measure_del_lbl->remove_css_class("dim-label");
    }
    m_measure_pop.popup();
  });
  measure_btn->add_controller(rclick);

  // ── Popover content ───────────────────────────────────────────────────
  curvz::utils::set_name(m_measure_pop, "pop_tb_meas", "popover_toolbar_measure_root");
  m_measure_pop.set_parent(*measure_btn);
  m_measure_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Measure");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // Helper — make a [checkbox][label] row with consistent spacing.
  // Mirrors the deleted build_measure_section's row helper.
  auto make_chk_row = [](Gtk::CheckButton *chk, Gtk::Label *lbl) -> Gtk::Box * {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(6);
    row->set_margin_top(2);
    row->set_margin_bottom(2);
    row->append(*chk);
    row->append(*lbl);
    return row;
  };

  // ── "Save measurements" checkbox row ────────────────────────────────────
  m_measure_save_chk = Gtk::make_managed<Gtk::CheckButton>();
  curvz::utils::set_name(m_measure_save_chk, "pop_tb_meas_sv",
                         "popover_toolbar_measure_save_check");
  m_measure_save_chk->set_tooltip_text(
      "When enabled, every completed measurement is appended to the "
      "Measurements layer and persists in the document.");
  auto *save_lbl = Gtk::make_managed<Gtk::Label>("Save measurements");
  save_lbl->set_xalign(0.0f);
  save_lbl->set_hexpand(true);
  save_lbl->add_css_class("tb-pop-label");
  outer->append(*make_chk_row(m_measure_save_chk, save_lbl));

  // ── "Delete on copy" checkbox row ───────────────────────────────────────
  m_measure_del_chk = Gtk::make_managed<Gtk::CheckButton>();
  curvz::utils::set_name(m_measure_del_chk, "pop_tb_meas_dc",
                         "popover_toolbar_measure_destruct_check");
  m_measure_del_chk->set_tooltip_text(
      "When enabled, copying a transient measurement label dismisses it "
      "from the canvas. Only applies when 'Save measurements' is off — "
      "saved entries are permanent.");
  m_measure_del_lbl = Gtk::make_managed<Gtk::Label>("Delete on copy");
  m_measure_del_lbl->set_xalign(0.0f);
  m_measure_del_lbl->set_hexpand(true);
  m_measure_del_lbl->add_css_class("tb-pop-label");
  outer->append(*make_chk_row(m_measure_del_chk, m_measure_del_lbl));

  // ── Save toggle handler ─────────────────────────────────────────────────
  // Writes through to m_doc, updates Delete-on-copy sensitivity inline,
  // emits signal_measure_settings_changed for MainWindow to schedule_save.
  m_measure_save_chk->signal_toggled().connect([this]() {
    if (!m_doc) return;
    bool on = m_measure_save_chk->get_active();
    m_doc->measure_save_to_layer = on;
    if (m_measure_del_chk) m_measure_del_chk->set_sensitive(!on);
    if (m_measure_del_lbl) {
      if (on) m_measure_del_lbl->add_css_class("dim-label");
      else    m_measure_del_lbl->remove_css_class("dim-label");
    }
    m_sig_measure_settings.emit();
  });

  m_measure_del_chk->signal_toggled().connect([this]() {
    if (!m_doc) return;
    m_doc->measure_destruct_after_copy = m_measure_del_chk->get_active();
    m_sig_measure_settings.emit();
  });

  m_measure_pop.set_child(*outer);
}

void Toolbar::build_ref_popover(Gtk::ToggleButton *ref_btn) {
  // ── Right-click on ref tool button opens placement popover ────────────
  // S89: was Ctrl+left-click; converted to plain right-click.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  rclick->signal_pressed().connect(
      [this](int, double, double) {
        Unit u = m_doc ? (m_doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(m_doc->canvas.phys_unit) : m_doc->canvas.display_unit) : m_popup_unit;
        auto [cw, ch] = canvas_display_size(m_doc);
        if (m_ref_unit_lbl)
          m_ref_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
        if (m_ref_adj_x)
          m_ref_adj_x->set_value(cw * 0.5);
        if (m_ref_adj_y)
          m_ref_adj_y->set_value(ch * 0.5);
        m_ref_pop.popup();
      });
  ref_btn->add_controller(rclick);

  curvz::utils::set_name(m_ref_pop, "pop_tb_ref", "popover_toolbar_ref_root");
  m_ref_pop.set_parent(*ref_btn);
  m_ref_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(10);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);

  // Title
  auto *title = Gtk::make_managed<Gtk::Label>("Place Reference Point");
  title->add_css_class("tb-pop-title");
  title->set_xalign(0.0f);
  outer->append(*title);

  // X row
  auto *x_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  x_row->set_spacing(8);
  auto *x_lbl = Gtk::make_managed<Gtk::Label>("X:");
  x_lbl->add_css_class("tb-pop-label");
  x_lbl->set_xalign(0.0f);
  x_lbl->set_width_chars(3);

  m_ref_adj_x = Gtk::Adjustment::create(0.0, -1e6, 1e6, 0.000001, 10.0);
  auto *x_spin = Gtk::make_managed<Gtk::SpinButton>(m_ref_adj_x, 0.000001, 6);
  curvz::utils::set_name(x_spin, "pop_tb_ref_x", "popover_toolbar_ref_x_spn");
  x_spin->set_hexpand(true);
  x_spin->set_width_chars(12);
  x_spin->set_numeric(true);

  x_row->append(*x_lbl);
  x_row->append(*x_spin);
  outer->append(*x_row);

  // Y row
  auto *y_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  y_row->set_spacing(8);
  auto *y_lbl = Gtk::make_managed<Gtk::Label>("Y:");
  y_lbl->add_css_class("tb-pop-label");
  y_lbl->set_xalign(0.0f);
  y_lbl->set_width_chars(3);

  m_ref_adj_y = Gtk::Adjustment::create(0.0, -1e6, 1e6, 0.000001, 10.0);
  auto *y_spin = Gtk::make_managed<Gtk::SpinButton>(m_ref_adj_y, 0.000001, 6);
  curvz::utils::set_name(y_spin, "pop_tb_ref_y", "popover_toolbar_ref_y_spn");
  y_spin->set_hexpand(true);
  y_spin->set_width_chars(12);
  y_spin->set_numeric(true);

  y_row->append(*y_lbl);
  y_row->append(*y_spin);
  outer->append(*y_row);

  m_ref_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_ref_unit_lbl->set_xalign(0.0f);
  m_ref_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_ref_unit_lbl);

  // Place button
  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_spacing(6);
  btn_row->set_halign(Gtk::Align::END);

  auto *place_btn = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place_btn, "pop_tb_ref_pl", "popover_toolbar_ref_place_btn");
  place_btn->add_css_class("suggested-action");
  place_btn->signal_clicked().connect([this]() {
    if (m_ref_adj_x && m_ref_adj_y)
      m_sig_place_ref.emit(m_ref_adj_x->get_value(), m_ref_adj_y->get_value());
    m_ref_pop.popdown();
    m_sig_canvas_focus.emit();
  });

  btn_row->append(*place_btn);
  outer->append(*btn_row);

  m_ref_pop.set_child(*outer);
}

// ── Shared helper: build a right-click popover with N spin rows + Place button
// S89: was Ctrl+left-click, converted to plain right-click to match world UI
// convention (right-click for context/configuration menus). Tool buttons
// remain ToggleButtons whose left-click toggles the active tool — right-click
// is a separate gesture stream so there's no conflict.
static void setup_rclick_popover(Gtk::ToggleButton *btn, Gtk::Popover &pop,
                                 std::function<void()> on_open) {
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3); // right mouse button
  rclick->signal_pressed().connect(
      [&pop, on_open](int, double, double) {
        if (on_open)
          on_open();
        pop.popup();
      });
  btn->add_controller(rclick);
  pop.set_parent(*btn);
  pop.set_position(Gtk::PositionType::RIGHT);
}

static Gtk::Box *make_pop_outer(const char *title) {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_spacing(8);
  outer->set_margin_top(12);
  outer->set_margin_bottom(12);
  outer->set_margin_start(14);
  outer->set_margin_end(14);
  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->add_css_class("tb-pop-title");
  lbl->set_xalign(0.0f);
  outer->append(*lbl);
  return outer;
}

// Helper: build a "[Label:] [SpinButton]" row inside a popover and append
// it to outer. Returns the Adjustment AND writes the SpinButton pointer
// out via the optional out_spin parameter — callers that want to
// curvz::utils::set_name() the spin pass &spin_ptr; pure-Adjustment
// callers pass nullptr.
//
// s119: the out-pointer pattern lets every call site name its spin with
// literal-string args (which is what widget_names_sync's harvester regex
// requires). An earlier attempt threaded abbrev/long_name through the
// helper and called set_name inside, but that hides the literals from
// the harvester — the set_name body sees only variable names, not the
// caller's strings.
static Glib::RefPtr<Gtk::Adjustment> make_pop_row(Gtk::Box *outer,
                                                  const char *label,
                                                  Gtk::SpinButton **out_spin = nullptr) {
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->set_spacing(8);
  auto *lbl = Gtk::make_managed<Gtk::Label>(label);
  lbl->add_css_class("tb-pop-label");
  lbl->set_xalign(0.0f);
  lbl->set_width_chars(4);
  row->append(*lbl);
  auto adj = Gtk::Adjustment::create(0.0, -1e6, 1e6, 0.000001, 10.0);
  auto *spin = Gtk::make_managed<Gtk::SpinButton>(adj, 0.000001, 6);
  spin->set_hexpand(true);
  spin->set_width_chars(10);
  spin->set_numeric(true);
  row->append(*spin);
  outer->append(*row);
  if (out_spin) *out_spin = spin;
  return adj;
}

// ── Rect popover
// ──────────────────────────────────────────────────────────────
void Toolbar::build_rect_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(m_rect_pop, "pop_tb_rec", "popover_toolbar_rect_root");
  setup_rclick_popover(btn, m_rect_pop, [this]() {
    Unit u = m_doc ? (m_doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(m_doc->canvas.phys_unit) : m_doc->canvas.display_unit) : m_popup_unit;
    auto [cw, ch] = canvas_display_size(m_doc);
    if (m_rect_unit_lbl)
      m_rect_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (m_rect_adj_x) {
      double sz_w = cw * 0.2;
      double sz_h = ch * 0.2;
      m_rect_adj_x->set_value(cw * 0.5 - sz_w * 0.5);
      m_rect_adj_y->set_value(ch * 0.5 - sz_h * 0.5);
      m_rect_adj_w->set_value(sz_w);
      m_rect_adj_h->set_value(sz_h);
    }
  });
  auto *outer = make_pop_outer("Place Rectangle");
  Gtk::SpinButton *rec_x_spin = nullptr, *rec_y_spin = nullptr,
                  *rec_w_spin = nullptr, *rec_h_spin = nullptr;
  m_rect_adj_x = make_pop_row(outer, "X:", &rec_x_spin);
  curvz::utils::set_name(rec_x_spin, "pop_tb_rec_x", "popover_toolbar_rect_x_spn");
  m_rect_adj_y = make_pop_row(outer, "Y:", &rec_y_spin);
  curvz::utils::set_name(rec_y_spin, "pop_tb_rec_y", "popover_toolbar_rect_y_spn");
  m_rect_adj_w = make_pop_row(outer, "W:", &rec_w_spin);
  curvz::utils::set_name(rec_w_spin, "pop_tb_rec_w", "popover_toolbar_rect_w_spn");
  m_rect_adj_h = make_pop_row(outer, "H:", &rec_h_spin);
  curvz::utils::set_name(rec_h_spin, "pop_tb_rec_h", "popover_toolbar_rect_h_spn");
  m_rect_adj_w->set_lower(0.000001);
  m_rect_adj_h->set_lower(0.000001);
  m_rect_adj_w->set_value(100);
  m_rect_adj_h->set_value(100);

  m_rect_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_rect_unit_lbl->set_xalign(0.0f);
  m_rect_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_rect_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_rec_pl", "popover_toolbar_rect_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (m_rect_adj_x)
      m_sig_place_rect.emit(
          m_rect_adj_x->get_value(), m_rect_adj_y->get_value(),
          m_rect_adj_w->get_value(), m_rect_adj_h->get_value());
    m_rect_pop.popdown();
    m_sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  m_rect_pop.set_child(*outer);
}

// ── Ellipse popover
// ───────────────────────────────────────────────────────────
void Toolbar::build_ellipse_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(m_ellipse_pop, "pop_tb_ova", "popover_toolbar_ellipse_root");
  setup_rclick_popover(btn, m_ellipse_pop, [this]() {
    Unit u = m_doc ? (m_doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(m_doc->canvas.phys_unit) : m_doc->canvas.display_unit) : m_popup_unit;
    auto [cw, ch] = canvas_display_size(m_doc);
    if (m_ellipse_unit_lbl)
      m_ellipse_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (m_ellipse_adj_x) {
      double sz_w = cw * 0.2;
      double sz_h = ch * 0.2;
      m_ellipse_adj_x->set_value(cw * 0.5 - sz_w * 0.5);
      m_ellipse_adj_y->set_value(ch * 0.5 - sz_h * 0.5);
      m_ellipse_adj_w->set_value(sz_w);
      m_ellipse_adj_h->set_value(sz_h);
    }
  });
  auto *outer = make_pop_outer("Place Ellipse");
  Gtk::SpinButton *ova_x_spin = nullptr, *ova_y_spin = nullptr,
                  *ova_w_spin = nullptr, *ova_h_spin = nullptr;
  m_ellipse_adj_x = make_pop_row(outer, "X:", &ova_x_spin);
  curvz::utils::set_name(ova_x_spin, "pop_tb_ova_x", "popover_toolbar_ellipse_x_spn");
  m_ellipse_adj_y = make_pop_row(outer, "Y:", &ova_y_spin);
  curvz::utils::set_name(ova_y_spin, "pop_tb_ova_y", "popover_toolbar_ellipse_y_spn");
  m_ellipse_adj_w = make_pop_row(outer, "W:", &ova_w_spin);
  curvz::utils::set_name(ova_w_spin, "pop_tb_ova_w", "popover_toolbar_ellipse_w_spn");
  m_ellipse_adj_h = make_pop_row(outer, "H:", &ova_h_spin);
  curvz::utils::set_name(ova_h_spin, "pop_tb_ova_h", "popover_toolbar_ellipse_h_spn");
  m_ellipse_adj_w->set_lower(0.000001);
  m_ellipse_adj_h->set_lower(0.000001);
  m_ellipse_adj_w->set_value(100);
  m_ellipse_adj_h->set_value(100);

  m_ellipse_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_ellipse_unit_lbl->set_xalign(0.0f);
  m_ellipse_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_ellipse_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_ova_pl", "popover_toolbar_ellipse_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (m_ellipse_adj_x) {
      double x = m_ellipse_adj_x->get_value();
      double y = m_ellipse_adj_y->get_value();
      double w = m_ellipse_adj_w->get_value();
      double h = m_ellipse_adj_h->get_value();
      double cx = x + w * 0.5;
      double cy = y + h * 0.5;
      double rx = w * 0.5;
      double ry = h * 0.5;
      m_sig_place_ellipse.emit(cx, cy, rx, ry);
    }
    m_ellipse_pop.popdown();
    m_sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  m_ellipse_pop.set_child(*outer);
}

// ── Line popover
// ──────────────────────────────────────────────────────────────
void Toolbar::build_line_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(m_line_pop, "pop_tb_lin", "popover_toolbar_line_root");
  setup_rclick_popover(btn, m_line_pop, [this]() {
    Unit u = m_doc ? (m_doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(m_doc->canvas.phys_unit) : m_doc->canvas.display_unit) : m_popup_unit;
    auto [cw, ch] = canvas_display_size(m_doc);
    if (m_line_unit_lbl)
      m_line_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (m_line_adj_x1) {
      m_line_adj_x1->set_value(cw * 0.25);
      m_line_adj_y1->set_value(ch * 0.5);
      m_line_adj_x2->set_value(cw * 0.75);
      m_line_adj_y2->set_value(ch * 0.5);
    }
  });
  auto *outer = make_pop_outer("Place Line");
  Gtk::SpinButton *lin_x1_spin = nullptr, *lin_y1_spin = nullptr,
                  *lin_x2_spin = nullptr, *lin_y2_spin = nullptr;
  m_line_adj_x1 = make_pop_row(outer, "X1:", &lin_x1_spin);
  curvz::utils::set_name(lin_x1_spin, "pop_tb_lin_x1", "popover_toolbar_line_x1_spn");
  m_line_adj_y1 = make_pop_row(outer, "Y1:", &lin_y1_spin);
  curvz::utils::set_name(lin_y1_spin, "pop_tb_lin_y1", "popover_toolbar_line_y1_spn");
  m_line_adj_x2 = make_pop_row(outer, "X2:", &lin_x2_spin);
  curvz::utils::set_name(lin_x2_spin, "pop_tb_lin_x2", "popover_toolbar_line_x2_spn");
  m_line_adj_y2 = make_pop_row(outer, "Y2:", &lin_y2_spin);
  curvz::utils::set_name(lin_y2_spin, "pop_tb_lin_y2", "popover_toolbar_line_y2_spn");
  m_line_adj_x1->set_value(100);
  m_line_adj_y1->set_value(500);
  m_line_adj_x2->set_value(900);
  m_line_adj_y2->set_value(500);

  m_line_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_line_unit_lbl->set_xalign(0.0f);
  m_line_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_line_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_lin_pl", "popover_toolbar_line_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (m_line_adj_x1)
      m_sig_place_line.emit(
          m_line_adj_x1->get_value(), m_line_adj_y1->get_value(),
          m_line_adj_x2->get_value(), m_line_adj_y2->get_value());
    m_line_pop.popdown();
    m_sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  m_line_pop.set_child(*outer);
}

// ── Text placement popover (right-click)
// ───────────────────────────────────────
void Toolbar::build_text_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(m_text_pop, "pop_tb_txt", "popover_toolbar_text_root");
  setup_rclick_popover(btn, m_text_pop, [this]() {
    Unit u = m_doc ? (m_doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(m_doc->canvas.phys_unit) : m_doc->canvas.display_unit) : m_popup_unit;
    auto [cw, ch] = canvas_display_size(m_doc);
    if (m_text_unit_lbl)
      m_text_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (m_text_adj_x)
      m_text_adj_x->set_value(cw * 0.1);
    if (m_text_adj_y)
      m_text_adj_y->set_value(ch * 0.75);
    if (m_text_adj_size)
      m_text_adj_size->set_value(72.0);  // always points
    m_text_bold_btn.set_active(false);
    m_text_italic_btn.set_active(false);
    if (m_text_anchor_drop)
      m_text_anchor_drop->set_selected(0);
  });

  auto *outer = make_pop_outer("Place Text");

  // X / Y position
  Gtk::SpinButton *txt_x_spin = nullptr, *txt_y_spin = nullptr,
                  *txt_sz_spin = nullptr;
  m_text_adj_x = make_pop_row(outer, "X:", &txt_x_spin);
  curvz::utils::set_name(txt_x_spin, "pop_tb_txt_x", "popover_toolbar_text_x_spn");
  m_text_adj_y = make_pop_row(outer, "Y:", &txt_y_spin);
  curvz::utils::set_name(txt_y_spin, "pop_tb_txt_y", "popover_toolbar_text_y_spn");
  m_text_adj_x->set_value(100);
  m_text_adj_y->set_value(500);

  // Font size — always in points
  m_text_adj_size = make_pop_row(outer, "Size (pt):", &txt_sz_spin);
  curvz::utils::set_name(txt_sz_spin, "pop_tb_txt_sz", "popover_toolbar_text_size_spn");
  m_text_adj_size->set_lower(1.0);
  m_text_adj_size->set_upper(2000.0);
  m_text_adj_size->set_step_increment(1.0);
  m_text_adj_size->set_value(72);

  // Font family
  // Font family dropdown — populated from system Pango font map
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(8);
    auto *lbl = Gtk::make_managed<Gtk::Label>("Font:");
    lbl->add_css_class("tb-pop-label");
    lbl->set_xalign(0.0f);
    lbl->set_width_chars(4);
    row->append(*lbl);

    // Enumerate system fonts via Pango
    PangoFontMap *fmap = pango_cairo_font_map_get_default();
    PangoFontFamily **families = nullptr;
    int n_families = 0;
    pango_font_map_list_families(fmap, &families, &n_families);
    std::vector<std::string> fnames;
    fnames.reserve(n_families);
    for (int i = 0; i < n_families; ++i)
      fnames.push_back(pango_font_family_get_name(families[i]));
    g_free(families);
    std::sort(fnames.begin(), fnames.end());

    auto slist = Gtk::StringList::create({});
    guint sans_idx = 0;
    for (guint i = 0; i < (guint)fnames.size(); ++i) {
      slist->append(fnames[i]);
      if (fnames[i] == "Sans")
        sans_idx = i;
    }

    m_text_family_drop = Gtk::make_managed<Gtk::DropDown>(slist);
    curvz::utils::set_name(m_text_family_drop, "pop_tb_txt_fam", "popover_toolbar_text_font_family_dd");
    m_text_family_drop->set_enable_search(true);
    m_text_family_drop->set_selected(sans_idx);
    m_text_family_drop->set_hexpand(true);
    row->append(*m_text_family_drop);
    outer->append(*row);
  }

  // Bold / Italic toggles
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(12);
    m_text_bold_btn.set_label("Bold");
    curvz::utils::set_name(m_text_bold_btn, "pop_tb_txt_b", "popover_toolbar_text_bold_check");
    m_text_italic_btn.set_label("Italic");
    curvz::utils::set_name(m_text_italic_btn, "pop_tb_txt_i", "popover_toolbar_text_italic_check");
    row->append(m_text_bold_btn);
    row->append(m_text_italic_btn);
    outer->append(*row);
  }

  // Anchor dropdown
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(8);
    auto *lbl = Gtk::make_managed<Gtk::Label>("Align:");
    lbl->add_css_class("tb-pop-label");
    lbl->set_xalign(0.0f);
    lbl->set_width_chars(4);
    row->append(*lbl);
    auto anchor_items =
        Gtk::StringList::create({"Left", "Centre", "Right", "Justify"});
    m_text_anchor_drop = Gtk::make_managed<Gtk::DropDown>(anchor_items);
    curvz::utils::set_name(m_text_anchor_drop, "pop_tb_txt_an", "popover_toolbar_text_anchor_dd");
    m_text_anchor_drop->set_selected(0);
    m_text_anchor_drop->set_hexpand(true);
    row->append(*m_text_anchor_drop);
    outer->append(*row);
  }

  // Place button
  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_txt_pl", "popover_toolbar_text_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (!m_text_adj_x)
      return;
    double x = m_text_adj_x->get_value();
    double y = m_text_adj_y->get_value();
    double size = m_text_adj_size->get_value();
    // Read selected font family from the dropdown
    std::string family = "Sans";
    if (m_text_family_drop) {
      auto *sl = dynamic_cast<Gtk::StringList *>(
          m_text_family_drop->get_model().get());
      auto sel = m_text_family_drop->get_selected();
      if (sl && sel != GTK_INVALID_LIST_POSITION)
        family = sl->get_string(sel);
    }
    bool bold = m_text_bold_btn.get_active();
    bool italic = m_text_italic_btn.get_active();
    std::string anchor = "start";
    std::string align = "left";
    if (m_text_anchor_drop) {
      auto sel = m_text_anchor_drop->get_selected();
      if (sel == 1) {
        anchor = "middle";
        align = "center";
      } else if (sel == 2) {
        anchor = "end";
        align = "right";
      } else if (sel == 3) {
        anchor = "start";
        align = "justify";
      }
    }
    m_sig_place_text.emit(x, y, family, size, bold, italic, anchor, align);
    m_text_pop.popdown();
    m_sig_canvas_focus.emit();
  });
  btn_row->append(*place);

  m_text_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_text_unit_lbl->set_xalign(0.0f);
  m_text_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_text_unit_lbl);

  outer->append(*btn_row);
  m_text_pop.set_child(*outer);
}

// ── Polygon / Star popover
// ────────────────────────────────────────────────────
void Toolbar::build_polygon_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(m_poly_pop, "pop_tb_pol", "popover_toolbar_polygon_root");
  setup_rclick_popover(btn, m_poly_pop, [this]() {
    Unit u = m_doc ? (m_doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(m_doc->canvas.phys_unit) : m_doc->canvas.display_unit) : m_popup_unit;
    auto [cw, ch] = canvas_display_size(m_doc);
    if (m_poly_unit_lbl)
      m_poly_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (m_poly_adj_sides) {
      m_poly_adj_sides->set_value(m_poly_sides);
      m_poly_adj_inflect->set_value(m_poly_inflection * 100.0);
      m_poly_adj_cx->set_value(cw * 0.5);
      m_poly_adj_cy->set_value(ch * 0.5);
      m_poly_adj_r->set_value(std::min(cw, ch) * 0.2);
    }
    m_poly_preview.queue_draw();
  });

  auto *outer = make_pop_outer("Place Polygon / Star");

  // ── Interactive preview (160×160) ────────────────────────────────────
  curvz::utils::set_name(m_poly_preview, "pop_tb_pol_pv", "popover_toolbar_polygon_preview_da");
  m_poly_preview.set_size_request(160, 160);
  m_poly_preview.set_halign(Gtk::Align::CENTER);
  m_poly_preview.set_margin_bottom(6);

  m_poly_preview.set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        int sides = m_poly_adj_sides ? (int)m_poly_adj_sides->get_value()
                                     : m_poly_sides;
        double inflect = m_poly_adj_inflect
                             ? m_poly_adj_inflect->get_value() / 100.0
                             : m_poly_inflection;

        double perfect_star =
            (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                         : -1.0; // no valid star for N<5
        bool snapped_star =
            (perfect_star > 0.0 && std::abs(inflect - perfect_star) < 0.001);
        bool snapped_poly = (inflect > 0.9990);

        double cx = w * 0.5;
        double cy = h * 0.5;
        double radius = std::min(w, h) * 0.40;
        double inner_r = radius * inflect;
        bool is_star = (inflect < 0.9999);
        int total = is_star ? sides * 2 : sides;
        double base_angle = -M_PI * 0.5; // point up

        // Dark background
        cr->set_source_rgb(0.13, 0.13, 0.13);
        cr->rectangle(0, 0, w, h);
        cr->fill();

        // Shape path
        cr->begin_new_path();
        for (int i = 0; i < total; ++i) {
          double angle =
              base_angle + i * (is_star ? M_PI / sides : 2.0 * M_PI / sides);
          double r = (!is_star || i % 2 == 0) ? radius : inner_r;
          double px = cx + r * std::cos(angle);
          double py = cy + r * std::sin(angle);
          if (i == 0)
            cr->move_to(px, py);
          else
            cr->line_to(px, py);
        }
        cr->close_path();

        // Fill
        cr->set_source_rgba(0.3, 0.6, 1.0, 0.20);
        cr->fill_preserve();

        // Stroke — green when snapped, blue otherwise
        if (snapped_star || snapped_poly)
          cr->set_source_rgba(0.3, 0.9, 0.4, 1.0);
        else
          cr->set_source_rgba(0.4, 0.7, 1.0, 0.95);
        cr->set_line_width(1.5);
        cr->set_line_join(Cairo::Context::LineJoin::MITER);
        cr->stroke();

        // ── Inflection handle ─────────────────────────────────────────────
        // Handle sits on the bisector closest to 0° (rightmost inner vertex).
        // For a star: bisector angle = base_angle + (M_PI / sides)
        // That's the angle of the first inner vertex (index 1).
        // For a regular polygon we draw it at outer radius on the same
        // bisector.
        double hdl_angle = base_angle + M_PI / sides; // first bisector
        double hdl_r = is_star ? inner_r : radius;
        double hdl_x = cx + hdl_r * std::cos(hdl_angle);
        double hdl_y = cy + hdl_r * std::sin(hdl_angle);

        // Dashed radial guide line from center to outer radius on bisector
        cr->set_source_rgba(0.5, 0.5, 0.5, 0.5);
        cr->set_line_width(0.75);
        std::vector<double> dash = {3.0, 3.0};
        cr->set_dash(dash, 0);
        cr->move_to(cx, cy);
        cr->line_to(cx + radius * std::cos(hdl_angle),
                    cy + radius * std::sin(hdl_angle));
        cr->stroke();
        cr->set_dash(std::vector<double>{}, 0);

        // Handle circle
        const double HDL_R = 5.0;
        cr->arc(hdl_x, hdl_y, HDL_R, 0, 2 * M_PI);
        if (snapped_star || snapped_poly) {
          cr->set_source_rgba(0.3, 0.9, 0.4, 1.0);
          cr->fill_preserve();
          cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
        } else {
          cr->set_source_rgba(0.25, 0.55, 1.0, 0.9);
          cr->fill_preserve();
          cr->set_source_rgba(1.0, 1.0, 1.0, 0.8);
        }
        cr->set_line_width(1.5);
        cr->stroke();

        // Snap label
        cr->set_source_rgba(0.5, 0.5, 0.5, 0.8);
        cr->set_font_size(9);
        if (snapped_poly) {
          cr->set_source_rgba(0.3, 0.9, 0.4, 0.9);
          cr->move_to(4, h - 4);
          cr->show_text("polygon");
        } else if (snapped_star) {
          cr->set_source_rgba(0.3, 0.9, 0.4, 0.9);
          cr->move_to(4, h - 4);
          cr->show_text("perfect star");
        }
      });

  // ── Inflection handle drag ────────────────────────────────────────────
  // Dragging the inner handle along the bisector radius sets inflection.
  // All positions are in preview widget coordinates.
  auto drag = Gtk::GestureDrag::create();
  drag->set_button(1);

  drag->signal_drag_begin().connect([this](double x, double y) {
    if (!m_poly_adj_sides || !m_poly_adj_inflect)
      return;
    int sides = (int)m_poly_adj_sides->get_value();
    double inflect = m_poly_adj_inflect->get_value() / 100.0;

    int w = m_poly_preview.get_width();
    int h = m_poly_preview.get_height();
    double cx = w * 0.5;
    double cy = h * 0.5;
    double radius = std::min(w, h) * 0.40;
    double inner_r = radius * inflect;
    bool is_star = (inflect < 0.9999);
    double base_angle = -M_PI * 0.5;
    double hdl_angle = base_angle + M_PI / sides;
    double hdl_r = is_star ? inner_r : radius;
    double hdl_x = cx + hdl_r * std::cos(hdl_angle);
    double hdl_y = cy + hdl_r * std::sin(hdl_angle);

    // Hit test: within 10px of handle
    if (std::hypot(x - hdl_x, y - hdl_y) <= 10.0) {
      m_poly_hdl_drag = true;
      m_poly_hdl_start_inflect = inflect;
    }
  });

  drag->signal_drag_update().connect([this](double dx, double dy) {
    if (!m_poly_hdl_drag || !m_poly_adj_sides || !m_poly_adj_inflect)
      return;
    int sides = (int)m_poly_adj_sides->get_value();
    int w = m_poly_preview.get_width();
    int h = m_poly_preview.get_height();
    double radius = std::min(w, h) * 0.40;
    double base_angle = -M_PI * 0.5;
    double hdl_angle = base_angle + M_PI / sides;

    // Project drag delta onto the bisector direction
    double dir_x = std::cos(hdl_angle);
    double dir_y = std::sin(hdl_angle);
    double proj = dx * dir_x + dy * dir_y;

    // New inner radius = start inner radius + projection
    double start_inner = radius * m_poly_hdl_start_inflect;
    double new_inner = std::clamp(start_inner + proj, 0.0, radius);
    double new_inflect = new_inner / radius;

    // Snap at perfect star and at 1.0 (polygon)
    // Perfect star snap point: cos(2π/N)/cos(π/N)
    double perfect_star =
        (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                     : -1.0;
    double snap_thresh = 0.08;
    if (perfect_star > 0.0 &&
        std::abs(new_inflect - perfect_star) < snap_thresh)
      new_inflect = perfect_star;
    else if (new_inflect > 1.0 - snap_thresh)
      new_inflect = 1.0;

    new_inflect = std::clamp(new_inflect, 0.01, 1.0);
    m_poly_adj_inflect->set_value(new_inflect * 100.0);
    // queue_draw fires automatically via signal_value_changed
  });

  drag->signal_drag_end().connect(
      [this](double, double) { m_poly_hdl_drag = false; });

  m_poly_preview.add_controller(drag);
  outer->append(m_poly_preview);

  // ── Sides spinner ─────────────────────────────────────────────────────
  auto *sides_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  sides_row->set_spacing(8);
  auto *sides_lbl = Gtk::make_managed<Gtk::Label>("Sides:");
  sides_lbl->add_css_class("tb-pop-label");
  sides_lbl->set_xalign(0.0f);
  sides_lbl->set_width_chars(8);
  sides_row->append(*sides_lbl);
  m_poly_adj_sides = Gtk::Adjustment::create(m_poly_sides, 3, 64, 1, 5);
  auto *sides_spin = Gtk::make_managed<Gtk::SpinButton>(m_poly_adj_sides, 1, 0);
  curvz::utils::set_name(sides_spin, "pop_tb_pol_sd", "popover_toolbar_polygon_sides_spn");
  sides_spin->set_hexpand(true);
  sides_row->append(*sides_spin);
  outer->append(*sides_row);

  // ── Inflection spinner ────────────────────────────────────────────────
  auto *inf_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  inf_row->set_spacing(8);
  auto *inf_lbl = Gtk::make_managed<Gtk::Label>("Inflect %:");
  inf_lbl->add_css_class("tb-pop-label");
  inf_lbl->set_xalign(0.0f);
  inf_lbl->set_width_chars(8);
  inf_row->append(*inf_lbl);
  m_poly_adj_inflect =
      Gtk::Adjustment::create(m_poly_inflection * 100.0, 1.0, 100.0, 0.5, 5.0);
  auto *inf_spin =
      Gtk::make_managed<Gtk::SpinButton>(m_poly_adj_inflect, 0.5, 1);
  curvz::utils::set_name(inf_spin, "pop_tb_pol_in", "popover_toolbar_polygon_inflect_spn");
  inf_spin->set_hexpand(true);
  inf_row->append(*inf_spin);
  outer->append(*inf_row);

  // Redraw preview when values change
  m_poly_adj_sides->signal_value_changed().connect(
      [this]() { m_poly_preview.queue_draw(); });
  m_poly_adj_inflect->signal_value_changed().connect(
      [this]() { m_poly_preview.queue_draw(); });

  // ── Placement fields ──────────────────────────────────────────────────
  Gtk::SpinButton *pol_cx_spin = nullptr, *pol_cy_spin = nullptr,
                  *pol_r_spin  = nullptr;
  m_poly_adj_cx = make_pop_row(outer, "CX:", &pol_cx_spin);
  curvz::utils::set_name(pol_cx_spin, "pop_tb_pol_cx", "popover_toolbar_polygon_cx_spn");
  m_poly_adj_cy = make_pop_row(outer, "CY:", &pol_cy_spin);
  curvz::utils::set_name(pol_cy_spin, "pop_tb_pol_cy", "popover_toolbar_polygon_cy_spn");
  m_poly_adj_r = make_pop_row(outer, "R:", &pol_r_spin);
  curvz::utils::set_name(pol_r_spin, "pop_tb_pol_r", "popover_toolbar_polygon_r_spn");
  m_poly_adj_cx->set_value(500);
  m_poly_adj_cy->set_value(500);
  m_poly_adj_r->set_value(200);
  m_poly_adj_r->set_lower(1.0);

  m_poly_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_poly_unit_lbl->set_xalign(0.0f);
  m_poly_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_poly_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_pol_pl", "popover_toolbar_polygon_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (!m_poly_adj_cx)
      return;
    m_poly_sides = (int)m_poly_adj_sides->get_value();
    m_poly_inflection = m_poly_adj_inflect->get_value() / 100.0;
    m_sig_place_polygon.emit(m_poly_adj_cx->get_value(),
                             m_poly_adj_cy->get_value(),
                             m_poly_adj_r->get_value(), m_poly_sides,
                             m_poly_inflection, -M_PI * 0.5);
    m_poly_pop.popdown();
    m_sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  m_poly_pop.set_child(*outer);
}

// ── Snap popover (right-click on snap switch)
// ─────────────────────────────────────────
void Toolbar::build_snap_popover(Gtk::Widget *widget) {
  curvz::utils::set_name(m_snap_pop, "pop_tb_snap", "popover_toolbar_snap_settings_root");
  // Attach a right-click gesture directly to the widget. CurvzSwitch is a
  // DrawingArea, not a ToggleButton, so we can't use setup_rclick_popover.
  // S89: was Ctrl+left-click; now plain right-click for world consistency.
  auto rclick = Gtk::GestureClick::create();
  rclick->set_button(3);
  rclick->signal_pressed().connect(
      [this](int, double, double) {
        m_sig_snap_pop_open.emit(); // let MainWindow push current settings first
        m_snap_pop.popup();
      });
  widget->add_controller(rclick);
  m_snap_pop.set_parent(*widget);
  m_snap_pop.set_position(Gtk::PositionType::RIGHT);

  auto *outer = make_pop_outer("Snap Settings");

  // Each checkbutton tagged inline with a literal-string set_name so
  // widget_names_sync's harvester picks up all six rows. Loop body would
  // hide the literals from the harvester (variable args don't match the
  // regex) — explicit unroll, six near-identical blocks, is the version
  // that actually populates the DB.
  auto add_snap_cb = [&](const char *label, Gtk::CheckButton **slot,
                         Gtk::CheckButton *cb) {
    cb->add_css_class("tb-pop-label");
    *slot = cb;
    cb->signal_toggled().connect([this]() {
      if (m_snap_loading || !m_snap_cb_guides) return;
      SnapSettings s;
      s.snap_guides  = m_snap_cb_guides->get_active();
      s.snap_grid    = m_snap_cb_grid->get_active();
      s.snap_margins = m_snap_cb_margins->get_active();
      s.snap_nodes   = m_snap_cb_nodes->get_active();
      s.snap_edges   = m_snap_cb_edges->get_active();
      s.snap_centers = m_snap_cb_centers->get_active();
      m_sig_snap_settings.emit(s);
    });
    outer->append(*cb);
    (void)label;  // label was used at construction, kept for clarity
  };

  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to guides");
    curvz::utils::set_name(cb, "pop_tb_snap_g", "popover_toolbar_snap_guides_check");
    add_snap_cb("Snap to guides", &m_snap_cb_guides, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to grid");
    curvz::utils::set_name(cb, "pop_tb_snap_gr", "popover_toolbar_snap_grid_check");
    add_snap_cb("Snap to grid", &m_snap_cb_grid, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to margins");
    curvz::utils::set_name(cb, "pop_tb_snap_m", "popover_toolbar_snap_margins_check");
    add_snap_cb("Snap to margins", &m_snap_cb_margins, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to nodes");
    curvz::utils::set_name(cb, "pop_tb_snap_n", "popover_toolbar_snap_nodes_check");
    add_snap_cb("Snap to nodes", &m_snap_cb_nodes, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to edges");
    curvz::utils::set_name(cb, "pop_tb_snap_e", "popover_toolbar_snap_edges_check");
    add_snap_cb("Snap to edges", &m_snap_cb_edges, cb);
  }
  {
    auto *cb = Gtk::make_managed<Gtk::CheckButton>("Snap to centers");
    curvz::utils::set_name(cb, "pop_tb_snap_c", "popover_toolbar_snap_centers_check");
    add_snap_cb("Snap to centers", &m_snap_cb_centers, cb);
  }

  m_snap_pop.set_child(*outer);
}

void Toolbar::set_snap_settings(const SnapSettings &s) {
  m_snap_loading = true;
  if (m_snap_cb_guides)  m_snap_cb_guides->set_active(s.snap_guides);
  if (m_snap_cb_grid)    m_snap_cb_grid->set_active(s.snap_grid);
  if (m_snap_cb_margins) m_snap_cb_margins->set_active(s.snap_margins);
  if (m_snap_cb_nodes)   m_snap_cb_nodes->set_active(s.snap_nodes);
  if (m_snap_cb_edges)   m_snap_cb_edges->set_active(s.snap_edges);
  if (m_snap_cb_centers) m_snap_cb_centers->set_active(s.snap_centers);
  m_snap_loading = false;
}

// ── Spiral popover
// ────────────────────────────────────────────────────────────
void Toolbar::build_spiral_popover(Gtk::ToggleButton *btn) {
  curvz::utils::set_name(m_spiral_pop, "pop_tb_spi", "popover_toolbar_spiral_root");
  setup_rclick_popover(btn, m_spiral_pop, [this]() {
    Unit u = m_doc ? (m_doc->canvas.display_mode == DisplayMode::Physical ? UnitSystem::parse_unit(m_doc->canvas.phys_unit) : m_doc->canvas.display_unit) : m_popup_unit;
    auto [cw, ch] = canvas_display_size(m_doc);
    if (m_spiral_unit_lbl)
      m_spiral_unit_lbl->set_text(std::string("Units: ") + UnitSystem::label(u));
    if (m_spiral_adj_turns) {
      m_spiral_adj_turns->set_value(m_spiral_turns);
      m_spiral_adj_inner->set_value(m_spiral_inner);
      m_spiral_adj_cx->set_value(cw * 0.5);
      m_spiral_adj_cy->set_value(ch * 0.5);
      m_spiral_adj_r->set_value(std::min(cw, ch) * 0.2);
    }
    m_spiral_preview.queue_draw();
  });

  auto *outer = make_pop_outer("Place Spiral");

  // ── Interactive preview (160×160) ────────────────────────────────────
  curvz::utils::set_name(m_spiral_preview, "pop_tb_spi_pv", "popover_toolbar_spiral_preview_da");
  m_spiral_preview.set_size_request(160, 160);
  m_spiral_preview.set_halign(Gtk::Align::CENTER);
  m_spiral_preview.set_margin_bottom(6);

  m_spiral_preview.set_draw_func([this](const Cairo::RefPtr<Cairo::Context> &cr,
                                        int w, int h) {
    double turns =
        m_spiral_adj_turns ? m_spiral_adj_turns->get_value() : m_spiral_turns;
    double inner_p =
        m_spiral_adj_inner ? m_spiral_adj_inner->get_value() : m_spiral_inner;

    double cx = w * 0.5;
    double cy = h * 0.5;
    double outer_r = std::min(w, h) * 0.43;
    double inner_r = outer_r * (inner_p / 100.0);

    // Dark background
    cr->set_source_rgb(0.13, 0.13, 0.13);
    cr->rectangle(0, 0, w, h);
    cr->fill();

    // S98: render via the same spiral_to_path the canvas uses, so the
    // preview always matches what gets placed. No inline reimplementation.
    PathData pd = spiral_to_path(cx, cy, outer_r, inner_r, turns,
                                 -M_PI * 0.5);

    cr->begin_new_path();
    for (size_t i = 0; i < pd.nodes.size(); ++i) {
      const auto &n = pd.nodes[i];
      if (i == 0) {
        cr->move_to(n.x, n.y);
      } else {
        const auto &p = pd.nodes[i - 1];
        cr->curve_to(p.cx2, p.cy2, n.cx1, n.cy1, n.x, n.y);
      }
    }

    cr->set_source_rgba(0.4, 0.7, 1.0, 0.95);
    cr->set_line_width(1.5);
    cr->set_line_cap(Cairo::Context::LineCap::ROUND);
    cr->stroke();

    // Small dot at centre
    cr->arc(cx, cy, 2.5, 0, 2 * M_PI);
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.7);
    cr->fill();
  });

  outer->append(m_spiral_preview);

  // ── Turns spinner ─────────────────────────────────────────────────────
  auto *turns_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  turns_row->set_spacing(8);
  auto *turns_lbl = Gtk::make_managed<Gtk::Label>("Turns:");
  turns_lbl->add_css_class("tb-pop-label");
  turns_lbl->set_xalign(0.0f);
  turns_lbl->set_width_chars(10);
  turns_row->append(*turns_lbl);
  m_spiral_adj_turns =
      Gtk::Adjustment::create(m_spiral_turns, 0.25, 20.0, 0.25, 1.0);
  auto *turns_spin =
      Gtk::make_managed<Gtk::SpinButton>(m_spiral_adj_turns, 0.25, 2);
  curvz::utils::set_name(turns_spin, "pop_tb_spi_tn", "popover_toolbar_spiral_turns_spn");
  turns_spin->set_hexpand(true);
  turns_row->append(*turns_spin);
  outer->append(*turns_row);

  // ── Inner radius % spinner ────────────────────────────────────────────
  auto *inner_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  inner_row->set_spacing(8);
  auto *inner_lbl = Gtk::make_managed<Gtk::Label>("Inner r %:");
  inner_lbl->add_css_class("tb-pop-label");
  inner_lbl->set_xalign(0.0f);
  inner_lbl->set_width_chars(10);
  inner_row->append(*inner_lbl);
  m_spiral_adj_inner =
      Gtk::Adjustment::create(m_spiral_inner, 0.0, 95.0, 1.0, 5.0);
  auto *inner_spin =
      Gtk::make_managed<Gtk::SpinButton>(m_spiral_adj_inner, 1.0, 1);
  curvz::utils::set_name(inner_spin, "pop_tb_spi_ir", "popover_toolbar_spiral_inner_r_spn");
  inner_spin->set_hexpand(true);
  inner_row->append(*inner_spin);
  outer->append(*inner_row);

  // Redraw preview when values change
  m_spiral_adj_turns->signal_value_changed().connect(
      [this]() { m_spiral_preview.queue_draw(); });
  m_spiral_adj_inner->signal_value_changed().connect(
      [this]() { m_spiral_preview.queue_draw(); });

  // ── Placement fields ──────────────────────────────────────────────────
  Gtk::SpinButton *spi_cx_spin = nullptr, *spi_cy_spin = nullptr,
                  *spi_r_spin  = nullptr;
  m_spiral_adj_cx = make_pop_row(outer, "CX:", &spi_cx_spin);
  curvz::utils::set_name(spi_cx_spin, "pop_tb_spi_cx", "popover_toolbar_spiral_cx_spn");
  m_spiral_adj_cy = make_pop_row(outer, "CY:", &spi_cy_spin);
  curvz::utils::set_name(spi_cy_spin, "pop_tb_spi_cy", "popover_toolbar_spiral_cy_spn");
  m_spiral_adj_r = make_pop_row(outer, "R:", &spi_r_spin);
  curvz::utils::set_name(spi_r_spin, "pop_tb_spi_r", "popover_toolbar_spiral_r_spn");
  m_spiral_adj_cx->set_value(500);
  m_spiral_adj_cy->set_value(500);
  m_spiral_adj_r->set_value(200);
  m_spiral_adj_r->set_lower(1.0);

  m_spiral_unit_lbl = Gtk::make_managed<Gtk::Label>("Units: px");
  m_spiral_unit_lbl->set_xalign(0.0f);
  m_spiral_unit_lbl->add_css_class("pop-unit-label");
  outer->append(*m_spiral_unit_lbl);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_halign(Gtk::Align::END);
  auto *place = Gtk::make_managed<Gtk::Button>("Place");
  curvz::utils::set_name(place, "pop_tb_spi_pl", "popover_toolbar_spiral_place_btn");
  place->add_css_class("suggested-action");
  place->signal_clicked().connect([this]() {
    if (!m_spiral_adj_cx)
      return;
    m_spiral_turns = m_spiral_adj_turns->get_value();
    m_spiral_inner = m_spiral_adj_inner->get_value();
    double r = m_spiral_adj_r->get_value();
    double inner_r = r * (m_spiral_inner / 100.0);
    m_sig_place_spiral.emit(m_spiral_adj_cx->get_value(),
                            m_spiral_adj_cy->get_value(), r, inner_r,
                            m_spiral_turns, -M_PI * 0.5);
    m_spiral_pop.popdown();
    m_sig_canvas_focus.emit();
  });
  btn_row->append(*place);
  outer->append(*btn_row);
  m_spiral_pop.set_child(*outer);
}

// ── S87 m1: swatch picker section ─────────────────────────────────────────────
//
// Per-popover layout:
//
//   ┌──────────────────────────────────────┐
//   │ Swatches      [palette dropdown    ▾] │
//   │ ┌──┬──┬──┬──┬──┬──┬──┐                │
//   │ │  │  │  │  │  │  │  │  ← FlowBox     │
//   │ └──┴──┴──┴──┴──┴──┴──┘                │
//   └──────────────────────────────────────┘
//
// Click a chip → m_def_*.{r,g,b} = swatch.color, type=Solid, refresh
// + emit_defaults. No binding state on the toolbar — copy-only.
//
// Visibility gate: section appended to outer at build time as a hidden
// Box. set_swatch_library() flips visibility when a non-null library
// arrives, and rebuilds the dropdown + chips.

void Toolbar::build_swatch_picker_section(Gtk::Box& outer, bool is_stroke) {
    auto* section = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    section->set_spacing(6);
    section->set_margin_top(6);

    // Header: "Swatches" label + palette dropdown (right-aligned).
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    header->set_spacing(6);
    auto* title = Gtk::make_managed<Gtk::Label>("Swatches");
    title->add_css_class("tb-pop-label");
    title->set_xalign(0.0f);
    title->set_hexpand(true);
    header->append(*title);

    auto* dd = Gtk::make_managed<Gtk::DropDown>();
    dd->add_css_class("tb-palette-dd");
    // Initial empty model — rebuild_palette_dropdown swaps real entries
    // in once a library is wired.
    dd->set_model(Gtk::StringList::create({}));
    header->append(*dd);
    section->append(*header);

    // Chip flow. Constraints chosen to roughly match the StylesPanel
    // chip-grid idiom (S85 cont-3 fix5): 24px chips, max 8 per row in
    // the toolbar popover (which is narrower than the styles panel).
    auto* flow = Gtk::make_managed<Gtk::FlowBox>();
    flow->set_selection_mode(Gtk::SelectionMode::NONE);
    flow->set_max_children_per_line(8);
    flow->set_min_children_per_line(1);
    flow->set_homogeneous(true);
    flow->set_row_spacing(4);
    flow->set_column_spacing(4);
    flow->add_css_class("tb-chip-flow");
    section->append(*flow);

    // Stash pointers per slot so refresh / set_swatch_library can find
    // the right widgets without conditionals scattered everywhere.
    // s119: also set_name the dd/flow per slot so GTK Inspector sees
    // distinct names for fill vs stroke. Branch is the natural seam —
    // each call site provides literal abbrev/long_name strings, which
    // is what widget_names_sync's harvester regex requires.
    if (is_stroke) {
        m_stroke_picker_section = section;
        m_stroke_palette_dd     = dd;
        m_stroke_chip_flow      = flow;
        curvz::utils::set_name(dd, "pop_tb_strk_pdd", "popover_toolbar_stroke_palette_dd");
        curvz::utils::set_name(flow, "pop_tb_strk_cf", "popover_toolbar_stroke_chip_flow");
    } else {
        m_fill_picker_section   = section;
        m_fill_palette_dd       = dd;
        m_fill_chip_flow        = flow;
        curvz::utils::set_name(dd, "pop_tb_fill_pdd", "popover_toolbar_fill_palette_dd");
        curvz::utils::set_name(flow, "pop_tb_fill_cf", "popover_toolbar_fill_chip_flow");
    }

    // Hidden by default — flipped on by set_swatch_library() when a
    // library is wired and has at least one swatch.
    section->set_visible(false);

    outer.append(*section);
}

void Toolbar::set_swatch_library(const color::SwatchLibrary* lib) {
    m_swatch_library = lib;
    rebuild_swatch_pickers();
}

void Toolbar::rebuild_swatch_pickers() {
    // Both pickers might not exist yet (set_swatch_library could be
    // called before build_*_popover; not the case in current MainWindow
    // wiring, but the guard makes the method idempotent).
    if (!m_fill_picker_section || !m_stroke_picker_section) return;

    const bool has_lib = (m_swatch_library != nullptr) &&
                         (m_swatch_library->swatch_count() > 0);
    if (has_lib) {
        rebuild_palette_dropdown(/*is_stroke=*/false);
        rebuild_palette_dropdown(/*is_stroke=*/true);
        rebuild_chip_grid(/*is_stroke=*/false);
        rebuild_chip_grid(/*is_stroke=*/true);
    }

    // S87 m1 v2: actual visibility is now driven by refresh_*_popover
    // (which respects m_*_picker_open and the type-toggle state). Force
    // a refresh so the Swatch button sensitivity, picker visibility,
    // and colour-row visibility all reflect current state. If the
    // popovers haven't been built yet (set_swatch_library called too
    // early), refresh is harmless — set_active and set_visible on
    // freshly-constructed widgets just no-op.
    refresh_fill_popover();
    refresh_stroke_popover();
}

void Toolbar::rebuild_palette_dropdown(bool is_stroke) {
    if (!m_swatch_library) return;
    Gtk::DropDown* dd = is_stroke ? m_stroke_palette_dd : m_fill_palette_dd;
    auto& palette_ids = is_stroke ? m_stroke_palette_ids : m_fill_palette_ids;
    auto& conn        = is_stroke ? m_stroke_palette_dd_conn
                                  : m_fill_palette_dd_conn;
    if (!dd) return;

    // Disconnect prior selection handler so programmatic refill doesn't
    // fire a user-flow chip rebuild before the IDs vector is populated.
    if (conn.connected()) conn.disconnect();

    palette_ids.clear();
    palette_ids.push_back(kAllPaletteId);

    auto sl = Gtk::StringList::create({});
    sl->append("All");

    for (const auto& pid : m_swatch_library->all_palette_ids()) {
        const color::Palette* p = m_swatch_library->find_palette(pid);
        if (!p) continue;
        palette_ids.push_back(pid);
        sl->append(p->name.empty() ? std::string("(unnamed)") : p->name);
    }

    dd->set_model(sl);
    dd->set_selected(0);  // default to "All"

    // Reconnect: row index → palette id; rebuild chip grid for the
    // active slot only. The "other" slot keeps its independent state.
    conn = dd->property_selected().signal_changed().connect(
        [this, is_stroke]() {
            rebuild_chip_grid(is_stroke);
        });
}

void Toolbar::rebuild_chip_grid(bool is_stroke) {
    if (!m_swatch_library) return;
    Gtk::FlowBox* flow = is_stroke ? m_stroke_chip_flow : m_fill_chip_flow;
    Gtk::DropDown* dd  = is_stroke ? m_stroke_palette_dd : m_fill_palette_dd;
    const auto& palette_ids = is_stroke ? m_stroke_palette_ids
                                        : m_fill_palette_ids;
    auto& shown_pid = is_stroke ? m_stroke_chips_palette_id
                                : m_fill_chips_palette_id;
    if (!flow || !dd || palette_ids.empty()) return;

    // Resolve the requested palette id from the dropdown selection.
    const auto sel = dd->get_selected();
    if (sel == GTK_INVALID_LIST_POSITION ||
        sel >= palette_ids.size()) return;
    const std::string& requested_pid = palette_ids[sel];

    // S87 m1 fix2: short-circuit when the requested palette is already
    // rendered. The dropdown's property_selected can fire spuriously
    // (panel rebuild gotcha from the handoff). Without this guard,
    // every spurious fire rebuilt the FlowBox children, tearing down
    // the chips' GestureClick controllers — causing the asymmetric
    // chip-click-then-popdown bug where only the first click after a
    // (spurious) rebuild dismissed the popover cleanly. Comparing the
    // requested vs already-shown palette id makes the rebuild idempotent.
    if (requested_pid == shown_pid && flow->get_first_child() != nullptr) {
        return;
    }

    // Tear down current children. FlowBox::remove takes a child widget;
    // walk first_child until the box is empty (GTK4 idiom, same pattern
    // as PaintEditor::rebuild_chip_grid).
    while (auto* child = flow->get_first_child()) {
        flow->remove(*child);
    }

    std::vector<std::string> ids;
    if (requested_pid == kAllPaletteId) {
        ids = m_swatch_library->all_swatch_ids();
    } else {
        const color::Palette* p =
            m_swatch_library->find_palette(requested_pid);
        if (p) ids = p->swatches;
    }

    for (const auto& id : ids) {
        std::string name;
        color::Color c;
        if (!tb_resolve_solid(*m_swatch_library, id, name, c)) continue;

        auto* area = Gtk::make_managed<Gtk::DrawingArea>();
        area->set_content_width(TB_CHIP_SIZE);
        area->set_content_height(TB_CHIP_SIZE);
        // Capture by value — chip flow gets rebuilt whenever the palette
        // changes, so closure lifetime is bounded by the next rebuild.
        area->set_draw_func(
            [c](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                tb_paint_chip(cr, w, h, c);
            });
        area->add_css_class("swatch-chip");
        area->set_cursor(Gdk::Cursor::create("pointer"));

        std::string hex = color::to_hex(c);
        std::string tip = name.empty() ? hex : (name + "  " + hex);
        area->set_tooltip_text(tip);

        // Left-click → copy swatch's RGB into m_def_* as Solid. Routes
        // through apply_swatch_pick_to_* so the popover refresh + emit
        // path matches the hex-paste flow.
        //
        // Important: signal_pressed (not signal_released). The toolbar's
        // fill/stroke popovers are autohide; a GestureClick connected to
        // signal_released claims the press-release sequence, which can
        // interfere with the popover's outside-click dismissal.
        //
        // S87 m1 fix4: popdown is deferred to idle. Calling popdown()
        // synchronously inside the gesture event handler races with
        // GTK's grab management — when the chip click is a no-op
        // (picked colour matches current colour, no broadcast chain
        // forces focus elsewhere), the synchronous popdown gets
        // cancelled and the popover stays open until the app loses
        // focus. By queuing popdown for idle dispatch, the gesture
        // event finishes first; idle then runs after the press is
        // fully processed and popdown completes cleanly regardless of
        // whether the pick caused a state change.
        auto gesture = Gtk::GestureClick::create();
        gesture->set_button(1);
        const std::string captured_id = id;
        const bool stroke_local = is_stroke;
        gesture->signal_pressed().connect(
            [this, captured_id, stroke_local](int, double, double) {
                if (stroke_local)
                    apply_swatch_pick_to_stroke(captured_id);
                else
                    apply_swatch_pick_to_fill(captured_id);
                Glib::signal_idle().connect_once(
                    [this, stroke_local]() {
                        if (stroke_local) m_stroke_pop.popdown();
                        else              m_fill_pop.popdown();
                    });
            });
        area->add_controller(gesture);

        flow->append(*area);
    }

    // S87 m1 fix2: record the palette this rebuild rendered, so a
    // subsequent spurious dropdown signal_changed for the same id
    // short-circuits at the guard above.
    shown_pid = requested_pid;
}

void Toolbar::apply_swatch_pick_to_fill(const std::string& swatch_id) {
    // S93 m4: defensive m_syncing guard. See apply_hex_to_fill.
    if (m_syncing) return;
    if (!m_swatch_library) return;
    std::string name;
    color::Color c;
    if (!tb_resolve_solid(*m_swatch_library, swatch_id, name, c)) return;
    m_def_fill.type = FillStyle::Type::Solid;
    m_def_fill.r = c.r;
    m_def_fill.g = c.g;
    m_def_fill.b = c.b;
    refresh_fill_popover();
    redraw_well();
    emit_defaults();
}

void Toolbar::apply_swatch_pick_to_stroke(const std::string& swatch_id) {
    // S93 m4: defensive m_syncing guard. See apply_hex_to_fill.
    if (m_syncing) return;
    if (!m_swatch_library) return;
    std::string name;
    color::Color c;
    if (!tb_resolve_solid(*m_swatch_library, swatch_id, name, c)) return;
    m_def_stroke.paint.type = FillStyle::Type::Solid;
    m_def_stroke.paint.r = c.r;
    m_def_stroke.paint.g = c.g;
    m_def_stroke.paint.b = c.b;
    refresh_stroke_popover();
    redraw_well();
    emit_defaults();
}

} // namespace Curvz
