#include "PropertiesPanel.hpp"
#include "AppPreferences.hpp" // s143 m1 — Application group section
#include "Canvas.hpp"
#include "CoordSpace.hpp"
#include "CurvzEntry.hpp"
#include "CurvzLog.hpp"
#include "CurvzSpinButton.hpp"
#include "CurvzSwitch.hpp"
#include "MacroSystem.hpp"
#include "PaintEditor.hpp" // S84 m4i: extracted paint-row widget
#include "UnitSystem.hpp"
#include "color/ColorRegion.hpp" // S83 m4h v2: region_name fallback when swatch.header.name is empty
#include "color/SwatchLibrary.hpp" // S83 m4h: find_swatch + PaintSlot for binding-indicator rows
#include "curvz_utils.hpp" // s119 — curvz::utils::set_name
#include "math/BezierPath.hpp"
#include "style/StyleInterop.hpp" // mutate_appearance — inspector appearance writers
#include "style/StyleLibrary.hpp" // S80 m4b: find_style for "Bound: <n>" indicator
#include <algorithm>
#include <array>
#include <cairomm/cairomm.h>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <gdk/gdkkeysyms.h>
#include <glibmm/main.h>
#include <glibmm/markup.h> // S83 m4h v4: Markup::escape_text
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/scale.h> // s143 m1 — quality slider in Application section
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>
#include <memory>
#include <numeric>
#include <pango/pangocairo.h>
#include <string>
#include <tuple>

namespace Curvz {

// Attach a capture-phase scroll controller that swallows all scroll events on
// a SpinButton — prevents the two-finger-scroll / mousewheel habit from
// changing values when the user intends to scroll the panel.
static void block_scroll(Gtk::SpinButton *spin,
                         std::function<void()> on_enter = {}) {
  auto sc = Gtk::EventControllerScroll::create();
  sc->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL |
                Gtk::EventControllerScroll::Flags::HORIZONTAL);
  sc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  sc->signal_scroll().connect([](double, double) -> bool { return true; },
                              false);
  spin->add_controller(sc);

  // Commit typed text on Enter and optionally return focus elsewhere.
  // Use CAPTURE phase so we run before GTK's own SpinButton key handler,
  // call update() to parse the text into the adjustment, then let the
  // event continue propagating (return false) so GTK fires
  // signal_value_changed.
  auto kc = Gtk::EventControllerKey::create();
  kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  kc->signal_key_pressed().connect(
      [spin, on_enter](guint keyval, guint, Gdk::ModifierType) -> bool {
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
          spin->update(); // parse text → adjustment
          if (on_enter)
            on_enter();
          return false; // let GTK also process it (fires signal_value_changed)
        }
        return false;
      },
      false);
  spin->add_controller(kc);
}

// Wire a commit action to a CurvzEntry. Uses on_commit() which fires on Return.
static void wire_entry_activate(CurvzEntry *entry,
                                std::function<void()> on_commit) {
  entry->on_commit(std::move(on_commit));
}

// ── Helpers
// ───────────────────────────────────────────────────────────────────
static std::string fmt(double v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}
static std::string hex_of(double r, double g, double b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", (int)(r * 255), (int)(g * 255),
           (int)(b * 255));
  return buf;
}

// ── Node coordinate conversion helpers ───────────────────────────────────────
// Convert a doc-space X coordinate to the display value shown in the inspector
// (matches the ruler: user space + origin offset + display-mode scaling).
static double node_doc_to_display_x(double doc_x, const CanvasModel *cm,
                                    double ruler_ox) {
  // X doesn't flip; subtract ruler origin
  double user_x = doc_x - ruler_ox;
  if (!cm)
    return user_x;
  int q = std::max(1, cm->quality);
  double scaled;
  if (cm->display_mode == DisplayMode::RatioQuality)
    scaled = user_x / q * 100.0;
  else if (cm->display_mode == DisplayMode::Physical) {
    double sp = std::min(cm->phys_width, cm->phys_height);
    scaled = user_x / q * sp;
    return scaled; // already in physical units (in/mm), no further conversion
  } else {
    scaled = user_x; // Pixel: 1:1
  }
  return UnitSystem::from_px(scaled, cm->display_unit);
}

static double node_doc_to_display_y(double doc_y, const CanvasModel *cm,
                                    double ruler_oy) {
  double ch = cm ? (double)cm->canvas_height() : 1.0;
  CoordSpace cs{ch};
  double user_y = cs.to_user_y(doc_y) - ruler_oy;
  if (!cm)
    return user_y;
  int q = std::max(1, cm->quality);
  double scaled;
  if (cm->display_mode == DisplayMode::RatioQuality)
    scaled = user_y / q * 100.0;
  else if (cm->display_mode == DisplayMode::Physical) {
    double sp = std::min(cm->phys_width, cm->phys_height);
    scaled = user_y / q * sp;
    return scaled; // already in physical units, no further conversion
  } else {
    scaled = user_y;
  }
  return UnitSystem::from_px(scaled, cm->display_unit);
}

// Inverse: display value → doc-space coordinate.
static double node_display_to_doc_x(double disp, const CanvasModel *cm,
                                    double ruler_ox) {
  double user_x = disp;
  if (cm) {
    int q = std::max(1, cm->quality);
    if (cm->display_mode == DisplayMode::RatioQuality)
      user_x = disp / 100.0 * q;
    else if (cm->display_mode == DisplayMode::Physical) {
      // disp is in physical units already — convert to doc-units
      double sp = std::min(cm->phys_width, cm->phys_height);
      user_x = (sp > 0) ? disp / sp * q : disp;
    } else {
      // Pixel: disp is in display_unit, convert to screen px then use 1:1
      user_x = UnitSystem::to_px(disp, cm->display_unit);
    }
  }
  return user_x + ruler_ox;
}

static double node_display_to_doc_y(double disp, const CanvasModel *cm,
                                    double ruler_oy) {
  double user_y = disp;
  if (cm) {
    int q = std::max(1, cm->quality);
    if (cm->display_mode == DisplayMode::RatioQuality)
      user_y = disp / 100.0 * q;
    else if (cm->display_mode == DisplayMode::Physical) {
      double sp = std::min(cm->phys_width, cm->phys_height);
      user_y = (sp > 0) ? disp / sp * q : disp;
    } else {
      user_y = UnitSystem::to_px(disp, cm->display_unit);
    }
  }
  user_y += ruler_oy;
  double ch = cm ? (double)cm->canvas_height() : 1.0;
  CoordSpace cs{ch};
  return cs.to_doc_y(user_y);
}

// ── Stroke unit helpers
// ─────────────────────────────────────────────────────── Convert internal
// stroke width (doc units) → display value + unit label. Pixel mode:    display
// = raw units,  label = "px" Physical mode: display = (width/quality) *
// short_phys,  label = phys_unit Ratio/Quality: display = (width/quality) *
// 100,  label = "%"

struct StrokeUnit {
  double display_value; // value shown in spinbox
  double display_step;  // spinbox increment
  double display_max;   // spinbox upper bound
  int decimals;         // spinbox decimal places
  std::string label;    // unit label shown beside spinbox
  std::string tooltip;
};

static StrokeUnit stroke_to_display(double internal_width,
                                    const CanvasModel *cm) {
  if (!cm)
    return {internal_width, 0.1, 9999, 2, "u", "Raw document units"};

  int q = std::max(1, cm->quality);

  switch (cm->display_mode) {
  case DisplayMode::Pixel:
    return {internal_width,
            0.5,
            (double)q,
            1,
            "px",
            "Stroke width in pixels (1 unit = 1 px at canvas size)"};

  case DisplayMode::Physical: {
    double short_phys = std::min(cm->phys_width, cm->phys_height);
    double display = (internal_width / q) * short_phys;
    double step = cm->phys_unit == "in" ? 0.001 : 0.01;
    double maxv = short_phys * 0.5;
    int dec = cm->phys_unit == "in" ? 3 : 2;
    char tip[128];
    snprintf(tip, sizeof(tip), "Stroke width in %s  (short axis = %.3g %s)",
             cm->phys_unit.c_str(), short_phys, cm->phys_unit.c_str());
    return {display, step, maxv, dec, cm->phys_unit, tip};
  }

  case DisplayMode::RatioQuality:
  default: {
    double display = (internal_width / q) * 100.0;
    return {display,
            0.05,
            50.0,
            2,
            "%",
            "Stroke width as % of short axis  (1% = quality/100 units)\n"
            "0.1% ≈ hairline   0.5% ≈ thin   1% ≈ normal   2% ≈ bold"};
  }
  }
}

// Convert display value back to internal doc units
static double stroke_to_internal(double display_value, const CanvasModel *cm) {
  if (!cm)
    return display_value;
  int q = std::max(1, cm->quality);
  switch (cm->display_mode) {
  case DisplayMode::Pixel:
    return display_value;
  case DisplayMode::Physical: {
    double short_phys = std::min(cm->phys_width, cm->phys_height);
    if (short_phys < 1e-9)
      return display_value;
    return (display_value / short_phys) * q;
  }
  case DisplayMode::RatioQuality:
  default:
    return (display_value / 100.0) * q;
  }
}

// ── Inspector undo coalescing
// ───────────────────────────────────────────────── Call AFTER writing the new
// value to obj. All inspector edits on the same object within an 800ms window
// collapse into one undo step. A gap > 800ms or a different object starts a new
// command.
void PropertiesPanel::push_inspector_command(SceneNode *obj) {
  if (!m_history || !obj)
    return;
  if (m_suppress_push)
    return; // suppress spurious post-undo/redo rebuild pushes

  using clock = std::chrono::steady_clock;
  auto now = clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - m_undo_last_time)
                     .count();
  bool same_window = (obj == m_undo_last_obj) && (elapsed < 1500);

  m_undo_last_obj = obj;
  m_undo_last_time = now;

  PathData cur_path = obj->path ? *obj->path : PathData{};

  if (!same_window) {
    // New window — m_undo_before/fill/stroke hold the pre-edit state
    // (snapshotted at refresh() or at the end of the previous window).
    // S82 m4f: m_undo_*_swatch_id_before holds the pre-edit swatch
    // bindings; the post-edit ids are read live from obj (the funnel
    // has already cleared them if a direct write happened, or set_paint
    // has populated them if a swatch apply happened).
    // S92 m3: same shape for bound_style — mutate_appearance clears it
    // as a break-on-override side effect during type-toggle / colour /
    // gradient edits. We snapshot the pre-edit value for undo.
    // S97 m3: shadow_before is the pre-window snapshot; shadow_after
    // is read live from obj. Inspector shadow edits route through
    // mutate_appearance (S98) — same break-on-override semantics as
    // fill/stroke. The funnel runs before this push, so by the time
    // we read obj->read_shadow() any pre-existing bound_style /
    // swatch ids have already been cleared.
    // s168 m1 DIAG — STRIP after triage
    LOG_INFO("[IIDDIAG] EditObject::push  capturing iid='{}' "
             "obj_name='{}' obj_type={}",
             obj->internal_id, obj->name, (int)obj->type);
    m_history->push(std::make_unique<EditObjectCommand>(
        m_project, obj->internal_id,
        m_undo_before, m_undo_fill_before, m_undo_stroke_before, cur_path,
        obj->fill, obj->stroke, m_undo_fill_swatch_id_before,
        m_undo_stroke_swatch_id_before, obj->fill_swatch_id,
        obj->stroke_swatch_id, m_undo_bound_style_before, obj->bound_style,
        m_undo_shadow_before, obj->read_shadow(), "Edit object"));
    // Slide before-snapshots to post-edit state for the next window boundary.
    // During a window (same_window), before stays pinned at window-open state.
    m_undo_before = cur_path;

    // ── Record macro steps for style changes ─────────────────────────────
    if (MacroManager::instance().is_recording()) {
      // Helper: convert FillStyle to hex string ("" = currentColor/none)
      auto fill_hex = [](const FillStyle &f) -> std::string {
        if (f.type == FillStyle::Type::Solid) {
          char buf[8];
          snprintf(buf, sizeof(buf), "#%02X%02X%02X", (int)(f.r * 255),
                   (int)(f.g * 255), (int)(f.b * 255));
          return buf;
        }
        return "";
      };
      // Fill changed?
      if (obj->fill.type != m_undo_fill_before.type ||
          obj->fill.r != m_undo_fill_before.r ||
          obj->fill.g != m_undo_fill_before.g ||
          obj->fill.b != m_undo_fill_before.b) {
        MacroStep s;
        s.op = MacroStep::Op::SetFill;
        s.color_hex = fill_hex(obj->fill);
        MacroManager::instance().record_step(s);
      }
      // Stroke colour changed?
      if (obj->stroke.paint.type != m_undo_stroke_before.paint.type ||
          obj->stroke.paint.r != m_undo_stroke_before.paint.r ||
          obj->stroke.paint.g != m_undo_stroke_before.paint.g ||
          obj->stroke.paint.b != m_undo_stroke_before.paint.b) {
        MacroStep s;
        s.op = MacroStep::Op::SetStroke;
        s.color_hex = fill_hex(obj->stroke.paint);
        MacroManager::instance().record_step(s);
      }
      // Stroke width changed?
      if (std::abs(obj->stroke.width - m_undo_stroke_before.width) > 0.001) {
        MacroStep s;
        s.op = MacroStep::Op::SetStrokeWidth;
        s.value = obj->stroke.width;
        MacroManager::instance().record_step(s);
      }
      // Opacity changes are recorded at the canvas seam
      // (Canvas::apply_opacity_to_selection) since s108 m3, not here.
      // The inspector dispatches opacity through Canvas, so this funnel
      // never sees opacity-only edits — keeping a recorder here would be
      // dead code AND a misleading second source of truth.
    }

    m_undo_fill_before = obj->fill;
    m_undo_stroke_before = obj->stroke;
    // S82 m4f: slide swatch-id snapshots forward in lockstep with
    // fill/stroke so the next window opens with the correct pre-edit
    // bindings.
    m_undo_fill_swatch_id_before = obj->fill_swatch_id;
    m_undo_stroke_swatch_id_before = obj->stroke_swatch_id;
    // S92 m3: same slide for bound_style.
    m_undo_bound_style_before = obj->bound_style;
    // S97 m3: slide shadow forward as well.
    m_undo_shadow_before = obj->read_shadow();
  } else {
    // Same window — patch the last command's "after" in place
    if (auto *cmd =
            dynamic_cast<EditObjectCommand *>(m_history->last_command())) {
      if (cmd->obj_iid == obj->internal_id) {
        cmd->path_after = cur_path;
        cmd->fill_after = obj->fill;
        cmd->stroke_after = obj->stroke;
        // S82 m4f: keep the swatch-id afters in sync. The "before" stays
        // pinned at window-open state (set when this command was pushed
        // in the !same_window branch above), so we only patch the after.
        cmd->fill_swatch_id_after = obj->fill_swatch_id;
        cmd->stroke_swatch_id_after = obj->stroke_swatch_id;
        // S92 m3: same patch for bound_style.
        cmd->bound_style_after = obj->bound_style;
        // S97 m3: same patch for shadow.
        cmd->shadow_after = obj->read_shadow();
      }
    }
  }
}

PropertiesPanel::PropertiesPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
  curvz::utils::set_name(*this, "ins", "inspector_root");
  add_css_class("properties-panel");
  m_inner.set_spacing(0);
  m_scroll.set_child(m_inner);
  m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::NEVER);
  // No internal vexpand — outer m_right_scroll (in MainWindow) owns the
  // scrolling for the whole sidebar.  This lets the Content group that
  // sits below PropertiesPanel dock immediately under the Object section
  // rather than being pushed to the bottom of the sidebar.
  append(m_scroll);

  // Colour-picker popover lives at panel level so it survives rebuilds
  // of the inner body. See ColorPickerPopover.hpp.
  m_color_popover.attach(*this);

  show_empty();
}

void PropertiesPanel::do_clear() {
  LOG_DEBUG("PropertiesPanel::do_clear()");
  ++m_build_gen;
  m_undo_last_obj = nullptr;
  m_undo_last_time = {};
  if (m_suppress_push) {
    Glib::signal_idle().connect_once([this]() {
      Glib::signal_idle().connect_once([this]() { m_suppress_push = false; });
    });
  }
  // Null out live pointers — widgets are about to be destroyed
  m_grid_lbl_spacing = nullptr;
  m_grid_lbl_offset = nullptr;
  m_grid_sp_sx = nullptr;
  m_grid_sp_sy = nullptr;
  m_grid_sp_ox = nullptr;
  m_grid_sp_oy = nullptr;
  m_margin_lbl_insets = nullptr;
  m_margin_lbl_cgap = nullptr;
  m_margin_lbl_rgap = nullptr;
  m_margin_sp_t = nullptr;
  m_margin_sp_b = nullptr;
  m_margin_sp_l = nullptr;
  m_margin_sp_r = nullptr;
  m_margin_sp_cg = nullptr;
  m_margin_sp_rg = nullptr;
  m_margin_sp_cgap = nullptr;
  m_margin_sp_rgap = nullptr;
  // Remove all children
  while (auto *c = m_inner.get_first_child())
    m_inner.remove(*c);
  m_inner.queue_resize();
}

// Emit prop_changed deferred — never during snapshot
void PropertiesPanel::emit_prop_changed() {
  // Visual labels recompute synchronously — they're local Gtk::Labels and
  // don't need to wait for the broader canvas/redraw cascade.
  update_visual_labels();
  Glib::signal_timeout().connect_once([this]() { m_sig_prop_changed.emit(); },
                                      1);
}

void PropertiesPanel::emit_canvas_focus() { m_sig_request_canvas_focus.emit(); }

// Read all 6 node coordinate adjustments and emit signal_request_node_edit.
// The canvas receives this and writes through its own pipeline — it is the
// sole owner of obj->path. Never write to obj->path directly from the
// inspector.
void PropertiesPanel::emit_request_node_edit() {
  if (!m_adj_ax || !m_adj_ix || !m_adj_ox)
    return;
  m_sig_request_node_edit.emit(
      m_node_idx, m_adj_ax->get_value(),
      m_adj_ay->get_value(), // anchor x, y  (display space)
      m_adj_ix->get_value(), m_adj_iy->get_value(), // handle-in x, y
      m_adj_ox->get_value(), m_adj_oy->get_value()  // handle-out x, y
  );
}

// ── Group-level collapsible
// ─────────────────────────────────────────────────── A top-level group header
// that hides/shows a set of child sections. Returns the container box — pass it
// as `parent` to add_collapsible().
Gtk::Box *PropertiesPanel::add_group_collapsible(const char *title,
                                                 bool expanded) {
  std::string key = std::string("__group__") + title;
  auto it = m_section_open.find(key);
  bool open_now = (it != m_section_open.end()) ? it->second : expanded;
  m_section_open[key] = open_now;

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->add_css_class("inspector-section");

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->add_css_class("inspector-section-header");
  hdr->set_spacing(5);

  auto *arrow = Gtk::make_managed<Gtk::Label>(open_now ? "▾" : "▸");
  arrow->add_css_class("inspector-arrow");
  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->set_xalign(0.0f);
  lbl->set_hexpand(true);
  lbl->add_css_class("inspector-section-title");
  lbl->add_css_class("inspector-group-title");
  hdr->append(*arrow);
  hdr->append(*lbl);
  outer->append(*hdr);

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer->append(*sep);

  // Container for child sections
  auto *container = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  container->add_css_class("inspector-group-container");
  container->set_visible(open_now);
  outer->append(*container);

  auto gesture = Gtk::GestureClick::create();
  gesture->signal_pressed().connect(
      [this, container, arrow, key](int, double, double) {
        bool on = !m_section_open[key];
        m_section_open[key] = on;
        container->set_visible(on);
        arrow->set_text(on ? "▾" : "▸");
      });
  hdr->add_controller(gesture);

  m_inner.append(*outer);
  return container;
}

// ── Collapsible section
// ─────────────────────────────────────────────────────── Returns the body box
// — append rows into it. If parent != nullptr, appends into that box instead of
// m_inner.
//
// s150: optional `accessory` text — when non-null, appended to the right
// of the title with dim styling. Used for at-a-glance state read-outs in
// the section header (e.g. Dimensions showing the active unit).
Gtk::Box *PropertiesPanel::add_collapsible(const char *title, bool expanded,
                                           Gtk::Box *parent,
                                           const char *accessory) {
  // Restore remembered state; use caller default on first appearance.
  auto it = m_section_open.find(title);
  bool open_now = (it != m_section_open.end()) ? it->second : expanded;
  // Write the default in so it's tracked from the start.
  m_section_open[title] = open_now;

  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->add_css_class("inspector-section");

  // Header row
  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->add_css_class("inspector-section-header");
  hdr->set_spacing(5);

  auto *arrow = Gtk::make_managed<Gtk::Label>(open_now ? "▾" : "▸");
  arrow->add_css_class("inspector-arrow");
  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->set_xalign(0.0f);
  lbl->set_hexpand(true);
  lbl->add_css_class("inspector-section-title");
  hdr->append(*arrow);
  hdr->append(*lbl);
  // s150: optional accessory text — right-aligned, dim. Used by
  // build_canvas_section to announce the active display unit so the
  // user sees "Dimensions  IN" / "MM" / etc. at a glance without
  // scanning into the section.
  if (accessory && *accessory) {
    auto *acc = Gtk::make_managed<Gtk::Label>(accessory);
    acc->add_css_class("inspector-section-accessory");
    acc->add_css_class("dim-label");
    acc->set_xalign(1.0f);
    hdr->append(*acc);
  }
  outer->append(*hdr);

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer->append(*sep);

  // Body
  auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  body->set_visible(open_now);
  outer->append(*body);

  // Toggle on click — write back to the persistent map
  std::string key{title};
  auto gesture = Gtk::GestureClick::create();
  gesture->signal_pressed().connect(
      [this, body, arrow, key](int, double, double) {
        bool on = !m_section_open[key];
        m_section_open[key] = on;
        body->set_visible(on);
        arrow->set_text(on ? "▾" : "▸");
      });
  hdr->add_controller(gesture);

  Gtk::Box *dest = parent ? parent : &m_inner;
  dest->append(*outer);
  return body;
}

// ── Disabled collapsible — shown collapsed, cannot be opened
// ────────────────── Used for sections that are always visible but only
// meaningful with context (e.g. Node section when no node is selected).
void PropertiesPanel::add_collapsible_disabled(const char *title,
                                               const char *placeholder,
                                               Gtk::Box *parent) {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->add_css_class("inspector-section");

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->add_css_class("inspector-section-header");
  hdr->set_spacing(5);

  auto *arrow = Gtk::make_managed<Gtk::Label>("▸");
  arrow->add_css_class("inspector-arrow");
  arrow->add_css_class("dim-label");
  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->set_xalign(0.0f);
  lbl->set_hexpand(true);
  lbl->add_css_class("inspector-section-title");
  lbl->add_css_class("dim-label");
  hdr->append(*arrow);
  hdr->append(*lbl);
  outer->append(*hdr);
  outer->append(
      *Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  // Body hidden — no click handler, cannot be opened
  auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  body->set_visible(false);
  auto *ph = Gtk::make_managed<Gtk::Label>(placeholder);
  ph->set_halign(Gtk::Align::START);
  ph->set_margin_start(10);
  ph->set_margin_top(4);
  ph->set_margin_bottom(4);
  ph->add_css_class("dim-label");
  body->append(*ph);
  outer->append(*body);

  Gtk::Box *dest = parent ? parent : &m_inner;
  dest->append(*outer);
}

void PropertiesPanel::add_row(const char *label, const std::string &value) {
  // Legacy: appends to m_inner. Not used in new collapsible sections.
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->add_css_class("prop-row");
  auto *k = Gtk::make_managed<Gtk::Label>(label);
  k->add_css_class("prop-lbl");
  k->set_width_chars(5);
  k->set_xalign(0.0f);
  auto *v = Gtk::make_managed<Gtk::Label>(value);
  v->add_css_class("prop-val-lbl");
  v->set_xalign(0.0f);
  v->set_hexpand(true);
  v->set_selectable(true);
  row->append(*k);
  row->append(*v);
  m_inner.append(*row);
}

// Helper: read-only row into a specific box
static void box_add_row(Gtk::Box *box, const char *label,
                        const std::string &value, int label_chars = 5) {
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->add_css_class("prop-row");
  auto *k = Gtk::make_managed<Gtk::Label>(label);
  k->add_css_class("prop-lbl");
  k->set_width_chars(label_chars);
  k->set_xalign(0.0f);
  auto *v = Gtk::make_managed<Gtk::Label>(value);
  v->add_css_class("prop-val-lbl");
  v->set_xalign(0.0f);
  v->set_hexpand(true);
  v->set_selectable(true);
  row->append(*k);
  row->append(*v);
  box->append(*row);
}

// Helper: spin row into a specific box — returns the Adjustment
// box_add_spin: helper that builds a "[Label] [SpinButton]" row inside box.
//
// s119: optional out_spin parameter so callers can curvz::utils::set_name()
// the spin with literal-string args. Default nullptr keeps existing
// callers compiling unchanged. Same idiom as make_pop_row in Toolbar.cpp:
// the harvester regex requires literal strings at the call site, so the
// helper returns the widget pointer rather than naming it internally.
static Glib::RefPtr<Gtk::Adjustment>
box_add_spin(Gtk::Box *box, const char *label, double val, double lo, double hi,
             double step, double page, int digits, int label_chars = 6,
             const char *tip = nullptr, std::function<void()> on_enter = {},
             Gtk::SpinButton **out_spin = nullptr) {
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->add_css_class("prop-row");
  auto *k = Gtk::make_managed<Gtk::Label>(label);
  k->add_css_class("prop-lbl");
  k->set_width_chars(label_chars);
  k->set_xalign(0.0f);
  auto adj = Gtk::Adjustment::create(val, lo, hi, step, page);
  auto *spin = Gtk::make_managed<Gtk::SpinButton>(adj, step, digits);
  spin->set_hexpand(true);
  spin->add_css_class("prop-width-entry");
  if (tip)
    spin->set_tooltip_text(tip);
  block_scroll(spin, on_enter);
  row->append(*k);
  row->append(*spin);
  box->append(*row);
  if (out_spin)
    *out_spin = spin;
  return adj;
}

// ── Dimensions section (was "Canvas" pre-s148 m2 fix2) ──────────────────
// The doc's measurable geometric properties: ratio, quality, DPI,
// physical/pixel size, ruler origin. The CanvasModel class and the
// "Canvas" terminology elsewhere in the codebase (function name,
// member fields, comments) are kept — only the user-facing section
// header label changed in s148 m2 fix2 to free "Canvas" for the new
// colours section under Document ▸ Theme (renamed from Motif in s150,
// which is more honestly "the canvas you draw on" than the geometry
// data here).
//
// s150: header now announces the active display unit as an accessory
// (e.g. "Dimensions  IN") so the user sees what unit they're typing in
// at a glance, without scanning into the section. The Units dropdown
// inside the section is the editor; the accessory is the readout.
void PropertiesPanel::build_canvas_section(std::shared_ptr<CanvasModel> cm,
                                           Gtk::Box *parent) {
  // s150: announce the active unit in the section header. UnitSystem::label
  // returns short labels ("px", "in", "mm", "pt"); we upper-case for header
  // emphasis (matches the GNOME-style "INCHES" announcement intent).
  std::string unit_label_upper = UnitSystem::label(cm->display_unit);
  for (char &c : unit_label_upper)
    c = std::toupper(static_cast<unsigned char>(c));
  auto *body =
      add_collapsible("Dimensions", false, parent, unit_label_upper.c_str());

  // ── Display units dropdown (governs inspector readouts, not canvas geometry)
  {
    static const std::array<Unit, 4> k_units = {Unit::Px, Unit::In, Unit::Mm,
                                                Unit::Pt};
    auto *u_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    u_row->add_css_class("prop-row");
    auto *u_key = Gtk::make_managed<Gtk::Label>("Units");
    u_key->add_css_class("prop-lbl");
    u_key->set_width_chars(8);
    u_key->set_xalign(0.0f);
    auto u_list = Gtk::StringList::create({});
    guint u_sel = 0;
    for (guint i = 0; i < k_units.size(); ++i) {
      u_list->append(UnitSystem::label(k_units[i]));
      if (k_units[i] == cm->display_unit)
        u_sel = i;
    }
    auto *u_drop = Gtk::make_managed<Gtk::DropDown>(u_list);
    curvz::utils::set_name(u_drop, "ins_can_un", "inspector_canvas_units_dd");
    u_drop->set_selected(u_sel);
    u_drop->set_hexpand(true);
    u_drop->add_css_class("prop-dropdown");
    uint32_t u_gen = m_build_gen;
    u_drop->property_selected().signal_changed().connect(
        [this, cm, u_drop, u_gen]() {
          if (u_gen != m_build_gen || m_loading)
            return;
          auto sel = u_drop->get_selected();
          if (sel < k_units.size())
            cm->display_unit = k_units[sel];
          m_sig_canvas_changed.emit(*cm);
          Glib::signal_idle().connect_once(
              [this]() { refresh(m_canvas, m_current); });
        });
    u_row->append(*u_key);
    u_row->append(*u_drop);
    body->append(*u_row);
  }

  // ── Mode switcher + orientation buttons (S63 M6)
  // Single row: [Mode ▼ ~50%] [📱 Portrait] [🖥️ Landscape]
  // Orientation buttons swap canvas width and height for the current mode.
  // The button matching the current orientation (W < H = portrait, W > H =
  // landscape) is highlighted via the .orient-active CSS class. A square
  // canvas (W == H) leaves both buttons inactive.
  {
    auto *m_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    m_row->add_css_class("prop-row");
    m_row->set_spacing(4);

    auto *m_key = Gtk::make_managed<Gtk::Label>("Mode");
    m_key->add_css_class("prop-lbl");
    m_key->set_width_chars(8);
    m_key->set_xalign(0.0f);
    m_row->append(*m_key);

    auto m_list = Gtk::StringList::create({"Pixel", "Physical", "Ratio"});
    auto *m_drop = Gtk::make_managed<Gtk::DropDown>(m_list);
    curvz::utils::set_name(m_drop, "ins_can_md", "inspector_canvas_mode_dd");
    guint m_sel = 0;
    if (cm->display_mode == DisplayMode::Physical)
      m_sel = 1;
    else if (cm->display_mode == DisplayMode::RatioQuality)
      m_sel = 2;
    m_drop->set_selected(m_sel);
    m_drop->add_css_class("prop-dropdown");
    // Hexpand so the dropdown takes the left half; the orientation button
    // box on the right is non-expanding and hugs the end edge.
    m_drop->set_hexpand(true);
    m_row->append(*m_drop);

    uint32_t m_gen = m_build_gen;
    m_drop->property_selected().signal_changed().connect(
        [this, cm, m_drop, m_gen]() {
          if (m_gen != m_build_gen || m_loading)
            return;
          auto sel = m_drop->get_selected();
          int cw = cm->canvas_width(), ch = cm->canvas_height();
          if (sel == 0) {
            cm->display_mode = DisplayMode::Pixel;
            cm->px_width = cw;
            cm->px_height = ch;
          } else if (sel == 1) {
            int dpi = (cm->dpi > 0) ? cm->dpi : 300;
            std::string unit = "in";
            if (cm->display_unit == Unit::Mm)
              unit = "mm";
            double scale = dpi;
            if (unit == "mm")
              scale = dpi / 25.4;
            cm->display_mode = DisplayMode::Physical;
            cm->phys_unit = unit;
            cm->phys_width = cw / scale;
            cm->phys_height = ch / scale;
            cm->dpi = dpi;
          } else {
            cm->display_mode = DisplayMode::RatioQuality;
          }
          m_sig_canvas_changed.emit(*cm);
          Glib::signal_idle().connect_once(
              [this]() { refresh(m_canvas, m_current); });
        });

    // ── Orientation buttons (portrait / landscape) ──────────────────────────
    // Compute current orientation from the current canvas size. One button
    // is always active — square (W == H) defaults to Portrait to match
    // print convention (Letter, A4, etc. are all portrait-first). Click the
    // inactive button to swap W↔H; clicking the active one is a no-op.
    const int cur_w = cm->canvas_width();
    const int cur_h = cm->canvas_height();
    const bool is_landscape = (cur_w > cur_h);
    const bool is_portrait = !is_landscape; // W < H, or square (W == H)

    auto *portrait_btn = Gtk::make_managed<Gtk::Button>();
    curvz::utils::set_name(portrait_btn, "ins_can_op",
                           "inspector_canvas_orient_portrait_btn");
    auto *landscape_btn = Gtk::make_managed<Gtk::Button>();
    curvz::utils::set_name(landscape_btn, "ins_can_ol",
                           "inspector_canvas_orient_landscape_btn");

    portrait_btn->set_icon_name("curvz-orientation-portrait-symbolic");
    portrait_btn->set_tooltip_text("Portrait — taller than wide");
    portrait_btn->add_css_class("flat");
    portrait_btn->add_css_class("orient-btn");
    if (is_portrait)
      portrait_btn->add_css_class("orient-active");

    landscape_btn->set_icon_name("curvz-orientation-landscape-symbolic");
    landscape_btn->set_tooltip_text("Landscape — wider than tall");
    landscape_btn->add_css_class("flat");
    landscape_btn->add_css_class("orient-btn");
    if (is_landscape)
      landscape_btn->add_css_class("orient-active");

    // Swap helper — flips W↔H for whichever mode the canvas is in. The
    // CanvasModel stores three independent representations (ratio, pixel,
    // physical), so we swap all three in lockstep to keep canvas_width() /
    // canvas_height() consistent.
    auto swap_wh = [this, cm, m_gen]() {
      if (m_gen != m_build_gen || m_loading)
        return;
      std::swap(cm->ratio_w, cm->ratio_h);
      std::swap(cm->px_width, cm->px_height);
      std::swap(cm->phys_width, cm->phys_height);
      m_sig_canvas_changed.emit(*cm);
      Glib::signal_idle().connect_once(
          [this]() { refresh(m_canvas, m_current); });
    };

    // Click handlers. The active button is a no-op; the inactive one swaps.
    // Square (W == H) is treated as Portrait — so Landscape-click on a
    // square triggers a swap (no visual change but formally transitions the
    // "orientation" state). This keeps the one-of-two rule clean.
    portrait_btn->signal_clicked().connect([swap_wh, cm]() {
      if (cm->canvas_width() > cm->canvas_height())
        swap_wh();
    });
    landscape_btn->signal_clicked().connect([swap_wh, cm]() {
      if (cm->canvas_width() <= cm->canvas_height())
        swap_wh();
    });

    m_row->append(*portrait_btn);
    m_row->append(*landscape_btn);

    body->append(*m_row);
  }

  // ── Pixel mode: Width / Height spinners + quick presets
  if (cm->display_mode == DisplayMode::Pixel) {
    // Preset buttons
    auto *pre_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    pre_row->set_spacing(4);
    pre_row->set_margin_bottom(4);
    auto *pre_lbl = Gtk::make_managed<Gtk::Label>("Presets");
    pre_lbl->add_css_class("prop-lbl");
    pre_lbl->set_width_chars(8);
    pre_lbl->set_xalign(0.0f);
    pre_row->append(*pre_lbl);
    static const int PX_PRESETS[] = {16, 24, 32, 48, 64, 128, 256};
    for (int px : PX_PRESETS) {
      auto *btn = Gtk::make_managed<Gtk::Button>(std::to_string(px));
      btn->add_css_class("flat");
      btn->set_margin_start(1);
      uint32_t b_gen = m_build_gen;
      btn->signal_clicked().connect([this, cm, px, b_gen]() {
        if (b_gen != m_build_gen)
          return;
        Unit saved_unit = cm->display_unit;
        *cm = CanvasModel::from_pixels(px, px);
        cm->display_unit = saved_unit;
        m_sig_canvas_changed.emit(*cm);
        Glib::signal_idle().connect_once(
            [this]() { refresh(m_canvas, m_current); });
      });
      pre_row->append(*btn);
    }
    body->append(*pre_row);

    int cw = cm->canvas_width(), ch = cm->canvas_height();
    Gtk::SpinButton *can_pw_spin = nullptr, *can_ph_spin = nullptr;
    auto adj_pw = box_add_spin(
        body, "Width (px)", cw, 1, 65536, 1, 8, 0, 8, "Canvas width in pixels",
        [this] { emit_canvas_focus(); }, &can_pw_spin);
    curvz::utils::set_name(can_pw_spin, "ins_can_pw",
                           "inspector_canvas_pixel_width_spn");
    auto adj_ph = box_add_spin(
        body, "Height (px)", ch, 1, 65536, 1, 8, 0, 8,
        "Canvas height in pixels", [this] { emit_canvas_focus(); },
        &can_ph_spin);
    curvz::utils::set_name(can_ph_spin, "ins_can_ph",
                           "inspector_canvas_pixel_height_spn");
    uint32_t p_gen = m_build_gen;
    adj_pw->signal_value_changed().connect([this, cm, adj_pw, adj_ph, p_gen]() {
      if (p_gen != m_build_gen || m_loading)
        return;
      int pw = (int)adj_pw->get_value(), ph = (int)adj_ph->get_value();
      *cm = CanvasModel::from_pixels(pw, ph);
      Glib::signal_timeout().connect_once(
          [this, cm]() { m_sig_canvas_changed.emit(*cm); }, 1);
    });
    adj_ph->signal_value_changed().connect([this, cm, adj_pw, adj_ph, p_gen]() {
      if (p_gen != m_build_gen || m_loading)
        return;
      int pw = (int)adj_pw->get_value(), ph = (int)adj_ph->get_value();
      *cm = CanvasModel::from_pixels(pw, ph);
      Glib::signal_timeout().connect_once(
          [this, cm]() { m_sig_canvas_changed.emit(*cm); }, 1);
    });
  }

  // ── Physical mode: W / H / Unit / DPI
  else if (cm->display_mode == DisplayMode::Physical) {
    // phys_width/height are always stored in inches internally.
    // Display and accept values in the current display_unit.
    auto inches_to_display = [](double inches, Unit u, int dpi) -> double {
      if (u == Unit::Mm)
        return inches * 25.4;
      if (u == Unit::Pt)
        return inches * 72.0;
      if (u == Unit::Px)
        return inches * dpi;
      return inches; // In
    };
    auto display_to_inches = [](double v, Unit u, int dpi) -> double {
      if (u == Unit::Mm)
        return v / 25.4;
      if (u == Unit::Pt)
        return v / 72.0;
      if (u == Unit::Px)
        return (dpi > 0) ? v / dpi : v;
      return v; // In
    };
    auto unit_to_phys = [](Unit u) -> std::string {
      if (u == Unit::Mm)
        return "mm";
      return "in"; // In, Pt, Px all stored as inches
    };

    Unit disp_unit = cm->display_unit;
    double disp_w = inches_to_display(cm->phys_width, disp_unit, cm->dpi);
    double disp_h = inches_to_display(cm->phys_height, disp_unit, cm->dpi);

    // Spinner range and step in display units
    double max_v = (disp_unit == Unit::Mm)   ? 100000.0
                   : (disp_unit == Unit::Pt) ? 72000.0
                   : (disp_unit == Unit::Px) ? 65536.0
                                             : 10000.0;
    double step = (disp_unit == Unit::Mm)   ? 1.0
                  : (disp_unit == Unit::Pt) ? 1.0
                  : (disp_unit == Unit::Px) ? 1.0
                                            : 0.1;
    double page = (disp_unit == Unit::Mm)   ? 10.0
                  : (disp_unit == Unit::Pt) ? 72.0
                  : (disp_unit == Unit::Px) ? 10.0
                                            : 1.0;
    int dec = (disp_unit == Unit::Mm)   ? 2
              : (disp_unit == Unit::Pt) ? 1
              : (disp_unit == Unit::Px) ? 0
                                        : 3;
    std::string w_lbl =
        std::string("Width (") + UnitSystem::label(disp_unit) + ")";
    std::string h_lbl =
        std::string("Height (") + UnitSystem::label(disp_unit) + ")";

    Gtk::SpinButton *can_phw_spin = nullptr, *can_phh_spin = nullptr;
    auto adj_pw = box_add_spin(
        body, w_lbl.c_str(), disp_w, 0.001, max_v, step, page, dec, 8,
        "Physical width in current unit", [this] { emit_canvas_focus(); },
        &can_phw_spin);
    curvz::utils::set_name(can_phw_spin, "ins_can_phw",
                           "inspector_canvas_phys_width_spn");
    auto adj_ph = box_add_spin(
        body, h_lbl.c_str(), disp_h, 0.001, max_v, step, page, dec, 8,
        "Physical height in current unit", [this] { emit_canvas_focus(); },
        &can_phh_spin);
    curvz::utils::set_name(can_phh_spin, "ins_can_phh",
                           "inspector_canvas_phys_height_spn");

    // DPI row — dropdown for common presets, spinner for any custom value.
    // The spinner is authoritative; the dropdown is a quick-pick that
    // writes to the spinner. Both sync to `cm->dpi` via commit_physical.
    auto *dpi_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    dpi_row->add_css_class("prop-row");
    dpi_row->set_spacing(4);
    auto *dpi_key = Gtk::make_managed<Gtk::Label>("DPI");
    dpi_key->add_css_class("prop-lbl");
    dpi_key->set_width_chars(8);
    dpi_key->set_xalign(0.0f);
    auto dpi_list =
        Gtk::StringList::create({"72", "96", "150", "300", "600", "Custom"});
    static const int DPI_VALS[] = {72, 96, 150, 300, 600};
    static const int DPI_PRESET_COUNT = 5;
    static const guint DPI_CUSTOM_INDEX = 5;
    auto *dpi_drop = Gtk::make_managed<Gtk::DropDown>(dpi_list);
    curvz::utils::set_name(dpi_drop, "ins_can_dpi", "inspector_canvas_dpi_dd");
    // Pick the matching preset index, else "Custom"
    guint dpi_sel = DPI_CUSTOM_INDEX;
    for (guint i = 0; i < DPI_PRESET_COUNT; ++i)
      if (DPI_VALS[i] == cm->dpi) {
        dpi_sel = i;
        break;
      }
    dpi_drop->set_selected(dpi_sel);
    dpi_drop->add_css_class("prop-dropdown");
    dpi_row->append(*dpi_key);
    dpi_row->append(*dpi_drop);

    // Custom DPI spinner — always visible. Values outside the preset
    // list are valid (e.g. 144 for retina, 203 for thermal printers,
    // 1200 for fine print). Range covers screen through commercial
    // print; lower bound 1 prevents divide-by-zero downstream.
    auto dpi_adj =
        Gtk::Adjustment::create((double)cm->dpi, 1.0, 9600.0, 1.0, 10.0);
    auto *dpi_spin = Gtk::make_managed<Gtk::SpinButton>(dpi_adj, 1.0, 0);
    curvz::utils::set_name(dpi_spin, "ins_can_dpi_spn",
                           "inspector_canvas_dpi_spn");
    dpi_spin->set_width_chars(5);
    dpi_spin->set_hexpand(true);
    dpi_spin->add_css_class("prop-width-entry");
    dpi_spin->set_tooltip_text(
        "DPI — pick a preset on the left or type any value here");
    block_scroll(dpi_spin, [this] { emit_canvas_focus(); });
    dpi_row->append(*dpi_spin);
    body->append(*dpi_row);

    // Wire — convert display unit → inches → CanvasModel.
    // dpi_spin is the authoritative DPI source. dpi_drop is a quick-pick
    // that writes to dpi_spin (which then triggers commit_physical via
    // the spinner's value-changed signal). Selecting "Custom" leaves
    // the spinner alone — the user types the value directly.
    //
    // m_loading flag (used by the ph_gen+m_loading guard) is leveraged
    // as a "we are programmatically updating, don't recurse" gate so
    // the dropdown→spinner write doesn't fire commit twice.
    uint32_t ph_gen = m_build_gen;
    auto commit_physical = [this, cm, adj_pw, adj_ph, dpi_drop, dpi_spin,
                            ph_gen, display_to_inches, unit_to_phys]() {
      if (ph_gen != m_build_gen || m_loading)
        return;
      double w_in =
          display_to_inches(adj_pw->get_value(), cm->display_unit, cm->dpi);
      double h_in =
          display_to_inches(adj_ph->get_value(), cm->display_unit, cm->dpi);
      int dpi_v = std::clamp((int)std::lround(dpi_spin->get_value()), 1, 9600);
      std::string phys_unit = unit_to_phys(cm->display_unit);
      Unit saved_display = cm->display_unit;
      *cm = CanvasModel::from_physical(w_in, h_in, phys_unit, dpi_v);
      cm->display_unit = saved_display;
      // Sync the dropdown to reflect preset/custom state. Guard
      // m_loading so this programmatic write doesn't re-enter
      // commit_physical from the dropdown's signal.
      m_loading = true;
      guint match = DPI_CUSTOM_INDEX;
      for (guint i = 0; i < DPI_PRESET_COUNT; ++i)
        if (DPI_VALS[i] == dpi_v) {
          match = i;
          break;
        }
      if (dpi_drop->get_selected() != match)
        dpi_drop->set_selected(match);
      m_loading = false;
      Glib::signal_timeout().connect_once(
          [this, cm]() { m_sig_canvas_changed.emit(*cm); }, 1);
    };
    adj_pw->signal_value_changed().connect(commit_physical);
    adj_ph->signal_value_changed().connect(commit_physical);
    dpi_spin->signal_value_changed().connect(commit_physical);
    dpi_drop->property_selected().signal_changed().connect(
        [this, dpi_drop, dpi_spin, ph_gen]() {
          if (ph_gen != m_build_gen || m_loading)
            return;
          guint sel = dpi_drop->get_selected();
          if (sel >= DPI_PRESET_COUNT)
            return; // "Custom" — leave spinner
          // Write through to the spinner, which fires its own
          // value-changed signal and runs commit_physical.
          dpi_spin->set_value((double)DPI_VALS[sel]);
        });

    // Read-only pixel size readout
    char buf[64];
    snprintf(buf, sizeof(buf), "%d × %d px", cm->canvas_width(),
             cm->canvas_height());
    box_add_row(body, "Pixels", buf, 8);
  }

  // ── Ratio / Quality mode
  else {
    // Ratio preset buttons
    struct RatioPreset {
      const char *label;
      double w, h;
    };
    static const RatioPreset RATIO_PRESETS[] = {
        {"1:1", 1.0, 1.0},      {"4:3", 4.0, 3.0},     {"16:9", 16.0, 9.0},
        {"√2", 1.0, 1.4142135}, {"φ", 1.0, 1.6180339},
    };
    auto *rp_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    rp_row->set_spacing(4);
    rp_row->set_margin_bottom(4);
    auto *rp_lbl = Gtk::make_managed<Gtk::Label>("Presets");
    rp_lbl->add_css_class("prop-lbl");
    rp_lbl->set_width_chars(8);
    rp_lbl->set_xalign(0.0f);
    rp_row->append(*rp_lbl);
    for (const auto &p : RATIO_PRESETS) {
      auto *btn = Gtk::make_managed<Gtk::Button>(p.label);
      btn->add_css_class("flat");
      btn->set_margin_start(1);
      uint32_t b_gen = m_build_gen;
      double bw = p.w, bh = p.h;
      btn->signal_clicked().connect([this, cm, bw, bh, b_gen]() {
        if (b_gen != m_build_gen)
          return;
        Unit saved_unit = cm->display_unit;
        int saved_dpi = cm->dpi;
        *cm = CanvasModel::from_ratio(bw, bh, cm->quality);
        cm->display_unit = saved_unit;
        cm->dpi = saved_dpi;
        m_sig_canvas_changed.emit(*cm);
        Glib::signal_idle().connect_once(
            [this]() { refresh(m_canvas, m_current); });
      });
      rp_row->append(*btn);
    }
    body->append(*rp_row);

    Gtk::SpinButton *can_rw_spin = nullptr, *can_rh_spin = nullptr,
                    *can_rq_spin = nullptr;
    auto adj_rw = box_add_spin(
        body, "Ratio W", cm->ratio_w, 0.001, 100.0, 0.001, 0.1, 4, 8,
        "Width ratio (normalised: short axis = 1.0)",
        [this] { emit_canvas_focus(); }, &can_rw_spin);
    curvz::utils::set_name(can_rw_spin, "ins_can_rw",
                           "inspector_canvas_ratio_w_spn");
    auto adj_rh = box_add_spin(
        body, "Ratio H", cm->ratio_h, 0.001, 100.0, 0.001, 0.1, 4, 8,
        "Height ratio (normalised: short axis = 1.0)",
        [this] { emit_canvas_focus(); }, &can_rh_spin);
    curvz::utils::set_name(can_rh_spin, "ins_can_rh",
                           "inspector_canvas_ratio_h_spn");

    // Quality with slider labels
    auto *ql_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    ql_row->set_spacing(4);
    for (const char *lbl : {"Icon", "Print", "Poster", "Billboard"}) {
      auto *l = Gtk::make_managed<Gtk::Label>(lbl);
      l->add_css_class("caption-label");
      l->set_hexpand(true);
      l->set_xalign(0.5f);
      ql_row->append(*l);
    }
    body->append(*ql_row);

    auto adj_q = box_add_spin(
        body, "Quality", cm->quality, 1, 100000, 100, 1000, 0, 8,
        "Unit count on short axis. Icon=1000 Print=4000 Poster=10000",
        [this] { emit_canvas_focus(); }, &can_rq_spin);
    curvz::utils::set_name(can_rq_spin, "ins_can_rq",
                           "inspector_canvas_ratio_quality_spn");

    uint32_t r_gen = m_build_gen;
    adj_rw->signal_value_changed().connect([this, cm, adj_rw, adj_rh, adj_q,
                                            r_gen]() {
      if (r_gen != m_build_gen || m_loading)
        return;
      double rw = adj_rw->get_value(), rh = adj_rh->get_value();
      double s = std::min(rw, rh);
      *cm = CanvasModel::from_ratio(rw / s, rh / s, (int)adj_q->get_value());
      Glib::signal_timeout().connect_once(
          [this, cm]() { m_sig_canvas_changed.emit(*cm); }, 1);
    });
    adj_rh->signal_value_changed().connect([this, cm, adj_rw, adj_rh, adj_q,
                                            r_gen]() {
      if (r_gen != m_build_gen || m_loading)
        return;
      double rw = adj_rw->get_value(), rh = adj_rh->get_value();
      double s = std::min(rw, rh);
      *cm = CanvasModel::from_ratio(rw / s, rh / s, (int)adj_q->get_value());
      Glib::signal_timeout().connect_once(
          [this, cm]() { m_sig_canvas_changed.emit(*cm); }, 1);
    });
    adj_q->signal_value_changed().connect([this, cm, adj_rw, adj_rh, adj_q,
                                           r_gen]() {
      if (r_gen != m_build_gen || m_loading)
        return;
      double rw = adj_rw->get_value(), rh = adj_rh->get_value();
      double s = std::min(rw, rh);
      *cm = CanvasModel::from_ratio(rw / s, rh / s, (int)adj_q->get_value());
      Glib::signal_timeout().connect_once(
          [this, cm]() { m_sig_canvas_changed.emit(*cm); }, 1);
    });

    // Read-only pixel size readout
    char buf[64];
    snprintf(buf, sizeof(buf), "%d × %d px", cm->canvas_width(),
             cm->canvas_height());
    box_add_row(body, "Pixels", buf, 8);
  }
}

// ── Document ▸ Theme disclosure (s150; was "Motif" s148 m2 fix2) ──────
// A nested collapsible that wraps the draftsman-setup facets of a
// document under a single header. The wrapper is labelled "Theme" —
// post-s150 the user-facing word for the saveable doc-style bundle
// (the Themes panel saves and applies what's inside this disclosure).
// Pre-s150 this disclosure was labelled "Motif"; that word is now
// internal-only (Motif enum + MotifSettings sub-bundle within Theme).
//
//   Theme ▸
//     Canvas    — surface colours (artboard / workspace / creation chips,
//                 plus Reset). The colours describe the canvas the user
//                 draws on; this label was freed in s148 m2 fix2 by
//                 renaming the geometry section to "Dimensions."
//     Margins   — page-edge boundaries (the frame within).
//     Grid      — regular mesh, if needed (background scaffold).
//
// s179 m3: Guides removed from the Theme disclosure. The remaining
// three entries cover doc-level surfaces with no selection model;
// Guides have a selection model and the entire Guides surface
// (colour, per-guide editor, construct tool) lives in Object ▸ Guides.
// ThemeLibrary still round-trips guide_color_r/g/b as part of the
// theme bundle — only the inspector edit location moved.
//
// The order mirrors a draftsman's setup chronology: surface → edges →
// mesh. Each step depends on the previous step's setup — the order is
// causal, not alphabetical.
//
// Why nest rather than keep these flat as siblings of
// Metadata/Dimensions: the three together are a coherent "saveable
// style preset" — what a Theme actually is. The disclosure makes the
// bundle visible to the user; the Themes panel saves and applies
// exactly what's inside this disclosure (plus a few hidden fields
// like Snap and Units that round-trip silently, and guide colour
// which is now edited from Object ▸ Guides but still saved here).
//
// Visual nesting: build_canvas_section is intentionally NOT in this
// disclosure — Dimensions (geometric bones) is set-and-forget at doc
// creation, lives at the bottom of the Document group adjacent to the
// Object group. Geometry is geometry; style is what's in the Theme
// disclosure (per ARC.md design rule 1).
//
// s150 note: m_section_open key changed from "Motif" to "Theme" with
// this rename. First launch after upgrade strands the old "Motif" key
// (harmless — it just means Theme starts collapsed by default, the
// add_collapsible default).
void PropertiesPanel::build_theme_disclosure(CurvzDocument *doc,
                                             Gtk::Box *parent) {
  if (!doc)
    return;
  auto *body = add_collapsible("Theme", false, parent);
  // s148 m2 fix3: indent the inner sections so the nesting reads as a
  // distinct level. See css.hpp::.inspector-disclosure-body.
  body->add_css_class("inspector-disclosure-body");
  // s179 m3: Guides removed from the Theme disclosure. The other three
  // theme entries cover doc-level surfaces with no selection model
  // (Canvas backdrop, Margins, Grid) — they belong in the saveable
  // style preset that is "Theme". Guides are objects on the canvas with
  // a selection model, so the entire Guides surface (colour, per-guide
  // editor, construct tool) lives in Object ▸ Guides instead. Theme
  // save/load still round-trips guide_color_r/g/b — see
  // ThemeLibrary::serialize/deserialize — the inspector edit surface
  // moved, the theme bundle's contents didn't change.
  build_canvas_colours_section(doc,
                               body); // labelled "Canvas" inside the function
  build_margin_section(doc, body);
  build_grid_section(doc, body);
}

// ── Document ▸ Motif ▸ Canvas section (was Document ▸ Motif pre-fix2) ──
// Per-document workspace appearance — each document carries its own
// artboard/workspace/creation tone. Switching tabs intentionally
// re-paints the canvas with the new doc's colours.
//
// Three knobs:
//   • Artboard — colour of the rectangle the icon sits on (per-doc).
//   • Workspace — colour of the area around the artboard (per-doc).
//   • Creation — colour of construction previews (rubber-band, drag
//     preview, etc.) (per-doc).
//
// All three are editor-only — never reach the exported SVG. Round-trip
// through project.json's per-doc record on save/load.
//
// The Theme (Dark/Light) switch moved out of this section in s148 m2
// and lives under Application ▸ Appearance now. Theme is app-tier
// (GNOME convention) — flipping it no longer alters per-doc canvas
// colours, only the app chrome's CSS fork.
//
// Section history:
//   S98          — added as "Background" (per-doc).
//   s116 m5      — renamed to "Motif"; added Theme toggle (per-doc).
//   s116 m6      — moved to project scope (per-motif: dark + light slots).
//   s148 m1      — demoted back to per-doc (single slot per doc); chips
//                  read/write doc fields, motif-aware getter/setter
//                  pairs collapsed.
//   s148 m2      — relocated from Project group to Document group; the
//                  in-disclosure builder for the Canvas-colours section
//                  was originally called build_motif_section. Theme
//                  switch extracted to Application ▸ Appearance.
//   s148 m2 fix2 — user-facing label changed from "Motif" to "Canvas"
//                  (the colours describe the canvas the user draws on).
//                  Function name still build_motif_section in s148.
//                  The geometry section that previously owned the
//                  "Canvas" label was simultaneously renamed to
//                  "Dimensions."
//   s150         — disclosure renamed user-facing from "Motif" to
//                  "Theme" (the saveable doc-style bundle); Motif goes
//                  internal-only (Motif enum + MotifSettings sub-bundle).
//                  This builder renamed build_motif_section →
//                  build_canvas_colours_section to match what it
//                  actually builds. Lives inside the Theme disclosure
//                  (build_theme_disclosure), alongside Margins / Grid /
//                  Guides.
//
// Same idiom as the inspector's existing colour swatches (build_guide_
// section, the shadow swatch in build_shadow_section): a small clickable
// DrawingArea opens m_color_popover with the current colour, the apply
// callback writes the new rgb back to the doc and queues a panel
// redraw + emits prop_changed so Canvas repaints.
//
// No undo coverage — matches the precedent set by guide_color_*. Undo
// for editor-presentation prefs is not consistently applied across the
// codebase; we keep the simpler scope here. A user can revert by re-
// picking, and project.json can be hand-edited.
void PropertiesPanel::build_canvas_colours_section(CurvzDocument *doc,
                                                   Gtk::Box *parent) {
  if (!doc)
    return;
  // s148 m2 fix2: header label is "Canvas" (the colours describe the
  // canvas the user draws on); see history block above.
  auto *body = add_collapsible("Canvas", false, parent);
  uint32_t gen = m_build_gen;

  // Chip pointers are populated by add_color_row below. The Reset
  // button handler queue_draws the chips after writing fresh defaults.
  //
  // Lifetime note: chip pointers captured BY VALUE into the Reset
  // lambda. Capturing &motif_artboard_da (address of a local) into a
  // lambda outliving the stack frame is a UAF; capturing by value
  // works because the chip widget pointer itself is stable for the
  // lifetime of the panel build (Gtk::DrawingArea is heap-allocated by
  // make_managed; the local just stores the same address).
  Gtk::DrawingArea *motif_artboard_da = nullptr;
  Gtk::DrawingArea *motif_workspace_da = nullptr;
  Gtk::DrawingArea *motif_creation_da = nullptr;

  // Helper: build one labelled colour-row. Captures plain getter/setter
  // callables that read/write doc fields directly. (s127's motif-aware
  // pair was collapsed in s148 m1 when the per-motif structure was
  // removed; the helper signature stayed the same — generic callables
  // still work.)
  using GetRGB = std::function<std::tuple<double, double, double>()>;
  using SetRGB = std::function<void(double, double, double)>;
  auto add_color_row = [this, gen,
                        body](const char *label, GetRGB get, SetRGB set,
                              Gtk::DrawingArea **out_swatch = nullptr) {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->add_css_class("prop-row");
    row->set_spacing(6);

    auto *key = Gtk::make_managed<Gtk::Label>(label);
    key->add_css_class("prop-lbl");
    key->set_width_chars(10);
    key->set_xalign(0.0f);
    row->append(*key);

    auto *swatch = Gtk::make_managed<Gtk::DrawingArea>();
    swatch->set_size_request(28, 18);
    swatch->set_valign(Gtk::Align::CENTER);
    swatch->set_can_target(true);
    // Draw reads through the getter — re-evaluated on every queue_draw.
    swatch->set_draw_func(
        [get](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
          auto [r, g, b] = get();
          cr->set_source_rgb(r, g, b);
          cr->rectangle(0, 0, w, h);
          cr->fill();
          // Hairline border so a near-workspace-colour swatch is visible.
          cr->set_source_rgba(0.0, 0.0, 0.0, 0.25);
          cr->set_line_width(1.0);
          cr->rectangle(0.5, 0.5, w - 1, h - 1);
          cr->stroke();
        });

    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect(
        [this, get, set, swatch, gen](int, double, double) {
          if (m_build_gen != gen)
            return;
          auto [r, g, b] = get();
          color::Color initial(r, g, b, 1.0);
          m_color_popover.open(*swatch, initial, /*with_alpha=*/false,
                               [this, set, swatch, gen](const color::Color &c) {
                                 if (m_build_gen != gen)
                                   return;
                                 set(c.r, c.g, c.b);
                                 swatch->queue_draw();
                                 emit_prop_changed();
                               });
        });
    swatch->add_controller(click);
    row->append(*swatch);
    body->append(*row);
    if (out_swatch)
      *out_swatch = swatch;
  };

  // s148 m1: chip getter/setter lambdas read/write the active doc's
  // single-slot fields. The motif-aware switch logic from s127 is gone
  // because each doc carries one set of values — flipping the Theme
  // switch (Dark/Light) no longer alters chip colours; it only drives
  // the CSS fork. Capture `doc` by value: panel rebuilds on doc-switch
  // (via update_all_panels), so a stale `doc` pointer in a still-live
  // lambda is prevented by the m_build_gen guard already in place at
  // each lambda entry.
  add_color_row(
      "Artboard",
      [doc]() {
        return std::make_tuple(doc->artboard_bg_r, doc->artboard_bg_g,
                               doc->artboard_bg_b);
      },
      [doc](double r, double g, double b) {
        doc->artboard_bg_r = r;
        doc->artboard_bg_g = g;
        doc->artboard_bg_b = b;
      },
      &motif_artboard_da);
  add_color_row(
      "Workspace",
      [doc]() {
        return std::make_tuple(doc->workspace_bg_r, doc->workspace_bg_g,
                               doc->workspace_bg_b);
      },
      [doc](double r, double g, double b) {
        doc->workspace_bg_r = r;
        doc->workspace_bg_g = g;
        doc->workspace_bg_b = b;
      },
      &motif_workspace_da);
  // Creation colour — used by every "creating something" preview surface
  // (rect/ellipse/line/polygon/spiral construction, pen tool segments
  // and handles).
  add_color_row(
      "Creation",
      [doc]() {
        return std::make_tuple(doc->creation_color_r, doc->creation_color_g,
                               doc->creation_color_b);
      },
      [doc](double r, double g, double b) {
        doc->creation_color_r = r;
        doc->creation_color_g = g;
        doc->creation_color_b = b;
      },
      &motif_creation_da);
  curvz::utils::set_name(motif_artboard_da, "ins_motif_ab",
                         "inspector_motif_artboard_swatch_da");
  curvz::utils::set_name(motif_workspace_da, "ins_motif_ws",
                         "inspector_motif_workspace_swatch_da");
  curvz::utils::set_name(motif_creation_da, "ins_motif_cr",
                         "inspector_motif_creation_swatch_da");

  // ── Reset row ─────────────────────────────────────────────────────
  // Single button, restores THIS doc's artboard/workspace/creation to
  // hardcoded defaults. Scope is per-doc — other docs in the project
  // are unaffected. Currently a single hardcoded set (dark defaults);
  // s148 m4 polish makes this app-mode-aware (Dark mode reset → dark
  // defaults, Light mode reset → light defaults) once the
  // AppPreferences::appearance_mode field is in place (m2 sub-ship 2).
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->add_css_class("prop-row");
    row->set_spacing(6);
    row->set_halign(Gtk::Align::END);

    auto *btn = Gtk::make_managed<Gtk::Button>("Reset");
    curvz::utils::set_name(btn, "ins_motif_rst", "inspector_motif_reset_btn");
    btn->add_css_class("flat");
    btn->set_tooltip_text("Restore this document's artboard, workspace, and "
                          "creation colours to defaults");
    // Capture chips BY VALUE — proj_*_da are local variables; capturing
    // by reference would be UAF once the function returns. The pointed-to
    // widgets are heap-allocated by make_managed and survive for the
    // panel's lifetime.
    Gtk::DrawingArea *artb_chip = motif_artboard_da;
    Gtk::DrawingArea *work_chip = motif_workspace_da;
    Gtk::DrawingArea *crea_chip = motif_creation_da;
    btn->signal_clicked().connect(
        [this, doc, gen, artb_chip, work_chip, crea_chip]() {
          if (m_build_gen != gen)
            return;
          // Hardcoded dark defaults — match CurvzDocument.hpp field
          // defaults. m4 polish will make these app-mode-aware.
          doc->artboard_bg_r = 0.157; // #282828
          doc->artboard_bg_g = 0.157;
          doc->artboard_bg_b = 0.157;
          doc->workspace_bg_r = 0.09; // #171717
          doc->workspace_bg_g = 0.09;
          doc->workspace_bg_b = 0.09;
          doc->creation_color_r = 0.30; // light blue against dark
          doc->creation_color_g = 0.60;
          doc->creation_color_b = 1.00;
          if (artb_chip)
            artb_chip->queue_draw();
          if (work_chip)
            work_chip->queue_draw();
          if (crea_chip)
            crea_chip->queue_draw();
          emit_prop_changed();
        });
    row->append(*btn);
    body->append(*row);
  }
}

// ── Guide section
// ───────────────────────────────────────────────────────────── Shows selected
// guide data only. No list — selection happens in LayersPanel or on canvas.
// set_selected_guide() is called from MainWindow when the canvas or LayersPanel
// fires signal_guide_selection_changed.

void PropertiesPanel::set_selected_guide(SceneNode *g) {
  // Update internal state — caller is responsible for triggering a refresh
  // if the visible content needs to change.
  m_selected_guide = g;
  m_guide_selection =
      g ? std::vector<SceneNode *>{g} : std::vector<SceneNode *>{};
}

void PropertiesPanel::set_guide_selection(const std::vector<SceneNode *> &sel) {
  m_selected_guide = sel.empty() ? nullptr : sel[0];
  m_guide_selection = sel;
}

// ── Application ▸ Appearance subsection (s148 m2) ────────────────────────────
// Holds the Dark/Light Theme switch. Relocated here from the old
// Project ▸ Motif section as part of the s148 motif → document
// migration: per-doc artboard/workspace/creation moved to Document
// scope, while Theme (Dark/Light) is genuinely app-tier — a GNOME
// convention where the user's appearance preference is the same across
// every project.
//
// Sub-ship 1 (this commit) keeps the storage on m_project->motif so
// the existing CSS apply path (apply_motif_to_window via
// signal_changed → MainWindow) continues to work without any other
// changes. Sub-ship 2 will migrate the storage to AppPreferences::
// instance().appearance_mode for true app-tier persistence; at that
// point the function loses its CurvzProject* parameter.
//
// Why CurvzSwitch (Cairo-drawn) rather than Gtk::Switch: matches the
// rest of the inspector's switch idiom (snap toggles, reopen-last-
// project, etc.) — visual cohesion. CurvzSwitch's signal_toggled fires
// after a user click only, never programmatically, so set_state()
// during build is safe.
void PropertiesPanel::build_app_appearance_section(CurvzProject *project,
                                                   Gtk::Box *parent) {
  if (!project)
    return;
  auto *body = add_collapsible("Appearance", false, parent);
  uint32_t gen = m_build_gen;

  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->add_css_class("prop-row");
  row->set_spacing(6);
  row->set_margin_start(6);
  row->set_margin_end(6);
  row->set_margin_top(2);
  row->set_margin_bottom(2);

  auto *key = Gtk::make_managed<Gtk::Label>("Theme");
  key->add_css_class("prop-lbl");
  key->set_xalign(0.0f);
  row->append(*key);

  // Live-text indicator of current state — same idiom the old Motif
  // section used. "Dark" or "Light" updates on toggle.
  auto *value_lbl = Gtk::make_managed<Gtk::Label>(
      project->motif == Motif::Light ? "Light" : "Dark");
  value_lbl->add_css_class("prop-val");
  value_lbl->set_xalign(0.0f);
  value_lbl->set_hexpand(true);
  row->append(*value_lbl);

  auto *sw = Gtk::make_managed<CurvzSwitch>();
  curvz::utils::set_name(sw, "ins_app_appearance_th",
                         "inspector_app_appearance_theme_switch");
  sw->set_state(project->motif == Motif::Light);
  sw->set_valign(Gtk::Align::CENTER);
  sw->set_tooltip_text(
      "Switch the app's appearance between Dark and Light.\n"
      "Affects the editor chrome (panels, menus, toolbar). Per-document\n"
      "canvas colours (artboard, workspace, creation) are unaffected —\n"
      "those live under Document ▸ Motif.");
  row->append(*sw);
  body->append(*row);

  // Toggle handler: write to project->motif, update the live label,
  // emit prop_changed. MainWindow's prop_changed handler runs
  // apply_motif_to_window → swaps the curvz-light CSS class on the
  // window tree → walks rulers/toolbar/corner via set_motif. Same
  // path the old in-Motif Theme switch used; only the inspector
  // location changed.
  sw->signal_toggled().connect([this, project, value_lbl, gen](bool on) {
    if (m_build_gen != gen)
      return;
    project->motif = on ? Motif::Light : Motif::Dark;
    value_lbl->set_text(on ? "Light" : "Dark");
    emit_prop_changed();
  });
}

// ── Application section (s143 m1) ────────────────────────────────────────────
// User-tier app preferences surfaced from AppPreferences::instance().
// Lives under the "Application" group at the top of the inspector,
// sibling of Project / Document / Object.
//
// Subsections in build order (s148 m2 places Appearance first;
// remaining subsections kept in their existing build order — full
// alphabetical reorder deferred to s148 m4 polish to limit m2 churn):
//   Appearance      — Theme (Dark/Light) toggle (s148 m2). Drives the
//                     CSS fork the app paints in. m2 sub-ship 1 reads/
//                     writes m_project->motif (project-level state);
//                     sub-ship 2 migrates the source to AppPreferences
//                     for true app-tier persistence.
//   Startup         — Reopen last project (s144 m2), Recent projects
//                     max count (s144 m3), Show rulers by default
//                     (s145 m1)
//   Editing         — Undo history depth (s145 m1), Tooltip delay ms
//                     (s145 m1)
//   Paths           — User library, User templates, Log file, Custom
//                     CSS (s145 m4 — all four are path-string overrides;
//                     empty = use default; built via curvz::utils::
//                     make_path_override_row)
//   Boolean cleanup — Quality slider (s143 m1). 0..10 int.
//                     End labels: "Approximate (fewer nodes)" left —
//                     "Faithful (more nodes)" right.
//   Warp            — Warp creation defaults (s146 m3)
//
// Future inhabitants of this section (one row each, planned):
//   - Autosave debounce  (SpinButton, seconds — needs semantics pinned)
//
// Mutation pattern: every input writes to AppPreferences, which fires
// AppPreferences::signal_changed; MainWindow's wiring catches that and
// pushes the new value into Canvas. The section is read-mostly — no
// emit_prop_changed because nothing here is a document property.
// (Exception: the Appearance Theme switch in m2 sub-ship 1 writes to
// m_project->motif and emits prop_changed for CSS reapply; sub-ship 2
// converts this to AppPreferences::signal_changed-driven CSS reapply.)
//
// m_loading guard: refresh() sets m_loading=true around builds; we
// honour it when handling user input so programmatic widget setup
// during refresh doesn't loop back into a setter.
void PropertiesPanel::build_app_section(Gtk::Box *parent) {
  uint32_t gen = m_build_gen;

  // ── Appearance subsection (s148 m2) ───────────────────────────────────────
  // Sorted first. Theme switch only — relocated here from the old
  // Project ▸ Motif section.
  build_app_appearance_section(m_project, parent);

  // ── Startup subsection (s144 m2/m3) ───────────────────────────────────────
  // The Application group's subsections are organised by user-facing
  // concern (Startup / Editing / Paths / etc.); this is the home for
  // boot-time prefs. Inhabitants:
  //   - Reopen last project (s144 m2) — bool switch
  //   - Recent projects (s144 m3) — int 1..50
  {
    auto *body = add_collapsible("Startup", false, parent);

    // ── Reopen last project ────────────────────────────────────────────────
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(6);
      row->set_margin_start(6);
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);

      auto *key = Gtk::make_managed<Gtk::Label>("Reopen last project");
      key->add_css_class("prop-lbl");
      key->set_xalign(0.0f);
      key->set_hexpand(true);
      row->append(*key);

      auto *sw = Gtk::make_managed<CurvzSwitch>();
      curvz::utils::set_name(sw, "ins_app_reopen",
                             "inspector_app_reopen_last_project_switch");
      sw->set_state(AppPreferences::instance().reopen_last_project());
      sw->set_valign(Gtk::Align::CENTER);
      sw->set_tooltip_text(
          "When on, Curvz reopens your last project on launch.\n"
          "When off, Curvz starts with a blank project.\n"
          "Takes effect on next launch.");
      row->append(*sw);
      body->append(*row);

      sw->signal_toggled().connect([this, sw, gen](bool on) {
        if (m_build_gen != gen || m_loading)
          return;
        AppPreferences::instance().set_reopen_last_project(on);
        LOG_INFO("PropertiesPanel: reopen_last_project → {}", on);
      });
    }

    // ── Recent projects max count (s144 m3) ────────────────────────────────
    // Plain Gtk::SpinButton (not CurvzSpinButton) — that one is unit-aware
    // and tied to the doc DPI plumbing; this is a unitless integer count
    // and SpinButton is the simpler, correct choice.
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(6);
      row->set_margin_start(6);
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);

      auto *key = Gtk::make_managed<Gtk::Label>("Recent projects");
      key->add_css_class("prop-lbl");
      key->set_xalign(0.0f);
      key->set_hexpand(true);
      row->append(*key);

      auto adj = Gtk::Adjustment::create(
          double(AppPreferences::instance().recent_projects_max_count()), 1.0,
          50.0, 1.0, 5.0, 0.0);
      auto *spin = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
      curvz::utils::set_name(spin, "ins_app_recents_max",
                             "inspector_app_recent_projects_max_count_spin");
      spin->set_valign(Gtk::Align::CENTER);
      spin->set_width_chars(4);
      spin->set_tooltip_text(
          "Maximum entries shown in File ▸ Open Recent.\n"
          "Range 1–50. Takes effect on the next project open.");
      row->append(*spin);
      body->append(*row);

      spin->signal_value_changed().connect([this, spin, gen]() mutable {
        if (m_build_gen != gen || m_loading)
          return;
        int n = static_cast<int>(std::lround(spin->get_value()));
        AppPreferences::instance().set_recent_projects_max_count(n);
        LOG_INFO("PropertiesPanel: recent_projects_max_count → {}", n);
      });
    }

    // ── Show rulers by default (s145 m1) ───────────────────────────────────
    // Boot-time ruler visibility. MainWindow seeds m_rulers_visible from
    // this pref before the toggle-rulers action is created (see
    // setup_menu) and applies it to the ruler widgets after setup_layout.
    // Per-session Ctrl+R toggling does NOT write back to this pref —
    // intentional: users frequently flip rulers during editing without
    // wanting to change their boot preference. Tooltip says so explicitly.
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(6);
      row->set_margin_start(6);
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);

      auto *key = Gtk::make_managed<Gtk::Label>("Show rulers by default");
      key->add_css_class("prop-lbl");
      key->set_xalign(0.0f);
      key->set_hexpand(true);
      row->append(*key);

      auto *sw = Gtk::make_managed<CurvzSwitch>();
      curvz::utils::set_name(sw, "ins_app_show_rulers",
                             "inspector_app_show_rulers_by_default_switch");
      sw->set_state(AppPreferences::instance().show_rulers_by_default());
      sw->set_valign(Gtk::Align::CENTER);
      sw->set_tooltip_text(
          "When on, Curvz shows rulers on launch.\n"
          "When off, rulers boot hidden.\n"
          "Use Ctrl+R during a session to toggle without changing this "
          "preference. Takes effect on next launch.");
      row->append(*sw);
      body->append(*row);

      sw->signal_toggled().connect([this, sw, gen](bool on) {
        if (m_build_gen != gen || m_loading)
          return;
        AppPreferences::instance().set_show_rulers_by_default(on);
        LOG_INFO("PropertiesPanel: show_rulers_by_default → {}", on);
      });
    }
  }

  // ── Editing subsection (s145 m1) ───────────────────────────────────────────
  // Editor-tier behaviour prefs: undo depth, tooltip delay. Sibling of
  // Startup. Both inhabitants are unitless integers with simple bounded
  // ranges, so plain Gtk::SpinButton rows (not CurvzSpinButton — that's
  // unit-aware and tied to doc DPI; not appropriate for app-tier counts).
  {
    auto *body = add_collapsible("Editing", false, parent);

    // ── Undo history depth ──────────────────────────────────────────────────
    // CommandHistory::push reads this every operation. Reducing the value
    // trims at the next push (does not retroactively prune); increasing
    // simply allows further growth.
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(6);
      row->set_margin_start(6);
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);

      auto *key = Gtk::make_managed<Gtk::Label>("Undo history depth");
      key->add_css_class("prop-lbl");
      key->set_xalign(0.0f);
      key->set_hexpand(true);
      row->append(*key);

      auto adj = Gtk::Adjustment::create(
          double(AppPreferences::instance().undo_history_depth()), 50.0,
          10000.0, 50.0, 100.0, 0.0);
      auto *spin = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
      curvz::utils::set_name(spin, "ins_app_undo_depth",
                             "inspector_app_undo_history_depth_spin");
      spin->set_valign(Gtk::Align::CENTER);
      spin->set_width_chars(6);
      spin->set_tooltip_text(
          "Maximum number of undo steps Curvz keeps in memory.\n"
          "Range 50–10000. Takes effect immediately — reducing the\n"
          "value trims the stack at the next operation.");
      row->append(*spin);
      body->append(*row);

      spin->signal_value_changed().connect([this, spin, gen]() mutable {
        if (m_build_gen != gen || m_loading)
          return;
        int n = static_cast<int>(std::lround(spin->get_value()));
        AppPreferences::instance().set_undo_history_depth(n);
        LOG_INFO("PropertiesPanel: undo_history_depth → {}", n);
      });
    }

    // ── Tooltip delay ───────────────────────────────────────────────────────
    // Applied to GTK's "gtk-tooltip-timeout" property at startup. The
    // property is deprecated and dropped on some recent GTK builds —
    // Application::on_activate probes for it and silently no-ops when
    // absent. The spinner accepts the user's value either way; on a
    // build with the property removed, the value persists but has no
    // visible effect. Takes effect on next launch.
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(6);
      row->set_margin_start(6);
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);

      auto *key = Gtk::make_managed<Gtk::Label>("Tooltip delay (ms)");
      key->add_css_class("prop-lbl");
      key->set_xalign(0.0f);
      key->set_hexpand(true);
      row->append(*key);

      auto adj = Gtk::Adjustment::create(
          double(AppPreferences::instance().tooltip_delay_ms()), 0.0, 2000.0,
          50.0, 100.0, 0.0);
      auto *spin = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
      curvz::utils::set_name(spin, "ins_app_tooltip_delay",
                             "inspector_app_tooltip_delay_ms_spin");
      spin->set_valign(Gtk::Align::CENTER);
      spin->set_width_chars(6);
      spin->set_tooltip_text(
          "Milliseconds before tooltips appear.\n"
          "Range 0–2000. Curvz default is 150 (GTK default is 500).\n"
          "Takes effect on next launch. May be a no-op on some recent\n"
          "GTK builds where the underlying property has been removed.");
      row->append(*spin);
      body->append(*row);

      spin->signal_value_changed().connect([this, spin, gen]() mutable {
        if (m_build_gen != gen || m_loading)
          return;
        int n = static_cast<int>(std::lround(spin->get_value()));
        AppPreferences::instance().set_tooltip_delay_ms(n);
        LOG_INFO("PropertiesPanel: tooltip_delay_ms → {}", n);
      });
    }
  }

  // ── Paths subsection (s145 m4) ─────────────────────────────────────────────
  // Override locations for the four built-in directories and files
  // Curvz manages on the user's behalf: the user library, the user
  // templates folder, the log file, and the user CSS file. Each row
  // is built by curvz::utils::make_path_override_row, which renders
  // a label + Entry + Browse + Reset. Empty Entry = use the default;
  // the placeholder shows what that default would be. All four take
  // effect on next launch (no live re-scan plumbing this milestone).
  //
  // The row helper handles its own commit semantics: writes happen
  // on Enter, focus loss, Browse confirmation, or Reset click. We
  // funnel each commit straight into the corresponding AppPreferences
  // setter, which trims and persists.
  {
    auto *body = add_collapsible("Paths", false, parent);

    // Walk to the top-level window for FileDialog modal-rooting. The
    // dynamic_cast can fail before the panel is attached (refresh
    // during construction) — the helper tolerates a nullptr by
    // making Browse a no-op, and Reset still works either way.
    Gtk::Window *root_win = dynamic_cast<Gtk::Window *>(get_root());

    // Default-path resolvers. These mirror the consumer-side fall-
    // through logic exactly, so the placeholder a user sees in the
    // Entry is the path that will be used when the override is empty.
    const std::string default_library_path =
        std::string(Glib::get_user_config_dir()) + "/curvz/library";
    const std::string default_templates_path =
        std::string(Glib::get_user_config_dir()) + "/curvz/templates";
    const std::string default_log_path =
        std::string(Glib::get_user_data_dir()) + "/curvz/curvz.log";
    const std::string default_css_path =
        std::string(Glib::get_user_config_dir()) + "/curvz/styles.css";

    body->append(*curvz::utils::make_path_override_row(
        "User library", AppPreferences::instance().library_path_override(),
        default_library_path,
        "Folder for your custom library categories and SVG icons.\n"
        "Empty = use the default. Takes effect on next launch.",
        /*pick_folder=*/true, root_win, [](const std::string &v) {
          AppPreferences::instance().set_library_path_override(v);
          LOG_INFO("PropertiesPanel: library_path_override → '{}'", v);
        }));

    body->append(*curvz::utils::make_path_override_row(
        "User templates", AppPreferences::instance().templates_path_override(),
        default_templates_path,
        "Folder for your custom document templates and the seed marker.\n"
        "Empty = use the default. Takes effect on next launch.",
        /*pick_folder=*/true, root_win, [](const std::string &v) {
          AppPreferences::instance().set_templates_path_override(v);
          LOG_INFO("PropertiesPanel: templates_path_override → '{}'", v);
        }));

    body->append(*curvz::utils::make_path_override_row(
        "Log file", AppPreferences::instance().log_path_override(),
        default_log_path,
        "Destination file for Curvz's log output.\n"
        "Empty = use the default. Takes effect on next launch — the\n"
        "logger initialises before this preference is read on the\n"
        "current run.",
        /*pick_folder=*/false, root_win, [](const std::string &v) {
          AppPreferences::instance().set_log_path_override(v);
          LOG_INFO("PropertiesPanel: log_path_override → '{}'", v);
        }));

    body->append(*curvz::utils::make_path_override_row(
        "Custom CSS", AppPreferences::instance().custom_css_path_override(),
        default_css_path,
        "User stylesheet loaded after Curvz's built-in CSS.\n"
        "Empty = use the default (which gets a stub written on first\n"
        "run). When set to a custom path, the stub is NOT seeded.\n"
        "Takes effect on next launch.",
        /*pick_folder=*/false, root_win, [](const std::string &v) {
          AppPreferences::instance().set_custom_css_path_override(v);
          LOG_INFO("PropertiesPanel: custom_css_path_override → '{}'", v);
        }));
  }

  // ── Boolean cleanup subsection (s143 m1) ──────────────────────────────────
  auto *body = add_collapsible("Boolean cleanup", false, parent);

  auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  box->set_spacing(2);
  box->set_margin_start(6);
  box->set_margin_end(6);
  box->set_margin_top(2);
  box->set_margin_bottom(4);
  body->append(*box);

  // ── End-label row ─────────────────────────────────────────────────────────
  // Two equal-width labels span the slider's track. Parentheticals do
  // the lifting — "Approximate" / "Faithful" alone are abstract; the
  // (fewer/more nodes) phrase tells the user what they're trading.
  auto *labels = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  labels->set_spacing(0);
  auto *lbl_lo = Gtk::make_managed<Gtk::Label>("Approximate (fewer nodes)");
  lbl_lo->set_xalign(0.0f);
  lbl_lo->set_hexpand(true);
  lbl_lo->add_css_class("dim-label");
  auto *lbl_hi = Gtk::make_managed<Gtk::Label>("Faithful (more nodes)");
  lbl_hi->set_xalign(1.0f);
  lbl_hi->set_hexpand(true);
  lbl_hi->add_css_class("dim-label");
  labels->append(*lbl_lo);
  labels->append(*lbl_hi);
  box->append(*labels);

  // ── Quality slider ────────────────────────────────────────────────────────
  // Range 0..10, integer steps. Current value comes from AppPreferences;
  // on user-driven change we write back through set_boolean_cleanup_quality
  // (which saves and emits signal_changed). MainWindow's connection to
  // that signal is what gets the new value into Canvas — we don't touch
  // Canvas here.
  //
  // No inline value readout (set_draw_value=false) — the inspector
  // column is narrow and the right-side value would crowd the
  // "Faithful" end label off-screen. Slider position + tick marks
  // already communicate the value visually.
  auto adj = Gtk::Adjustment::create(
      double(AppPreferences::instance().boolean_cleanup_quality()), 0.0, 10.0,
      1.0, 1.0, 0.0);
  auto *scale =
      Gtk::make_managed<Gtk::Scale>(adj, Gtk::Orientation::HORIZONTAL);
  curvz::utils::set_name(scale, "ins_app_bcq",
                         "inspector_app_boolean_cleanup_quality_scale");
  scale->set_digits(0); // integer values only
  scale->set_round_digits(0);
  scale->set_draw_value(false); // no inline number — keeps Faithful onscreen
  scale->set_hexpand(true);
  // Tooltip explains the tradeoff. The end labels carry the words but
  // not what the right end actually does — the tooltip is where the
  // user finds out that q=10 means "no cleanup, raw Clipper2 output."
  scale->set_tooltip_text(
      "Boolean cleanup quality.\n"
      "Approximate (left): fewer nodes, looser shape match.\n"
      "Faithful (right): more nodes, tighter shape match.\n"
      "Far right: no cleanup — raw Clipper2 output.");
  // Marks: tick at the default position (5), and a labeled "Raw" mark
  // at the right end so q=10's special meaning is visible without
  // hovering. Keeping it short ("Raw" not "Clipper2") so the label
  // doesn't push neighbouring marks around.
  scale->add_mark(5.0, Gtk::PositionType::BOTTOM, "");
  scale->add_mark(10.0, Gtk::PositionType::BOTTOM, "Raw");
  box->append(*scale);

  scale->signal_value_changed().connect([this, scale, gen]() mutable {
    if (m_build_gen != gen || m_loading)
      return;
    int q = static_cast<int>(std::lround(scale->get_value()));
    AppPreferences::instance().set_boolean_cleanup_quality(q);
    LOG_INFO("PropertiesPanel: boolean cleanup quality → {}", q);
  });

  // s155: inspector ▸ Application ▸ Warp removed. AppPreferences
  // remains the source of truth for warp defaults — populated
  // from the Object ▸ Warp section of the most recently edited
  // Warp, with hard-coded initial values for first launch.
}

// ── Object ▸ Guides section (s179 m3) ───────────────────────────────────────
//
// Always-visible Object-tier section. Mirrors build_warp_section's
// dual-mode pattern: the section is always present in the Object group,
// the body adapts to selection state.
//
// Selection cardinality:
//   - empty   → "From 2 points…" tool only (always-available creation verb)
//   - single  → per-guide editor: type label + lock toggle + delete,
//               then X / Y / A spinners (disabled when guide or layer
//               is locked); plus the construct tool below
//   - multi   → "N guides" + bulk-delete; plus the construct tool below
//
// The construct tool stays at the bottom of the body for all three
// states because it's a creation verb, not selection-driven editing —
// available regardless of what (if anything) is selected.
//
// Pre-s179, this content lived inside Document ▸ Theme ▸ Guides. Two
// problems with that placement: (1) when a guide was selected, users
// looked for it in Object first per the inspector's selection-drives-
// content convention; (2) the construct tool was buried inside the
// Theme disclosure which is set-and-forget for most workflows. Moving
// the editor + tool to Object ▸ Guides puts the right things in the
// right place. The Document-tier section keeps the colour swatch
// (theme-saveable style) and nothing else.
void PropertiesPanel::build_object_guides_section(CurvzDocument *doc,
                                                  Gtk::Box *parent) {
  uint32_t gen = m_build_gen;

  auto *body = add_collapsible("Guides", false, parent);
  curvz::utils::set_name(body, "ins_obj_gd", "inspector_object_guides_body");

  // No project / no doc — nothing to do. Section is still drawn (for
  // visual consistency in the Object group); body is empty.
  if (!doc) return;

  const SceneNode *gl = doc->guide_layer();
  bool layer_locked = gl && gl->locked;

  // ── Color row — always visible at the top of the body ────────────────
  // s179 m3: absorbed from build_guide_section. Edits doc->guide_color_*
  // directly; ThemeLibrary still round-trips these as part of the saved
  // theme bundle, so moving the inspector edit surface to Object scope
  // doesn't change what gets saved with a theme.
  {
    auto *color_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    color_row->add_css_class("prop-row");
    color_row->set_spacing(6);
    color_row->set_margin_top(2);
    color_row->set_margin_bottom(4);

    auto *color_lbl = Gtk::make_managed<Gtk::Label>("Color:");
    color_lbl->set_halign(Gtk::Align::START);
    color_lbl->set_hexpand(false);
    color_lbl->add_css_class("prop-lbl");
    color_row->append(*color_lbl);

    auto *swatch = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(swatch, "ins_obj_gd_color",
                           "inspector_object_guides_color_swatch_da");
    swatch->set_size_request(14, 14);
    swatch->set_valign(Gtk::Align::CENTER);
    swatch->set_can_target(true);
    swatch->set_draw_func(
        [doc](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
          cr->set_source_rgb(doc->guide_color_r, doc->guide_color_g,
                             doc->guide_color_b);
          cr->rectangle(0, 0, w, h);
          cr->fill();
        });
    auto swatch_click = Gtk::GestureClick::create();
    swatch_click->set_button(1);
    swatch_click->signal_pressed().connect(
        [this, doc, swatch, gen](int, double, double) {
          if (m_build_gen != gen)
            return;
          color::Color initial(doc->guide_color_r, doc->guide_color_g,
                               doc->guide_color_b, 1.0);
          m_color_popover.open(
              *swatch, initial, /*with_alpha=*/false,
              [this, doc, swatch, gen](const color::Color &c) {
                if (m_build_gen != gen)
                  return;
                doc->guide_color_r = c.r;
                doc->guide_color_g = c.g;
                doc->guide_color_b = c.b;
                swatch->queue_draw();
                emit_prop_changed();
              });
        });
    swatch->add_controller(swatch_click);
    color_row->append(*swatch);
    body->append(*color_row);
  }

  // ── Selection content ─────────────────────────────────────────────────
  if (m_guide_selection.empty()) {
    // Nothing selected — placeholder; construct tool below.
    auto *none_lbl = Gtk::make_managed<Gtk::Label>("No guide selected");
    none_lbl->set_xalign(0.0f);
    none_lbl->set_margin_start(10);
    none_lbl->add_css_class("dim-label");
    body->append(*none_lbl);

  } else if (m_guide_selection.size() == 1) {
    // ── Single guide selected — full editor ───────────────────────────
    SceneNode *g = m_guide_selection[0];

    // Type label row: "H", "V", or "A" + per-guide lock toggle + delete.
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(4);

      const char *type_label = g->guide_is_horizontal() ? "H"
                               : g->guide_is_vertical() ? "V"
                                                        : "A"; // Angled
      auto *hv_lbl = Gtk::make_managed<Gtk::Label>(type_label);
      hv_lbl->set_width_chars(2);
      hv_lbl->add_css_class("prop-lbl");
      row->append(*hv_lbl);

      auto *spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      spacer->set_hexpand(true);
      row->append(*spacer);

      // Per-guide lock toggle. s179 m3v3: switched from ToggleButton+
      // emoji glyph to plain Button+symbolic-icon to match the codebase's
      // canonical lock idiom (LayersPanel uses Button + curvz-locked-
      // symbolic / curvz-unlocked-symbolic everywhere). The emoji set
      // was the only place in the inspector using glyph-as-state; the
      // symbolic-icon set respects theme tinting and renders cleanly
      // at small sizes. Plain Button is correct here too: it's a
      // one-shot action that flips state and rebuilds the section,
      // not a stateful toggle (state lives on the SceneNode, not on
      // the button).
      auto *lock_btn = Gtk::make_managed<Gtk::Button>();
      curvz::utils::set_name(lock_btn, "ins_obj_gd_lock",
                             "inspector_object_guide_lock_btn");
      lock_btn->set_has_frame(false);
      lock_btn->set_icon_name(g->locked ? "curvz-locked-symbolic"
                                        : "curvz-unlocked-symbolic");
      lock_btn->set_tooltip_text(g->locked ? "Unlock guide"
                                           : "Lock guide");
      lock_btn->set_sensitive(!layer_locked);
      lock_btn->signal_clicked().connect([this, g, gen]() mutable {
        if (m_build_gen != gen)
          return;
        g->locked = !g->locked;
        emit_prop_changed();
        Glib::signal_idle().connect_once([this]() {
          if (m_project && m_canvas)
            show_document_props(m_canvas);
        });
      });
      row->append(*lock_btn);

      auto *del_btn = Gtk::make_managed<Gtk::Button>("×");
      curvz::utils::set_name(del_btn, "ins_obj_gd_del",
                             "inspector_object_guide_delete_btn");
      del_btn->set_has_frame(false);
      del_btn->set_tooltip_text("Delete guide");
      del_btn->set_sensitive(!layer_locked);
      del_btn->signal_clicked().connect([this, doc, g, gen]() mutable {
        if (m_build_gen != gen)
          return;
        m_guide_selection.clear();
        m_selected_guide = nullptr;
        SceneNode *gl2 = doc->guide_layer();
        if (gl2) {
          gl2->children.erase(
              std::remove_if(gl2->children.begin(), gl2->children.end(),
                             [g](const std::unique_ptr<SceneNode> &c) {
                               return c.get() == g;
                             }),
              gl2->children.end());
        }
        emit_prop_changed();
        m_sig_guide_layer_changed.emit();
        Glib::signal_idle().connect_once([this]() {
          if (m_project && m_canvas)
            show_document_props(m_canvas);
        });
      });
      row->append(*del_btn);
      body->append(*row);
    }

    // Edits disabled when the guide or its layer is locked.
    const bool edit_on = !layer_locked && !g->locked;

    // X position
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(4);

      auto *lbl = Gtk::make_managed<Gtk::Label>("X:");
      lbl->set_width_chars(2);
      lbl->set_xalign(1.0f);
      lbl->add_css_class("prop-lbl");
      row->append(*lbl);

      auto *sp = Gtk::make_managed<CurvzSpinButton>(SpinType::PositionX,
                                                    m_canvas, m_ruler_ox);
      curvz::utils::set_name(sp, "ins_obj_gd_x",
                             "inspector_object_guide_x_spn");
      sp->with_value(g->guide_x);
      sp->set_hexpand(true);
      sp->set_sensitive(edit_on);
      sp->on_changed([this, g, gen](double v) {
        if (m_build_gen != gen)
          return;
        g->guide_x = v;
        emit_prop_changed();
      });
      row->append(*sp);
      if (auto *ul = sp->get_unit_label())
        row->append(*ul);
      body->append(*row);
    }

    // Y position
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(4);

      auto *lbl = Gtk::make_managed<Gtk::Label>("Y:");
      lbl->set_width_chars(2);
      lbl->set_xalign(1.0f);
      lbl->add_css_class("prop-lbl");
      row->append(*lbl);

      auto *sp = Gtk::make_managed<CurvzSpinButton>(SpinType::PositionY,
                                                    m_canvas, m_ruler_oy);
      curvz::utils::set_name(sp, "ins_obj_gd_y",
                             "inspector_object_guide_y_spn");
      sp->with_value(g->guide_y);
      sp->set_hexpand(true);
      sp->set_sensitive(edit_on);
      sp->on_changed([this, g, gen](double v) {
        if (m_build_gen != gen)
          return;
        g->guide_y = v;
        emit_prop_changed();
      });
      row->append(*sp);
      if (auto *ul = sp->get_unit_label())
        row->append(*ul);
      body->append(*row);
    }

    // Angle (degrees)
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->add_css_class("prop-row");
      row->set_spacing(4);

      auto *lbl = Gtk::make_managed<Gtk::Label>("A:");
      lbl->set_width_chars(2);
      lbl->set_xalign(1.0f);
      lbl->add_css_class("prop-lbl");
      row->append(*lbl);

      auto *sp = Gtk::make_managed<CurvzSpinButton>(SpinType::Angle, m_canvas);
      curvz::utils::set_name(sp, "ins_obj_gd_a",
                             "inspector_object_guide_angle_spn");
      sp->with_value(g->guide_angle);
      sp->set_hexpand(true);
      sp->set_sensitive(edit_on);
      sp->on_changed([this, g, gen](double v) {
        if (m_build_gen != gen)
          return;
        g->guide_angle = v;
        emit_prop_changed();
      });
      row->append(*sp);
      if (auto *ul = sp->get_unit_label())
        row->append(*ul);
      body->append(*row);
    }

  } else {
    // ── Multiple guides selected ──────────────────────────────────────
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->add_css_class("prop-row");
    row->set_spacing(4);

    char lbl_buf[32];
    snprintf(lbl_buf, sizeof(lbl_buf), "%d guides",
             (int)m_guide_selection.size());
    auto *count_lbl = Gtk::make_managed<Gtk::Label>(lbl_buf);
    count_lbl->set_xalign(0.0f);
    count_lbl->set_hexpand(true);
    count_lbl->add_css_class("prop-val-lbl");
    row->append(*count_lbl);

    auto *del_btn = Gtk::make_managed<Gtk::Button>("×");
    curvz::utils::set_name(del_btn, "ins_obj_gd_mdel",
                           "inspector_object_guides_multi_delete_btn");
    del_btn->set_has_frame(false);
    del_btn->set_tooltip_text("Delete selected guides");
    del_btn->set_sensitive(!layer_locked);
    del_btn->signal_clicked().connect([this, doc, gen]() mutable {
      if (m_build_gen != gen)
        return;
      SceneNode *gl2 = doc->guide_layer();
      if (gl2) {
        for (SceneNode *g : m_guide_selection) {
          gl2->children.erase(
              std::remove_if(gl2->children.begin(), gl2->children.end(),
                             [g](const std::unique_ptr<SceneNode> &c) {
                               return c.get() == g;
                             }),
              gl2->children.end());
        }
      }
      m_guide_selection.clear();
      m_selected_guide = nullptr;
      emit_prop_changed();
      m_sig_guide_layer_changed.emit();
      Glib::signal_idle().connect_once([this]() {
        if (m_project && m_canvas)
          show_document_props(m_canvas);
      });
    });
    row->append(*del_btn);
    body->append(*row);
  }

  // ── Construct guide from 2 points — always available ─────────────────
  // Launches a canvas-side mode that captures two clicks (with node-snap),
  // shows a preview + review dialog, and commits on OK. Sits at the bottom
  // of the body regardless of selection state — it's a creation verb,
  // available whether or not a guide is selected.
  {
    auto *tools_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    tools_row->add_css_class("prop-row");
    tools_row->set_spacing(4);
    tools_row->set_margin_top(4);

    auto *from_nodes_btn = Gtk::make_managed<Gtk::Button>("From 2 points…");
    curvz::utils::set_name(from_nodes_btn, "ins_obj_gd_f2p",
                           "inspector_object_guides_from_2_points_btn");
    from_nodes_btn->set_has_frame(false);
    from_nodes_btn->add_css_class("flat");
    from_nodes_btn->set_halign(Gtk::Align::START);
    from_nodes_btn->set_tooltip_text(
        "Click two points (snaps to path nodes) to construct a guide. "
        "Press P in the review dialog to flip to the perpendicular.");
    from_nodes_btn->set_sensitive(!layer_locked);
    from_nodes_btn->signal_clicked().connect(
        [this]() { m_sig_guide_construct_requested.emit(); });
    tools_row->append(*from_nodes_btn);
    body->append(*tools_row);
  }
}

// ── Snap section: deleted s150 ───────────────────────────────────────────
// Snap behaviour now lives at the toolbar Snap switch + its right-click
// popover (see Toolbar::build_snap_popover and the m_toolbar.signal_snap_*
// handlers in MainWindow.cpp). Storage on doc.snap is unchanged; only
// the inspector surface went away. The toolbar is the canonical writer
// (it already wrote to both project->snap and every doc->snap).
//
// Per ARC.md design rule 1 ("behaviour at the tool, style in the
// inspector"), Snap is behaviour — how the editor reacts during use —
// not style, so it doesn't belong in an inspector section.

// ── Grid section
// ──────────────────────────────────────────────────────────────
void PropertiesPanel::build_grid_section(CurvzDocument *doc, Gtk::Box *parent) {
  uint32_t gen = m_build_gen;
  auto *body = add_collapsible("Grid", false, parent);

  SceneNode *gl = doc->grid_layer();
  bool enabled = (gl != nullptr);

  // ── Enable toggle ────────────────────────────────────────────────────
  auto *en_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  en_row->add_css_class("prop-row");
  auto *en_lbl = Gtk::make_managed<Gtk::Label>("Enable");
  en_lbl->add_css_class("prop-lbl");
  en_lbl->set_width_chars(8);
  en_lbl->set_xalign(0.0f);
  auto *en_sw = Gtk::make_managed<Gtk::CheckButton>();
  curvz::utils::set_name(en_sw, "ins_grd_en", "inspector_grid_enable_check");
  en_sw->set_active(enabled);
  en_sw->set_halign(Gtk::Align::START);
  en_row->append(*en_lbl);
  en_row->append(*en_sw);
  body->append(*en_row);

  if (!enabled) {
    en_sw->signal_toggled().connect([this, doc, gen]() {
      if (m_build_gen != gen || m_loading)
        return;
      doc->ensure_grid_layer();
      emit_prop_changed();
      Glib::signal_idle().connect_once(
          [this]() { refresh(m_canvas, m_current); });
    });
    return;
  }

  body->set_sensitive(!gl->locked);

  // ── Style dropdown ───────────────────────────────────────────────────
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->add_css_class("prop-row");
    auto *lbl = Gtk::make_managed<Gtk::Label>("Style");
    lbl->add_css_class("prop-lbl");
    lbl->set_width_chars(8);
    lbl->set_xalign(0.0f);
    auto *drop = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"Lines", "Dots"}));
    curvz::utils::set_name(drop, "ins_grd_st", "inspector_grid_style_dd");
    drop->set_selected(gl->grid_dots ? 1 : 0);
    drop->set_hexpand(true);
    drop->add_css_class("prop-dropdown");
    drop->property_selected().signal_changed().connect(
        [this, doc, drop, gen]() {
          if (m_build_gen != gen || m_loading)
            return;
          if (auto *g = doc->grid_layer())
            g->grid_dots = (drop->get_selected() == 1);
          emit_prop_changed();
        });
    row->append(*lbl);
    row->append(*drop);
    body->append(*row);
  }

  const CanvasModel *cm = m_canvas;

  auto make_pair_grid = [&]() {
    auto *g = Gtk::make_managed<Gtk::Grid>();
    g->set_row_spacing(3);
    g->set_column_spacing(6);
    g->set_margin_start(8);
    g->set_margin_end(6);
    g->set_margin_top(4);
    g->set_margin_bottom(2);
    return g;
  };
  auto make_hdr = [](const char *t) {
    auto *l = Gtk::make_managed<Gtk::Label>(t);
    l->set_xalign(0.5f);
    l->set_hexpand(true);
    l->add_css_class("prop-lbl");
    return l;
  };

  // ── Spacing X / Y ────────────────────────────────────────────────────
  {
    auto *pg = make_pair_grid();
    auto *lbl = Gtk::make_managed<Gtk::Label>("SPACING");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("prop-lbl");
    m_grid_lbl_spacing = lbl;
    pg->attach(*lbl, 0, 0, 3, 1);
    pg->attach(*make_hdr("X"), 1, 1);
    pg->attach(*make_hdr("Y"), 2, 1);

    m_grid_sp_sx =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    curvz::utils::set_name(m_grid_sp_sx, "ins_grd_sx",
                           "inspector_grid_spacing_x_spn");
    m_grid_sp_sx->with_value(gl->grid_spacing_x)->with_css("prop-width-entry");
    m_grid_sp_sx->set_hexpand(true);
    block_scroll(m_grid_sp_sx, [this] { emit_canvas_focus(); });

    m_grid_sp_sy =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    curvz::utils::set_name(m_grid_sp_sy, "ins_grd_sy",
                           "inspector_grid_spacing_y_spn");
    m_grid_sp_sy->with_value(gl->grid_spacing_y)->with_css("prop-width-entry");
    m_grid_sp_sy->set_hexpand(true);
    block_scroll(m_grid_sp_sy, [this] { emit_canvas_focus(); });

    pg->attach(*m_grid_sp_sx, 1, 2);
    pg->attach(*m_grid_sp_sy, 2, 2);
    body->append(*pg);

    m_grid_sp_sx->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      if (auto *g = doc->grid_layer())
        g->grid_spacing_x = v;
      emit_prop_changed();
    });
    m_grid_sp_sy->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      if (auto *g = doc->grid_layer())
        g->grid_spacing_y = v;
      emit_prop_changed();
    });
  }

  // ── Offset X / Y ─────────────────────────────────────────────────────
  {
    auto *pg = make_pair_grid();
    auto *lbl = Gtk::make_managed<Gtk::Label>("OFFSET");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("prop-lbl");
    m_grid_lbl_offset = lbl;
    pg->attach(*lbl, 0, 0, 3, 1);
    pg->attach(*make_hdr("X"), 1, 1);
    pg->attach(*make_hdr("Y"), 2, 1);

    m_grid_sp_ox =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
    curvz::utils::set_name(m_grid_sp_ox, "ins_grd_ox",
                           "inspector_grid_offset_x_spn");
    m_grid_sp_ox->with_value(gl->grid_offset_x)->with_css("prop-width-entry");
    m_grid_sp_ox->set_hexpand(true);
    block_scroll(m_grid_sp_ox, [this] { emit_canvas_focus(); });

    m_grid_sp_oy =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
    curvz::utils::set_name(m_grid_sp_oy, "ins_grd_oy",
                           "inspector_grid_offset_y_spn");
    m_grid_sp_oy->with_value(gl->grid_offset_y)->with_css("prop-width-entry");
    m_grid_sp_oy->set_hexpand(true);
    block_scroll(m_grid_sp_oy, [this] { emit_canvas_focus(); });

    pg->attach(*m_grid_sp_ox, 1, 2);
    pg->attach(*m_grid_sp_oy, 2, 2);
    body->append(*pg);

    m_grid_sp_ox->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      if (auto *g = doc->grid_layer())
        g->grid_offset_x = v;
      emit_prop_changed();
    });
    m_grid_sp_oy->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      if (auto *g = doc->grid_layer())
        g->grid_offset_y = v;
      emit_prop_changed();
    });
  }

  // ── Color swatch ─────────────────────────────────────────────────────
  {
    auto *crow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    crow->add_css_class("prop-row");
    auto *clbl = Gtk::make_managed<Gtk::Label>("Color");
    clbl->add_css_class("prop-lbl");
    clbl->set_width_chars(8);
    clbl->set_xalign(0.0f);
    auto *swatch = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(swatch, "ins_grd_color",
                           "inspector_grid_color_swatch_da");
    swatch->set_size_request(40, 18);
    swatch->set_valign(Gtk::Align::CENTER);
    swatch->set_draw_func(
        [doc](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
          auto *g = doc->grid_layer();
          if (!g)
            return;
          cr->set_source_rgba(g->grid_color_r, g->grid_color_g, g->grid_color_b,
                              g->grid_color_a);
          cr->rectangle(0, 0, w, h);
          cr->fill();
        });
    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect(
        [this, doc, swatch, gen](int, double, double) {
          if (m_build_gen != gen)
            return;
          auto *g = doc->grid_layer();
          if (!g)
            return;
          color::Color initial(g->grid_color_r, g->grid_color_g,
                               g->grid_color_b, g->grid_color_a);
          m_color_popover.open(*swatch, initial, /*with_alpha=*/true,
                               [this, doc, swatch, gen](const color::Color &c) {
                                 if (m_build_gen != gen)
                                   return;
                                 auto *g = doc->grid_layer();
                                 if (!g)
                                   return;
                                 g->grid_color_r = c.r;
                                 g->grid_color_g = c.g;
                                 g->grid_color_b = c.b;
                                 g->grid_color_a = c.a;
                                 swatch->queue_draw();
                                 emit_prop_changed();
                               });
        });
    swatch->add_controller(click);
    crow->append(*clbl);
    crow->append(*swatch);
    body->append(*crow);
  }

  // ── Disable toggle ───────────────────────────────────────────────────
  en_sw->signal_toggled().connect([this, doc, gen]() {
    if (m_build_gen != gen || m_loading)
      return;
    auto &layers = doc->layers;
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                [](const std::unique_ptr<SceneNode> &l) {
                                  return l->is_grid_layer();
                                }),
                 layers.end());
    emit_prop_changed();
    Glib::signal_idle().connect_once(
        [this]() { refresh(m_canvas, m_current); });
  });
}

// ── Margin section
// ────────────────────────────────────────────────────────────
void PropertiesPanel::build_margin_section(CurvzDocument *doc,
                                           Gtk::Box *parent) {
  uint32_t gen = m_build_gen;
  auto *body = add_collapsible("Margins", false, parent);

  SceneNode *ml = doc->margin_layer();
  bool enabled = (ml != nullptr);

  // ── Enable toggle ────────────────────────────────────────────────────
  auto *en_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  en_row->add_css_class("prop-row");
  auto *en_lbl = Gtk::make_managed<Gtk::Label>("Enable");
  en_lbl->add_css_class("prop-lbl");
  en_lbl->set_width_chars(8);
  en_lbl->set_xalign(0.0f);
  auto *en_sw = Gtk::make_managed<Gtk::CheckButton>();
  curvz::utils::set_name(en_sw, "ins_mrg_en", "inspector_margins_enable_check");
  en_sw->set_active(enabled);
  en_sw->set_halign(Gtk::Align::START);
  en_row->append(*en_lbl);
  en_row->append(*en_sw);
  body->append(*en_row);

  if (!enabled) {
    en_sw->signal_toggled().connect([this, doc, gen]() {
      if (m_build_gen != gen || m_loading)
        return;
      doc->ensure_margin_layer();
      emit_prop_changed();
      Glib::signal_idle().connect_once(
          [this]() { refresh(m_canvas, m_current); });
    });
    return;
  }

  body->set_sensitive(!ml->locked);
  const CanvasModel *mcm = m_canvas;

  auto make_pg = [&]() {
    auto *g = Gtk::make_managed<Gtk::Grid>();
    g->set_row_spacing(3);
    g->set_column_spacing(6);
    g->set_margin_start(8);
    g->set_margin_end(6);
    g->set_margin_top(4);
    g->set_margin_bottom(2);
    return g;
  };
  auto make_hdr = [](const char *t) {
    auto *l = Gtk::make_managed<Gtk::Label>(t);
    l->set_xalign(0.5f);
    l->set_hexpand(true);
    l->add_css_class("prop-lbl");
    return l;
  };
  auto make_sp = [&](double doc_val) {
    auto *sp = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    sp->with_value(doc_val)->with_css("prop-width-entry");
    sp->set_hexpand(true);
    block_scroll(sp, [this] { emit_canvas_focus(); });
    return sp;
  };

  // ── TOP / BOTTOM ─────────────────────────────────────────────────────
  {
    auto *pg = make_pg();
    auto *lbl = Gtk::make_managed<Gtk::Label>("MARGINS");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("prop-lbl");
    m_margin_lbl_insets = lbl;
    pg->attach(*lbl, 0, 0, 3, 1);
    pg->attach(*make_hdr("TOP"), 1, 1);
    pg->attach(*make_hdr("BOTTOM"), 2, 1);
    m_margin_sp_t = make_sp(ml->margin_top);
    curvz::utils::set_name(m_margin_sp_t, "ins_mrg_t",
                           "inspector_margins_top_spn");
    m_margin_sp_b = make_sp(ml->margin_bottom);
    curvz::utils::set_name(m_margin_sp_b, "ins_mrg_b",
                           "inspector_margins_bottom_spn");
    pg->attach(*m_margin_sp_t, 1, 2);
    pg->attach(*m_margin_sp_b, 2, 2);
    body->append(*pg);

    m_margin_sp_t->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_top = v;
      emit_prop_changed();
    });
    m_margin_sp_b->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_bottom = v;
      emit_prop_changed();
    });
  }

  // ── LEFT / RIGHT ─────────────────────────────────────────────────────
  {
    auto *pg = make_pg();
    pg->attach(*make_hdr("LEFT"), 1, 0);
    pg->attach(*make_hdr("RIGHT"), 2, 0);
    m_margin_sp_l = make_sp(ml->margin_left);
    curvz::utils::set_name(m_margin_sp_l, "ins_mrg_l",
                           "inspector_margins_left_spn");
    m_margin_sp_r = make_sp(ml->margin_right);
    curvz::utils::set_name(m_margin_sp_r, "ins_mrg_r",
                           "inspector_margins_right_spn");
    pg->attach(*m_margin_sp_l, 1, 1);
    pg->attach(*m_margin_sp_r, 2, 1);
    body->append(*pg);

    m_margin_sp_l->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_left = v;
      emit_prop_changed();
    });
    m_margin_sp_r->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_right = v;
      emit_prop_changed();
    });
  }

  // ── COLUMNS / GAP ────────────────────────────────────────────────────
  {
    auto *pg = make_pg();
    auto *lbl = Gtk::make_managed<Gtk::Label>("COLUMNS  GAP");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("prop-lbl");
    m_margin_lbl_cgap = lbl;
    pg->attach(*lbl, 0, 0, 3, 1);
    pg->attach(*make_hdr("COUNT"), 1, 1);
    pg->attach(*make_hdr("GAP"), 2, 1);

    auto adj_cols =
        Gtk::Adjustment::create((double)ml->margin_columns, 1, 100, 1, 5);
    m_margin_sp_cg = Gtk::make_managed<Gtk::SpinButton>(adj_cols, 1.0, 0);
    curvz::utils::set_name(m_margin_sp_cg, "ins_mrg_cn",
                           "inspector_margins_col_count_spn");
    m_margin_sp_cg->set_hexpand(true);
    m_margin_sp_cg->add_css_class("prop-width-entry");
    block_scroll(m_margin_sp_cg, [this] { emit_canvas_focus(); });

    m_margin_sp_cgap =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    curvz::utils::set_name(m_margin_sp_cgap, "ins_mrg_cg",
                           "inspector_margins_col_gap_spn");
    m_margin_sp_cgap->with_value(ml->margin_col_gap)
        ->with_css("prop-width-entry");
    m_margin_sp_cgap->set_hexpand(true);
    block_scroll(m_margin_sp_cgap, [this] { emit_canvas_focus(); });

    pg->attach(*m_margin_sp_cg, 1, 2);
    pg->attach(*m_margin_sp_cgap, 2, 2);
    body->append(*pg);

    adj_cols->signal_value_changed().connect([this, doc, adj_cols, gen]() {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_columns = std::max(1, (int)adj_cols->get_value());
      emit_prop_changed();
    });
    m_margin_sp_cgap->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_col_gap = v;
      emit_prop_changed();
    });
  }

  // ── ROWS / GAP ───────────────────────────────────────────────────────
  {
    auto *pg = make_pg();
    auto *lbl = Gtk::make_managed<Gtk::Label>("ROWS  GAP");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("prop-lbl");
    m_margin_lbl_rgap = lbl;
    pg->attach(*lbl, 0, 0, 3, 1);
    pg->attach(*make_hdr("COUNT"), 1, 1);
    pg->attach(*make_hdr("GAP"), 2, 1);

    auto adj_rows =
        Gtk::Adjustment::create((double)ml->margin_rows, 1, 100, 1, 5);
    m_margin_sp_rg = Gtk::make_managed<Gtk::SpinButton>(adj_rows, 1.0, 0);
    curvz::utils::set_name(m_margin_sp_rg, "ins_mrg_rn",
                           "inspector_margins_row_count_spn");
    m_margin_sp_rg->set_hexpand(true);
    m_margin_sp_rg->add_css_class("prop-width-entry");
    block_scroll(m_margin_sp_rg, [this] { emit_canvas_focus(); });

    m_margin_sp_rgap =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    curvz::utils::set_name(m_margin_sp_rgap, "ins_mrg_rg",
                           "inspector_margins_row_gap_spn");
    m_margin_sp_rgap->with_value(ml->margin_row_gap)
        ->with_css("prop-width-entry");
    m_margin_sp_rgap->set_hexpand(true);
    block_scroll(m_margin_sp_rgap, [this] { emit_canvas_focus(); });

    pg->attach(*m_margin_sp_rg, 1, 2);
    pg->attach(*m_margin_sp_rgap, 2, 2);
    body->append(*pg);

    adj_rows->signal_value_changed().connect([this, doc, adj_rows, gen]() {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_rows = std::max(1, (int)adj_rows->get_value());
      emit_prop_changed();
    });
    m_margin_sp_rgap->on_changed([this, doc, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      auto *m = doc->margin_layer();
      if (!m)
        return;
      m->margin_row_gap = v;
      emit_prop_changed();
    });
  }

  // ── Color swatch ─────────────────────────────────────────────────────
  {
    auto *crow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    crow->add_css_class("prop-row");
    auto *clbl = Gtk::make_managed<Gtk::Label>("Color");
    clbl->add_css_class("prop-lbl");
    clbl->set_width_chars(8);
    clbl->set_xalign(0.0f);
    auto *swatch = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(swatch, "ins_mrg_color",
                           "inspector_margins_color_swatch_da");
    swatch->set_size_request(40, 18);
    swatch->set_valign(Gtk::Align::CENTER);
    swatch->set_draw_func(
        [doc](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
          auto *m = doc->margin_layer();
          if (!m)
            return;
          cr->set_source_rgba(m->margin_color_r, m->margin_color_g,
                              m->margin_color_b, m->margin_color_a);
          cr->rectangle(0, 0, w, h);
          cr->fill();
        });
    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect(
        [this, doc, swatch, gen](int, double, double) {
          if (m_build_gen != gen)
            return;
          auto *m = doc->margin_layer();
          if (!m)
            return;
          color::Color initial(m->margin_color_r, m->margin_color_g,
                               m->margin_color_b, m->margin_color_a);
          m_color_popover.open(*swatch, initial, /*with_alpha=*/true,
                               [this, doc, swatch, gen](const color::Color &c) {
                                 if (m_build_gen != gen)
                                   return;
                                 auto *m = doc->margin_layer();
                                 if (!m)
                                   return;
                                 m->margin_color_r = c.r;
                                 m->margin_color_g = c.g;
                                 m->margin_color_b = c.b;
                                 m->margin_color_a = c.a;
                                 swatch->queue_draw();
                                 emit_prop_changed();
                               });
        });
    swatch->add_controller(click);
    crow->append(*clbl);
    crow->append(*swatch);
    body->append(*crow);
  }

  // ── Disable toggle ───────────────────────────────────────────────────
  en_sw->signal_toggled().connect([this, doc, gen]() {
    if (m_build_gen != gen || m_loading)
      return;
    auto &layers = doc->layers;
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                [](const std::unique_ptr<SceneNode> &l) {
                                  return l->is_margin_layer();
                                }),
                 layers.end());
    emit_prop_changed();
    Glib::signal_idle().connect_once(
        [this]() { refresh(m_canvas, m_current); });
  });
}

// ── update_grid_margin_units — refresh CurvzSpinButton unit display ──────────
// Called when the canvas display unit changes. Each CurvzSpinButton handles
// its own reconversion via refresh_units().
void PropertiesPanel::update_grid_margin_units() {
  for (auto *sp : {m_grid_sp_sx, m_grid_sp_sy, m_grid_sp_ox, m_grid_sp_oy,
                   m_margin_sp_t, m_margin_sp_b, m_margin_sp_l, m_margin_sp_r,
                   m_margin_sp_cgap, m_margin_sp_rgap})
    if (sp)
      sp->refresh_units();
}

// ── Inspector bbox helper ───────────────────────────────────────────────────
//
// Expand minx/maxx/miny/maxy to enclose `path`.
//
// For Ellipse-kind NodeSets we sample the cubic segments (32 points/seg,
// matching Canvas::object_bbox) instead of reading node_sets[0].params.
// The params drift stale the moment an oval is moved, scaled, or rotated —
// nothing on the mutation side syncs them back — and the inspector was the
// last reader trusting them. Curve-sampling makes the inspector bbox track
// the canvas selection bbox exactly for any oval (axis-aligned or rotated).
//
// For non-Ellipse paths we keep the legacy handle-extent bbox: it's an
// upper bound on the curve, not a tight bound, but Scott hasn't flagged
// this as a problem and tightening freeform-path bboxes is out of scope
// for s93. Filed as backlog if/when canvas-vs-inspector mismatch becomes
// visible there too.
static void expand_bbox_for_path(const PathData &p, double &minx, double &maxx,
                                 double &miny, double &maxy) {
  if (p.nodes.empty())
    return;

  const bool is_oval =
      !p.node_sets.empty() && p.node_sets[0].kind == NodeSet::Kind::Ellipse;

  if (is_oval) {
    auto expand_cubic = [&](double p0x, double p0y, double p1x, double p1y,
                            double p2x, double p2y, double p3x, double p3y) {
      for (int s = 0; s <= 32; ++s) {
        double t = s / 32.0;
        double mt = 1.0 - t;
        double x = mt * mt * mt * p0x + 3 * mt * mt * t * p1x +
                   3 * mt * t * t * p2x + t * t * t * p3x;
        double y = mt * mt * mt * p0y + 3 * mt * mt * t * p1y +
                   3 * mt * t * t * p2y + t * t * t * p3y;
        minx = std::min(minx, x);
        maxx = std::max(maxx, x);
        miny = std::min(miny, y);
        maxy = std::max(maxy, y);
      }
    };
    int n = (int)p.nodes.size();
    for (int i = 0; i < n - 1; ++i) {
      expand_cubic(p.nodes[i].x, p.nodes[i].y, p.nodes[i].cx2, p.nodes[i].cy2,
                   p.nodes[i + 1].cx1, p.nodes[i + 1].cy1, p.nodes[i + 1].x,
                   p.nodes[i + 1].y);
    }
    if (p.closed && n > 1) {
      expand_cubic(p.nodes[n - 1].x, p.nodes[n - 1].y, p.nodes[n - 1].cx2,
                   p.nodes[n - 1].cy2, p.nodes[0].cx1, p.nodes[0].cy1,
                   p.nodes[0].x, p.nodes[0].y);
    }
    return;
  }

  // Non-oval: handle-extent bbox (legacy behaviour).
  for (const auto &n : p.nodes) {
    minx = std::min({minx, n.x, n.cx1, n.cx2});
    maxx = std::max({maxx, n.x, n.cx1, n.cx2});
    miny = std::min({miny, n.y, n.cy1, n.cy2});
    maxy = std::max({maxy, n.y, n.cy1, n.cy2});
  }
}

// ── compute_visual_bbox ──────────────────────────────────────────────────────
// Visual (stroked) bounding box across N leaves. Each leaf contributes its
// construction bbox padded by stroke.width/2 on all four sides if it has a
// non-None stroke with positive width; otherwise the leaf's construction
// bbox is used unchanged.
//
// Center-stroke only — Curvz doesn't support inside/outside stroke alignment
// (and isn't planned to). The width/2 pad is also a conservative
// approximation for round/square caps and miter joins; sharp miters can
// extend further than width/2 past a corner but that's a rare edge case for
// the Inspector readout.
//
// Returns false in `any_stroke` (out param) when no leaf contributes a
// stroke contribution — caller uses this to hide the Visual rows.
static void compute_visual_bbox(const std::vector<SceneNode *> &leaves,
                                double &vx, double &vy, double &vw, double &vh,
                                bool &any_stroke) {
  double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
  any_stroke = false;
  for (SceneNode *leaf : leaves) {
    if (!leaf || !leaf->path)
      continue;
    double lminx = 1e18, lmaxx = -1e18, lminy = 1e18, lmaxy = -1e18;
    expand_bbox_for_path(*leaf->path, lminx, lmaxx, lminy, lmaxy);
    if (lminx > lmaxx)
      continue; // empty path
    double pad = 0.0;
    if (leaf->stroke.paint.type != FillStyle::Type::None &&
        leaf->stroke.width > 0.0) {
      pad = leaf->stroke.width * 0.5;
      any_stroke = true;
    }
    minx = std::min(minx, lminx - pad);
    maxx = std::max(maxx, lmaxx + pad);
    miny = std::min(miny, lminy - pad);
    maxy = std::max(maxy, lmaxy + pad);
  }
  if (minx > maxx) {
    vx = vy = vw = vh = 0.0;
    return;
  }
  vx = minx;
  vy = miny;
  vw = maxx - minx;
  vh = maxy - miny;
}

// ── Selection section — X Y W H ──────────────────────────────────────────────
void PropertiesPanel::build_selection_section(SceneNode *obj,
                                              Gtk::Box *parent) {
  // ── Ref point(s) ──────────────────────────────────────────────────────
  // obj is the primary selected — check if it's a ref, or if canvas has
  // a multi-ref selection. We get the full selection via m_canvas (if set).
  bool is_ref = obj && obj->type == SceneNode::Type::Ref;

  // Collect ref points from the full canvas selection
  std::vector<SceneNode *> ref_pts;
  if (m_canvas) {
    // Walk selection from canvas — we need a way to get it.
    // PropertiesPanel stores m_canvas (CanvasModel*). If obj is a ref,
    // gather all refs from canvas selection via signal or just handle single.
  }
  // For now: if primary selected obj is a ref, build ref inspector
  if (is_ref) {
    auto *body = add_collapsible("Selection", false, parent);
    curvz::utils::set_name(body, "ins_sel_ref", "inspector_selection_ref_body");

    auto *grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(3);
    grid->set_column_spacing(4);
    grid->set_margin_start(8);
    grid->set_margin_end(6);
    grid->set_margin_top(4);
    grid->set_margin_bottom(4);

    auto make_pos = [this](SpinType t, double doc_val, double ro) {
      auto *s = Gtk::make_managed<CurvzSpinButton>(t, m_canvas, ro);
      s->with_value(doc_val)
          ->with_css("prop-width-entry")
          ->with_css("node-spin");
      s->with_width_chars(10);
      s->set_hexpand(true);
      block_scroll(s, [this] { emit_canvas_focus(); });
      return s;
    };
    auto make_lbl = [](const char *t) {
      auto *l = Gtk::make_managed<Gtk::Label>(t);
      l->add_css_class("prop-lbl");
      l->set_xalign(0.0f);
      return l;
    };
    auto make_hdr = [](const char *t) {
      auto *l = Gtk::make_managed<Gtk::Label>(t);
      l->add_css_class("prop-lbl");
      l->set_xalign(0.5f);
      l->set_hexpand(true);
      return l;
    };

    grid->attach(*make_hdr("X"), 1, 0);
    grid->attach(*make_hdr("Y"), 2, 0);

    auto *sp_x = make_pos(SpinType::PositionX, obj->ref_x, m_ruler_ox);
    auto *sp_y = make_pos(SpinType::PositionY, obj->ref_y, m_ruler_oy);
    curvz::utils::set_name(sp_x, "ins_sel_ref_x",
                           "inspector_selection_ref_x_spn");
    curvz::utils::set_name(sp_y, "ins_sel_ref_y",
                           "inspector_selection_ref_y_spn");
    m_ref_sp_x = sp_x;
    m_ref_sp_y = sp_y;
    grid->attach(*make_lbl("POS"), 0, 1);
    grid->attach(*sp_x, 1, 1);
    grid->attach(*sp_y, 2, 1);

    // W/H = 0, read-only
    grid->attach(*make_hdr("WIDTH"), 1, 2);
    grid->attach(*make_hdr("HEIGHT"), 2, 2);
    auto *w_lbl = Gtk::make_managed<Gtk::Label>("0.000000");
    curvz::utils::set_name(w_lbl, "ins_sel_ref_wv",
                           "inspector_selection_ref_width_value_lbl");
    w_lbl->add_css_class("prop-lbl");
    w_lbl->set_xalign(0.5f);
    w_lbl->set_hexpand(true);
    auto *h_lbl = Gtk::make_managed<Gtk::Label>("0.000000");
    curvz::utils::set_name(h_lbl, "ins_sel_ref_hv",
                           "inspector_selection_ref_height_value_lbl");
    h_lbl->add_css_class("prop-lbl");
    h_lbl->set_xalign(0.5f);
    h_lbl->set_hexpand(true);
    grid->attach(*make_lbl("SIZE"), 0, 3);
    grid->attach(*w_lbl, 1, 3);
    grid->attach(*h_lbl, 2, 3);

    body->append(*grid);

    sp_x->on_changed([this, obj](double v) {
      if (m_loading)
        return;
      double old_x = obj->ref_x;
      obj->ref_x = v;
      // s177 m6: drop the coordinate-as-name stomp here — same bug
      // class as Canvas_input's drag handler. Position changes are
      // position changes; the user's name survives.
      // s168 m1 DIAG — STRIP after triage
      LOG_INFO("[IIDDIAG] RefMove::push X  capturing iid='{}' "
               "obj_name='{}' obj_type={}  old_x={} new_x={} ref_y={}",
               obj->internal_id, obj->name, (int)obj->type,
               old_x, v, obj->ref_y);
      if (m_history)
        m_history->push(std::make_unique<RefMoveCommand>(
            m_project, obj->internal_id,
            old_x, obj->ref_y, v, obj->ref_y));
      emit_prop_changed();
    });

    sp_y->on_changed([this, obj](double v) {
      if (m_loading)
        return;
      double old_y = obj->ref_y;
      obj->ref_y = v;
      // s177 m6: drop the coordinate-as-name stomp here too.
      // s168 m1 DIAG — STRIP after triage
      LOG_INFO("[IIDDIAG] RefMove::push Y  capturing iid='{}' "
               "obj_name='{}' obj_type={}  ref_x={} old_y={} new_y={}",
               obj->internal_id, obj->name, (int)obj->type,
               obj->ref_x, old_y, v);
      if (m_history)
        m_history->push(std::make_unique<RefMoveCommand>(
            m_project, obj->internal_id,
            obj->ref_x, old_y, obj->ref_x, v));
      emit_prop_changed();
    });

    // ── Flip buttons ────────────────────────────────────────────────────
    {
      auto *flip_row =
          Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      flip_row->set_spacing(4);
      flip_row->set_margin_start(8);
      flip_row->set_margin_end(6);
      flip_row->set_margin_top(4);
      flip_row->set_margin_bottom(6);
      auto *btn_fh = Gtk::make_managed<Gtk::Button>("⇔ Flip H");
      curvz::utils::set_name(btn_fh, "ins_sel_ref_fh",
                             "inspector_selection_ref_flip_h_btn");
      btn_fh->add_css_class("prop-toggle");
      btn_fh->set_hexpand(true);
      btn_fh->signal_clicked().connect([this]() {
        if (!m_loading)
          m_sig_request_flip.emit(true);
      });
      flip_row->append(*btn_fh);
      auto *btn_fv = Gtk::make_managed<Gtk::Button>("⇕ Flip V");
      curvz::utils::set_name(btn_fv, "ins_sel_ref_fv",
                             "inspector_selection_ref_flip_v_btn");
      btn_fv->add_css_class("prop-toggle");
      btn_fv->set_hexpand(true);
      btn_fv->signal_clicked().connect([this]() {
        if (!m_loading)
          m_sig_request_flip.emit(false);
      });
      flip_row->append(*btn_fv);
      body->append(*flip_row);
    }

    return;
  }

  // ── Text node — position + text properties ────────────────────────────
  if (obj && obj->is_text()) {
    uint32_t gen = m_build_gen;

    auto *body = add_collapsible("Text", true, parent);
    curvz::utils::set_name(body, "ins_txt", "inspector_text_body");

    auto make_lbl = [](const char *t) {
      auto *l = Gtk::make_managed<Gtk::Label>(t);
      l->add_css_class("prop-lbl");
      l->set_xalign(0.0f);
      return l;
    };
    auto make_spin_adj = [this](Glib::RefPtr<Gtk::Adjustment> adj) {
      auto *s = Gtk::make_managed<Gtk::SpinButton>(adj, 0.01, 2);
      s->set_hexpand(true);
      s->set_width_chars(10);
      s->add_css_class("prop-width-entry");
      s->add_css_class("node-spin");
      block_scroll(s, [this] { emit_canvas_focus(); });
      return s;
    };

    // ── Content entry ──────────────────────────────────────────────────
    auto *content_row =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    content_row->set_spacing(6);
    content_row->set_margin_start(8);
    content_row->set_margin_end(6);
    content_row->set_margin_top(4);
    content_row->set_margin_bottom(2);
    content_row->append(*make_lbl("Text"));
    auto *content_entry = Gtk::make_managed<Gtk::Entry>();
    curvz::utils::set_name(content_entry, "ins_txt_ct",
                           "inspector_text_content_entry");
    content_entry->set_text(obj->text_content);
    content_entry->set_hexpand(true);
    content_entry->add_css_class("prop-width-entry");
    content_row->append(*content_entry);
    body->append(*content_row);

    content_entry->signal_changed().connect([this, obj, content_entry, gen]() {
      if (m_build_gen != gen || m_loading)
        return;
      obj->text_content = content_entry->get_text();
      emit_prop_changed();
    });

    // ── Position X / Y ────────────────────────────────────────────────
    auto *pos_grid = Gtk::make_managed<Gtk::Grid>();
    pos_grid->set_row_spacing(3);
    pos_grid->set_column_spacing(6);
    pos_grid->set_margin_start(8);
    pos_grid->set_margin_end(6);
    pos_grid->set_margin_top(4);
    pos_grid->set_margin_bottom(2);

    auto *hdr_x = Gtk::make_managed<Gtk::Label>("X");
    hdr_x->set_xalign(0.5f);
    hdr_x->set_hexpand(true);
    hdr_x->add_css_class("prop-lbl");
    auto *hdr_y = Gtk::make_managed<Gtk::Label>("Y");
    hdr_y->set_xalign(0.5f);
    hdr_y->set_hexpand(true);
    hdr_y->add_css_class("prop-lbl");
    pos_grid->attach(*make_lbl("POS"), 0, 0);
    pos_grid->attach(*hdr_x, 1, 0);
    pos_grid->attach(*hdr_y, 2, 0);

    auto *sp_tx = Gtk::make_managed<CurvzSpinButton>(SpinType::PositionX,
                                                     m_canvas, m_ruler_ox);
    auto *sp_ty = Gtk::make_managed<CurvzSpinButton>(SpinType::PositionY,
                                                     m_canvas, m_ruler_oy);
    curvz::utils::set_name(sp_tx, "ins_txt_x", "inspector_text_x_spn");
    curvz::utils::set_name(sp_ty, "ins_txt_y", "inspector_text_y_spn");
    sp_tx->with_value(obj->text_x)->with_css("prop-width-entry");
    sp_tx->set_hexpand(true);
    sp_ty->with_value(obj->text_y)->with_css("prop-width-entry");
    sp_ty->set_hexpand(true);
    block_scroll(sp_tx, [this] { emit_canvas_focus(); });
    block_scroll(sp_ty, [this] { emit_canvas_focus(); });
    pos_grid->attach(*sp_tx, 1, 1);
    pos_grid->attach(*sp_ty, 2, 1);
    body->append(*pos_grid);

    sp_tx->on_changed([this, obj, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      obj->text_x = v;
      emit_prop_changed();
    });
    sp_ty->on_changed([this, obj, gen](double v) {
      if (m_build_gen != gen || m_loading)
        return;
      obj->text_y = v;
      emit_prop_changed();
    });

    // ── Font family dropdown (system fonts via Pango) ─────────────────
    {
      auto *font_row =
          Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      font_row->set_spacing(6);
      font_row->set_margin_start(8);
      font_row->set_margin_end(6);
      font_row->set_margin_top(4);
      font_row->set_margin_bottom(2);
      font_row->append(*make_lbl("Font"));

      PangoFontMap *fmap = pango_cairo_font_map_get_default();
      PangoFontFamily **families = nullptr;
      int n_fam = 0;
      pango_font_map_list_families(fmap, &families, &n_fam);
      std::vector<std::string> fnames;
      fnames.reserve(n_fam);
      for (int fi = 0; fi < n_fam; ++fi)
        fnames.push_back(pango_font_family_get_name(families[fi]));
      g_free(families);
      std::sort(fnames.begin(), fnames.end());

      auto slist = Gtk::StringList::create({});
      guint sel_idx = 0;
      for (guint fi = 0; fi < (guint)fnames.size(); ++fi) {
        slist->append(fnames[fi]);
        if (fnames[fi] == obj->text_font_family)
          sel_idx = fi;
      }

      auto *font_drop = Gtk::make_managed<Gtk::DropDown>(slist);
      curvz::utils::set_name(font_drop, "ins_txt_fam",
                             "inspector_text_font_family_dd");
      font_drop->set_enable_search(true);
      font_drop->set_selected(sel_idx);
      font_drop->set_hexpand(true);
      font_drop->add_css_class("prop-dropdown");
      font_row->append(*font_drop);
      body->append(*font_row);

      font_drop->property_selected().signal_changed().connect(
          [this, obj, font_drop, slist, gen]() {
            if (m_build_gen != gen || m_loading)
              return;
            auto sel = font_drop->get_selected();
            if (sel != GTK_INVALID_LIST_POSITION)
              obj->text_font_family = slist->get_string(sel);
            emit_prop_changed();
          });
    }

    // ── Font size ─────────────────────────────────────────────────────
    auto *size_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    size_row->set_spacing(6);
    size_row->set_margin_start(8);
    size_row->set_margin_end(6);
    size_row->set_margin_top(4);
    size_row->set_margin_bottom(2);
    size_row->append(*make_lbl("Size"));
    auto size_adj =
        Gtk::Adjustment::create(obj->text_font_size, 1.0, 2000.0, 1.0, 10.0);
    auto *size_spin = make_spin_adj(size_adj);
    curvz::utils::set_name(size_spin, "ins_txt_sz", "inspector_text_size_spn");
    size_row->append(*size_spin);
    body->append(*size_row);

    size_adj->signal_value_changed().connect(
        [this, obj, size_adj, gen]() mutable {
          if (m_build_gen != gen || m_loading)
            return;
          obj->text_font_size = size_adj->get_value();
          emit_prop_changed();
        });

    // ── Bold / Italic ─────────────────────────────────────────────────
    {
      auto *style_row =
          Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      style_row->set_spacing(12);
      style_row->set_margin_start(8);
      style_row->set_margin_end(6);
      style_row->set_margin_top(4);
      style_row->set_margin_bottom(2);
      auto *bold_btn = Gtk::make_managed<Gtk::CheckButton>("Bold");
      auto *italic_btn = Gtk::make_managed<Gtk::CheckButton>("Italic");
      curvz::utils::set_name(bold_btn, "ins_txt_b",
                             "inspector_text_bold_check");
      curvz::utils::set_name(italic_btn, "ins_txt_i",
                             "inspector_text_italic_check");
      bold_btn->set_active(obj->text_bold);
      bold_btn->set_css_classes({"prop-lbl"});
      italic_btn->set_active(obj->text_italic);
      italic_btn->set_css_classes({"prop-lbl"});
      style_row->append(*bold_btn);
      style_row->append(*italic_btn);
      body->append(*style_row);

      bold_btn->signal_toggled().connect([this, obj, bold_btn, gen]() {
        if (m_build_gen != gen || m_loading)
          return;
        obj->text_bold = bold_btn->get_active();
        emit_prop_changed();
      });
      italic_btn->signal_toggled().connect([this, obj, italic_btn, gen]() {
        if (m_build_gen != gen || m_loading)
          return;
        obj->text_italic = italic_btn->get_active();
        emit_prop_changed();
      });
    }

    // ── Baseline shift ────────────────────────────────────────────────
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->set_spacing(6);
      row->set_margin_start(8);
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);
      row->append(*make_lbl("Baseline"));
      auto *sp =
          Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
      sp->with_value(obj->text_baseline_shift)
          ->with_tooltip(
              "Baseline shift — perpendicular offset from path or baseline")
          ->with_css("prop-width-entry");
      curvz::utils::set_name(sp, "ins_txt_bsl", "inspector_text_baseline_spn");
      sp->set_hexpand(true);
      block_scroll(sp, [this] { emit_canvas_focus(); });
      row->append(*sp);
      if (auto *ul = sp->get_unit_label())
        row->append(*ul);
      body->append(*row);
      uint32_t gen_bs = m_build_gen;
      sp->on_changed([this, obj, gen_bs](double v) {
        if (m_build_gen != gen_bs || m_loading)
          return;
        obj->text_baseline_shift = v;
        emit_prop_changed();
      });
    }

    // ── Letter spacing ────────────────────────────────────────────────
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->set_spacing(6);
      row->set_margin_start(8);
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);
      row->append(*make_lbl("Spacing"));
      auto *sp =
          Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
      sp->with_value(obj->text_letter_spacing)
          ->with_tooltip("Letter spacing — extra advance between glyphs")
          ->with_css("prop-width-entry");
      curvz::utils::set_name(sp, "ins_txt_ls",
                             "inspector_text_letter_spacing_spn");
      sp->set_hexpand(true);
      block_scroll(sp, [this] { emit_canvas_focus(); });
      row->append(*sp);
      if (auto *ul = sp->get_unit_label())
        row->append(*ul);
      body->append(*row);
      uint32_t gen_ls = m_build_gen;
      sp->on_changed([this, obj, gen_ls](double v) {
        if (m_build_gen != gen_ls || m_loading)
          return;
        obj->text_letter_spacing = v;
        emit_prop_changed();
      });
    }

    // ── Alignment: L / C / R / J ──────────────────────────────────────
    // These set BOTH text_anchor (SVG origin point) and text_align
    // (Pango paragraph flow). For icon text they map as a unified concept:
    //   L → anchor=start,  align=left
    //   C → anchor=middle, align=center
    //   R → anchor=end,    align=right
    //   J → anchor=start,  align=justify
    {
      auto *align_row =
          Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      align_row->set_spacing(4);
      align_row->set_margin_start(8);
      align_row->set_margin_end(6);
      align_row->set_margin_top(4);
      align_row->set_margin_bottom(6);
      align_row->append(*make_lbl("Align"));

      auto *btn_l = Gtk::make_managed<Gtk::ToggleButton>("⇤"); // ⇤
      auto *btn_c = Gtk::make_managed<Gtk::ToggleButton>("≡"); // ≡
      auto *btn_r = Gtk::make_managed<Gtk::ToggleButton>("⇥"); // ⇥
      auto *btn_j = Gtk::make_managed<Gtk::ToggleButton>("≣"); // ≣
      curvz::utils::set_name(btn_l, "ins_txt_al",
                             "inspector_text_align_left_btn");
      curvz::utils::set_name(btn_c, "ins_txt_ac",
                             "inspector_text_align_center_btn");
      curvz::utils::set_name(btn_r, "ins_txt_ar",
                             "inspector_text_align_right_btn");
      curvz::utils::set_name(btn_j, "ins_txt_aj",
                             "inspector_text_align_justify_btn");
      btn_l->set_tooltip_text("Left");
      btn_c->set_tooltip_text("Centre");
      btn_r->set_tooltip_text("Right");
      btn_j->set_tooltip_text("Justify");
      btn_c->set_group(*btn_l);
      btn_r->set_group(*btn_l);
      btn_j->set_group(*btn_l);

      // Determine active button from current state
      if (obj->text_align == "justify")
        btn_j->set_active(true);
      else if (obj->text_anchor == "middle")
        btn_c->set_active(true);
      else if (obj->text_anchor == "end")
        btn_r->set_active(true);
      else
        btn_l->set_active(true);

      align_row->append(*btn_l);
      align_row->append(*btn_c);
      align_row->append(*btn_r);
      align_row->append(*btn_j);
      body->append(*align_row);

      btn_l->signal_toggled().connect([this, obj, btn_l, gen]() {
        if (m_build_gen != gen || m_loading || !btn_l->get_active())
          return;
        obj->text_anchor = "start";
        obj->text_align = "left";
        emit_prop_changed();
      });
      btn_c->signal_toggled().connect([this, obj, btn_c, gen]() {
        if (m_build_gen != gen || m_loading || !btn_c->get_active())
          return;
        obj->text_anchor = "middle";
        obj->text_align = "center";
        emit_prop_changed();
      });
      btn_r->signal_toggled().connect([this, obj, btn_r, gen]() {
        if (m_build_gen != gen || m_loading || !btn_r->get_active())
          return;
        obj->text_anchor = "end";
        obj->text_align = "right";
        emit_prop_changed();
      });
      btn_j->signal_toggled().connect([this, obj, btn_j, gen]() {
        if (m_build_gen != gen || m_loading || !btn_j->get_active())
          return;
        obj->text_anchor = "start";
        obj->text_align = "justify";
        emit_prop_changed();
      });
    }

    // ── Text-on-Path section ─────────────────────────────────────────────
    {
      // Path row — shows linked path id or "—", with Detach button
      auto *path_row =
          Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      path_row->set_spacing(6);
      path_row->set_margin_start(8);
      path_row->set_margin_end(6);
      path_row->set_margin_top(4);
      path_row->set_margin_bottom(2);

      auto *path_key = Gtk::make_managed<Gtk::Label>("Path");
      path_key->add_css_class("prop-lbl");
      path_key->set_width_chars(6);
      path_key->set_xalign(0.0f);
      path_row->append(*path_key);

      // s175 m3: render the linked path's iid as "Layer Name → Path Name"
      // rather than the raw UUID. The breadcrumb helper handles empty
      // iid (returns em-dash) and unresolved iid (e.g. linked path was
      // deleted) without us re-checking the empty case here.
      std::string path_label = m_project
          ? curvz::utils::iid_breadcrumb(*m_project, obj->text_path_id)
          : (obj->text_path_id.empty() ? "\xE2\x80\x94" : "(unresolved)");
      auto *path_val = Gtk::make_managed<Gtk::Label>(path_label);
      path_val->set_hexpand(true);
      path_val->set_xalign(0.0f);
      path_val->add_css_class("dim-label");
      path_row->append(*path_val);

      if (!obj->text_path_id.empty()) {
        auto *detach_btn = Gtk::make_managed<Gtk::Button>("Detach");
        curvz::utils::set_name(detach_btn, "ins_txt_dt",
                               "inspector_text_path_detach_btn");
        detach_btn->add_css_class("prop-toggle");
        uint32_t gen = m_build_gen;
        detach_btn->signal_clicked().connect([this, obj, gen]() {
          if (gen != m_build_gen || m_loading)
            return;
          // Delegate to Canvas via signal — it has arc-table machinery to
          // compute the correct detach position and push the undo command.
          m_sig_request_detach_text.emit(obj);
        });
        path_row->append(*detach_btn);
      }
      body->append(*path_row);

      // Offset spinner — only shown when linked
      if (!obj->text_path_id.empty()) {
        auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        row->set_spacing(6);
        row->set_margin_start(8);
        row->set_margin_end(6);
        row->set_margin_top(2);
        row->set_margin_bottom(2);
        row->append(*make_lbl("Offset"));
        auto *sp =
            Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
        sp->with_value(obj->text_path_offset)
            ->with_tooltip("Start offset along path")
            ->with_css("prop-width-entry");
        curvz::utils::set_name(sp, "ins_txt_pof",
                               "inspector_text_path_offset_spn");
        sp->set_hexpand(true);
        block_scroll(sp, [this] { emit_canvas_focus(); });
        row->append(*sp);
        if (auto *ul = sp->get_unit_label())
          row->append(*ul);
        body->append(*row);
        uint32_t gen = m_build_gen;
        sp->on_changed([this, obj, gen](double v) {
          if (gen != m_build_gen || m_loading)
            return;
          obj->text_path_offset = v;
          emit_prop_changed();
        });

        // Flip toggle
        auto *flip_row2 =
            Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        flip_row2->set_spacing(6);
        flip_row2->set_margin_start(8);
        flip_row2->set_margin_end(6);
        flip_row2->set_margin_top(2);
        flip_row2->set_margin_bottom(4);

        auto *flip_key = Gtk::make_managed<Gtk::Label>("Flip");
        flip_key->add_css_class("prop-lbl");
        flip_key->set_width_chars(6);
        flip_key->set_xalign(0.0f);
        flip_row2->append(*flip_key);

        auto *flip_chk = Gtk::make_managed<Gtk::CheckButton>("Below path");
        curvz::utils::set_name(flip_chk, "ins_txt_pfp",
                               "inspector_text_path_flip_check");
        flip_chk->set_active(obj->text_path_flip);
        flip_chk->set_tooltip_text("Flip text to the other side of the path");
        uint32_t gen2 = m_build_gen;
        flip_chk->signal_toggled().connect([this, obj, flip_chk, gen2]() {
          if (gen2 != m_build_gen || m_loading)
            return;
          obj->text_path_flip = flip_chk->get_active();
          emit_prop_changed();
        });
        flip_row2->append(*flip_chk);
        body->append(*flip_row2);
      }
    }

    return;
  } else {
    add_collapsible("Text", false, parent);
  }

  // ── Image node — dedicated section ──────────────────────────────────────
  if (obj && obj->is_image()) {
    auto *body = add_collapsible("Selection", false, parent);
    curvz::utils::set_name(body, "ins_sel_img",
                           "inspector_selection_image_body");

    auto make_lbl = [](const char *t) {
      auto *l = Gtk::make_managed<Gtk::Label>(t);
      l->add_css_class("prop-lbl");
      l->set_xalign(0.0f);
      return l;
    };
    auto make_hdr = [](const char *t) {
      auto *l = Gtk::make_managed<Gtk::Label>(t);
      l->add_css_class("prop-lbl");
      l->set_xalign(0.5f);
      l->set_hexpand(true);
      return l;
    };
    auto make_spin = [this](Glib::RefPtr<Gtk::Adjustment> adj) {
      auto *s = Gtk::make_managed<Gtk::SpinButton>(adj, 0.01, 2);
      s->set_hexpand(true);
      s->set_width_chars(10);
      s->add_css_class("prop-width-entry");
      s->add_css_class("node-spin");
      block_scroll(s, [this] { emit_canvas_focus(); });
      return s;
    };

    auto *grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(3);
    grid->set_column_spacing(4);
    grid->set_margin_start(8);
    grid->set_margin_end(6);
    grid->set_margin_top(4);
    grid->set_margin_bottom(4);

    grid->attach(*make_hdr("X"), 1, 0);
    grid->attach(*make_hdr("Y"), 2, 0);

    // image_y is doc Y-down top; display Y is bottom of image in Y-up space
    auto *sp_ix = Gtk::make_managed<CurvzSpinButton>(SpinType::PositionX,
                                                     m_canvas, m_ruler_ox);
    auto *sp_iy = Gtk::make_managed<CurvzSpinButton>(SpinType::PositionY,
                                                     m_canvas, m_ruler_oy);
    curvz::utils::set_name(sp_ix, "ins_sel_img_x",
                           "inspector_selection_image_x_spn");
    curvz::utils::set_name(sp_iy, "ins_sel_img_y",
                           "inspector_selection_image_y_spn");
    sp_ix->with_value(obj->image_x)->with_css("prop-width-entry");
    sp_ix->set_hexpand(true);
    sp_iy->with_value(obj->image_y + obj->image_h)
        ->with_css("prop-width-entry");
    sp_iy->set_hexpand(true);
    block_scroll(sp_ix, [this] { emit_canvas_focus(); });
    block_scroll(sp_iy, [this] { emit_canvas_focus(); });
    grid->attach(*make_lbl("POS"), 0, 1);
    grid->attach(*sp_ix, 1, 1);
    grid->attach(*sp_iy, 2, 1);

    grid->attach(*make_hdr("WIDTH"), 1, 2);
    grid->attach(*make_hdr("HEIGHT"), 2, 2);

    auto *sp_iw = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    auto *sp_ih = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    curvz::utils::set_name(sp_iw, "ins_sel_img_w",
                           "inspector_selection_image_w_spn");
    curvz::utils::set_name(sp_ih, "ins_sel_img_h",
                           "inspector_selection_image_h_spn");
    sp_iw->with_value(obj->image_w)->with_css("prop-width-entry");
    sp_iw->set_hexpand(true);
    sp_ih->with_value(obj->image_h)->with_css("prop-width-entry");
    sp_ih->set_hexpand(true);
    block_scroll(sp_iw, [this] { emit_canvas_focus(); });
    block_scroll(sp_ih, [this] { emit_canvas_focus(); });
    grid->attach(*make_lbl("SIZE"), 0, 3);
    grid->attach(*sp_iw, 1, 3);
    grid->attach(*sp_ih, 2, 3);

    body->append(*grid);

    sp_ix->on_changed([this, obj](double v) {
      if (m_loading)
        return;
      ScaleImageCommand::Snap snap{obj->internal_id,
                                   obj->image_x,
                                   obj->image_y,
                                   obj->image_w,
                                   obj->image_h,
                                   obj->transform,
                                   v,
                                   obj->image_y,
                                   obj->image_w,
                                   obj->image_h,
                                   obj->transform};
      obj->image_x = v;
      if (m_history)
        m_history->push(std::make_unique<ScaleImageCommand>(
            m_project, std::vector<ScaleImageCommand::Snap>{snap}, "Move image"));
      emit_prop_changed();
    });

    sp_iy->on_changed([this, obj](double v) {
      if (m_loading)
        return;
      // v is doc Y of image top (CurvzSpinButton PositionY stores doc-y
      // directly)
      double new_top_y = v - obj->image_h;
      ScaleImageCommand::Snap snap{obj->internal_id, obj->image_x,  obj->image_y,
                                   obj->image_w, obj->image_h,  obj->transform,
                                   obj->image_x, new_top_y,     obj->image_w,
                                   obj->image_h, obj->transform};
      obj->image_y = new_top_y;
      if (m_history)
        m_history->push(std::make_unique<ScaleImageCommand>(
            m_project, std::vector<ScaleImageCommand::Snap>{snap}, "Move image"));
      emit_prop_changed();
    });

    sp_iw->on_changed([this, obj](double v) {
      if (m_loading || v < 0.001)
        return;
      ScaleImageCommand::Snap snap{obj->internal_id, obj->image_x,  obj->image_y,
                                   obj->image_w, obj->image_h,  obj->transform,
                                   obj->image_x, obj->image_y,  v,
                                   obj->image_h, obj->transform};
      obj->image_w = v;
      if (m_history)
        m_history->push(std::make_unique<ScaleImageCommand>(
            m_project, std::vector<ScaleImageCommand::Snap>{snap}, "Resize image"));
      emit_prop_changed();
    });

    sp_ih->on_changed([this, obj](double v) {
      if (m_loading || v < 0.001)
        return;
      ScaleImageCommand::Snap snap{obj->internal_id,
                                   obj->image_x,
                                   obj->image_y,  obj->image_w,
                                   obj->image_h,  obj->transform,
                                   obj->image_x,  obj->image_y,
                                   obj->image_w,  v,
                                   obj->transform};
      obj->image_h = v;
      if (m_history)
        m_history->push(std::make_unique<ScaleImageCommand>(
            m_project, std::vector<ScaleImageCommand::Snap>{snap}, "Resize image"));
      emit_prop_changed();
    });

    // ── File path (read-only) ──────────────────────────────────────────
    {
      auto *path_row =
          Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      path_row->set_spacing(6);
      path_row->set_margin_start(8);
      path_row->set_margin_end(6);
      path_row->set_margin_top(2);
      path_row->set_margin_bottom(2);
      auto *path_lbl = Gtk::make_managed<Gtk::Label>("File:");
      path_lbl->add_css_class("prop-lbl");
      path_lbl->set_xalign(0.0f);
      path_row->append(*path_lbl);
      // Show filename only (not full path) but tooltip shows full path
      std::string full_path = obj->image_path;
      std::string fname = full_path;
      auto slash = full_path.rfind('/');
      if (slash != std::string::npos)
        fname = full_path.substr(slash + 1);
      auto *path_val = Gtk::make_managed<Gtk::Label>(fname);
      curvz::utils::set_name(path_val, "ins_sel_img_fl",
                             "inspector_selection_image_file_lbl");
      path_val->set_xalign(0.0f);
      path_val->set_hexpand(true);
      path_val->set_ellipsize(Pango::EllipsizeMode::START);
      path_val->add_css_class("dim-label");
      path_val->set_tooltip_text(full_path);
      path_row->append(*path_val);
      body->append(*path_row);
    }

    // ── Flip buttons ───────────────────────────────────────────────────
    auto *flip_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    flip_row->set_spacing(4);
    flip_row->set_margin_start(8);
    flip_row->set_margin_end(6);
    flip_row->set_margin_bottom(6);

    auto *btn_fh = Gtk::make_managed<Gtk::Button>("⇔ Flip H");
    curvz::utils::set_name(btn_fh, "ins_sel_img_fh",
                           "inspector_selection_image_flip_h_btn");
    btn_fh->add_css_class("prop-toggle");
    btn_fh->set_hexpand(true);
    btn_fh->set_tooltip_text("Flip horizontal");
    btn_fh->signal_clicked().connect([this]() {
      if (!m_loading)
        m_sig_request_flip.emit(true);
    });
    flip_row->append(*btn_fh);

    auto *btn_fv = Gtk::make_managed<Gtk::Button>("⇕ Flip V");
    curvz::utils::set_name(btn_fv, "ins_sel_img_fv",
                           "inspector_selection_image_flip_v_btn");
    btn_fv->add_css_class("prop-toggle");
    btn_fv->set_hexpand(true);
    btn_fv->set_tooltip_text("Flip vertical");
    btn_fv->signal_clicked().connect([this]() {
      if (!m_loading)
        m_sig_request_flip.emit(false);
    });
    flip_row->append(*btn_fv);

    body->append(*flip_row);
    return;
  }

  // Accept Path, Group, Compound, ClipGroup, Blend, and Warp — anything
  // with geometry. ClipGroup's clip_shape, Blend's source_a/source_b/
  // cache, and Warp's source/glyph_cache/warp_cache all live in
  // dedicated slots, handled below in collect_leaves.
  bool is_path = obj && obj->path && !obj->path->nodes.empty();
  bool is_group = obj && (obj->type == SceneNode::Type::Group ||
                          obj->type == SceneNode::Type::Compound ||
                          obj->type == SceneNode::Type::ClipGroup ||
                          obj->type == SceneNode::Type::Blend ||
                          obj->type == SceneNode::Type::Warp);
  if (!obj || (!is_path && !is_group)) {
    add_collapsible("Selection", false, parent);
    return;
  }

  // Collect all leaf path nodes (handles nested groups/compounds/clipgroups/
  // blends/warps). ClipGroup: descend into children AND clip_shape. Blend:
  // descend into blend_source_a / blend_source_b / blend_cache. Warp:
  // descend into warp_source / warp_glyph_cache / warp_cache.
  std::function<void(SceneNode *, std::vector<SceneNode *> &)> collect_leaves;
  collect_leaves = [&](SceneNode *n, std::vector<SceneNode *> &out) {
    if (!n)
      return;
    if (n->type == SceneNode::Type::Path && n->path &&
        !n->path->nodes.empty()) {
      out.push_back(n);
      return;
    }
    for (auto &child : n->children)
      collect_leaves(child.get(), out);
    if (n->type == SceneNode::Type::ClipGroup && n->clip_shape)
      collect_leaves(n->clip_shape.get(), out);
    if (n->type == SceneNode::Type::Blend) {
      collect_leaves(n->blend_source_a.get(), out);
      collect_leaves(n->blend_source_b.get(), out);
      for (auto &step : n->blend_cache)
        collect_leaves(step.get(), out);
    }
    if (n->type == SceneNode::Type::Warp) {
      collect_leaves(n->warp_source.get(), out);
      collect_leaves(n->warp_glyph_cache.get(), out);
      collect_leaves(n->warp_cache.get(), out);
    }
  };

  std::vector<SceneNode *> leaves;
  if (is_path)
    leaves.push_back(obj);
  else
    collect_leaves(obj, leaves);

  if (leaves.empty()) {
    add_collapsible("Selection", false, parent);
    return;
  }

  // Compute combined bounding box across all leaves
  double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
  for (SceneNode *leaf : leaves) {
    if (leaf->path)
      expand_bbox_for_path(*leaf->path, minx, maxx, miny, maxy);
  }

  double bw_doc = maxx - minx;
  double bh_doc = maxy - miny;
  double bx_doc = minx;
  double by_doc = miny;

  auto *body = add_collapsible("Selection", false, parent);
  curvz::utils::set_name(body, "ins_sel", "inspector_selection_body");

  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(3);
  grid->set_column_spacing(4);
  grid->set_margin_start(8);
  grid->set_margin_end(6);
  grid->set_margin_top(4);
  grid->set_margin_bottom(4);

  auto *hx = Gtk::make_managed<Gtk::Label>("X");
  hx->add_css_class("prop-lbl");
  hx->set_xalign(0.5f);
  hx->set_hexpand(true);
  auto *hy = Gtk::make_managed<Gtk::Label>("Y");
  hy->add_css_class("prop-lbl");
  hy->set_xalign(0.5f);
  hy->set_hexpand(true);
  grid->attach(*hx, 1, 0);
  grid->attach(*hy, 2, 0);

  auto make_pos_spin = [this](SpinType t, double doc_val, double ruler_orig) {
    auto *s = Gtk::make_managed<CurvzSpinButton>(t, m_canvas, ruler_orig);
    s->with_value(doc_val)->with_css("prop-width-entry")->with_css("node-spin");
    s->with_width_chars(10);
    s->set_hexpand(true);
    block_scroll(s, [this] { emit_canvas_focus(); });
    return s;
  };
  auto make_width_spin = [this](double doc_val) {
    auto *s = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
    s->with_value(doc_val)->with_css("prop-width-entry")->with_css("node-spin");
    s->with_width_chars(10);
    s->set_hexpand(true);
    block_scroll(s, [this] { emit_canvas_focus(); });
    return s;
  };

  auto make_row_lbl = [](const char *text) {
    auto *l = Gtk::make_managed<Gtk::Label>(text);
    l->add_css_class("prop-lbl");
    l->set_xalign(0.0f);
    return l;
  };

  auto *sp_x = make_pos_spin(SpinType::PositionX, bx_doc, m_ruler_ox);
  auto *sp_y = make_pos_spin(SpinType::PositionY, by_doc + bh_doc, m_ruler_oy);
  curvz::utils::set_name(sp_x, "ins_sel_x", "inspector_selection_x_spn");
  curvz::utils::set_name(sp_y, "ins_sel_y", "inspector_selection_y_spn");
  m_sel_sp_x = sp_x;
  m_sel_adj_x = sp_x->get_adjustment();
  m_sel_sp_y = sp_y;
  m_sel_adj_y = sp_y->get_adjustment();
  grid->attach(*make_row_lbl("POS"), 0, 1);
  grid->attach(*sp_x, 1, 1);
  grid->attach(*sp_y, 2, 1);

  // ── Visual-position readout: Xv/Yv under sp_x/sp_y. Read-only, dim,
  //    populated by update_visual_labels(). Hidden when no stroke.
  //    Tooltip uniform across the four visual labels.
  const char *kVisualTooltip = "Visual size — includes stroke";
  auto make_visual_lbl = [kVisualTooltip](const char *prefix) {
    auto *l = Gtk::make_managed<Gtk::Label>(prefix);
    l->add_css_class("dim-label");
    l->set_xalign(0.5f);
    l->set_hexpand(true);
    l->set_tooltip_text(kVisualTooltip);
    return l;
  };
  m_sel_lbl_xv = make_visual_lbl("Xv —");
  m_sel_lbl_yv = make_visual_lbl("Yv —");
  curvz::utils::set_name(m_sel_lbl_xv, "ins_sel_xv",
                         "inspector_selection_xv_lbl");
  curvz::utils::set_name(m_sel_lbl_yv, "ins_sel_yv",
                         "inspector_selection_yv_lbl");
  grid->attach(*m_sel_lbl_xv, 1, 2);
  grid->attach(*m_sel_lbl_yv, 2, 2);

  auto *hw = Gtk::make_managed<Gtk::Label>("WIDTH");
  hw->add_css_class("prop-lbl");
  hw->set_xalign(0.5f);
  hw->set_hexpand(true);
  auto *hh = Gtk::make_managed<Gtk::Label>("HEIGHT");
  hh->add_css_class("prop-lbl");
  hh->set_xalign(0.5f);
  hh->set_hexpand(true);
  grid->attach(*hw, 1, 3);
  grid->attach(*hh, 2, 3);

  auto *sp_w = make_width_spin(bw_doc);
  auto *sp_h = make_width_spin(bh_doc);
  curvz::utils::set_name(sp_w, "ins_sel_w", "inspector_selection_w_spn");
  curvz::utils::set_name(sp_h, "ins_sel_h", "inspector_selection_h_spn");
  m_sel_sp_w = sp_w;
  m_sel_adj_w = sp_w->get_adjustment();
  m_sel_sp_h = sp_h;
  m_sel_adj_h = sp_h->get_adjustment();
  grid->attach(*make_row_lbl("SIZE"), 0, 4);
  grid->attach(*sp_w, 1, 4);
  grid->attach(*sp_h, 2, 4);

  // ── Visual-size readout: Wv/Hv under sp_w/sp_h.
  m_sel_lbl_wv = make_visual_lbl("Wv —");
  m_sel_lbl_hv = make_visual_lbl("Hv —");
  curvz::utils::set_name(m_sel_lbl_wv, "ins_sel_wv",
                         "inspector_selection_wv_lbl");
  curvz::utils::set_name(m_sel_lbl_hv, "ins_sel_hv",
                         "inspector_selection_hv_lbl");
  grid->attach(*m_sel_lbl_wv, 1, 5);
  grid->attach(*m_sel_lbl_hv, 2, 5);

  body->append(*grid);

  // Populate the visual labels with their initial values (also handles
  // the visibility toggle if the selection has no stroke contribution).
  update_visual_labels();

  // ── Helper: snapshot all leaves, apply transform, push ScaleObjectsCommand
  // ── For plain paths we also keep the existing push_inspector_command
  // coalescing.
  auto push_leaves = [this, obj,
                      is_path](const std::vector<SceneNode *> &lvs,
                               const std::vector<PathData> &before_snaps,
                               const std::string &desc) {
    if (is_path && lvs.size() == 1) {
      // Single path — reuse coalescing inspector command
      push_inspector_command(obj);
    } else {
      // Group/compound or multi-leaf — push ScaleObjectsCommand
      if (!m_history)
        return;
      std::vector<ScaleObjectsCommand::LeafSnap> snaps;
      for (size_t i = 0; i < lvs.size(); ++i)
        snaps.push_back({lvs[i]->internal_id, before_snaps[i], *lvs[i]->path});
      m_history->push(
          std::make_unique<ScaleObjectsCommand>(
              m_project, std::move(snaps), desc));
    }
  };

  // X position — v is doc-unit X (CurvzSpinButton converts internally)
  sp_x->on_changed([this, leaves, bx_doc, push_leaves](double v) mutable {
    if (m_loading)
      return;
    std::vector<PathData> before;
    for (auto *l : leaves)
      before.push_back(*l->path);
    double delta = v - bx_doc;
    bx_doc = v;
    for (auto *l : leaves)
      for (auto &n : l->path->nodes) {
        n.x += delta;
        n.cx1 += delta;
        n.cx2 += delta;
      }
    push_leaves(leaves, before, "Move object");
    emit_prop_changed();
  });

  // Y position — v is doc-unit Y (bottom of bbox in Y-up space = miny)
  sp_y->on_changed(
      [this, leaves, by_doc, bh_doc, push_leaves](double v) mutable {
        if (m_loading)
          return;
        std::vector<PathData> before;
        for (auto *l : leaves)
          before.push_back(*l->path);
        // sp_y stores the top-of-bbox in Y-up (maxy in doc = by_doc + bh_doc).
        // The spinner value is that doc coordinate; miny = v - bh_doc.
        double new_miny = v - bh_doc;
        double delta = new_miny - by_doc;
        by_doc = new_miny;
        for (auto *l : leaves)
          for (auto &n : l->path->nodes) {
            n.y += delta;
            n.cy1 += delta;
            n.cy2 += delta;
          }
        push_leaves(leaves, before, "Move object");
        emit_prop_changed();
      });

  // Width scale — v is doc-unit width
  sp_w->on_changed(
      [this, leaves, bx_doc, bw_doc, push_leaves](double v) mutable {
        if (m_loading || bw_doc < 0.001 || v < 0.001)
          return;
        std::vector<PathData> before;
        for (auto *l : leaves)
          before.push_back(*l->path);
        double scale = v / bw_doc;
        bw_doc = v;
        for (auto *l : leaves)
          for (auto &n : l->path->nodes) {
            n.x = bx_doc + (n.x - bx_doc) * scale;
            n.cx1 = bx_doc + (n.cx1 - bx_doc) * scale;
            n.cx2 = bx_doc + (n.cx2 - bx_doc) * scale;
          }
        push_leaves(leaves, before, "Scale object");
        emit_prop_changed();
      });

  // Height scale — v is doc-unit height.
  //
  // Node storage is doc-Y-DOWN (per Canvas::place_rect_precise): smaller
  // y = visual TOP, larger y = visual BOTTOM.  So `miny` (= by_doc) is
  // the visual top and `maxy` (= by_doc + bh_doc) is the visual bottom.
  // To mirror W's bottom-left convention (W keeps minx / visual-LEFT fixed),
  // H must keep `maxy` / visual-BOTTOM fixed — the resize grows/shrinks
  // upward toward the top.
  sp_h->on_changed(
      [this, leaves, by_doc, bh_doc, push_leaves, sp_y](double v) mutable {
        if (m_loading || bh_doc < 0.001 || v < 0.001)
          return;
        std::vector<PathData> before;
        for (auto *l : leaves)
          before.push_back(*l->path);
        double anchor = by_doc + bh_doc; // maxy in doc-Y-down = visual bottom
        double scale = v / bh_doc;
        for (auto *l : leaves)
          for (auto &n : l->path->nodes) {
            n.y = anchor - (anchor - n.y) * scale;
            n.cy1 = anchor - (anchor - n.cy1) * scale;
            n.cy2 = anchor - (anchor - n.cy2) * scale;
          }
        // Update captured baselines for repeat edits in the same lambda:
        // maxy (anchor) unchanged; miny = anchor - v.
        by_doc = anchor - v;
        bh_doc = v;
        // Sync sp_y — its internal stores maxy (= anchor).  Under this anchor,
        // maxy is invariant, so this write is typically a no-op — but call it
        // explicitly so future changes to the anchor convention won't leave
        // sp_y stale.  Guard with m_loading to prevent sp_y's handler re-fire.
        if (sp_y) {
          bool was_loading = m_loading;
          m_loading = true;
          sp_y->set_internal_value(anchor);
          m_loading = was_loading;
        }
        push_leaves(leaves, before, "Scale object");
        emit_prop_changed();
      });

  // ── Transform section: Scale / Rotate / Skew ─────────────────────────────
  // Relative operations applied from bbox centre. The row LABEL is the apply
  // trigger — click it to execute. Fields reset to neutral after apply.

  auto *xf_sep =
      Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  xf_sep->set_margin_top(6);
  xf_sep->set_margin_bottom(4);
  xf_sep->set_margin_start(8);
  xf_sep->set_margin_end(6);
  body->append(*xf_sep);

  double cx_doc = minx + bw_doc * 0.5;
  double cy_doc = miny + bh_doc * 0.5;

  // Helper: apply affine transform around bbox centre, push undo
  auto apply_xf =
      [this, leaves, cx_doc, cy_doc,
       push_leaves](std::function<std::pair<double, double>(double, double)> fn,
                    const std::string &desc) {
        std::vector<PathData> before;
        for (auto *l : leaves)
          before.push_back(*l->path);
        for (auto *l : leaves)
          for (auto &n : l->path->nodes) {
            auto [nx, ny] = fn(n.x - cx_doc, n.y - cy_doc);
            auto [nx1, ny1] = fn(n.cx1 - cx_doc, n.cy1 - cy_doc);
            auto [nx2, ny2] = fn(n.cx2 - cx_doc, n.cy2 - cy_doc);
            n.x = cx_doc + nx;
            n.y = cy_doc + ny;
            n.cx1 = cx_doc + nx1;
            n.cy1 = cy_doc + ny1;
            n.cx2 = cx_doc + nx2;
            n.cy2 = cy_doc + ny2;
          }
        push_leaves(leaves, before, desc);
        emit_prop_changed();
      };

  // Helper: make a transform spinner (prop-width-entry style, 1dp)
  auto make_xf_spin = [this](Glib::RefPtr<Gtk::Adjustment> adj,
                             std::function<void()> on_enter = {}) {
    auto *s = Gtk::make_managed<Gtk::SpinButton>(adj, 0.1, 1);
    s->set_hexpand(false);
    s->set_width_chars(5);
    s->add_css_class("prop-width-entry");
    s->add_css_class("node-spin");
    block_scroll(s, on_enter ? on_enter : [this] { emit_canvas_focus(); });
    return s;
  };

  // Helper: make a clickable label that acts as the Apply button

  // ── Scale ─────────────────────────────────────────────────────────────────
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(2);
    row->set_margin_start(8);
    row->set_margin_end(6);
    row->set_margin_bottom(2);

    auto adj_sx = Gtk::Adjustment::create(100.0, 0.1, 10000.0, 0.1, 10.0);
    auto adj_sy = Gtk::Adjustment::create(100.0, 0.1, 10000.0, 0.1, 10.0);

    // Define apply action up front so it can be passed to make_xf_spin
    auto do_scale = [this, adj_sx, adj_sy, apply_xf]() mutable {
      double sx = adj_sx->get_value() / 100.0;
      double sy = adj_sy->get_value() / 100.0;
      if (sx < 0.001 || sy < 0.001)
        return;
      apply_xf([sx, sy](double dx,
                        double dy) { return std::make_pair(dx * sx, dy * sy); },
               "Scale object");
      m_loading = true;
      adj_sx->set_value(100.0);
      adj_sy->set_value(100.0);
      Glib::signal_idle().connect_once([this] { m_loading = false; });
    };

    auto *spin_sx = make_xf_spin(adj_sx, do_scale);
    auto *spin_sy = make_xf_spin(adj_sy, do_scale);
    curvz::utils::set_name(spin_sx, "ins_sel_sx",
                           "inspector_selection_scale_x_spn");
    curvz::utils::set_name(spin_sy, "ins_sel_sy",
                           "inspector_selection_scale_y_spn");
    spin_sx->set_hexpand(true);
    spin_sy->set_hexpand(true);
    spin_sx->set_width_chars(10);
    spin_sy->set_width_chars(10);

    // Clickable "SCALE" label = Apply
    auto *apply_lbl = Gtk::make_managed<Gtk::Button>();
    curvz::utils::set_name(apply_lbl, "ins_sel_sa",
                           "inspector_selection_scale_apply_btn");
    apply_lbl->add_css_class("flat");
    apply_lbl->add_css_class("prop-xf-apply");
    apply_lbl->set_has_frame(false);
    auto *al = Gtk::make_managed<Gtk::Label>("SCALE");
    al->add_css_class("prop-lbl");
    al->set_xalign(0.0f);
    al->set_width_chars(6);
    apply_lbl->set_child(*al);
    apply_lbl->set_tooltip_text("Click to apply scale");
    apply_lbl->signal_clicked().connect([this, adj_sx, adj_sy,
                                         apply_xf]() mutable {
      double sx = adj_sx->get_value() / 100.0;
      double sy = adj_sy->get_value() / 100.0;
      if (sx < 0.001 || sy < 0.001)
        return;
      apply_xf([sx, sy](double dx,
                        double dy) { return std::make_pair(dx * sx, dy * sy); },
               "Scale object");
      m_loading = true;
      adj_sx->set_value(100.0);
      adj_sy->set_value(100.0);
      Glib::signal_idle().connect_once([this] { m_loading = false; });
    });

    // Link toggle — monochrome, bright = linked, dim = independent
    auto *link_btn = Gtk::make_managed<Gtk::ToggleButton>();
    curvz::utils::set_name(link_btn, "ins_sel_sln",
                           "inspector_selection_scale_link_toggle");
    link_btn->set_active(m_scale_linked);
    link_btn->set_tooltip_text("Link X/Y scale");
    link_btn->add_css_class("flat");
    link_btn->set_has_frame(false);
    link_btn->set_icon_name("curvz-link-symbolic");
    link_btn->set_opacity(m_scale_linked ? 1.0 : 0.3);
    link_btn->signal_toggled().connect([this, link_btn]() {
      m_scale_linked = link_btn->get_active();
      link_btn->set_opacity(m_scale_linked ? 1.0 : 0.3);
    });

    row->append(*apply_lbl);
    row->append(*spin_sx);
    row->append(*link_btn);
    row->append(*spin_sy);

    adj_sx->signal_value_changed().connect([this, adj_sx, adj_sy]() {
      if (m_loading || !m_scale_linked)
        return;
      m_loading = true;
      adj_sy->set_value(adj_sx->get_value());
      m_loading = false;
    });
    adj_sy->signal_value_changed().connect([this, adj_sx, adj_sy]() {
      if (m_loading || !m_scale_linked)
        return;
      m_loading = true;
      adj_sx->set_value(adj_sy->get_value());
      m_loading = false;
    });

    body->append(*row);
  }

  // ── Rotate ────────────────────────────────────────────────────────────────
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(2);
    row->set_margin_start(8);
    row->set_margin_end(6);
    row->set_margin_bottom(2);

    auto adj_r = Gtk::Adjustment::create(0.0, -360.0, 360.0, 0.1, 10.0);

    auto do_rotate = [this, adj_r, apply_xf]() mutable {
      double rad = -adj_r->get_value() * M_PI / 180.0;
      double c = std::cos(rad), s = std::sin(rad);
      apply_xf(
          [c, s](double dx, double dy) {
            return std::make_pair(dx * c - dy * s, dx * s + dy * c);
          },
          "Rotate object");
      m_loading = true;
      adj_r->set_value(0.0);
      Glib::signal_idle().connect_once([this] { m_loading = false; });
    };

    auto *spin_r = Gtk::make_managed<Gtk::SpinButton>(adj_r, 0.000001, 6);
    curvz::utils::set_name(spin_r, "ins_sel_rt",
                           "inspector_selection_rotate_spn");
    spin_r->set_hexpand(true);
    spin_r->set_width_chars(10);
    spin_r->add_css_class("prop-width-entry");
    spin_r->add_css_class("node-spin");
    block_scroll(spin_r, do_rotate);

    auto *apply_lbl = Gtk::make_managed<Gtk::Button>();
    curvz::utils::set_name(apply_lbl, "ins_sel_ra",
                           "inspector_selection_rotate_apply_btn");
    apply_lbl->add_css_class("flat");
    apply_lbl->add_css_class("prop-xf-apply");
    apply_lbl->set_has_frame(false);
    auto *rl = Gtk::make_managed<Gtk::Label>("ROTATE");
    rl->add_css_class("prop-lbl");
    rl->set_xalign(0.0f);
    rl->set_width_chars(6);
    apply_lbl->set_child(*rl);
    apply_lbl->set_tooltip_text("Click to apply rotation");
    apply_lbl->signal_clicked().connect([this, adj_r, apply_xf]() mutable {
      double rad = -adj_r->get_value() * M_PI / 180.0;
      double c = std::cos(rad), s = std::sin(rad);
      apply_xf(
          [c, s](double dx, double dy) {
            return std::make_pair(dx * c - dy * s, dx * s + dy * c);
          },
          "Rotate object");
      m_loading = true;
      adj_r->set_value(0.0);
      Glib::signal_idle().connect_once([this] { m_loading = false; });
    });

    row->append(*apply_lbl);
    row->append(*spin_r);
    auto *r_filler = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    r_filler->set_hexpand(true);
    row->append(*r_filler);
    body->append(*row);
  }

  // ── Skew ──────────────────────────────────────────────────────────────────
  {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(2);
    row->set_margin_start(8);
    row->set_margin_end(6);
    row->set_margin_bottom(4);

    auto adj_kx = Gtk::Adjustment::create(0.0, -89.0, 89.0, 0.1, 5.0);
    auto adj_ky = Gtk::Adjustment::create(0.0, -89.0, 89.0, 0.1, 5.0);

    auto do_skew = [this, adj_kx, adj_ky, apply_xf]() mutable {
      double kx = std::tan(adj_kx->get_value() * M_PI / 180.0);
      double ky = std::tan(adj_ky->get_value() * M_PI / 180.0);
      apply_xf(
          [kx, ky](double dx, double dy) {
            return std::make_pair(dx + kx * dy, dy + ky * dx);
          },
          "Skew object");
      m_loading = true;
      adj_kx->set_value(0.0);
      adj_ky->set_value(0.0);
      Glib::signal_idle().connect_once([this] { m_loading = false; });
    };

    auto *spin_kx = make_xf_spin(adj_kx, do_skew);
    auto *spin_ky = make_xf_spin(adj_ky, do_skew);
    curvz::utils::set_name(spin_kx, "ins_sel_kx",
                           "inspector_selection_skew_x_spn");
    curvz::utils::set_name(spin_ky, "ins_sel_ky",
                           "inspector_selection_skew_y_spn");
    spin_kx->set_hexpand(true);
    spin_ky->set_hexpand(true);
    spin_kx->set_width_chars(10);
    spin_ky->set_width_chars(10);

    auto *apply_lbl = Gtk::make_managed<Gtk::Button>();
    curvz::utils::set_name(apply_lbl, "ins_sel_ka",
                           "inspector_selection_skew_apply_btn");
    apply_lbl->add_css_class("flat");
    apply_lbl->add_css_class("prop-xf-apply");
    apply_lbl->set_has_frame(false);
    auto *sl = Gtk::make_managed<Gtk::Label>("SKEW");
    sl->add_css_class("prop-lbl");
    sl->set_xalign(0.0f);
    sl->set_width_chars(6);
    apply_lbl->set_child(*sl);
    apply_lbl->set_tooltip_text("Click to apply skew");
    apply_lbl->signal_clicked().connect(
        [this, adj_kx, adj_ky, apply_xf]() mutable {
          double kx = std::tan(adj_kx->get_value() * M_PI / 180.0);
          double ky = std::tan(adj_ky->get_value() * M_PI / 180.0);
          apply_xf(
              [kx, ky](double dx, double dy) {
                return std::make_pair(dx + kx * dy, dy + ky * dx);
              },
              "Skew object");
          m_loading = true;
          adj_kx->set_value(0.0);
          adj_ky->set_value(0.0);
          Glib::signal_idle().connect_once([this] { m_loading = false; });
        });

    // ▐X fixed-left indicator, ▄Y fixed-bottom indicator
    auto *x_ind = Gtk::make_managed<Gtk::Label>("▐X");
    x_ind->add_css_class("prop-lbl");
    x_ind->set_tooltip_text("X skew — left edge fixed");
    auto *y_ind = Gtk::make_managed<Gtk::Label>("▄Y");
    y_ind->add_css_class("prop-lbl");
    y_ind->set_tooltip_text("Y skew — bottom edge fixed");
    row->append(*apply_lbl);
    row->append(*x_ind);
    row->append(*spin_kx);
    row->append(*y_ind);
    row->append(*spin_ky);
    body->append(*row);
  }

  // ── Flip buttons (Selection section) ─────────────────────────────────
  {
    auto *flip_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    flip_row->set_spacing(4);
    flip_row->set_margin_start(8);
    flip_row->set_margin_end(6);
    flip_row->set_margin_top(4);
    flip_row->set_margin_bottom(6);
    auto *btn_fh = Gtk::make_managed<Gtk::Button>("⇔ Flip H");
    curvz::utils::set_name(btn_fh, "ins_sel_fh",
                           "inspector_selection_flip_h_btn");
    btn_fh->add_css_class("prop-toggle");
    btn_fh->set_hexpand(true);
    btn_fh->set_tooltip_text("Flip selection horizontal");
    btn_fh->signal_clicked().connect([this]() {
      if (!m_loading)
        m_sig_request_flip.emit(true);
    });
    flip_row->append(*btn_fh);
    auto *btn_fv = Gtk::make_managed<Gtk::Button>("⇕ Flip V");
    curvz::utils::set_name(btn_fv, "ins_sel_fv",
                           "inspector_selection_flip_v_btn");
    btn_fv->add_css_class("prop-toggle");
    btn_fv->set_hexpand(true);
    btn_fv->set_tooltip_text("Flip selection vertical");
    btn_fv->signal_clicked().connect([this]() {
      if (!m_loading)
        m_sig_request_flip.emit(false);
    });
    flip_row->append(*btn_fv);
    body->append(*flip_row);
  }
}

// ── Metadata section
// ────────────────────────────────────────────────────────── Project and
// document metadata — read-only, always collapsed by default.
void PropertiesPanel::build_metadata_section(Gtk::Box *parent) {
  namespace fs = std::filesystem;

  auto *body = add_collapsible("Metadata", false, parent);
  curvz::utils::set_name(body, "ins_meta", "inspector_metadata_body");

  // ── Project directory ─────────────────────────────────────────────────
  if (!m_project->directory.empty()) {
    // Project folder name
    std::string proj_name = fs::path(m_project->directory).filename().string();
    box_add_row(body, "Project", proj_name, 9);

    // Full path — selectable so user can copy it
    auto *path_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    path_row->add_css_class("prop-row");
    auto *path_key = Gtk::make_managed<Gtk::Label>("Path");
    path_key->add_css_class("prop-lbl");
    path_key->set_width_chars(9);
    path_key->set_xalign(0.0f);
    auto *path_val = Gtk::make_managed<Gtk::Label>(m_project->directory);
    path_val->add_css_class("prop-val-lbl");
    path_val->set_xalign(0.0f);
    path_val->set_hexpand(true);
    path_val->set_selectable(true);
    path_val->set_ellipsize(Pango::EllipsizeMode::START);
    path_row->append(*path_key);
    path_row->append(*path_val);
    body->append(*path_row);

    // Document count
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)m_project->documents.size());
    box_add_row(body, "Documents", buf, 9);

    // Total package folder size (walk dir)
    uintmax_t total_bytes = 0;
    int file_count = 0;
    try {
      for (auto &entry :
           fs::recursive_directory_iterator(m_project->directory)) {
        if (entry.is_regular_file()) {
          total_bytes += entry.file_size();
          ++file_count;
        }
      }
    } catch (...) {
    }

    // Format size nicely
    char size_buf[32];
    if (total_bytes < 1024)
      snprintf(size_buf, sizeof(size_buf), "%d B", (int)total_bytes);
    else if (total_bytes < 1024 * 1024)
      snprintf(size_buf, sizeof(size_buf), "%.1f KB", total_bytes / 1024.0);
    else
      snprintf(size_buf, sizeof(size_buf), "%.2f MB",
               total_bytes / (1024.0 * 1024.0));

    box_add_row(body, "Pkg size", size_buf, 9);

    char fc_buf[32];
    snprintf(fc_buf, sizeof(fc_buf), "%d", file_count);
    box_add_row(body, "Files", fc_buf, 9);
  } else {
    box_add_row(body, "Project", "unsaved", 9);
  }

  // ── Active document ───────────────────────────────────────────────────
  auto *doc = m_project->active_doc();
  if (doc) {
    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4);
    sep->set_margin_bottom(4);
    body->append(*sep);

    // Filename — editable entry so user can rename the document
    std::string dname = doc->filename.empty()
                            ? "untitled"
                            : fs::path(doc->filename).stem().string();

    auto *file_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    file_row->add_css_class("prop-row");
    auto *file_key = Gtk::make_managed<Gtk::Label>("File");
    file_key->add_css_class("prop-lbl");
    file_key->set_width_chars(9);
    file_key->set_xalign(0.0f);
    auto *file_entry = Gtk::make_managed<CurvzEntry>();
    curvz::utils::set_name(file_entry, "ins_meta_fn",
                           "inspector_metadata_filename_entry");
    file_entry->set_text(dname);
    file_entry->set_hexpand(true);
    file_entry->add_css_class("prop-entry");
    uint32_t gen = m_build_gen;
    auto commit = [this, file_entry, gen]() {
      if (gen != m_build_gen)
        return;
      std::string new_name = file_entry->get_text();
      if (!new_name.empty())
        m_sig_doc_renamed.emit(new_name);
    };
    wire_entry_activate(file_entry, commit);
    file_entry->signal_icon_press().connect(
        [commit](Gtk::Entry::IconPosition) { commit(); });
    file_row->append(*file_key);
    file_row->append(*file_entry);
    body->append(*file_row);

    // SVG file size on disk
    if (!doc->filename.empty() && !m_project->directory.empty()) {
      fs::path svg_path = fs::path(m_project->directory) / doc->filename;
      char fsz[32] = "—";
      try {
        uintmax_t sz = fs::file_size(svg_path);
        if (sz < 1024)
          snprintf(fsz, sizeof(fsz), "%d B", (int)sz);
        else
          snprintf(fsz, sizeof(fsz), "%.1f KB", sz / 1024.0);
      } catch (...) {
      }
      box_add_row(body, "SVG size", fsz, 9);
    }

    // Canvas dimensions — pixel count (unit display is in the Canvas section)
    char dim_buf[64];
    snprintf(dim_buf, sizeof(dim_buf), "%d × %d px", doc->canvas_width(),
             doc->canvas_height());
    box_add_row(body, "Canvas", dim_buf, 9);

    // Object count
    int obj_count = 0;
    for (const auto &layer : doc->layers)
      obj_count += (int)layer->children.size();
    char oc_buf[32];
    snprintf(oc_buf, sizeof(oc_buf), "%d", obj_count);
    box_add_row(body, "Objects", oc_buf, 9);

    // ── Export metadata ───────────────────────────────────────────────────
    auto *exp_sep =
        Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    exp_sep->set_margin_top(4);
    exp_sep->set_margin_bottom(4);
    body->append(*exp_sep);

    // Export name entry
    auto *name_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    name_row->add_css_class("prop-row");
    auto *name_key = Gtk::make_managed<Gtk::Label>("Icon name");
    name_key->add_css_class("prop-lbl");
    name_key->set_width_chars(9);
    name_key->set_xalign(0.0f);
    auto *name_entry = Gtk::make_managed<CurvzEntry>();
    curvz::utils::set_name(name_entry, "ins_meta_xn",
                           "inspector_metadata_export_name_entry");
    name_entry->set_text(doc->export_name);
    name_entry->set_placeholder_text("edit-copy-symbolic");
    name_entry->set_hexpand(true);
    name_entry->add_css_class("prop-entry");
    uint32_t exp_gen = m_build_gen;
    auto commit_name = [this, doc, name_entry, exp_gen]() {
      if (exp_gen != m_build_gen)
        return;
      doc->export_name = name_entry->get_text();
      emit_prop_changed();
    };
    wire_entry_activate(name_entry, commit_name);
    name_entry->signal_icon_press().connect(
        [commit_name](Gtk::Entry::IconPosition) { commit_name(); });
    name_row->append(*name_key);
    name_row->append(*name_entry);
    body->append(*name_row);

    // Export category dropdown
    auto *cat_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    cat_row->add_css_class("prop-row");
    auto *cat_key = Gtk::make_managed<Gtk::Label>("Category");
    cat_key->add_css_class("prop-lbl");
    cat_key->set_width_chars(9);
    cat_key->set_xalign(0.0f);

    static const std::vector<std::string> k_categories = {
        "",        "actions", "apps",   "mimetypes", "devices",
        "emblems", "places",  "status", "categories"};
    auto cat_list = Gtk::StringList::create({});
    guint cat_sel = 0;
    for (guint ci = 0; ci < (guint)k_categories.size(); ++ci) {
      cat_list->append(k_categories[ci].empty() ? "(none)" : k_categories[ci]);
      if (k_categories[ci] == doc->export_category)
        cat_sel = ci;
    }
    auto *cat_drop = Gtk::make_managed<Gtk::DropDown>(cat_list);
    curvz::utils::set_name(cat_drop, "ins_meta_xc",
                           "inspector_metadata_export_category_dd");
    cat_drop->set_selected(cat_sel);
    cat_drop->set_hexpand(true);
    cat_drop->add_css_class("prop-dropdown");
    cat_drop->property_selected().signal_changed().connect(
        [this, doc, cat_drop, exp_gen]() {
          if (exp_gen != m_build_gen || m_loading)
            return;
          auto sel = cat_drop->get_selected();
          if (sel != GTK_INVALID_LIST_POSITION && sel < k_categories.size())
            doc->export_category = k_categories[sel];
          emit_prop_changed();
        });
    cat_row->append(*cat_key);
    cat_row->append(*cat_drop);
    body->append(*cat_row);
  }
}

// ── Measure section: deleted s150 ────────────────────────────────────────
// Measure behaviour now lives at the toolbar Measure button + its right-
// click popover (see Toolbar::build_measure_popover and the
// m_toolbar.signal_measure_settings_changed handler in MainWindow.cpp).
// Storage on doc.measure_save_to_layer / doc.measure_destruct_after_copy
// is unchanged; only the inspector surface went away.
//
// Per ARC.md design rule 1 ("behaviour at the tool, style in the
// inspector"), Measure is behaviour — how the measure tool acts during
// use — not style, so it doesn't belong in an inspector section. The
// Measure tool is its natural home.

// ── Public unified entry point
// ────────────────────────────────────────────────
void PropertiesPanel::refresh(CanvasModel *canvas, SceneNode *obj) {
  m_canvas = canvas;
  m_current = obj;
  m_node_idx = -1;
  m_loading = true;
  // Clear selection pointers — rebuilt by build_selection_section
  m_sel_sp_x = nullptr;
  m_sel_adj_x.reset();
  m_sel_sp_y = nullptr;
  m_sel_adj_y.reset();
  m_sel_sp_w = nullptr;
  m_sel_adj_w.reset();
  m_sel_sp_h = nullptr;
  m_sel_adj_h.reset();
  // Visual-size readout labels — managed by build_selection_section.
  // Cleared here so update_visual_labels (which guards on m_sel_lbl_xv)
  // bails cleanly between the pointer reset and the next build.
  m_sel_lbl_xv = nullptr;
  m_sel_lbl_yv = nullptr;
  m_sel_lbl_wv = nullptr;
  m_sel_lbl_hv = nullptr;
  m_ref_sp_x = nullptr;
  m_ref_sp_y = nullptr;
  do_clear();

  // Snapshot "before" state for the newly selected object so the first
  // inspector edit has a valid baseline even before any run transition.
  if (obj) {
    if (obj->path)
      m_undo_before = *obj->path;
    m_undo_fill_before = obj->fill;
    m_undo_stroke_before = obj->stroke;
    // S82 m4f: capture pre-edit swatch bindings alongside fill/stroke
    // so the first inspector edit on a swatch-bound selection has the
    // right baseline for undo to restore the binding.
    m_undo_fill_swatch_id_before = obj->fill_swatch_id;
    m_undo_stroke_swatch_id_before = obj->stroke_swatch_id;
    // S92 m3: same baseline capture for bound_style.
    m_undo_bound_style_before = obj->bound_style;
    // S97 m3: same baseline capture for shadow.
    m_undo_shadow_before = obj->read_shadow();
  }

  if (!canvas) {
    // No document — show placeholder then collapsed sections
    auto *lbl = Gtk::make_managed<Gtk::Label>("No document");
    lbl->set_halign(Gtk::Align::CENTER);
    lbl->set_margin_top(16);
    lbl->set_margin_bottom(8);
    lbl->add_css_class("dim-label");
    m_inner.append(*lbl);
    auto *obj_grp = add_group_collapsible("Object", false);
    build_selection_section(nullptr, obj_grp);
    build_node_section(nullptr, -1, obj_grp);
    add_fill_stroke_section(nullptr, obj_grp);
    m_loading = false;
    return;
  }

  // ── Application group (s143 m1; reordered s145 m2) ──────────────
  // Sits at the top of the inspector by scope-hierarchy convention:
  // Application → Project → Document → Object, broadest to narrowest.
  // User-tier app preferences (AppPreferences::instance) — persists
  // across projects and launches. Default-collapsed because it's
  // read-mostly (boot-time prefs, occasional tweaks) and the user's
  // attention typically lives in Document/Object during editing.
  {
    auto *app_grp = add_group_collapsible("Application", false);
    build_app_section(app_grp);
  }

  // ── Project group (s116 m6 → s148 m2: deleted) ───────────────
  // The Project group lost its only resident (Motif) when the s148
  // motif → document migration relocated artboard/workspace/creation
  // to per-doc scope and pulled Theme into Application ▸ Appearance.
  // The empty group was deleted in m2 rather than left as a placeholder
  // — collapsibles that open to nothing are bad UX. Future per-project
  // settings (anything genuinely project-tier, not app-tier or doc-tier)
  // can re-add the group when there's something to put in it.

  // ── Document group (s148 m2 fix2: Motif disclosure groups setup phase) ──
  // Top-level structure:
  //
  //   Metadata    — paperwork: what is this drawing called?
  //   Motif ▸     — disclosure wrapping the facets of doc setup:
  //     Canvas    — surface colours (artboard / workspace / creation)
  //     Margins   — page boundaries (the frame within)
  //     Grid      — regular mesh, if needed (background scaffold)
  //   Measure     — distance-checking aid during work
  //   Snap        — cursor-stickiness behaviour
  //   Dimensions  — the doc's underlying geometric bones (ratio,
  //                 quality, DPI, ruler origin). Set once at doc
  //                 creation, rarely revisited. Sits LAST so it abuts
  //                 the Object group below — the doc's frame, sitting
  //                 at the boundary between doc-level and object-level
  //                 editing where the user spends most of their time.
  //
  // Two ordering principles in play:
  //   • Frequency-of-use — Dimensions at the bottom because it's
  //     set-and-forget; setup-phase items grouped under Motif so they
  //     can be collapsed once setup is done.
  //   • Workflow chronology inside Motif — Canvas → Margins → Grid
  //     mirrors how a draftsman lays out a drawing: surface first,
  //     then page edges, then mesh.
  //
  // s179 m3: Guides used to live inside the Motif disclosure as the
  // fourth entry. Moved to Object ▸ Guides — guides are objects on
  // the canvas with a selection model, not a doc-level surface
  // setting. ThemeLibrary still round-trips guide colour as part of
  // the theme bundle.
  //
  // Resist re-sorting alphabetically or by "structural" hierarchy —
  // the current order encodes domain meaning.
  {
    auto *doc_grp = add_group_collapsible("Document", false);
    if (m_project) {
      build_metadata_section(doc_grp);
      if (auto *doc = m_project->active_doc()) {
        build_theme_disclosure(doc, doc_grp);
        // s150: build_measure_section + build_snap_section calls removed.
        // Both are now toolbar right-click popovers (Ruler, Snap).
      }
    }
    auto cm = std::make_shared<CanvasModel>(*canvas);
    build_canvas_section(cm, doc_grp);
  }

  // ── Object group ─────────────────────────────────────────────
  {
    auto *obj_grp = add_group_collapsible("Object", false);
    // s179 m3v2: Guides section sits FIRST in the Object group, above
    // build_selection_section's output. Build_selection_section emits
    // an empty "Text" placeholder disclosure for non-Text selections
    // (so the Text section is structurally always present even when
    // its body is empty); placing Guides above the selection_section
    // call puts Guides above that Text placeholder, which matches the
    // user's mental model — Guides as a first-class object surface
    // distinct from the per-selection branches below it. The doc may
    // be null if no project is open; build_object_guides_section
    // guards on that.
    build_object_guides_section(
        m_project ? m_project->active_doc() : nullptr, obj_grp);
    build_selection_section(obj, obj_grp);
    if (obj && obj->is_blend())
      build_blend_section(obj, obj_grp);
    // s155: Warp section is always visible. When a Warp is selected it
    // edits that instance; otherwise it edits AppPreferences (the
    // template the next new Warp will inherit). build_warp_section
    // handles both binding modes via the obj sentinel.
    build_warp_section(obj, obj_grp);
    if (obj && m_node_idx >= 0)
      build_node_section(obj, m_node_idx, obj_grp);
    else
      build_node_section(nullptr, -1, obj_grp);
    add_fill_stroke_section(obj, obj_grp);
    // S97 m3 — drop shadow section, gated by SceneNode::can_have_shadow.
    // Sits below Appearance per Affinity/Illustrator convention (effects
    // are presentational layers stacked over fill/stroke).
    if (obj && obj->can_have_shadow())
      build_shadow_section(obj, obj_grp);
  }
  m_loading = false;
  LOG_DEBUG("PropertiesPanel::refresh DONE");
}

// ── Node section — anchor + handles + type
// ────────────────────────────────────
void PropertiesPanel::build_node_section(SceneNode *obj, int node_idx,
                                         Gtk::Box *parent) {
  if (!obj || !obj->path || node_idx < 0 ||
      node_idx >= (int)obj->path->nodes.size()) {
    auto *body = add_collapsible("Node", false, parent);
    curvz::utils::set_name(body, "ins_nod_empty", "inspector_node_empty_body");
    // auto *ph = Gtk::make_managed<Gtk::Label>("No node selected");
    // ph->set_halign(Gtk::Align::START);
    // ph->set_margin_start(10);
    // ph->set_margin_top(4);
    // ph->set_margin_bottom(4);
    // ph->add_css_class("dim-label");x
    // body->append(*ph);
    return;
  }

  const auto &nodes = obj->path->nodes;
  const BezierNode &nd = nodes[node_idx];
  LOG_DEBUG("build_node_section: node={} type={} cx1=({:.2f},{:.2f}) "
            "cx2=({:.2f},{:.2f})",
            node_idx, (int)nd.type, nd.cx1, nd.cy1, nd.cx2, nd.cy2);

  // Coordinate conversion helpers — match ruler display exactly.
  // Applies Y-flip, ruler origin, and display-mode scaling.
  const CanvasModel *cm = m_canvas;
  double ox = m_ruler_ox;
  double oy = m_ruler_oy;

  // doc → display (what the spinners show)
  auto to_ux = [cm, ox](double dx) {
    return node_doc_to_display_x(dx, cm, ox);
  };
  auto to_uy = [cm, oy](double dy) {
    return node_doc_to_display_y(dy, cm, oy);
  };
  // display → doc (what write-backs store)
  auto to_dx = [cm, ox](double ux) {
    return node_display_to_doc_x(ux, cm, ox);
  };
  auto to_dy = [cm, oy](double uy) {
    return node_display_to_doc_y(uy, cm, oy);
  };

  char hdr[48];
  snprintf(hdr, sizeof(hdr), "Node  %d / %d", node_idx + 1, (int)nodes.size());
  // Use fixed key "Node" so persistence survives node switching;
  // the displayed title still shows the current index.
  auto it = m_section_open.find("Node");
  bool node_open = (it != m_section_open.end()) ? it->second : false;
  m_section_open["Node"] = node_open;

  auto *outer_n = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer_n->add_css_class("inspector-section");
  auto *hdr_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr_row->add_css_class("inspector-section-header");
  hdr_row->set_spacing(5);
  auto *arrow_n = Gtk::make_managed<Gtk::Label>(node_open ? "▾" : "▸");
  arrow_n->add_css_class("inspector-arrow");
  auto *title_n = Gtk::make_managed<Gtk::Label>(hdr);
  title_n->set_xalign(0.0f);
  title_n->set_hexpand(true);
  title_n->add_css_class("inspector-section-title");
  hdr_row->append(*arrow_n);
  hdr_row->append(*title_n);
  outer_n->append(*hdr_row);
  auto *sep_n = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer_n->append(*sep_n);
  auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  curvz::utils::set_name(body, "ins_nod", "inspector_node_body");
  body->set_visible(node_open);
  outer_n->append(*body);
  auto gesture_n = Gtk::GestureClick::create();
  gesture_n->signal_pressed().connect(
      [this, body, arrow_n](int, double, double) {
        bool on = !m_section_open["Node"];
        m_section_open["Node"] = on;
        body->set_visible(on);
        arrow_n->set_text(on ? "▾" : "▸");
      });
  hdr_row->add_controller(gesture_n);
  // Honour the `parent` arg so the Node section lives inside the Object
  // group (matches the early-return path above that uses add_collapsible
  // with `parent`).  Fallback to m_inner preserves legacy callers that
  // pass nullptr.
  Gtk::Box *dest_n = parent ? parent : &m_inner;
  dest_n->append(*outer_n);

  // ── Coordinate grid ───────────────────────────────────────────────────
  //
  //              X            Y
  //  In hdl   [cx1 spin]   [cy1 spin]    click label: retract handle
  //  Node     [ x  spin]   [ y  spin]
  //  Out hdl  [cx2 spin]   [cy2 spin]    click label: retract handle
  //
  // Handle rows are always shown regardless of node type — Corner nodes
  // have unconstrained handles that still influence curve shape. Only
  // retraction (clicking the IN/OUT row label) zeros them to produce a
  // truly straight segment.
  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(3);
  grid->set_column_spacing(4);
  grid->set_margin_start(8);
  grid->set_margin_end(6);
  grid->set_margin_top(4);
  grid->set_margin_bottom(4);

  // Helper: make a compact position spin for the node grid
  auto make_spin = [this](SpinType t, double doc_val, double ro) {
    auto *s = Gtk::make_managed<CurvzSpinButton>(t, m_canvas, ro);
    s->with_value(doc_val)->with_css("prop-width-entry")->with_css("node-spin");
    s->with_width_chars(10);
    s->set_hexpand(true);
    block_scroll(s, [this] { emit_canvas_focus(); });
    return s;
  };

  // ── Column headers ────────────────────────────────────────────────────
  auto *hx = Gtk::make_managed<Gtk::Label>("X");
  hx->add_css_class("prop-lbl");
  hx->set_xalign(0.5f);
  hx->set_hexpand(true);
  auto *hy = Gtk::make_managed<Gtk::Label>("Y");
  hy->add_css_class("prop-lbl");
  hy->set_xalign(0.5f);
  hy->set_hexpand(true);
  // col 0 = row-label, col 1 = X spin, col 2 = Y spin
  grid->attach(*hx, 1, 0);
  grid->attach(*hy, 2, 0);

  int grid_row = 1;

  // Capture generation now — used by all lambdas in this build to detect stale
  // signals.
  uint32_t gen = m_build_gen;

  // Helper: make a handle row-label that clicks to retract the handle to
  // node position. s131 m20: route through the spinner adjustments + the
  // canvas pipeline (so the canvas remains the sole owner of obj->path),
  // then call push_inspector_command to actually push the EditObjectCommand
  // onto the history.
  //
  // What didn't work, for the next person who reads this:
  //
  //   - m16 wrote obj->path->nodes[i].cx1 = n.x directly from the
  //     inspector. Visually correct, but silent to undo because nothing
  //     pushed a command. Also violates the inspector contract — the
  //     canvas owns obj->path writes.
  //
  //   - m19 routed the change through emit_request_node_edit, which
  //     fires signal_request_node_edit, which MainWindow handles by
  //     calling Canvas::apply_node_edit. That fixed the contract
  //     violation but was *still* silent to undo, because
  //     apply_node_edit (Canvas.cpp) only writes the node fields and
  //     queue_draw — it does NOT push anything onto m_history. Compare
  //     with set_selected_nodes_type a few lines below it, which DOES
  //     snapshot before, mutate, and push EditPathCommand. So the
  //     canvas-side wiring for inspector node coordinate edits has
  //     been undo-blind by design (latent for spinner edits too — they
  //     don't undo either, separate bug to chase).
  //
  // m20 fix: run the canvas pipeline as before so the path is mutated
  // through the right owner, then call push_inspector_command(obj).
  // That helper reads obj->path post-edit, diffs against m_undo_before
  // (the pre-edit snapshot taken at panel refresh), and pushes an
  // EditObjectCommand. The 1500ms time-window coalesce inside
  // push_inspector_command means a quick double-click stays one undo
  // step.
  auto make_hdl_label = [&](const char *text, const char *tip,
                            bool is_in, // true=In, false=Out
                            int row) {
    auto *lbl = Gtk::make_managed<Gtk::Label>(text);
    lbl->add_css_class("prop-lbl");
    lbl->add_css_class("hdl-reset-lbl");
    lbl->set_xalign(0.0f);
    lbl->set_tooltip_text(tip);

    auto gc = Gtk::GestureClick::create();
    gc->set_button(1); // left button
    gc->signal_pressed().connect(
        [this, gen, obj, node_idx, is_in](int, double, double) {
          if (m_build_gen != gen)
            return;
          if (!m_adj_ax || !m_adj_ay)
            return;
          auto &n = obj->path->nodes[node_idx];

          // Read the anchor in display space — that's the unit the
          // adjustments live in. node_doc_to_display_* converts from doc-
          // space (n.x, n.y) to ruler-relative display space; the same
          // helper the spinner-edit handlers above use.
          const double anchor_disp_x =
              node_doc_to_display_x(n.x, m_canvas, m_ruler_ox);
          const double anchor_disp_y =
              node_doc_to_display_y(n.y, m_canvas, m_ruler_oy);

          // Symmetric: clicking either label retracts both handles at once.
          // emit_request_node_edit reads ALL six adjustments and emits one
          // signal — the canvas turns that into a single mutation, so a
          // Symmetric retract is one Ctrl+Z step regardless of how many
          // handles moved.
          const bool both = (n.type == BezierNode::Type::Symmetric);

          // m_loading suppresses the spinner-edit handlers from re-firing
          // emit_request_node_edit while we set up the new values. We fire
          // it ourselves once at the end.
          m_loading = true;
          if ((is_in || both) && m_adj_ix && m_adj_iy) {
            m_adj_ix->set_value(anchor_disp_x);
            m_adj_iy->set_value(anchor_disp_y);
          }
          if ((!is_in || both) && m_adj_ox && m_adj_oy) {
            m_adj_ox->set_value(anchor_disp_x);
            m_adj_oy->set_value(anchor_disp_y);
          }
          m_loading = false;

          // 1) Drive the canvas pipeline — synchronous; by the time this
          //    returns, obj->path has been mutated by Canvas::apply_node_edit.
          emit_request_node_edit();

          // 2) Push the command onto the history. push_inspector_command
          //    reads obj->path post-edit, diffs against m_undo_before
          //    (set at panel refresh), and pushes EditObjectCommand. This
          //    is what makes Ctrl+Z work.
          push_inspector_command(obj);

          // 3) Visual labels and downstream signals.
          emit_prop_changed();
        });
    lbl->add_controller(gc);
    grid->attach(*lbl, 0, row);
  };

  // ── Handle In row (cx1/cy1) ───────────────────────────────────────────
  auto *sp_ix = make_spin(SpinType::PositionX, nd.cx1, m_ruler_ox);
  auto *sp_iy = make_spin(SpinType::PositionY, nd.cy1, m_ruler_oy);
  curvz::utils::set_name(sp_ix, "ins_nod_ix", "inspector_node_handle_in_x_spn");
  curvz::utils::set_name(sp_iy, "ins_nod_iy", "inspector_node_handle_in_y_spn");
  m_adj_ix = sp_ix->get_adjustment();
  m_adj_iy = sp_iy->get_adjustment();
  make_hdl_label("IN",
                 "Handle In (cx1, cy1)\nClick to retract to "
                 "anchor\n(Symmetric: retracts both)",
                 true, grid_row);
  grid->attach(*sp_ix, 1, grid_row);
  grid->attach(*sp_iy, 2, grid_row);
  ++grid_row;

  // ── Node anchor row (x/y) ─────────────────────────────────────────────
  auto *sp_ax = make_spin(SpinType::PositionX, nd.x, m_ruler_ox);
  auto *sp_ay = make_spin(SpinType::PositionY, nd.y, m_ruler_oy);
  curvz::utils::set_name(sp_ax, "ins_nod_ax", "inspector_node_anchor_x_spn");
  curvz::utils::set_name(sp_ay, "ins_nod_ay", "inspector_node_anchor_y_spn");
  m_adj_ax = sp_ax->get_adjustment();
  m_adj_ay = sp_ay->get_adjustment();
  {
    auto *rl = Gtk::make_managed<Gtk::Label>("NODE");
    rl->add_css_class("prop-lbl");
    rl->set_xalign(0.0f);
    rl->set_tooltip_text("Anchor point (x, y)");
    grid->attach(*rl, 0, grid_row);
    grid->attach(*sp_ax, 1, grid_row);
    grid->attach(*sp_ay, 2, grid_row);
    ++grid_row;
  }

  // ── Handle Out row (cx2/cy2) ──────────────────────────────────────────
  auto *sp_ox = make_spin(SpinType::PositionX, nd.cx2, m_ruler_ox);
  auto *sp_oy = make_spin(SpinType::PositionY, nd.cy2, m_ruler_oy);
  curvz::utils::set_name(sp_ox, "ins_nod_ox",
                         "inspector_node_handle_out_x_spn");
  curvz::utils::set_name(sp_oy, "ins_nod_oy",
                         "inspector_node_handle_out_y_spn");
  m_adj_ox = sp_ox->get_adjustment();
  m_adj_oy = sp_oy->get_adjustment();
  make_hdl_label("OUT",
                 "Handle Out (cx2, cy2)\nClick to retract to "
                 "anchor\n(Symmetric: retracts both)",
                 false, grid_row);
  grid->attach(*sp_ox, 1, grid_row);
  grid->attach(*sp_oy, 2, grid_row);

  body->append(*grid);

  // ── Wire signals ──────────────────────────────────────────────────────
  // Spinners never write to obj->path directly. Each change emits
  // signal_request_node_edit with all 6 current adjustment values.
  // The canvas receives it, writes through its own pipeline, and redraws.

  // Anchor spinners carry handles by delta in doc space before emitting.
  sp_ax->on_changed([this, gen, obj, node_idx](double v) {
    if (m_loading || m_build_gen != gen)
      return;
    // Compute delta in display space to carry handles consistently
    double old_disp = node_doc_to_display_x(obj->path->nodes[node_idx].x,
                                            m_canvas, m_ruler_ox);
    double new_disp = m_adj_ax->get_value();
    double disp_delta = new_disp - old_disp;
    m_loading = true;
    m_adj_ix->set_value(m_adj_ix->get_value() + disp_delta);
    m_adj_ox->set_value(m_adj_ox->get_value() + disp_delta);
    m_loading = false;
    emit_request_node_edit();
    emit_prop_changed();
  });
  sp_ay->on_changed([this, gen, obj, node_idx](double v) {
    if (m_loading || m_build_gen != gen)
      return;
    auto *cm = m_canvas;
    double oy = m_ruler_oy;
    double old_disp =
        node_doc_to_display_y(obj->path->nodes[node_idx].y, cm, oy);
    double new_disp = m_adj_ay->get_value();
    double disp_delta = new_disp - old_disp;
    m_loading = true;
    m_adj_iy->set_value(m_adj_iy->get_value() + disp_delta);
    m_adj_oy->set_value(m_adj_oy->get_value() + disp_delta);
    m_loading = false;
    emit_request_node_edit();
    emit_prop_changed();
  });

  // Handle spinners — emit directly, no carry needed
  auto on_handle_changed = [this, gen]() {
    if (m_loading || m_build_gen != gen)
      return;
    emit_request_node_edit();
    emit_prop_changed();
  };
  m_adj_ix->signal_value_changed().connect(on_handle_changed);
  m_adj_iy->signal_value_changed().connect(on_handle_changed);
  m_adj_ox->signal_value_changed().connect(on_handle_changed);
  m_adj_oy->signal_value_changed().connect(on_handle_changed);

  // ── Node type dropdown ────────────────────────────────────────────────
  {
    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(2);
    sep->set_margin_bottom(2);
    body->append(*sep);

    auto *t_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    t_row->add_css_class("prop-row");
    auto *t_lbl = Gtk::make_managed<Gtk::Label>("Type");
    t_lbl->add_css_class("prop-lbl");
    t_lbl->set_width_chars(5);
    t_lbl->set_xalign(0.0f);
    t_row->append(*t_lbl);

    auto items = Gtk::StringList::create(
        {"Symmetric \xe2\x97\x86", "Smooth \xe2\x97\x87", "Cusp \xe2\x96\xa1",
         "Corner \xe2\x97\x8b"});
    auto *dd = Gtk::make_managed<Gtk::DropDown>(items);
    curvz::utils::set_name(dd, "ins_nod_tp", "inspector_node_type_dd");
    dd->set_hexpand(true);
    dd->add_css_class("prop-dropdown");

    guint sel_idx = 0;
    switch (nd.type) {
    case BezierNode::Type::Symmetric:
      sel_idx = 0;
      break;
    case BezierNode::Type::Smooth:
      sel_idx = 1;
      break;
    case BezierNode::Type::Cusp:
      sel_idx = 2;
      break;
    case BezierNode::Type::Corner:
      sel_idx = 3;
      break;
    }
    dd->set_selected(sel_idx);

    dd->property_selected().signal_changed().connect(
        [this, gen, obj, node_idx, dd]() {
          if (m_loading || m_build_gen != gen)
            return;
          BezierNode::Type new_type;
          switch (dd->get_selected()) {
          case 0:
            new_type = BezierNode::Type::Symmetric;
            break;
          case 1:
            new_type = BezierNode::Type::Smooth;
            break;
          case 2:
            new_type = BezierNode::Type::Cusp;
            break;
          default:
            new_type = BezierNode::Type::Corner;
            break;
          }
          // Guard: if the type already matches, this is a spurious signal
          // (e.g. from GTK delivering a queued notification after a rebuild).
          // Never call set_node_type unless the user actually changed the
          // value.
          if (!obj->path || node_idx >= (int)obj->path->nodes.size())
            return;
          if (obj->path->nodes[node_idx].type == new_type) {
            LOG_DEBUG("PropertiesPanel: dropdown signal spurious (type already "
                      "{}), ignoring",
                      (int)new_type);
            return;
          }
          LOG_DEBUG("PropertiesPanel: dropdown REAL change node {} type → {}",
                    node_idx, (int)new_type);
          // Emit signal so Canvas can apply to all selected nodes atomically
          m_sig_request_node_type_change.emit(new_type);
          m_node_type = new_type;
        });

    t_row->append(*dd);
    body->append(*t_row);
  }

  // ── Path splitter buttons ─────────────────────────────────────────────
  // Shown only when the operation is valid for this node + path state.
  bool is_closed = obj->path->closed;
  int n_nodes = (int)obj->path->nodes.size();
  bool can_open = is_closed && n_nodes >= 2;
  bool can_split =
      !is_closed && n_nodes >= 3 && node_idx > 0 && node_idx < n_nodes - 1;

  if (can_open || can_split) {
    auto *split_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    split_row->add_css_class("prop-row");
    split_row->set_margin_top(4);

    if (can_open) {
      auto *open_btn = Gtk::make_managed<Gtk::Button>("Open here");
      curvz::utils::set_name(open_btn, "ins_nod_op",
                             "inspector_node_open_here_btn");
      open_btn->add_css_class("prop-toggle");
      open_btn->set_tooltip_text(
          "Open closed path at this node — path starts and ends here");
      open_btn->signal_clicked().connect([this]() {
        if (m_loading)
          return;
        m_sig_request_open_at_node.emit();
      });
      split_row->append(*open_btn);
    }

    if (can_split) {
      auto *split_btn = Gtk::make_managed<Gtk::Button>("Split here");
      curvz::utils::set_name(split_btn, "ins_nod_sp",
                             "inspector_node_split_here_btn");
      split_btn->add_css_class("prop-toggle");
      split_btn->set_tooltip_text(
          "Split open path into two objects at this node");
      split_btn->set_margin_start(can_open ? 4 : 0);
      split_btn->signal_clicked().connect([this]() {
        if (m_loading)
          return;
        m_sig_request_split.emit();
      });
      split_row->append(*split_btn);
    }

    body->append(*split_row);
  }

  /*









  */
  // ── Open / Closed toggle + node count — first row ─────────────────────
  if (obj->path) {
    auto *path_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    path_row->add_css_class("prop-row");

    auto *path_lbl = Gtk::make_managed<Gtk::Label>("Path");
    path_lbl->add_css_class("prop-lbl");
    path_lbl->set_width_chars(5);
    path_lbl->set_xalign(0.0f);
    path_row->append(*path_lbl);

    bool closed = obj->path->closed;
    auto *close_btn =
        Gtk::make_managed<Gtk::ToggleButton>(closed ? "Closed" : "Open");
    curvz::utils::set_name(close_btn, "ins_nod_cl",
                           "inspector_node_path_closed_toggle");
    close_btn->set_active(closed);
    close_btn->add_css_class("prop-toggle");
    close_btn->add_css_class(closed ? "prop-toggle-closed"
                                    : "prop-toggle-open");
    close_btn->set_tooltip_text("Toggle open / closed path  (J in Node tool)");
    close_btn->signal_toggled().connect([this, obj, close_btn]() {
      if (m_loading)
        return;
      bool now = close_btn->get_active();
      obj->path->closed = now;
      close_btn->set_label(now ? "Closed" : "Open");
      close_btn->remove_css_class("prop-toggle-closed");
      close_btn->remove_css_class("prop-toggle-open");
      close_btn->add_css_class(now ? "prop-toggle-closed" : "prop-toggle-open");
      push_inspector_command(obj);
      emit_prop_changed();
    });
    path_row->append(*close_btn);

    // ── Direction indicator + Reverse button ──────────────────────────
    // Compute winding from anchor shoelace. Doc space is Y-down so
    // positive signed_area = CW on screen.
    BezierPath bp_dir = BezierPath::from_path_data(*obj->path);
    double sa = bp_dir.signed_area();
    bool is_cw = (sa >= 0.0);
    // Only meaningful for closed paths with enough nodes; open paths still
    // show direction (matches the amber arrow in the canvas overlay).
    const char *dir_sym = is_cw ? "↻" : "↺";
    const char *dir_tip =
        is_cw ? "Clockwise  (R in Node tool to reverse)"
              : "Counter-clockwise  (R in Node tool to reverse)";

    auto *dir_lbl = Gtk::make_managed<Gtk::Label>(std::string("  ") + dir_sym);
    dir_lbl->add_css_class("prop-val-lbl");
    dir_lbl->set_tooltip_text(dir_tip);
    path_row->append(*dir_lbl);

    auto *rev_btn = Gtk::make_managed<Gtk::Button>("Reverse");
    curvz::utils::set_name(rev_btn, "ins_nod_rv", "inspector_node_reverse_btn");
    rev_btn->add_css_class("prop-toggle");
    rev_btn->set_tooltip_text("Reverse path direction  (R in Node tool)");
    rev_btn->set_margin_start(4);
    rev_btn->signal_clicked().connect([this]() {
      if (m_loading)
        return;
      m_sig_request_reverse.emit();
    });
    path_row->append(*rev_btn);

    auto *nc = Gtk::make_managed<Gtk::Label>(
        "  " + std::to_string(obj->path->nodes.size()) + " nodes");
    nc->add_css_class("prop-val-lbl");
    path_row->append(*nc);
    body->append(*path_row);

    // Separator between path state and fill/stroke
    auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    sep->set_margin_top(4);
    sep->set_margin_bottom(4);
    body->append(*sep);
  }
}

// ── build_blend_section — properties for Blend objects ──────────────────────
// Live controls for a selected Blend: steps spinner, reverse toggle,
// stroke-width override + start/end, A/B node-count labels, Release button.
// Every edit sets blend_cache_dirty on the Blend and emits prop_changed so
// the canvas redraws and the lazy rebuild picks up the new parameters.
void PropertiesPanel::build_blend_section(SceneNode *obj, Gtk::Box *parent) {
  auto *body = add_collapsible("Blend", true, parent);
  curvz::utils::set_name(body, "ins_blnd", "inspector_blend_body");
  if (!obj || !obj->is_blend())
    return;

  uint32_t gen = m_build_gen;

  // Grid for the main param rows.
  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(4);
  grid->set_column_spacing(6);
  grid->set_margin_start(8);
  grid->set_margin_end(6);
  grid->set_margin_top(4);
  grid->set_margin_bottom(4);

  auto make_lbl = [](const char *t) {
    auto *l = Gtk::make_managed<Gtk::Label>(t);
    l->add_css_class("prop-lbl");
    l->set_xalign(0.0f);
    return l;
  };

  int row = 0;

  // ── Steps ──────────────────────────────────────────────────────────────
  grid->attach(*make_lbl("STEPS"), 0, row);
  auto adj_steps =
      Gtk::Adjustment::create(std::clamp(obj->blend_steps, 1, 50), 1, 50, 1, 5);
  auto *sp_steps = Gtk::make_managed<Gtk::SpinButton>(adj_steps, 1.0, 0);
  curvz::utils::set_name(sp_steps, "ins_blnd_st", "inspector_blend_steps_spn");
  sp_steps->set_hexpand(true);
  sp_steps->add_css_class("prop-width-entry");
  block_scroll(sp_steps, [this] { emit_canvas_focus(); });
  grid->attach(*sp_steps, 1, row, 2, 1);
  ++row;

  adj_steps->signal_value_changed().connect([this, obj, adj_steps, gen]() {
    if (m_build_gen != gen || m_loading)
      return;
    int v = std::clamp((int)adj_steps->get_value(), 1, 50);
    obj->blend_steps = v;
    obj->blend_cache_dirty = true; // force rebuild next draw
    emit_prop_changed();
  });

  // ── A / B node counts ─────────────────────────────────────────────────
  int a_count = (obj->blend_source_a && obj->blend_source_a->path)
                    ? (int)obj->blend_source_a->path->nodes.size()
                    : 0;
  int b_count = (obj->blend_source_b && obj->blend_source_b->path)
                    ? (int)obj->blend_source_b->path->nodes.size()
                    : 0;
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d / %d  (locked)", a_count, b_count);
    grid->attach(*make_lbl("A/B NODES"), 0, row);
    auto *lbl = Gtk::make_managed<Gtk::Label>(buf);
    curvz::utils::set_name(lbl, "ins_blnd_ab", "inspector_blend_ab_nodes_lbl");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("dim-label");
    grid->attach(*lbl, 1, row, 2, 1);
    ++row;
  }

  // ── Reverse direction ─────────────────────────────────────────────────
  // Swaps A and B. Implementation: swap the unique_ptrs in-place. Because
  // blend_cache is regenerated from scratch each dirty rebuild, no cache
  // surgery is needed.
  {
    auto *chk =
        Gtk::make_managed<Gtk::CheckButton>("Reverse direction (swap A↔B)");
    curvz::utils::set_name(chk, "ins_blnd_rv", "inspector_blend_reverse_check");
    chk->set_active(false);
    grid->attach(*chk, 0, row, 3, 1);
    ++row;
    chk->signal_toggled().connect([this, obj, chk, gen]() {
      if (m_build_gen != gen || m_loading)
        return;
      std::swap(obj->blend_source_a, obj->blend_source_b);
      obj->blend_cache_dirty = true;
      // Reset checkbox — the swap is applied immediately; leaving it
      // "checked" would be stale once the user opens the section again.
      m_loading = true;
      chk->set_active(false);
      m_loading = false;
      emit_prop_changed();
    });
  }

  // ── Stroke-width override ─────────────────────────────────────────────
  auto *chk_over =
      Gtk::make_managed<Gtk::CheckButton>("Override stroke width range");
  curvz::utils::set_name(chk_over, "ins_blnd_so",
                         "inspector_blend_stroke_override_check");
  chk_over->set_active(obj->blend_stroke_w_user_set);
  grid->attach(*chk_over, 0, row, 3, 1);
  ++row;

  // Start / End rows — always attached; enabled state follows the checkbox.
  auto *lbl_start = make_lbl("START W");
  grid->attach(*lbl_start, 0, row);
  auto *sp_start =
      Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
  curvz::utils::set_name(sp_start, "ins_blnd_sw",
                         "inspector_blend_start_w_spn");
  sp_start
      ->with_value(
          obj->blend_stroke_w_user_set
              ? obj->blend_stroke_w_start
              : (obj->blend_source_a ? obj->blend_source_a->stroke.width : 0.0))
      ->with_css("prop-width-entry");
  sp_start->set_hexpand(true);
  block_scroll(sp_start, [this] { emit_canvas_focus(); });
  grid->attach(*sp_start, 1, row, 2, 1);
  ++row;

  auto *lbl_end = make_lbl("END W");
  grid->attach(*lbl_end, 0, row);
  auto *sp_end = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
  curvz::utils::set_name(sp_end, "ins_blnd_ew", "inspector_blend_end_w_spn");
  sp_end
      ->with_value(
          obj->blend_stroke_w_user_set
              ? obj->blend_stroke_w_end
              : (obj->blend_source_b ? obj->blend_source_b->stroke.width : 0.0))
      ->with_css("prop-width-entry");
  sp_end->set_hexpand(true);
  block_scroll(sp_end, [this] { emit_canvas_focus(); });
  grid->attach(*sp_end, 1, row, 2, 1);
  ++row;

  auto apply_stroke_sensitive = [lbl_start, sp_start, lbl_end,
                                 sp_end](bool on) {
    lbl_start->set_sensitive(on);
    sp_start->set_sensitive(on);
    lbl_end->set_sensitive(on);
    sp_end->set_sensitive(on);
  };
  apply_stroke_sensitive(obj->blend_stroke_w_user_set);

  chk_over->signal_toggled().connect(
      [this, obj, chk_over, sp_start, sp_end, apply_stroke_sensitive, gen]() {
        if (m_build_gen != gen || m_loading)
          return;
        bool on = chk_over->get_active();
        obj->blend_stroke_w_user_set = on;
        if (on) {
          obj->blend_stroke_w_start = sp_start->get_internal_value();
          obj->blend_stroke_w_end = sp_end->get_internal_value();
        }
        obj->blend_cache_dirty = true;
        apply_stroke_sensitive(on);
        emit_prop_changed();
      });

  sp_start->on_changed([this, obj, gen](double v) {
    if (m_build_gen != gen || m_loading)
      return;
    if (!obj->blend_stroke_w_user_set)
      return; // only meaningful when on
    obj->blend_stroke_w_start = v;
    obj->blend_cache_dirty = true;
    emit_prop_changed();
  });
  sp_end->on_changed([this, obj, gen](double v) {
    if (m_build_gen != gen || m_loading)
      return;
    if (!obj->blend_stroke_w_user_set)
      return;
    obj->blend_stroke_w_end = v;
    obj->blend_cache_dirty = true;
    emit_prop_changed();
  });

  body->append(*grid);

  // ── Release button ────────────────────────────────────────────────────
  {
    auto *row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row_box->set_margin_start(8);
    row_box->set_margin_end(6);
    row_box->set_margin_top(4);
    row_box->set_margin_bottom(6);
    row_box->set_halign(Gtk::Align::END);
    auto *btn = Gtk::make_managed<Gtk::Button>("Release");
    curvz::utils::set_name(btn, "ins_blnd_rl", "inspector_blend_release_btn");
    btn->set_tooltip_text("Dissolve this Blend into A, a Group of baked "
                          "step paths, and B as siblings.");
    btn->signal_clicked().connect([this, gen]() {
      if (m_build_gen != gen)
        return;
      m_sig_request_release_blend.emit();
    });
    row_box->append(*btn);
    body->append(*row_box);
  }
}

// ── build_warp_section — properties for a Warp instance OR app defaults ─────
//
// s146 m2: Live in-place editor for a selected Warp. Mirrors the controls
// that used to live in WarpDialog (top/bot anchor counts, preset dropdown,
// quality slider) but as inspector rows rather than a modal — fits the
// "in your face for objects" rule. Edits route directly to obj.warp_env_top
// / warp_env_bottom / warp_quality and flag warp_cache_dirty;
// emit_prop_changed triggers a canvas redraw and the lazy cache rebuild
// picks up the new envelope.
//
// s155: Section is always visible (no longer selection-gated). Two
// binding modes:
//   - defaults_mode = false (a Warp is selected) — current behaviour:
//     edits write to obj->warp_*, envelope regenerates on count/preset
//     change, Release/Flatten buttons are visible.
//   - defaults_mode = true (no Warp selected) — edits write to
//     AppPreferences::warp_default_* setters. The next new Warp made
//     via the toolbar inherits these. No envelope generation here
//     (no instance to envelope), no Release/Flatten buttons (nothing
//     to release). The "(Custom)" preset row is suppressed because
//     defaults can't be in Custom mode — only instances drift.
//
// Differences from the dialog:
//   - No Apply / Cancel buttons. Edits are live — same shape as Blend.
//   - Preset dropdown gets an extra "(Custom)" entry at index 0, ONLY
//     in instance mode. This is the initial selection on every refresh
//     because the existing envelope shape can't be reliably reverse-
//     inferred to a preset (matches a comment in WarpDialog::show).
//     Selecting any non-Custom preset stomps the envelope with the
//     preset shape; (Custom) is a no-op marker meaning "leave envelope
//     alone."
//   - No undo command pushed yet. Same precedent as build_blend_section
//     — banked as a follow-up: coalesced EditWarpCommand pushes here
//     would let the inspector's slider drag collapse to one undo step.
//
// Release / Flatten buttons (instance mode only) dispatch to the
// existing on_warp_release / on_warp_flatten handlers (single source of
// truth with the Path menu).
void PropertiesPanel::build_warp_section(SceneNode *obj, Gtk::Box *parent) {
  auto *body = add_collapsible("Warp", true, parent);
  curvz::utils::set_name(body, "ins_wrp", "inspector_warp_body");

  // s155: dual-binding sentinel. instance vs defaults dispatch is
  // controlled by this single bool throughout the function.
  const bool defaults_mode = !(obj && obj->is_warp());

  uint32_t gen = m_build_gen;

  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(4);
  grid->set_column_spacing(6);
  grid->set_margin_start(8);
  grid->set_margin_end(6);
  grid->set_margin_top(4);
  grid->set_margin_bottom(4);

  auto make_lbl = [](const char *t) {
    auto *l = Gtk::make_managed<Gtk::Label>(t);
    l->add_css_class("prop-lbl");
    l->set_xalign(0.0f);
    return l;
  };

  int row = 0;

  // ── Top / Bottom anchor counts ─────────────────────────────────────────
  // Both clamp 2..4. Initial values come from the warp's stored
  // top/bot counts (s147 m2 — formerly envelope-size-derived). Storing
  // the counts as first-class fields means a saved Bulge with top=4
  // bot=2 reloads with those numbers showing in the spinners, AND a
  // hand-edited (Custom) warp remembers what counts the user last
  // chose for it.
  //
  // s155: in defaults_mode the values come from AppPreferences instead.
  int top_n, bot_n;
  if (defaults_mode) {
    top_n = std::clamp(AppPreferences::instance().warp_default_top_count(), 2, 4);
    bot_n = std::clamp(AppPreferences::instance().warp_default_bot_count(), 2, 4);
  } else {
    top_n = std::clamp(obj->warp_top_count, 2, 4);
    bot_n = std::clamp(obj->warp_bot_count, 2, 4);
  }

  auto adj_top = Gtk::Adjustment::create(top_n, 2, 4, 1, 1);
  auto *sp_top = Gtk::make_managed<Gtk::SpinButton>(adj_top, 1.0, 0);
  curvz::utils::set_name(sp_top, "ins_wrp_tn", "inspector_warp_top_count_spn");
  sp_top->set_hexpand(true);
  sp_top->add_css_class("prop-width-entry");
  block_scroll(sp_top, [this] { emit_canvas_focus(); });
  grid->attach(*make_lbl("TOP NODES"), 0, row);
  grid->attach(*sp_top, 1, row, 2, 1);
  ++row;

  auto adj_bot = Gtk::Adjustment::create(bot_n, 2, 4, 1, 1);
  auto *sp_bot = Gtk::make_managed<Gtk::SpinButton>(adj_bot, 1.0, 0);
  curvz::utils::set_name(sp_bot, "ins_wrp_bn", "inspector_warp_bot_count_spn");
  sp_bot->set_hexpand(true);
  sp_bot->add_css_class("prop-width-entry");
  block_scroll(sp_bot, [this] { emit_canvas_focus(); });
  grid->attach(*make_lbl("BOT NODES"), 0, row);
  grid->attach(*sp_bot, 1, row, 2, 1);
  ++row;

  // ── Preset dropdown ────────────────────────────────────────────────────
  // Index 0 is "(Custom)" — selected when warp_preset_idx == -1
  // (envelope is hand-edited or has been drifted by a translate /
  // drag / arrow-nudge). Indices 1..N map to preset_idx 0..N-1 in
  // curvz::utils::warp_presets. s147 m2: dropdown now reflects
  // honest provenance — saved Bulge reloads showing "Bulge", and
  // the moment the user drifts the envelope by drag, the editing
  // command sets preset_idx=-1 and a subsequent inspector refresh
  // shows "(Custom)".
  //
  // s155: defaults_mode skips the (Custom) entry. AppPreferences only
  // stores the next-warp preset as 0..7, never -1; defaults can't be
  // Custom because there's no envelope to drift. So in defaults mode
  // dd_idx maps directly to preset_idx (no offset).
  auto *preset_lbl = make_lbl("PRESET");
  grid->attach(*preset_lbl, 0, row);
  std::vector<Glib::ustring> preset_strs;
  if (!defaults_mode) {
    preset_strs.push_back("(Custom)");
  }
  const char *const *names = curvz::utils::warp_presets::preset_names();
  for (int i = 0; i < curvz::utils::warp_presets::PRESET_COUNT; ++i) {
    preset_strs.push_back(names[i]);
  }
  auto preset_model = Gtk::StringList::create(preset_strs);
  auto *dd_preset = Gtk::make_managed<Gtk::DropDown>(preset_model);
  curvz::utils::set_name(dd_preset, "ins_wrp_pr", "inspector_warp_preset_dd");
  dd_preset->set_hexpand(true);
  // Initial selection:
  //   - instance mode: 0 = (Custom) when preset_idx == -1, else preset_idx + 1
  //   - defaults mode: preset_idx directly (no Custom row offset)
  int initial_dd;
  if (defaults_mode) {
    initial_dd = std::clamp(
        AppPreferences::instance().warp_default_preset(), 0,
        curvz::utils::warp_presets::PRESET_COUNT - 1);
  } else {
    initial_dd =
        (obj->warp_preset_idx >= 0 &&
         obj->warp_preset_idx < curvz::utils::warp_presets::PRESET_COUNT)
            ? (obj->warp_preset_idx + 1)
            : 0;
  }
  dd_preset->set_selected((guint)initial_dd);
  grid->attach(*dd_preset, 1, row, 2, 1);
  ++row;

  // ── Quality slider ─────────────────────────────────────────────────────
  // 1..16 same as the dialog. Drag-friendly; live cache rebuild on each
  // value change.
  //
  // s155: defaults_mode reads from AppPreferences.
  int initial_q = defaults_mode
                      ? std::clamp(AppPreferences::instance().warp_default_quality(),
                                   1, 16)
                      : std::clamp(obj->warp_quality, 1, 16);
  auto adj_q = Gtk::Adjustment::create(initial_q, 1, 16, 1, 1);
  auto *sc_q =
      Gtk::make_managed<Gtk::Scale>(adj_q, Gtk::Orientation::HORIZONTAL);
  curvz::utils::set_name(sc_q, "ins_wrp_q", "inspector_warp_quality_scale");
  sc_q->set_hexpand(true);
  sc_q->set_draw_value(true);
  sc_q->set_digits(0);
  sc_q->set_value_pos(Gtk::PositionType::RIGHT);
  grid->attach(*make_lbl("QUALITY"), 0, row);
  grid->attach(*sc_q, 1, row, 2, 1);
  ++row;

  body->append(*grid);

  // ── Helper: regenerate envelope from current control state ────────────
  // Called when count spinners or preset dropdown change. Reads bbox
  // from warp_source (the untouched original shape), generates envelope
  // through the lifted preset pump, writes back to obj, marks cache
  // dirty, redraws.
  //
  // s146 m4: bbox source MUST be warp_source (not envelope endpoints).
  // After the user has dragged envelope handles, picking a preset
  // means "throw away my drift and lay out fresh against the original
  // shape." Envelope-derived bbox would carry the drift forward —
  // wrong intent.
  //
  // When preset dropdown is "(Custom)" (index 0), counts change is a
  // no-op against envelope: the existing shape stays untouched, the
  // new count takes effect on the next non-Custom preset selection.
  // Quality is handled by its own dedicated handler — orthogonal to
  // envelope shape, never stomps regardless of preset state.
  //
  // s155: in defaults_mode this function writes directly to
  // AppPreferences setters. There's no instance to envelope, no cache
  // to dirty, no canvas to redraw — just persist the four values for
  // the next new Warp to inherit. The (Custom) row doesn't exist in
  // defaults mode so the index-mapping is direct: dd_idx == preset_idx.
  auto regen_from_controls = [this, obj, sp_top, sp_bot, dd_preset, gen,
                              defaults_mode]() {
    if (m_build_gen != gen || m_loading)
      return;
    int t_n = std::clamp((int)sp_top->get_value(), 2, 4);
    int b_n = std::clamp((int)sp_bot->get_value(), 2, 4);
    int dd_idx = (int)dd_preset->get_selected();

    // ── Defaults mode: write to AppPreferences and return ─────────────
    if (defaults_mode) {
      // dd_idx maps directly to preset_idx (no (Custom) row offset).
      int preset_idx = std::clamp(
          dd_idx, 0, curvz::utils::warp_presets::PRESET_COUNT - 1);
      // Wave / arc presets need count >= 3. Auto-bump same way instance
      // mode does, and reflect into the spinners so the user sees what
      // happened.
      if (curvz::utils::warp_presets::requires_three_anchors(preset_idx)) {
        bool changed = false;
        if (t_n < 3) { t_n = 3; changed = true; }
        if (b_n < 3) { b_n = 3; changed = true; }
        if (changed) {
          m_loading = true;
          sp_top->set_value((double)t_n);
          sp_bot->set_value((double)b_n);
          m_loading = false;
        }
      }
      auto& prefs = AppPreferences::instance();
      prefs.set_warp_default_top_count(t_n);
      prefs.set_warp_default_bot_count(b_n);
      prefs.set_warp_default_preset(preset_idx);
      return;
    }

    // ── Instance mode: existing path (s146/s147 logic) ────────────────

    // dd_idx 0 = "(Custom)" → leave envelope alone. Counts change but
    // don't stomp the envelope; they take effect on the next preset
    // pick. This is the "I'm tweaking how many anchors my next preset
    // will have" workflow.
    //
    // s147 m2: still stamp top/bot counts onto the SceneNode in
    // (Custom) mode — the user is expressing intent for "next preset
    // will have N anchors," and that intent should survive save/load
    // and selection-change refresh. Preset_idx stays -1 (Custom).
    if (dd_idx == 0) {
      obj->warp_top_count = t_n;
      obj->warp_bot_count = b_n;
      return;
    }

    // s147 m1: bbox source MUST come from Canvas::warp_source_bbox,
    // which returns the same rectangle warp_subtree uses internally
    // when remapping points. Earlier (m4) we used
    // object_bbox_query(*warp_source) here — directionally right
    // ("query the original shape, not the mangled envelope") but
    // wrong tool: object_bbox can include stroke/recipe metadata that
    // warp_subtree doesn't see, so the envelope we generated was
    // sized to a slightly different rectangle than the renderer's
    // reference frame. The result was a warp where envelope and
    // source disagreed on coordinate frame, producing visually-
    // unchanged output (the deformations cancel against the bbox
    // mismatch).
    //
    // warp_source_bbox is path-walk-based (subtree_path_bbox of
    // glyph_cache), exactly what warp_subtree consumes. One canonical
    // bbox shared between inspector and renderer eliminates the class.
    if (!obj->warp_source || !m_canvas_widget)
      return;

    int preset_idx = dd_idx - 1;

    // Wave needs count >= 3. Auto-bump and reflect back into the
    // spinners so the user sees what happened.
    if (curvz::utils::warp_presets::requires_three_anchors(preset_idx)) {
      bool changed = false;
      if (t_n < 3) {
        t_n = 3;
        changed = true;
      }
      if (b_n < 3) {
        b_n = 3;
        changed = true;
      }
      if (changed) {
        m_loading = true;
        sp_top->set_value((double)t_n);
        sp_bot->set_value((double)b_n);
        m_loading = false;
      }
    }

    // Canonical source bbox — same one warp_subtree will use to
    // interpret the envelope we're about to write.
    double bx = 0, by = 0, bw = 1, bh = 1;
    if (!m_canvas_widget->warp_source_bbox(*obj, bx, by, bw, bh)) {
      // Source has no path-bearing geometry (shouldn't happen for
      // warpable types, but defensive). Bail without disturbing
      // envelope.
      return;
    }
    if (bw < 1e-9)
      bw = 1.0;
    if (bh < 1e-9)
      bh = 1.0;

    curvz::utils::generate_warp_preset(preset_idx, bx, by, bw, bh, t_n, b_n,
                                       obj->warp_env_top, obj->warp_env_bottom);

    // s147 m2: stamp provenance — the envelope now matches preset
    // (preset_idx, t_n, b_n) over warp_source_bbox. Honest label for
    // the dropdown on next refresh, and round-trips to SVG so a saved
    // Bulge reloads as Bulge.
    obj->warp_preset_idx = preset_idx;
    obj->warp_top_count = t_n;
    obj->warp_bot_count = b_n;

    obj->warp_cache_dirty = true;
    // Direct canvas redraw — emit_prop_changed defers via timer and the
    // user wants the envelope handles to snap visibly the instant they
    // pick a preset. queue_draw is idempotent against the deferred
    // signal so doing both is safe.
    if (m_canvas_widget)
      m_canvas_widget->queue_draw();
    emit_prop_changed();
  };

  adj_top->signal_value_changed().connect(regen_from_controls);
  adj_bot->signal_value_changed().connect(regen_from_controls);
  dd_preset->property_selected().signal_changed().connect(regen_from_controls);
  // Quality is orthogonal to envelope shape — adjusting it should never
  // stomp the envelope, even when a non-Custom preset is selected. So
  // it gets its own minimal handler that only syncs the int + redraws.
  //
  // s155: defaults_mode writes to AppPreferences instead of obj.
  adj_q->signal_value_changed().connect([this, obj, sc_q, gen, defaults_mode]() {
    if (m_build_gen != gen || m_loading)
      return;
    int q = std::clamp((int)sc_q->get_value(), 1, 16);
    if (defaults_mode) {
      AppPreferences::instance().set_warp_default_quality(q);
      return;
    }
    obj->warp_quality = q;
    obj->warp_cache_dirty = true;
    if (m_canvas_widget)
      m_canvas_widget->queue_draw();
    emit_prop_changed();
  });

  // ── Release / Flatten buttons ─────────────────────────────────────────
  // Two-button row, end-aligned to match the Blend section's idiom.
  // Release dissolves the Warp into source + cache as siblings; Flatten
  // bakes the warped result into a single path. Both route to existing
  // MainWindow handlers via the panel's signals.
  //
  // s155: only shown in instance mode — there's nothing to release or
  // flatten in defaults mode.
  if (!defaults_mode) {
    auto *row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row_box->set_margin_start(8);
    row_box->set_margin_end(6);
    row_box->set_margin_top(4);
    row_box->set_margin_bottom(6);
    row_box->set_spacing(6);
    row_box->set_halign(Gtk::Align::END);

    auto *btn_release = Gtk::make_managed<Gtk::Button>("Release");
    curvz::utils::set_name(btn_release, "ins_wrp_rl",
                           "inspector_warp_release_btn");
    btn_release->set_tooltip_text(
        "Dissolve this Warp back into its source and cached glyph siblings.");
    btn_release->signal_clicked().connect([this, gen]() {
      if (m_build_gen != gen)
        return;
      m_sig_request_release_warp.emit();
    });

    auto *btn_flatten = Gtk::make_managed<Gtk::Button>("Flatten");
    curvz::utils::set_name(btn_flatten, "ins_wrp_fl",
                           "inspector_warp_flatten_btn");
    btn_flatten->set_tooltip_text(
        "Bake the warped result into a single path, discarding the envelope.");
    btn_flatten->signal_clicked().connect([this, gen]() {
      if (m_build_gen != gen)
        return;
      m_sig_request_flatten_warp.emit();
    });

    row_box->append(*btn_release);
    row_box->append(*btn_flatten);
    body->append(*row_box);
  }
}

// ── build_shadow_section — drop shadow for any non-special node ─────────────
//
// S97 m3. Shown for Path / Compound / Group / Text / Image / ClipGroup /
// Blend / Warp (per SceneNode::can_have_shadow). Hidden for layers, guides,
// refs, measurements, grids, margins — those have no render pass to shadow.
//
// Layout:
//   [✓] Enable                 (toggle row, full width)
//   OFFSET   [dx][dy]          (two CurvzSpinButton, SpinType::Distance)
//   BLUR     [r ]              (CurvzSpinButton, SpinType::Width)
//   COLOUR   [swatch] [α slider]
//   OPACITY  [slider 0..100]
//
// Edits route directly to obj.shadow_* fields and call push_inspector_command
// for undo coalescing. The colour swatch opens m_color_popover with
// with_alpha=false; the alpha slider next to it controls shadow_color_a
// independently. shadow_opacity is a separate slider (the multiplier), so a
// user can dim/brighten without re-picking the colour.
//
// Disable behaviour: when Enable is unchecked, the rest of the controls go
// insensitive (read-only) but stay visible at their current values, so a
// quick toggle off/on doesn't lose the configured shadow. The existing
// fields sit in obj across the toggle — only shadow_enabled flips.
void PropertiesPanel::build_shadow_section(SceneNode *obj, Gtk::Box *parent) {
  auto *body = add_collapsible("Shadow", false, parent);
  curvz::utils::set_name(body, "ins_shdw", "inspector_shadow_body");
  if (!obj || !obj->can_have_shadow())
    return;

  uint32_t gen = m_build_gen;

  auto make_lbl = [](const char *t) {
    auto *l = Gtk::make_managed<Gtk::Label>(t);
    l->add_css_class("prop-lbl");
    l->set_xalign(0.0f);
    return l;
  };

  // Container grid for parameter rows.
  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(4);
  grid->set_column_spacing(6);
  grid->set_margin_start(8);
  grid->set_margin_end(6);
  grid->set_margin_top(4);
  grid->set_margin_bottom(4);

  int row = 0;

  // ── Enable toggle ──────────────────────────────────────────────────
  // Spans all three columns. Drives the sensitive() state of every other
  // row in the section.
  auto *chk_enable = Gtk::make_managed<Gtk::CheckButton>("Enable shadow");
  curvz::utils::set_name(chk_enable, "ins_shdw_en",
                         "inspector_shadow_enable_check");
  chk_enable->set_active(obj->shadow_enabled);
  grid->attach(*chk_enable, 0, row, 3, 1);
  ++row;

  // ── Offset (dx, dy) ────────────────────────────────────────────────
  // Two spinbuttons side-by-side under one OFFSET label. Y-up convention
  // applies — SpinType::Distance is unsigned-distance with conversion;
  // signed offsets need it without flip, which is what Distance gives.
  // Each spin is wrapped in a small HBox alongside its unit label so the
  // current display unit (px/in/mm/pt) is visible — matches the idiom
  // used by every other distance row in the inspector.
  grid->attach(*make_lbl("OFFSET"), 0, row);
  auto *sp_dx =
      Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
  curvz::utils::set_name(sp_dx, "ins_shdw_dx", "inspector_shadow_dx_spn");
  sp_dx->with_value(obj->shadow_dx)
      ->with_css("prop-width-entry")
      ->with_tooltip("Horizontal offset (+right)");
  sp_dx->set_hexpand(true);
  block_scroll(sp_dx, [this] { emit_canvas_focus(); });
  auto *dx_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  dx_row->set_spacing(4);
  dx_row->set_hexpand(true);
  dx_row->append(*sp_dx);
  if (auto *ul = sp_dx->get_unit_label())
    dx_row->append(*ul);
  grid->attach(*dx_row, 1, row);

  auto *sp_dy =
      Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
  curvz::utils::set_name(sp_dy, "ins_shdw_dy", "inspector_shadow_dy_spn");
  sp_dy->with_value(obj->shadow_dy)
      ->with_css("prop-width-entry")
      ->with_tooltip("Vertical offset (+down)");
  sp_dy->set_hexpand(true);
  block_scroll(sp_dy, [this] { emit_canvas_focus(); });
  auto *dy_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  dy_row->set_spacing(4);
  dy_row->set_hexpand(true);
  dy_row->append(*sp_dy);
  if (auto *ul = sp_dy->get_unit_label())
    dy_row->append(*ul);
  grid->attach(*dy_row, 2, row);
  ++row;

  // ── Blur ───────────────────────────────────────────────────────────
  // Gaussian stddev in doc units. SpinType::Width is non-negative —
  // matches the renderer's clamp blur to >= 0.
  grid->attach(*make_lbl("BLUR"), 0, row);
  auto *sp_blur = Gtk::make_managed<CurvzSpinButton>(SpinType::Width, m_canvas);
  curvz::utils::set_name(sp_blur, "ins_shdw_bl", "inspector_shadow_blur_spn");
  sp_blur->with_value(obj->shadow_blur)
      ->with_css("prop-width-entry")
      ->with_tooltip("Blur radius (Gaussian stddev)");
  sp_blur->set_hexpand(true);
  block_scroll(sp_blur, [this] { emit_canvas_focus(); });
  auto *blur_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  blur_row->set_spacing(4);
  blur_row->set_hexpand(true);
  blur_row->append(*sp_blur);
  if (auto *ul = sp_blur->get_unit_label())
    blur_row->append(*ul);
  grid->attach(*blur_row, 1, row, 2, 1);
  ++row;

  // ── Colour: swatch + alpha slider ──────────────────────────────────
  // The swatch shows the RGB colour at full alpha; the slider next to it
  // edits shadow_color_a (multiplied by shadow_opacity at render to give
  // the final flood-opacity). Two sliders (this + opacity below) feels
  // redundant on first read but matches Affinity / Illustrator: colour-
  // alpha lets you dim a colour without re-picking; opacity is a
  // post-effect dimmer that survives colour changes.
  grid->attach(*make_lbl("COLOUR"), 0, row);
  auto *colour_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  colour_row->set_spacing(6);
  colour_row->set_hexpand(true);
  auto *swatch = Gtk::make_managed<Gtk::DrawingArea>();
  curvz::utils::set_name(swatch, "ins_shdw_color",
                         "inspector_shadow_color_swatch_da");
  swatch->set_size_request(28, 22);
  swatch->set_valign(Gtk::Align::CENTER);
  swatch->set_can_target(true);
  // Capture obj — the draw lambda reads obj->shadow_color_* at draw time
  // so the swatch live-updates after a colour pick. gen guard happens at
  // click time, not draw time, since draw happens passively for visible
  // widgets and a stale obj* would be a deeper issue (obj outliving the
  // panel state).
  swatch->set_draw_func(
      [obj](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        // Background checker for transparent colours — borrowed pattern
        // from the appearance section colour wells. Skip for opaque to
        // keep the common case crisp.
        if (obj->shadow_color_a < 0.999) {
          for (int yy = 0; yy < h; yy += 4)
            for (int xx = 0; xx < w; xx += 4) {
              bool dark = ((xx / 4) + (yy / 4)) % 2;
              cr->set_source_rgb(dark ? 0.7 : 0.85, dark ? 0.7 : 0.85,
                                 dark ? 0.7 : 0.85);
              cr->rectangle(xx, yy, 4, 4);
              cr->fill();
            }
        }
        cr->set_source_rgba(obj->shadow_color_r, obj->shadow_color_g,
                            obj->shadow_color_b, obj->shadow_color_a);
        cr->rectangle(0, 0, w, h);
        cr->fill();
        // Hairline border so a near-white shadow on the panel background
        // isn't invisible.
        cr->set_source_rgba(0, 0, 0, 0.25);
        cr->set_line_width(1.0);
        cr->rectangle(0.5, 0.5, w - 1, h - 1);
        cr->stroke();
      });
  auto swatch_click = Gtk::GestureClick::create();
  swatch_click->set_button(1);
  swatch_click->signal_pressed().connect(
      [this, obj, swatch, gen](int, double, double) {
        if (m_build_gen != gen)
          return;
        color::Color initial(obj->shadow_color_r, obj->shadow_color_g,
                             obj->shadow_color_b, 1.0);
        m_color_popover.open(*swatch, initial, /*with_alpha=*/false,
                             [this, obj, swatch, gen](const color::Color &c) {
                               if (m_build_gen != gen)
                                 return;
                               // S98: route through mutate_appearance so a
                               // bound style is unbound on direct shadow-colour
                               // edit. Symmetric with the fill/stroke editors
                               // above.
                               Curvz::style::mutate_appearance(
                                   *obj, [&](SceneNode & /*n*/) {
                                     obj->shadow_color_r = c.r;
                                     obj->shadow_color_g = c.g;
                                     obj->shadow_color_b = c.b;
                                   });
                               swatch->queue_draw();
                               push_inspector_command(obj);
                               emit_prop_changed();
                             });
      });
  swatch->add_controller(swatch_click);
  colour_row->append(*swatch);

  // Colour alpha slider — 0..100 mapped to 0..1.
  auto adj_ca = Gtk::Adjustment::create(obj->shadow_color_a * 100.0, 0.0, 100.0,
                                        1.0, 10.0);
  auto *sl_ca = Gtk::make_managed<Gtk::Scale>(adj_ca);
  curvz::utils::set_name(sl_ca, "ins_shdw_ca",
                         "inspector_shadow_color_alpha_slider");
  sl_ca->set_draw_value(true);
  sl_ca->set_value_pos(Gtk::PositionType::RIGHT);
  sl_ca->set_digits(0);
  sl_ca->set_hexpand(true);
  sl_ca->set_tooltip_text("Colour alpha (separate from Opacity below)");
  colour_row->append(*sl_ca);
  grid->attach(*colour_row, 1, row, 2, 1);
  ++row;

  // ── Opacity ────────────────────────────────────────────────────────
  grid->attach(*make_lbl("OPACITY"), 0, row);
  auto adj_op = Gtk::Adjustment::create(obj->shadow_opacity * 100.0, 0.0, 100.0,
                                        1.0, 10.0);
  auto *sl_op = Gtk::make_managed<Gtk::Scale>(adj_op);
  curvz::utils::set_name(sl_op, "ins_shdw_op",
                         "inspector_shadow_opacity_slider");
  sl_op->set_draw_value(true);
  sl_op->set_value_pos(Gtk::PositionType::RIGHT);
  sl_op->set_digits(0);
  sl_op->set_hexpand(true);
  sl_op->set_tooltip_text(
      "Final shadow strength (multiplied with colour alpha)");
  grid->attach(*sl_op, 1, row, 2, 1);
  ++row;

  body->append(*grid);

  // ── Sensitivity slave ──────────────────────────────────────────────
  // Hold all child widgets in a small list and toggle them as a unit
  // when the enable checkbox flips. Keeping the section visible-but-
  // disabled lets a user iterate (re-enable to see the same config) —
  // matches the Blend section's stroke-width override slave pattern.
  auto apply_enabled = [sp_dx, sp_dy, sp_blur, swatch, sl_ca, sl_op,
                        grid](bool on) {
    // grid children that aren't the enable checkbox itself need slaving.
    // We slave at the widget level, not the row level — checkbox row is
    // intentionally left always-active so the user can re-enable.
    sp_dx->set_sensitive(on);
    sp_dy->set_sensitive(on);
    sp_blur->set_sensitive(on);
    swatch->set_sensitive(on);
    sl_ca->set_sensitive(on);
    sl_op->set_sensitive(on);
    (void)grid;
  };
  apply_enabled(obj->shadow_enabled);

  // ── Wire signals ───────────────────────────────────────────────────
  // S98: every shadow-write handler routes through mutate_appearance so
  // a bound style on `obj` is unbound on direct edit. Symmetric with
  // fill/stroke editors (break-on-override). The funnel runs first;
  // push_inspector_command then captures the post-funnel state for undo.
  chk_enable->signal_toggled().connect(
      [this, obj, chk_enable, apply_enabled, gen]() {
        if (m_build_gen != gen || m_loading)
          return;
        Curvz::style::mutate_appearance(*obj, [&](SceneNode & /*n*/) {
          obj->shadow_enabled = chk_enable->get_active();
        });
        apply_enabled(obj->shadow_enabled);
        push_inspector_command(obj);
        emit_prop_changed();
      });

  sp_dx->on_changed([this, obj, gen](double v) {
    if (m_build_gen != gen || m_loading)
      return;
    Curvz::style::mutate_appearance(
        *obj, [&](SceneNode & /*n*/) { obj->shadow_dx = v; });
    push_inspector_command(obj);
    emit_prop_changed();
  });
  sp_dy->on_changed([this, obj, gen](double v) {
    if (m_build_gen != gen || m_loading)
      return;
    Curvz::style::mutate_appearance(
        *obj, [&](SceneNode & /*n*/) { obj->shadow_dy = v; });
    push_inspector_command(obj);
    emit_prop_changed();
  });
  sp_blur->on_changed([this, obj, gen](double v) {
    if (m_build_gen != gen || m_loading)
      return;
    Curvz::style::mutate_appearance(*obj, [&](SceneNode & /*n*/) {
      obj->shadow_blur = std::max(0.0, v); // defensive: clamp at 0
    });
    push_inspector_command(obj);
    emit_prop_changed();
  });

  adj_ca->signal_value_changed().connect([this, obj, swatch, adj_ca, gen]() {
    if (m_build_gen != gen || m_loading)
      return;
    Curvz::style::mutate_appearance(*obj, [&](SceneNode & /*n*/) {
      obj->shadow_color_a = std::clamp(adj_ca->get_value() / 100.0, 0.0, 1.0);
    });
    swatch->queue_draw();
    push_inspector_command(obj);
    emit_prop_changed();
  });
  adj_op->signal_value_changed().connect([this, obj, adj_op, gen]() {
    if (m_build_gen != gen || m_loading)
      return;
    Curvz::style::mutate_appearance(*obj, [&](SceneNode & /*n*/) {
      obj->shadow_opacity = std::clamp(adj_op->get_value() / 100.0, 0.0, 1.0);
    });
    push_inspector_command(obj);
    emit_prop_changed();
  });
}

void PropertiesPanel::refresh_node(CanvasModel *canvas, SceneNode *obj,
                                   int node_idx) {
  // Fast path: same object + same node + same type — just update the adjustment
  // values. If the node type changed (e.g. Corner→Symmetric via hotkey) we must
  // do a full rebuild so the handle rows are shown/hidden correctly.
  if (canvas && obj && node_idx >= 0 && obj == m_current &&
      node_idx == m_node_idx && m_adj_ax) {
    if (!obj->path || node_idx >= (int)obj->path->nodes.size())
      return;
    const BezierNode &nd = obj->path->nodes[node_idx];
    if (nd.type != m_node_type) {
      // Type changed — fall through to full rebuild below.
      LOG_DEBUG("refresh_node: node type changed ({} → {}), forcing rebuild",
                (int)m_node_type, (int)nd.type);
    } else {
      LOG_DEBUG("refresh_node FAST: obj={} node={} x={:.2f} y={:.2f}",
                (void *)obj, node_idx, nd.x, nd.y);
      m_loading = true;
      m_adj_ax->set_value(node_doc_to_display_x(nd.x, m_canvas, m_ruler_ox));
      m_adj_ay->set_value(node_doc_to_display_y(nd.y, m_canvas, m_ruler_oy));
      if (m_adj_ix) {
        m_adj_ix->set_value(
            node_doc_to_display_x(nd.cx1, m_canvas, m_ruler_ox));
        m_adj_iy->set_value(
            node_doc_to_display_y(nd.cy1, m_canvas, m_ruler_oy));
      }
      if (m_adj_ox) {
        m_adj_ox->set_value(
            node_doc_to_display_x(nd.cx2, m_canvas, m_ruler_ox));
        m_adj_oy->set_value(
            node_doc_to_display_y(nd.cy2, m_canvas, m_ruler_oy));
      }
      // Defer m_loading reset — GTK may deliver signal_value_changed from the
      // set_value() calls above asynchronously, after this function returns.
      // Keeping m_loading=true until the next idle ensures those signals are
      // blocked and don't feed stale adjustment values back into
      // apply_node_edit.
      Glib::signal_idle().connect_once([this]() { m_loading = false; });
      return;
    } // end else (type unchanged — fast path taken)
  } // end fast-path if block — fall through to full rebuild if type changed

  // Full rebuild — new object, new node, type change, or panel was cleared.
  LOG_DEBUG("refresh_node REBUILD: obj={} node={} prev_obj={} prev_node={}",
            (void *)obj, node_idx, (void *)m_current, m_node_idx);
  m_canvas = canvas;
  m_current = obj;
  m_node_idx = node_idx;
  // Record the type so the fast path can detect a future type change.
  if (obj && obj->path && node_idx >= 0 &&
      node_idx < (int)obj->path->nodes.size())
    m_node_type = obj->path->nodes[node_idx].type;
  m_loading = true; // block signals before nulling adjustments
  m_adj_ax = m_adj_ay = m_adj_ix = m_adj_iy = m_adj_ox = m_adj_oy = {};
  do_clear(); // increments m_build_gen — all old lambdas now invalid

  // Snapshot "before" state for the newly selected object/node.
  if (obj) {
    if (obj->path)
      m_undo_before = *obj->path;
    m_undo_fill_before = obj->fill;
    m_undo_stroke_before = obj->stroke;
    // S82 m4f: capture pre-edit swatch bindings alongside fill/stroke.
    m_undo_fill_swatch_id_before = obj->fill_swatch_id;
    m_undo_stroke_swatch_id_before = obj->stroke_swatch_id;
    // S92 m3: same baseline capture for bound_style.
    m_undo_bound_style_before = obj->bound_style;
    // S97 m3: same baseline capture for shadow.
    m_undo_shadow_before = obj->read_shadow();
  }

  if (!canvas) {
    m_loading = false;
    return;
  }

  auto cm = std::make_shared<CanvasModel>(*canvas);

  // ── Application group (s143 m1; reordered s145 m2) ──────────────
  // Mirrored from the selection-empty assembly above. Application
  // sits first by scope-hierarchy convention.
  {
    auto *app_grp = add_group_collapsible("Application", false);
    build_app_section(app_grp);
  }

  // ── Project group (s116 m6 → s148 m2: deleted; see twin in setup_layout) ─

  // ── Document group (s148 m2 fix2: Motif disclosure;
  //                    see twin in setup_layout for full rationale) ──
  {
    auto *doc_grp = add_group_collapsible("Document", false);
    if (m_project) {
      build_metadata_section(doc_grp);
      if (auto *doc = m_project->active_doc()) {
        build_theme_disclosure(doc, doc_grp);
        // s150: build_measure_section + build_snap_section calls removed.
        // Both are now toolbar right-click popovers (Ruler, Snap).
      }
    }
    build_canvas_section(cm, doc_grp);
  }

  // ── Object group ─────────────────────────────────────────────
  {
    auto *obj_grp = add_group_collapsible("Object", false);
    if (obj) {
      build_selection_section(obj, obj_grp);
      if (node_idx >= 0) {
        LOG_DEBUG("refresh_node: building node section for node {}", node_idx);
        build_node_section(obj, node_idx, obj_grp);
      } else {
        build_node_section(obj, -1, obj_grp);
      }
      add_fill_stroke_section(obj, obj_grp);
      // S97 m3 — drop shadow section, gated by can_have_shadow.
      if (obj->can_have_shadow())
        build_shadow_section(obj, obj_grp);
    } else {
      build_selection_section(nullptr, obj_grp);
      build_node_section(nullptr, -1, obj_grp);
      add_fill_stroke_section(nullptr, obj_grp);
    }
  }

  m_loading = false;
}

// ── Colour well + swatches
// ────────────────────────────────────────────────────
struct ColorWidgets {
  Gtk::DrawingArea *swatch;
  CurvzEntry *entry;
  std::shared_ptr<double> r, g, b;
};

static void redraw_swatch(Gtk::DrawingArea *da, std::shared_ptr<double> r,
                          std::shared_ptr<double> g,
                          std::shared_ptr<double> b) {
  da->set_draw_func(
      [r, g, b](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        double rx = 3;
        cr->set_source_rgb(*r, *g, *b);
        cr->arc(rx, rx, rx, M_PI, 1.5 * M_PI);
        cr->arc(w - rx, rx, rx, 1.5 * M_PI, 0);
        cr->arc(w - rx, h - rx, rx, 0, .5 * M_PI);
        cr->arc(rx, h - rx, rx, .5 * M_PI, M_PI);
        cr->close_path();
        cr->fill();
        cr->set_source_rgba(1, 1, 1, .15);
        cr->set_line_width(.8);
        cr->arc(rx, rx, rx, M_PI, 1.5 * M_PI);
        cr->arc(w - rx, rx, rx, 1.5 * M_PI, 0);
        cr->arc(w - rx, h - rx, rx, 0, .5 * M_PI);
        cr->arc(rx, h - rx, rx, .5 * M_PI, M_PI);
        cr->close_path();
        cr->stroke();
      });
  da->queue_draw();
}

static ColorWidgets make_color_widgets(Gtk::Box *parent, double r, double g,
                                       double b) {
  ColorWidgets cw;
  cw.r = std::make_shared<double>(r);
  cw.g = std::make_shared<double>(g);
  cw.b = std::make_shared<double>(b);

  // Color row
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->add_css_class("prop-row");
  auto *lbl = Gtk::make_managed<Gtk::Label>("Color");
  lbl->add_css_class("prop-lbl");
  lbl->set_width_chars(5);
  lbl->set_xalign(0.0f);
  row->append(*lbl);

  cw.swatch = Gtk::make_managed<Gtk::DrawingArea>();
  cw.swatch->set_size_request(18, 15);
  cw.swatch->set_margin_end(4);
  redraw_swatch(cw.swatch, cw.r, cw.g, cw.b);
  row->append(*cw.swatch);

  cw.entry = Gtk::make_managed<CurvzEntry>();
  cw.entry->set_max_length(7);
  cw.entry->set_placeholder_text("#rrggbb");
  cw.entry->set_text(hex_of(r, g, b));
  cw.entry->add_css_class("prop-hex-entry");
  cw.entry->set_hexpand(true);
  row->append(*cw.entry);
  parent->append(*row);

  // Swatches row
  static const struct {
    double r, g, b;
    const char *hex;
  } PAL[] = {
      {0, 0, 0, "#000000"},       {1, 1, 1, "#ffffff"},
      {.8, .8, .8, "#cccccc"},    {.4, .4, .4, "#666666"},
      {.89, .11, .11, "#e31c1c"}, {.93, .49, .09, "#ed7d17"},
      {.98, .85, .10, "#fad919"}, {.13, .69, .30, "#22b04c"},
      {.09, .46, .82, "#1775d1"}, {.42, .19, .80, "#6b30cc"},
      {.89, .17, .55, "#e32c8c"}, {.09, .71, .71, "#17b5b5"},
  };
  auto *sw_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  sw_row->add_css_class("prop-swatches");
  for (const auto &s : PAL) {
    auto *btn = Gtk::make_managed<Gtk::Button>();
    btn->set_size_request(13, 13);
    btn->set_tooltip_text(s.hex);
    btn->add_css_class("swatch-btn");
    auto *da = Gtk::make_managed<Gtk::DrawingArea>();
    da->set_size_request(10, 10);
    double sr = s.r, sg = s.g, sb = s.b;
    da->set_draw_func(
        [sr, sg, sb](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
          cr->set_source_rgb(sr, sg, sb);
          cr->paint();
          cr->set_source_rgba(0, 0, 0, .3);
          cr->set_line_width(.5);
          cr->rectangle(.5, .5, w - 1, h - 1);
          cr->stroke();
        });
    btn->set_child(*da);
    double br = s.r, bg = s.g, bb = s.b;
    const char *bh = s.hex;
    auto sr2 = cw.r, sg2 = cw.g, sb2 = cw.b;
    Gtk::DrawingArea *sw_ptr = cw.swatch;
    CurvzEntry *en_ptr = cw.entry;
    btn->signal_clicked().connect(
        [br, bg, bb, bh, sr2, sg2, sb2, sw_ptr, en_ptr]() {
          *sr2 = br;
          *sg2 = bg;
          *sb2 = bb;
          en_ptr->set_text(bh);
          redraw_swatch(sw_ptr, sr2, sg2, sb2);
        });
    sw_row->append(*btn);
  }
  parent->append(*sw_row);

  return cw;
}

// ── Visual shape picker (cap / join)
// ────────────────────────────────────────── Icon-style cells: compact squares
// (~28×28), no text labels, shape is self- documenting. Cap shows full line
// with both ends capped. Join shows a right- angle corner with the join style.
// Matches Inkscape/Affinity icon approach.
enum class ShapeKind { Cap, Join };

// Helper: draw a compact rounded-rect button background
static void draw_chip_bg(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h,
                         bool active) {
  double rx = 3.0;
  if (active)
    cr->set_source_rgb(0.11, 0.18, 0.27);
  else
    cr->set_source_rgb(0.12, 0.12, 0.12);
  cr->arc(rx, rx, rx, M_PI, 1.5 * M_PI);
  cr->arc(w - rx, rx, rx, 1.5 * M_PI, 0);
  cr->arc(w - rx, h - rx, rx, 0, .5 * M_PI);
  cr->arc(rx, h - rx, rx, .5 * M_PI, M_PI);
  cr->close_path();
  cr->fill();

  if (active)
    cr->set_source_rgba(.21, .52, .89, .85);
  else
    cr->set_source_rgba(1, 1, 1, .08);
  cr->set_line_width(active ? .9 : .5);
  cr->arc(rx, rx, rx, M_PI, 1.5 * M_PI);
  cr->arc(w - rx, rx, rx, 1.5 * M_PI, 0);
  cr->arc(w - rx, h - rx, rx, 0, .5 * M_PI);
  cr->arc(rx, h - rx, rx, .5 * M_PI, M_PI);
  cr->close_path();
  cr->stroke();
}

static void draw_cap_cell(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h,
                          int style, bool active) {
  draw_chip_bg(cr, w, h, active);

  // Simple rect-based cap icons. All three show a filled rectangle
  // representing the stroke body. Cap differences shown via the right end:
  //   Butt   (0): flat right edge — rect ends exactly at midpoint
  //   Round  (1): rounded right end — semicircle on right only
  //   Square (2): rect extends past midpoint by half the stroke height

  double ic_h = h * 0.38;         // icon stroke thickness
  double ic_y = (h - ic_h) * 0.5; // vertically centred
  double x1 = w * 0.18;           // left end (always flat)
  double mid = w * 0.60;          // nominal right endpoint
  double ext = ic_h * 0.5;        // square cap extension

  double fr = active ? .75 : .52;
  double fg = active ? .84 : .52;
  double fb = active ? 1.0 : .52;
  cr->set_source_rgb(fr, fg, fb);

  switch (style) {
  case 0: // Butt — flat rect, ends at mid
    cr->rectangle(x1, ic_y, mid - x1, ic_h);
    cr->fill();
    break;

  case 1: // Round — rect body + semicircle on right end only
    cr->rectangle(x1, ic_y, mid - x1, ic_h);
    cr->fill();
    cr->arc(mid, ic_y + ic_h * 0.5, ic_h * 0.5, -M_PI * 0.5, M_PI * 0.5);
    cr->fill();
    break;

  case 2: // Square — rect extends past mid by ext
    cr->rectangle(x1, ic_y, (mid + ext) - x1, ic_h);
    cr->fill();
    break;
  }
}

static void draw_join_cell(const Cairo::RefPtr<Cairo::Context> &cr, int w,
                           int h, int style, bool active) {
  draw_chip_bg(cr, w, h, active);

  // Simple rect-based join icons. Two arms meet at bottom-left corner.
  // All drawn as filled rectangles — the outer corner is what differs:
  //   Miter (0): sharp outer corner — arms extend to a point
  //   Round (1): rounded outer corner — quarter-circle arc
  //   Bevel (2): flat cut — diagonal line closes the corner

  double arm = h * 0.32;     // arm thickness
  double mg = w * 0.18;      // margin from edges
  double top = mg;           // top of vertical arm
  double bot = h - mg - arm; // bottom of vertical arm = corner inner Y
  double left = mg;          // left of horizontal arm = corner inner X
  double right = w - mg;     // right end of horizontal arm

  double fr = active ? .75 : .52;
  double fg = active ? .84 : .52;
  double fb = active ? 1.0 : .52;
  cr->set_source_rgb(fr, fg, fb);

  switch (style) {
  case 0: { // Miter — sharp outer corner: fill corner rect + both arms
    // vertical arm (includes corner square)
    cr->rectangle(left, top, arm, (bot + arm) - top);
    cr->fill();
    // horizontal arm (excludes corner square — already filled)
    cr->rectangle(left + arm, bot, (right) - (left + arm), arm);
    cr->fill();
    break;
  }
  case 1: { // Round — same arms, replace outer corner with arc
    // vertical arm body
    cr->rectangle(left, top, arm, bot - top);
    cr->fill();
    // horizontal arm body
    cr->rectangle(left + arm, bot, right - (left + arm), arm);
    cr->fill();
    // quarter-circle at outer corner, centred on inner corner
    double ic_x = left + arm;                     // inner corner X
    double ic_y = bot;                            // inner corner Y
    cr->arc(ic_x, ic_y, arm, -M_PI, -M_PI * 0.5); // outer arc
    cr->line_to(ic_x, ic_y);
    cr->close_path();
    cr->fill();
    break;
  }
  case 2: { // Bevel — arms end at 45° cut
    // vertical arm body
    cr->rectangle(left, top, arm, bot - top);
    cr->fill();
    // horizontal arm body
    cr->rectangle(left + arm, bot, right - (left + arm), arm);
    cr->fill();
    // bevel triangle fills the diagonal cut
    cr->move_to(left, bot);             // inner top-left of corner
    cr->line_to(left + arm, bot);       // inner top-right
    cr->line_to(left + arm, bot + arm); // inner bottom-right
    cr->close_path();
    cr->fill();
    break;
  }
  }
}

static Gtk::Box *make_visual_picker(ShapeKind kind, int current,
                                    std::function<void(int)> on_change) {
  // Use Gtk::Button containing a DrawingArea child.
  // Buttons are always properly realized and their signal_clicked is safe
  // to connect — no GestureClick on DrawingArea needed, eliminating the
  // "snapshot without allocation" crash that plagued the previous approach.
  auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  box->set_spacing(3);
  box->set_hexpand(true);

  auto sel = std::make_shared<int>(current);

  // shared_ptr so all 3 button lambdas reference the SAME vector,
  // fully populated after the loop. A by-value capture of a stack vector
  // inside the loop would snapshot a partially-filled vector each time.
  auto das = std::make_shared<std::vector<Gtk::DrawingArea *>>(3, nullptr);

  static const char *cap_tips[] = {
      "Butt — line ends flush at anchor point",
      "Round — semicircle cap extends past endpoint",
      "Square — flat cap extends half stroke width past endpoint"};
  static const char *join_tips[] = {"Miter — sharp pointed outer corner",
                                    "Round — rounded outer corner",
                                    "Bevel — flat cut outer corner"};

  for (int i = 0; i < 3; ++i) {
    // Button wrapper — handles click, focus, keyboard activation safely
    auto *btn = Gtk::make_managed<Gtk::Button>();
    btn->set_has_frame(false);
    btn->add_css_class("visual-picker-btn");
    btn->set_hexpand(true);
    btn->set_tooltip_text(kind == ShapeKind::Cap ? cap_tips[i] : join_tips[i]);

    // DrawingArea child — pure rendering, no event handling
    auto *da = Gtk::make_managed<Gtk::DrawingArea>();
    da->set_size_request(44, 32);
    da->set_can_target(false); // don't intercept events from the button
    (*das)[i] = da;

    int opt = i;
    if (kind == ShapeKind::Cap) {
      da->set_draw_func(
          [sel, opt](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
            draw_cap_cell(cr, w, h, opt, *sel == opt);
          });
    } else {
      da->set_draw_func(
          [sel, opt](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
            draw_join_cell(cr, w, h, opt, *sel == opt);
          });
    }
    btn->set_child(*da);

    // alive flag — shared with the button, set false when the
    // containing section is destroyed (obj deselected / inspector cleared).
    // Guards against signal_clicked firing on a dead object.
    auto alive = std::make_shared<bool>(true);
    box->signal_destroy().connect([alive]() { *alive = false; });

    btn->signal_clicked().connect([sel, opt, das, on_change, alive]() {
      LOG_DEBUG("picker clicked: opt={} alive={}", opt, *alive);
      if (!*alive) {
        LOG_DEBUG("picker clicked: stale button (box destroyed), skip");
        return;
      }
      LOG_DEBUG("picker clicked: setting sel");
      *sel = opt;
      LOG_DEBUG("picker clicked: sel set, queue_draw {} das", das->size());
      for (int di = 0; di < (int)das->size(); ++di) {
        LOG_DEBUG("picker clicked: queue_draw das[{}]={}", di,
                  (void *)(*das)[di]);
        if ((*das)[di])
          (*das)[di]->queue_draw();
        LOG_DEBUG("picker clicked: queue_draw das[{}] done", di);
      }
      LOG_DEBUG("picker clicked: scheduling timeout for on_change");
      Glib::signal_timeout().connect_once(
          [opt, on_change, alive]() {
            if (!*alive) {
              LOG_DEBUG("picker timeout: stale, skip");
              return;
            }
            LOG_DEBUG("picker timeout: on_change opt={}", opt);
            on_change(opt);
            LOG_DEBUG("picker timeout: on_change done");
          },
          1);
      LOG_DEBUG("picker clicked: handler done");
    });

    box->append(*btn);
  }
  return box;
}

// ══════════════════════════════════════════════════════════════════════════════
// S58f — Multi-select broadcast helpers for the Appearance section.
//
// The existing single-object commit path (push_inspector_command) is left
// alone — it handles coalescing, macro recording, and the undo-before
// snapshot logic. For multi-select, we run that path as usual for the
// primary object, then BROADCAST the same write to every other selected
// object and push ONE composite command covering those siblings. Undo/redo
// is atomic: primary's EditObjectCommand + siblings' CompositeCommand are
// pushed in order, and undo pops the composite first (latest pushed first)
// — so end-to-end Ctrl+Z reverts everything.
//
// Display side: each widget checks whether the selection is uniform on its
// attribute. If not, the widget renders an indeterminate state (no toggle
// active, diagonal-stripe swatch, empty hex with "mixed" placeholder). A
// fresh commit on a mixed selection snaps everyone to the new value, so the
// mixed state disappears on the next refresh.
// ══════════════════════════════════════════════════════════════════════════════

namespace {

// Sibling selection excluding the primary. Returns empty when there's only
// one object selected (or no canvas wired). Callers fall back to the
// single-object commit path; these helpers only handle the broadcast delta.
std::vector<SceneNode *> siblings_excluding_primary(Canvas *canvas,
                                                    SceneNode *primary) {
  std::vector<SceneNode *> out;
  if (!canvas || !primary)
    return out;
  const auto &sel = canvas->selection();
  if (sel.size() <= 1)
    return out;
  out.reserve(sel.size() - 1);
  for (SceneNode *n : sel)
    if (n && n != primary && n->type == SceneNode::Type::Path)
      out.push_back(n);
  // Compounds deserve the same fill/stroke propagation — Compound owns its
  // own paint per S58d. Include them.
  for (SceneNode *n : sel)
    if (n && n != primary && n->type == SceneNode::Type::Compound)
      out.push_back(n);
  return out;
}

// Full style-eligible selection INCLUDING primary (used for read-side
// uniformity checks). Returns empty when canvas isn't wired.
std::vector<SceneNode *> style_selection(Canvas *canvas) {
  std::vector<SceneNode *> out;
  if (!canvas)
    return out;
  for (SceneNode *n : canvas->selection()) {
    if (!n)
      continue;
    if (n->type == SceneNode::Type::Path ||
        n->type == SceneNode::Type::Compound)
      out.push_back(n);
  }
  return out;
}

bool fills_equal(const FillStyle &a, const FillStyle &b) {
  if (a.type != b.type)
    return false;
  if (a.type == FillStyle::Type::Solid) {
    // Compare at 8-bit hex granularity. Floating-point r/g/b can differ
    // by small amounts between set-paths (hex entry, Gdk::RGBA dialog,
    // round-trips through formatters) even when they display as the
    // same hex — comparing the rounded 0..255 integers makes "looks
    // the same on screen" the criterion for uniformity.
    auto q = [](double v) {
      int i = (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
      return i;
    };
    return q(a.r) == q(b.r) && q(a.g) == q(b.g) && q(a.b) == q(b.b);
  }
  return true; // None / CurrentColor carry no parameters
}

bool strokes_equal_paint(const StrokeStyle &a, const StrokeStyle &b) {
  return fills_equal(a.paint, b.paint);
}

// Uniformity probes — return true iff every object in `sel` matches the primary
// on the relevant attribute. `is_stroke` distinguishes which FillStyle to read.
bool selection_uniform_paint(const std::vector<SceneNode *> &sel,
                             bool is_stroke) {
  if (sel.size() <= 1)
    return true;
  const FillStyle &first = is_stroke ? sel[0]->stroke.paint : sel[0]->fill;
  for (size_t i = 1; i < sel.size(); ++i) {
    const FillStyle &cur = is_stroke ? sel[i]->stroke.paint : sel[i]->fill;
    if (!fills_equal(first, cur))
      return false;
  }
  return true;
}

bool selection_uniform_cap(const std::vector<SceneNode *> &sel) {
  if (sel.size() <= 1)
    return true;
  LineCap first = sel[0]->stroke.cap;
  for (size_t i = 1; i < sel.size(); ++i)
    if (sel[i]->stroke.cap != first)
      return false;
  return true;
}

bool selection_uniform_join(const std::vector<SceneNode *> &sel) {
  if (sel.size() <= 1)
    return true;
  LineJoin first = sel[0]->stroke.join;
  for (size_t i = 1; i < sel.size(); ++i)
    if (sel[i]->stroke.join != first)
      return false;
  return true;
}

// Diagonal-stripe swatch painter — used when the selection has a mixed paint.
// 4pt stripes on a 45° angle; mid-grey on white. Familiar from Illustrator /
// Affinity Designer.
void paint_mixed_swatch(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
  cr->set_source_rgb(1.0, 1.0, 1.0);
  cr->rectangle(0, 0, w, h);
  cr->fill();
  cr->save();
  cr->set_source_rgb(0.55, 0.55, 0.55);
  cr->set_line_width(2.0);
  // Draw stripes sweeping diagonally — rotate user space +45° then stroke
  // horizontal lines every 5pt. Clip to swatch rect.
  cr->rectangle(0, 0, w, h);
  cr->clip();
  const double span = (double)(w + h) * 1.5;
  const double step = 5.0;
  cr->translate(w * 0.5, h * 0.5);
  cr->rotate(M_PI / 4.0);
  cr->translate(-span * 0.5, -span * 0.5);
  for (double y = 0; y < span; y += step) {
    cr->move_to(0, y);
    cr->line_to(span, y);
  }
  cr->stroke();
  cr->restore();
  cr->set_source_rgba(0, 0, 0, 0.4);
  cr->set_line_width(1.0);
  cr->rectangle(0.5, 0.5, w - 1, h - 1);
  cr->stroke();
}

} // anonymous namespace

// Broadcast the primary's fill+stroke to all sibling selected objects and
// push ONE composite command capturing their before/after state. Call AFTER
// the primary has had its own single-object command pushed; this wraps the
// whole user action in two sequential history entries (primary + siblings)
// which undo in reverse order — Ctrl+Z reverts everything.
//
// `primary_has_fill_change` / `primary_has_stroke_change` tell us which of
// fill/stroke actually changed on the primary (so we broadcast only that
// attribute, preserving the sibling's other attribute). For simple commits
// (swatch, hex, type toggle) only one of these is true; for completeness we
// accept both.
void PropertiesPanel::broadcast_appearance_to_siblings(SceneNode *primary,
                                                       bool fill_changed,
                                                       bool stroke_changed) {
  if (!m_history || !primary)
    return;
  auto sibs = siblings_excluding_primary(m_canvas_widget, primary);
  if (sibs.empty())
    return;

  auto composite = std::make_unique<CompositeCommand>(
      std::string("Broadcast appearance to ") + std::to_string(sibs.size()) +
      " sibling(s)");

  for (SceneNode *s : sibs) {
    FillStyle fb = s->fill;
    StrokeStyle sb = s->stroke;
    FillStyle fa = fb;
    StrokeStyle sa = sb;

    if (fill_changed)
      fa = primary->fill;
    if (stroke_changed)
      sa = primary->stroke;

    // S82 m4f: capture pre-edit swatch ids before the funnel clears
    // them. Post-funnel ids are read after mutate_appearance — for
    // siblings receiving a flat fill/stroke broadcast, the funnel
    // clears any existing swatch binding (override-unbinds), so the
    // after ids are empty by construction. We still read them
    // post-mutation rather than hard-coding "" so the snapshot is
    // robust against any future funnel change that re-populates ids.
    std::string fsib = s->fill_swatch_id;
    std::string ssib = s->stroke_swatch_id;
    // S92 m3: same shape for bound_style — funnel clears it too.
    std::string bsb = s->bound_style;

    // Apply immediately (execute() will restore on redo).
    // Broadcast is a user-driven appearance write to each sibling —
    // any existing Style binding breaks per addendum (S74 m2).
    Curvz::style::mutate_appearance(*s, [&](SceneNode &n) {
      n.fill = fa;
      n.stroke = sa;
    });

    std::string fsia = s->fill_swatch_id;
    std::string ssia = s->stroke_swatch_id;
    std::string bsa = s->bound_style;

    // s168 m1 DIAG — STRIP after triage
    LOG_INFO("[IIDDIAG] EditAppearance::push (sibling)  capturing iid='{}' "
             "obj_name='{}' obj_type={}",
             s->internal_id, s->name, (int)s->type);
    composite->add(std::make_unique<EditAppearanceCommand>(
        m_project, s->internal_id,
        std::move(fb), std::move(sb), std::move(fa), std::move(sa),
        std::move(fsib), std::move(ssib), std::move(fsia), std::move(ssia),
        std::move(bsb), std::move(bsa), "Sibling appearance sync"));
  }

  m_history->push(std::move(composite));
}

// ── Object ▸ Styling unified section (was "Appearance" pre-s148 m2) ────
// A 2-state toggle selects which target (Fill or Stroke) the colour/
// style widgets below apply to. Switching target refreshes the widgets
// in-place.
//
// s148 m2 rename: header was "Appearance" through s147. Renamed to
// "Styling" so the App-tier "Appearance" section (Dark/Light, GNOME
// convention) could claim that label without collision. "Styling"
// is design-domain vocabulary — fill/stroke/shadow are surface
// Styling applied to the object — and slots cleanly alongside
// "Selection," "Node," "Blend," etc. in the Object group.
//
// Internal widget IDs (ins_fs_*) intentionally NOT renamed: "fs" for
// fill+stroke is still an accurate internal abbreviation, and a
// widget-ID rename across the whole section is busywork that buys
// nothing. Only the user-facing header label changes.
void PropertiesPanel::add_fill_stroke_section(SceneNode *obj,
                                              Gtk::Box *parent) {
  auto *outer_body = add_collapsible("Styling", false, parent);
  curvz::utils::set_name(outer_body, "ins_fs", "inspector_fill_stroke_body");
  if (!obj)
    return;

  // ── Binding indicator block (S83 m4h) ─────────────────────────────────────
  //
  // Up to three rows at the top of the Appearance section: style binding,
  // fill swatch binding, stroke swatch binding. Each row is conditional on
  // its own non-empty state — if nothing is bound, the row is omitted.
  // When all three are empty, no rows append and the block is invisible
  // (zero added height). The Appearance widgets below convey "unbound"
  // implicitly; we don't need a placeholder row for the empty case.
  //
  // This block replaces the old build_style_section (S80 m4b → deleted in
  // m4h). Rationale: style and swatch bindings are siblings — both drive
  // the cache that Appearance edits, both break on direct override, both
  // deserve unbind affordances. Putting all three together is the single
  // place to glance for binding state.
  //
  // Multi-select / mixed-id treatment: same as the old style section. Any
  // node in style_selection that has a binding contributes; if every bound
  // node points at the same id, show the name; if they point at different
  // ids, show "<multiple>". Mixed = (some bound + some unbound) is also
  // "<multiple>" — the row appears but the label conveys the inconsistency.
  //
  // m_build_gen guards: lambdas snapshot gen at build time and bail when
  // the panel rebuilds. Same pattern as every other inspector lambda.
  uint32_t gen = m_build_gen;
  std::vector<SceneNode *> bind_sel = style_selection(m_canvas_widget);
  if (bind_sel.empty() && (obj->type == SceneNode::Type::Path ||
                           obj->type == SceneNode::Type::Compound)) {
    bind_sel.push_back(obj);
  }

  // Helper: render a single binding-indicator row (key label + value
  // label + Unbind button). Returns the row Box for caller to append.
  // The on_unbind callback fires when the button is clicked, after the
  // gen + m_loading + m_history guards.
  //
  // s121: out_btn lets the caller tag the Unbind button via
  // curvz::utils::set_name at the call site (the harvester sees only
  // literal-string args, so tagging inside the helper won't be picked
  // up). Default nullptr keeps the helper compatible with any
  // hypothetical future caller that doesn't need the pointer.
  auto make_binding_row =
      [&](const std::string &key, const std::string &display,
          const std::string &tooltip, const std::string &btn_label,
          std::function<void()> on_unbind,
          Gtk::Button **out_btn = nullptr) -> Gtk::Box * {
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(6);
    row->set_margin_start(8);
    row->set_margin_end(6);
    row->set_margin_top(4);
    row->set_margin_bottom(2);

    auto *key_lbl = Gtk::make_managed<Gtk::Label>(key);
    key_lbl->add_css_class("prop-lbl");
    key_lbl->set_xalign(0.0f);
    row->append(*key_lbl);

    auto *val_lbl = Gtk::make_managed<Gtk::Label>(display);
    val_lbl->set_hexpand(true);
    val_lbl->set_xalign(0.0f);
    val_lbl->set_ellipsize(Pango::EllipsizeMode::END);
    val_lbl->add_css_class("dim-label");
    if (!tooltip.empty())
      val_lbl->set_tooltip_text(tooltip);
    row->append(*val_lbl);

    auto *btn = Gtk::make_managed<Gtk::Button>(btn_label);
    btn->add_css_class("prop-toggle");
    btn->signal_clicked().connect([this, gen, cb = std::move(on_unbind)]() {
      if (gen != m_build_gen || m_loading)
        return;
      if (!m_history)
        return;
      cb();
    });
    row->append(*btn);
    if (out_btn)
      *out_btn = btn;
    return row;
  };

  // ── Style binding row ────────────────────────────────────────────────────
  // Lifted verbatim from build_style_section's count + mixed-detection +
  // name-lookup loop. The row is omitted entirely when no node in the
  // selection has a bound_style — mirrors the "(a) Bound: —, button hidden"
  // case from the old code, but expressed as no-row-at-all rather than a
  // dimmed em-dash. The Appearance widgets show the cache; that's enough.
  {
    bool any_bound = false;
    std::string first_id;
    bool mixed = false;
    for (SceneNode *n : bind_sel) {
      if (!n)
        continue;
      if (n->bound_style.empty()) {
        if (any_bound)
          mixed = true;
        continue;
      }
      if (!any_bound) {
        any_bound = true;
        first_id = n->bound_style;
      } else if (n->bound_style != first_id)
        mixed = true;
    }

    if (any_bound) {
      std::string display;
      std::string tooltip;
      if (mixed) {
        display = "<multiple>";
        tooltip = "Selected nodes are bound to multiple styles";
      } else if (m_project) {
        if (const auto *st = m_project->styles.find_style(first_id)) {
          display = st->header.name.empty() ? first_id : st->header.name;
        } else {
          // Dangling — shouldn't happen, but surface it rather than drop.
          display = first_id + "?";
        }
        tooltip = std::string("Bound to style id: ") + first_id;
      } else {
        display = first_id;
      }

      // Capture selection at build time. Lambda reads bind_sel via copy
      // so subsequent rebuilds (which mutate the panel's selection state)
      // don't tear out from under us.
      std::vector<SceneNode *> sel_cap = bind_sel;
      auto on_unbind = [this, sel_cap]() {
        // Mutate-then-push pattern (matches the rest of the inspector).
        // Skip already-unbound nodes in the mixed case — keeps the
        // command minimal per the m4b convention.
        std::vector<UnbindStyleCommand::TargetSnap> snaps;
        snaps.reserve(sel_cap.size());
        for (SceneNode *n : sel_cap) {
          if (!n)
            continue;
          if (n->bound_style.empty())
            continue;
          UnbindStyleCommand::TargetSnap ts;
          ts.obj = n;
          ts.bound_style_before = n->bound_style;
          ts.fill_before = n->fill;
          ts.fill_swatch_id_before = n->fill_swatch_id;
          ts.stroke_before = n->stroke;
          ts.stroke_swatch_id_before = n->stroke_swatch_id;
          snaps.push_back(std::move(ts));
          n->bound_style.clear();
        }
        if (snaps.empty())
          return;
        const bool plural = snaps.size() > 1;
        m_history->push(std::make_unique<UnbindStyleCommand>(
            std::move(snaps), plural ? std::string("Unbind style (multiple)")
                                     : std::string("Unbind style")));
        if (m_canvas_widget)
          m_canvas_widget->queue_draw();
        if (m_canvas)
          refresh(m_canvas, m_current);
        emit_prop_changed();
      };

      Gtk::Button *unbind_btn = nullptr;
      outer_body->append(*make_binding_row("Style", display, tooltip, "Unbind",
                                           std::move(on_unbind), &unbind_btn));
      curvz::utils::set_name(unbind_btn, "ins_fs_bind_style_ub",
                             "inspector_fill_stroke_bind_style_unbind_btn");
    }
  }

  // ── Per-slot swatch unbind helper (S83 m4h v2) ───────────────────────────
  // Used by the inline × button rendered inside build_paint_row's colour
  // row when a swatch binding is active for the slot. Pre-spec, this
  // logic also drove standalone swatch rows above the Appearance widgets;
  // v2 reverted to the inline-on-colour-row layout per design (single
  // glance shows hex + bound name + unbind together), so the helper is
  // now invoked from build_paint_row only.
  //
  // Captures bind_sel by COPY (passed as sel arg below) — the helper
  // must outlive the snapshot point because the × callback fires after
  // build_paint_row's lambda has returned. Slot is passed per-invocation
  // so the same helper handles fill and stroke.
  auto unbind_swatch_for_slot = [this](color::PaintSlot slot,
                                       std::vector<SceneNode *> sel) {
    // UnbindSwatchCommand shape: per-target snap with full appearance
    // cache + the slot's pre-unbind id. Cache preserved on execute
    // (break-on-override v1 — moment-of-unbind appearance is what
    // the user keeps), restored on undo. Mirrors the m4f harness
    // shape from MainWindow.cpp pre-cleanup.
    if (!m_history)
      return;
    std::vector<UnbindSwatchCommand::TargetSnap> snaps;
    snaps.reserve(sel.size());
    for (SceneNode *n : sel) {
      if (!n)
        continue;
      const std::string &id = (slot == color::PaintSlot::Fill)
                                  ? n->fill_swatch_id
                                  : n->stroke_swatch_id;
      if (id.empty())
        continue;
      UnbindSwatchCommand::TargetSnap ts;
      ts.obj = n;
      ts.swatch_id_before = id;
      ts.fill_before = n->fill;
      ts.stroke_before = n->stroke;
      snaps.push_back(std::move(ts));
      // Mutate now (cache untouched per v1 break-on-override).
      if (slot == color::PaintSlot::Fill)
        n->fill_swatch_id.clear();
      else
        n->stroke_swatch_id.clear();
    }
    if (snaps.empty())
      return;
    const bool plural = snaps.size() > 1;
    m_history->push(std::make_unique<UnbindSwatchCommand>(
        slot, std::move(snaps),
        plural ? std::string("Unbind swatch (multiple)")
               : std::string("Unbind swatch")));
    if (m_canvas_widget)
      m_canvas_widget->queue_draw();
    if (m_canvas)
      refresh(m_canvas, m_current);
    emit_prop_changed();
  };

  // ── Helper: build one fill/stroke row ────────────────────────────────────
  // Produces:
  //   Label:  [Solid] [None] [currentColor]   ← ToggleButton radio group
  //           [swatch] [#hex entry]            ← color row (Solid only)
  //
  // s121: out_editor lets the caller tag the PaintEditor instance via
  // curvz::utils::set_name at the call site. The lambda runs twice
  // (Fill, Stroke) so internal set_name calls would emit duplicate
  // abbrevs from the harvester's perspective; tagging at the call site
  // is the only way to give Fill and Stroke distinct names.
  auto build_paint_row = [&](const std::string &label_str, FillStyle &paint,
                             bool is_stroke,
                             PaintEditor **out_editor = nullptr) {
    // Section label
    auto *sec_lbl = Gtk::make_managed<Gtk::Label>(label_str + ":");
    sec_lbl->add_css_class("prop-lbl");
    sec_lbl->set_xalign(0.0f);
    sec_lbl->set_margin_top(6);
    outer_body->append(*sec_lbl);

    // ── PaintEditor (S84 m4i) ────────────────────────────────────────────
    //
    // The type-toggle row + colour row + refresh-helper + swatch-click +
    // hex-commit + three type-toggle handlers that used to live inline
    // here are now packaged in PaintEditor. The widget is purely a view:
    // it pushes signals up to the host on user action, and the host is
    // responsible for the actual mutation, undo command, sibling
    // broadcast, and re-rendering via set_render_state().
    //
    // The shared ColorPickerPopover member is passed by reference — the
    // widget never owns one, so we don't end up with a popover instance
    // per fill/stroke editor.
    //
    // Re-rendering after a host-side mutation: each signal handler calls
    // compute_render_state() and pushes the result back. The same
    // function runs on initial show. This replaces the pre-extraction
    // refresh_color_row lambda + its multiple call sites.
    //
    // No m_build_gen guard inside the editor's own signal handlers:
    // do_clear() removes outer_body's children before rebuild, which
    // destroys the PaintEditor widget; signal connections die with it.
    // The unbind handler still snapshots `gen` because unbind_swatch_for_
    // slot is a panel-level lambda capturing m_history; if that fired
    // post-rebuild it could surprise the new tree.

    auto compute_render_state = [this, obj, &paint,
                                 is_stroke]() -> PaintEditor::RenderState {
      PaintEditor::RenderState s;
      s.paint = paint;
      s.has_alpha = false; // object fill/stroke ignores alpha at picker level

      auto sel_now = style_selection(m_canvas_widget);
      s.uniform =
          (sel_now.size() <= 1) || selection_uniform_paint(sel_now, is_stroke);

      // ── Binding walk (lifted from refresh_color_row pre-extraction) ─
      bool any_bound = false;
      std::string first_id;
      bool mixed_bind = false;
      auto get_id = [is_stroke](const SceneNode *n) -> const std::string & {
        return is_stroke ? n->stroke_swatch_id : n->fill_swatch_id;
      };
      for (SceneNode *n : sel_now) {
        if (!n)
          continue;
        const std::string &id = get_id(n);
        if (id.empty()) {
          if (any_bound)
            mixed_bind = true;
          continue;
        }
        if (!any_bound) {
          any_bound = true;
          first_id = id;
        } else if (id != first_id)
          mixed_bind = true;
      }
      // Edge case: sel_now empty (panel built off the obj alone, canvas
      // not wired). Fall back to obj's own slot id.
      if (!any_bound && sel_now.empty()) {
        const std::string &id =
            is_stroke ? obj->stroke_swatch_id : obj->fill_swatch_id;
        if (!id.empty()) {
          any_bound = true;
          first_id = id;
        }
      }

      s.bound = any_bound;
      s.bound_mixed = mixed_bind;
      s.bound_tooltip = is_stroke
                            ? "Unbind stroke from swatch (keep current colour)"
                            : "Unbind fill from swatch (keep current colour)";

      if (any_bound && !mixed_bind) {
        // Resolve display name for the single bound id.
        if (m_project) {
          if (const auto *sw = m_project->swatches.find_swatch(first_id)) {
            const std::string &nm = color::swatch_header(*sw).name;
            s.bound_display_name =
                nm.empty() ? color::region_name(paint.r, paint.g, paint.b) : nm;
          } else {
            // Dangling — defensive; m4h v4's delete cascade prevents this.
            s.bound_display_name = first_id + "?";
          }
        } else {
          s.bound_display_name = first_id;
        }
      }

      // Region-name fallback used by the widget when bound is false and
      // paint.type is Solid. Computed unconditionally; the widget gates
      // visibility on paint type itself.
      s.unbound_display_name = color::region_name(paint.r, paint.g, paint.b);

      // ── S85 swatch-pick fields ──────────────────────────────────────
      //
      // Library — wired iff the project is. Hosts without a project
      // (degenerate panel build paths) get nullptr and a greyed-out
      // Swatch toggle. The library reference is non-owning; the
      // panel guarantees it outlives the widget.
      s.library = m_project ? &m_project->swatches : nullptr;

      // Active binding id — the single bound id when uniform, empty
      // when mixed (PaintEditor uses bound_mixed for that). When
      // uniform-bound, surface the id so the picker can highlight
      // the active chip and pre-select its palette.
      if (any_bound && !mixed_bind) {
        s.binding_id = first_id;
      }

      // is_swatch_active — does the editor's toggle row sit on Swatch?
      // For the inspector, the answer is "yes iff this slot is bound,
      // and the binding type carries through to here." The inspector
      // doesn't preserve a per-session "user clicked Swatch toggle but
      // hasn't picked yet" state — it would need to live on the
      // SceneNode for that, which would be wrong. So: bound = swatch-
      // active. PaintEditor's m_btn_swatch.signal_toggled() handler
      // toggles the picker locally without bothering the host until
      // the user picks; once they pick, our signal_swatch_picked
      // handler routes through Canvas::apply_swatch_to_selection which
      // populates fill_swatch_id, and the next compute_render_state
      // sees any_bound=true → is_swatch_active=true.
      s.is_swatch_active = any_bound && !mixed_bind;

      // S91: inspector opts into gradient editing — Edit button shows
      // when paint is a gradient, and the type-toggle's Gradient
      // button is sensitive. The actual editor (GradientDialog) opens
      // via signal_request_gradient_edit, routed through MainWindow.
      s.gradients_enabled = true;

      return s;
    };

    auto *editor = Gtk::make_managed<PaintEditor>(m_color_popover);
    if (out_editor)
      *out_editor = editor;
    outer_body->append(*editor);
    editor->set_render_state(compute_render_state());

    // Type change — radio-toggle click. The S58f mixed gate (cannot
    // early-exit on "primary already this type" when selection is
    // mixed) is preserved: compute_render_state already drives the
    // editor's visible toggle state, so even a "no-op for primary"
    // click still has to snap siblings.
    //
    // S91: when transitioning into a gradient type with empty stops,
    // we seed a default 2-stop linear gradient (black → white,
    // horizontal across the bbox) so the user has something to render
    // and edit. This mirrors GradientDialog::show's same-shaped seed
    // for a non-gradient initial — the dialog's seed only fires
    // through the dialog path; the toggle path needs its own.
    editor->signal_type_changed().connect(
        [this, obj, is_stroke, &paint, editor,
         compute_render_state](FillStyle::Type new_type) {
          if (m_loading)
            return;
          auto sel_now = style_selection(m_canvas_widget);
          bool mixed = (sel_now.size() > 1) &&
                       !selection_uniform_paint(sel_now, is_stroke);
          if (!mixed && paint.type == new_type)
            return;
          // S92 m3: route the paint mutation through mutate_appearance
          // so bound_style + swatch_id are cleared by the funnel
          // (break-on-override). Pre-S92 the handler manually cleared
          // *_swatch_id but not bound_style, so a Style-bound object
          // type-toggled to (e.g.) Gradient would keep its bound_style
          // set — and the next Style edit / live_recolour_walk would
          // overwrite the user's gradient with the Style's solid
          // colour. Same bug existed for Solid→None and Solid→CC.
          // Funnelling brings this in line with signal_color_changed
          // (also funnelled below) and the gradient apply_cb.
          //
          // The funnel also clears *_swatch_id, so the old explicit
          // clear is now redundant — removed.
          Curvz::style::mutate_appearance(
              *obj, [&paint, new_type](SceneNode & /*n*/) {
                paint.type = new_type;
                // Seed default stops + geometry when promoting into a
                // gradient type from a non-gradient state. Existing stops
                // are preserved on Linear↔Radial swaps within the gradient
                // family (which this handler doesn't actually receive — the
                // editor's Gradient toggle only emits LinearGradient — but
                // defensive against any future host that emits Radial here).
                if ((new_type == FillStyle::Type::LinearGradient ||
                     new_type == FillStyle::Type::RadialGradient) &&
                    paint.stops.empty()) {
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
                  paint.stops = {s0, s1};
                  // Geometry: linear-default endpoints, full-bbox spread.
                  // Matches GradientDialog::show. RadialGradient promotion
                  // would override these to centre-and-radius defaults; the
                  // dialog handles that on Linear→Radial toggle in its own
                  // sync path.
                  paint.g_x1 = 0.0;
                  paint.g_y1 = 0.5;
                  paint.g_x2 = 1.0;
                  paint.g_y2 = 0.5;
                  paint.g_r = 0.5;
                }
              });
          // S58L: broadcast BEFORE refresh so uniformity check sees
          // siblings in sync.
          push_inspector_command(obj);
          broadcast_appearance_to_siblings(obj, !is_stroke, is_stroke);
          editor->set_render_state(compute_render_state());
          emit_prop_changed();
        });

    // Colour change — fires from picker drag ticks AND hex entry commit.
    // Both paths are treated identically: snap to Solid, write the new
    // colour, break the swatch binding (break-on-override v1), broadcast
    // to siblings, push a coalesced command, refresh.
    editor->signal_color_changed().connect(
        [this, obj, is_stroke, &paint, editor,
         compute_render_state](double r, double g, double b) {
          if (m_loading)
            return;
          // S92 m3: route through mutate_appearance so bound_style is
          // cleared alongside the swatch_id. Pre-S92 the manual
          // *_swatch_id.clear() left bound_style intact, so a
          // Style-bound object's user-typed hex would be silently
          // overwritten on the next Style edit. Funnelling makes
          // colour edits and gradient edits behave consistently.
          Curvz::style::mutate_appearance(*obj,
                                          [&paint, r, g, b](SceneNode & /*n*/) {
                                            paint.type = FillStyle::Type::Solid;
                                            paint.r = r;
                                            paint.g = g;
                                            paint.b = b;
                                          });
          // S58L: broadcast before refresh.
          push_inspector_command(obj);
          broadcast_appearance_to_siblings(obj, !is_stroke, is_stroke);
          editor->set_render_state(compute_render_state());
          emit_prop_changed();
        });

    // × click — route to the per-slot unbind helper defined at the top
    // of add_fill_stroke_section. Selection captured at click time, gen
    // guarded for the same reason as other panel-level lambdas (the
    // helper reaches up to m_history).
    {
      const color::PaintSlot slot =
          is_stroke ? color::PaintSlot::Stroke : color::PaintSlot::Fill;
      editor->signal_unbind_clicked().connect(
          [this, gen, slot, unbind_swatch_for_slot]() {
            if (gen != m_build_gen || m_loading)
              return;
            std::vector<SceneNode *> sel = style_selection(m_canvas_widget);
            unbind_swatch_for_slot(slot, std::move(sel));
          });
    }

    // S85: swatch picked from the embedded picker. Route through
    // Canvas::apply_swatch_to_selection — same path SwatchesPanel uses
    // for chip clicks. That builds a BindSwatchCommand atomic across
    // the selection, fires signal_paint_changed, fires signal_doc_
    // changed, and the inspector's existing refresh path picks up the
    // new state. We then push a fresh RenderState here so the editor
    // re-renders the chip ring + binding annotation without waiting
    // for the document-changed roundtrip.
    //
    // m_loading guard: same as every other inspector handler. gen
    // guard: apply_swatch_to_selection touches m_history via Canvas,
    // and a stale lambda firing post-rebuild would push a command
    // against a different selection.
    {
      const color::PaintSlot slot =
          is_stroke ? color::PaintSlot::Stroke : color::PaintSlot::Fill;
      editor->signal_swatch_picked().connect(
          [this, gen, slot, editor, compute_render_state](color::SwatchId id) {
            if (gen != m_build_gen || m_loading)
              return;
            if (!m_canvas_widget || id.empty())
              return;
            // Canvas's path uses the canvas's own selection; the
            // inspector and canvas selections are in sync (the
            // inspector follows canvas selection-changed), so this
            // is safe.
            m_canvas_widget->apply_swatch_to_selection(id, slot);
            editor->set_render_state(compute_render_state());
          });
    }

    // signal_picker_closed: not wired here. The inspector path doesn't
    // need create-on-Esc-remove (it edits an existing object's paint;
    // there's nothing to remove). SwatchesPanel will be the first
    // consumer of that signal when it's migrated in a later slice.

    // ── S91 gradient edit request ────────────────────────────────────
    //
    // Editor → Edit gradient button → bubble up to MainWindow with a
    // callback that performs the inspector-style commit. MainWindow
    // owns the GradientDialog instance; this side just packages "what
    // to do on Apply" as a callback.
    //
    // The callback closes over the same gen + obj + is_stroke + editor
    // + compute_render_state set as the colour-change handler — the
    // commit shape is identical: write into the primary's paint slot,
    // funnel through mutate_appearance for the swatch-clear side
    // effect, push EditAppearanceCommand, broadcast to siblings,
    // refresh.
    //
    // gen + m_loading guards mirror the swatch_picked handler — the
    // dialog's Apply could fire after a panel rebuild (the dialog is
    // modeless to the inspector; user could trigger a doc-level event
    // while it's open).
    editor->signal_gradient_edit_requested().connect(
        [this, gen, obj, is_stroke, editor,
         compute_render_state](FillStyle current) {
          if (gen != m_build_gen || m_loading)
            return;
          // Build the apply callback.  m_history is panel-level, the
          // helpers (push_inspector_command, broadcast) are panel-level,
          // and obj is captured by value (raw pointer; see the same
          // pattern in signal_color_changed). Same lifetime caveats.
          auto apply_cb = [this, gen, obj, is_stroke, editor,
                           compute_render_state](FillStyle edited) {
            if (gen != m_build_gen || m_loading)
              return;
            // Funnel through mutate_appearance so swatch-binding clears
            // happen consistently with the rest of the inspector (the
            // gradient is a user-driven appearance write — break-on-
            // override applies the same as a colour edit).
            Curvz::style::mutate_appearance(*obj,
                                            [&edited, is_stroke](SceneNode &n) {
                                              if (is_stroke)
                                                n.stroke.paint = edited;
                                              else
                                                n.fill = edited;
                                            });
            push_inspector_command(obj);
            broadcast_appearance_to_siblings(obj, !is_stroke, is_stroke);
            editor->set_render_state(compute_render_state());
            emit_prop_changed();
            if (m_canvas_widget)
              m_canvas_widget->queue_draw();
          };
          m_sig_request_gradient_edit.emit(current, apply_cb);
        });

    // Stroke-specific: width + cap + join
    if (is_stroke) {
      // Width row
      auto *w_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      w_row->set_spacing(6);
      w_row->set_margin_top(4);
      auto *wl = Gtk::make_managed<Gtk::Label>("Thickness:");
      wl->add_css_class("prop-lbl");
      wl->set_xalign(0.0f);
      w_row->append(*wl);
      auto *w_spin =
          Gtk::make_managed<CurvzSpinButton>(SpinType::Distance, m_canvas);
      curvz::utils::set_name(w_spin, "ins_fs_strk_w",
                             "inspector_fill_stroke_width_spn");
      w_spin->set_width_chars(6);
      w_spin->add_css_class("tb-well-spin");
      w_spin->set_tooltip_text("Stroke thickness in current canvas units");
      w_spin->set_internal_value(obj->stroke.width);
      block_scroll(w_spin, [this] { emit_canvas_focus(); });
      w_spin->signal_internal_changed().connect([this, obj](double v) {
        if (m_loading)
          return;
        Curvz::style::mutate_appearance(
            *obj, [v](SceneNode &n) { n.stroke.width = v; });
        // Broadcast just the width — siblings keep their own paint/cap/
        // join but pick up the new width.
        auto sibs = siblings_excluding_primary(m_canvas_widget, obj);
        push_inspector_command(obj);
        if (!sibs.empty()) {
          auto composite = std::make_unique<CompositeCommand>(
              std::string("Broadcast stroke width to ") +
              std::to_string(sibs.size()) + " sibling(s)");
          for (SceneNode *s : sibs) {
            FillStyle fb = s->fill;
            FillStyle fa = fb;
            StrokeStyle sb = s->stroke;
            StrokeStyle sa = sb;
            sa.width = v;
            // S82 m4f: snap swatch ids around the funnel call. A
            // stroke-width edit is technically not an override on
            // the colour, but the funnel treats every direct
            // mutate_appearance call as an override and clears both
            // bindings — that's the simpler invariant. If we ever
            // want stroke-width edits to preserve bindings, the
            // funnel needs an attribute-aware variant; under v1
            // break-on-override, capturing swatch ids is the
            // correct behaviour for undo.
            // S92 m3: same shape for bound_style.
            std::string fsib = s->fill_swatch_id;
            std::string ssib = s->stroke_swatch_id;
            std::string bsb = s->bound_style;
            Curvz::style::mutate_appearance(
                *s, [v](SceneNode &n) { n.stroke.width = v; });
            std::string fsia = s->fill_swatch_id;
            std::string ssia = s->stroke_swatch_id;
            std::string bsa = s->bound_style;
            // s168 m1 DIAG — STRIP after triage
            LOG_INFO("[IIDDIAG] EditAppearance::push (sib stroke-width) "
                     "iid='{}' obj_name='{}' obj_type={}",
                     s->internal_id, s->name, (int)s->type);
            composite->add(std::make_unique<EditAppearanceCommand>(
                m_project, s->internal_id,
                std::move(fb), std::move(sb), std::move(fa), std::move(sa),
                std::move(fsib), std::move(ssib), std::move(fsia),
                std::move(ssia), std::move(bsb), std::move(bsa),
                "Sibling stroke width sync"));
          }
          if (m_history)
            m_history->push(std::move(composite));
        }
        emit_prop_changed();
      });
      w_row->append(*w_spin);
      w_row->append(*w_spin->get_unit_label());
      outer_body->append(*w_row);

      // Cap row
      auto *cap_lbl = Gtk::make_managed<Gtk::Label>("Cap:");
      cap_lbl->add_css_class("prop-lbl");
      cap_lbl->set_xalign(0.0f);
      cap_lbl->set_margin_top(4);
      outer_body->append(*cap_lbl);

      auto *cap_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      cap_row->set_spacing(4);
      cap_row->set_homogeneous(false);
      cap_row->set_halign(Gtk::Align::START);

      auto *cap_butt = Gtk::make_managed<Gtk::ToggleButton>();
      auto *cap_round = Gtk::make_managed<Gtk::ToggleButton>();
      auto *cap_square = Gtk::make_managed<Gtk::ToggleButton>();
      curvz::utils::set_name(cap_butt, "ins_fs_strk_cb",
                             "inspector_fill_stroke_cap_butt_btn");
      curvz::utils::set_name(cap_round, "ins_fs_strk_cr",
                             "inspector_fill_stroke_cap_round_btn");
      curvz::utils::set_name(cap_square, "ins_fs_strk_cs",
                             "inspector_fill_stroke_cap_square_btn");
      cap_round->set_group(*cap_butt);
      cap_square->set_group(*cap_butt);
      cap_butt->set_icon_name("curvz-cap-butt-symbolic");
      cap_round->set_icon_name("curvz-cap-round-symbolic");
      cap_square->set_icon_name("curvz-cap-square-symbolic");
      cap_butt->set_tooltip_text("Butt cap");
      cap_round->set_tooltip_text("Round cap");
      cap_square->set_tooltip_text("Square cap");
      // S117 m1: route through the .prop-type-btn class so cap toggles
      // pick up the same motif-aware base/hover/checked styling as
      // PaintEditor's type-row buttons. Without this they fell through
      // to GTK's system theme and rendered dark-on-dark in Light motif.
      cap_butt->add_css_class("prop-type-btn");
      cap_round->add_css_class("prop-type-btn");
      cap_square->add_css_class("prop-type-btn");
      // Initial state. S58f: mixed cap across selection → all three off.
      auto sel_cap = style_selection(m_canvas_widget);
      bool cap_uniform =
          (sel_cap.size() <= 1) || selection_uniform_cap(sel_cap);
      if (cap_uniform) {
        cap_butt->set_active(obj->stroke.cap == LineCap::Butt);
        cap_round->set_active(obj->stroke.cap == LineCap::Round);
        cap_square->set_active(obj->stroke.cap == LineCap::Square);
      } else {
        cap_butt->set_active(false);
        cap_round->set_active(false);
        cap_square->set_active(false);
      }
      cap_row->append(*cap_butt);
      cap_row->append(*cap_round);
      cap_row->append(*cap_square);
      outer_body->append(*cap_row);

      // Helper: broadcast a cap change to all sibling selected objects.
      auto broadcast_cap = [this, obj](LineCap c) {
        auto sibs = siblings_excluding_primary(m_canvas_widget, obj);
        if (sibs.empty() || !m_history)
          return;
        auto composite = std::make_unique<CompositeCommand>(
            std::string("Broadcast cap to ") + std::to_string(sibs.size()) +
            " sibling(s)");
        for (SceneNode *s : sibs) {
          FillStyle fb = s->fill;
          FillStyle fa = fb;
          StrokeStyle sb = s->stroke;
          StrokeStyle sa = sb;
          sa.cap = c;
          // S82 m4f: snap swatch ids around the funnel call (see
          // stroke-width broadcast above for rationale).
          // S92 m3: same shape for bound_style.
          std::string fsib = s->fill_swatch_id;
          std::string ssib = s->stroke_swatch_id;
          std::string bsb = s->bound_style;
          Curvz::style::mutate_appearance(
              *s, [c](SceneNode &n) { n.stroke.cap = c; });
          std::string fsia = s->fill_swatch_id;
          std::string ssia = s->stroke_swatch_id;
          std::string bsa = s->bound_style;
          // s168 m1 DIAG — STRIP after triage
          LOG_INFO("[IIDDIAG] EditAppearance::push (sib cap) "
                   "iid='{}' obj_name='{}' obj_type={}",
                   s->internal_id, s->name, (int)s->type);
          composite->add(std::make_unique<EditAppearanceCommand>(
              m_project, s->internal_id,
              std::move(fb), std::move(sb), std::move(fa), std::move(sa),
              std::move(fsib), std::move(ssib), std::move(fsia),
              std::move(ssia), std::move(bsb), std::move(bsa),
              "Sibling cap sync"));
        }
        m_history->push(std::move(composite));
      };

      cap_butt->signal_toggled().connect(
          [this, obj, cap_butt, broadcast_cap]() {
            if (m_loading || !cap_butt->get_active())
              return;
            Curvz::style::mutate_appearance(
                *obj, [](SceneNode &n) { n.stroke.cap = LineCap::Butt; });
            push_inspector_command(obj);
            broadcast_cap(LineCap::Butt);
            emit_prop_changed();
          });
      cap_round->signal_toggled().connect(
          [this, obj, cap_round, broadcast_cap]() {
            if (m_loading || !cap_round->get_active())
              return;
            Curvz::style::mutate_appearance(
                *obj, [](SceneNode &n) { n.stroke.cap = LineCap::Round; });
            push_inspector_command(obj);
            broadcast_cap(LineCap::Round);
            emit_prop_changed();
          });
      cap_square->signal_toggled().connect(
          [this, obj, cap_square, broadcast_cap]() {
            if (m_loading || !cap_square->get_active())
              return;
            Curvz::style::mutate_appearance(
                *obj, [](SceneNode &n) { n.stroke.cap = LineCap::Square; });
            push_inspector_command(obj);
            broadcast_cap(LineCap::Square);
            emit_prop_changed();
          });

      // Join row
      auto *join_lbl = Gtk::make_managed<Gtk::Label>("Join:");
      join_lbl->add_css_class("prop-lbl");
      join_lbl->set_xalign(0.0f);
      join_lbl->set_margin_top(4);
      outer_body->append(*join_lbl);

      auto *join_row =
          Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      join_row->set_spacing(4);
      join_row->set_homogeneous(false);
      join_row->set_halign(Gtk::Align::START);

      auto *join_miter = Gtk::make_managed<Gtk::ToggleButton>();
      auto *join_round = Gtk::make_managed<Gtk::ToggleButton>();
      auto *join_bevel = Gtk::make_managed<Gtk::ToggleButton>();
      curvz::utils::set_name(join_miter, "ins_fs_strk_jm",
                             "inspector_fill_stroke_join_miter_btn");
      curvz::utils::set_name(join_round, "ins_fs_strk_jr",
                             "inspector_fill_stroke_join_round_btn");
      curvz::utils::set_name(join_bevel, "ins_fs_strk_jb",
                             "inspector_fill_stroke_join_bevel_btn");
      join_round->set_group(*join_miter);
      join_bevel->set_group(*join_miter);
      join_miter->set_icon_name("curvz-join-miter-symbolic");
      join_round->set_icon_name("curvz-join-round-symbolic");
      join_bevel->set_icon_name("curvz-join-bevel-symbolic");
      join_miter->set_tooltip_text("Miter join");
      join_round->set_tooltip_text("Round join");
      join_bevel->set_tooltip_text("Bevel join");
      // S117 m1: motif-aware styling — see cap_butt block above.
      join_miter->add_css_class("prop-type-btn");
      join_round->add_css_class("prop-type-btn");
      join_bevel->add_css_class("prop-type-btn");
      // Initial state. S58f: mixed join across selection → all three off.
      auto sel_join = style_selection(m_canvas_widget);
      bool join_uniform =
          (sel_join.size() <= 1) || selection_uniform_join(sel_join);
      if (join_uniform) {
        join_miter->set_active(obj->stroke.join == LineJoin::Miter);
        join_round->set_active(obj->stroke.join == LineJoin::Round);
        join_bevel->set_active(obj->stroke.join == LineJoin::Bevel);
      } else {
        join_miter->set_active(false);
        join_round->set_active(false);
        join_bevel->set_active(false);
      }
      join_row->append(*join_miter);
      join_row->append(*join_round);
      join_row->append(*join_bevel);
      outer_body->append(*join_row);

      auto broadcast_join = [this, obj](LineJoin j) {
        auto sibs = siblings_excluding_primary(m_canvas_widget, obj);
        if (sibs.empty() || !m_history)
          return;
        auto composite = std::make_unique<CompositeCommand>(
            std::string("Broadcast join to ") + std::to_string(sibs.size()) +
            " sibling(s)");
        for (SceneNode *s : sibs) {
          FillStyle fb = s->fill;
          FillStyle fa = fb;
          StrokeStyle sb = s->stroke;
          StrokeStyle sa = sb;
          sa.join = j;
          // S82 m4f: snap swatch ids around the funnel call.
          // S92 m3: same shape for bound_style.
          std::string fsib = s->fill_swatch_id;
          std::string ssib = s->stroke_swatch_id;
          std::string bsb = s->bound_style;
          Curvz::style::mutate_appearance(
              *s, [j](SceneNode &n) { n.stroke.join = j; });
          std::string fsia = s->fill_swatch_id;
          std::string ssia = s->stroke_swatch_id;
          std::string bsa = s->bound_style;
          // s168 m1 DIAG — STRIP after triage
          LOG_INFO("[IIDDIAG] EditAppearance::push (sib join) "
                   "iid='{}' obj_name='{}' obj_type={}",
                   s->internal_id, s->name, (int)s->type);
          composite->add(std::make_unique<EditAppearanceCommand>(
              m_project, s->internal_id,
              std::move(fb), std::move(sb), std::move(fa), std::move(sa),
              std::move(fsib), std::move(ssib), std::move(fsia),
              std::move(ssia), std::move(bsb), std::move(bsa),
              "Sibling join sync"));
        }
        m_history->push(std::move(composite));
      };

      join_miter->signal_toggled().connect(
          [this, obj, join_miter, broadcast_join]() {
            if (m_loading || !join_miter->get_active())
              return;
            Curvz::style::mutate_appearance(
                *obj, [](SceneNode &n) { n.stroke.join = LineJoin::Miter; });
            push_inspector_command(obj);
            broadcast_join(LineJoin::Miter);
            emit_prop_changed();
          });
      join_round->signal_toggled().connect(
          [this, obj, join_round, broadcast_join]() {
            if (m_loading || !join_round->get_active())
              return;
            Curvz::style::mutate_appearance(
                *obj, [](SceneNode &n) { n.stroke.join = LineJoin::Round; });
            push_inspector_command(obj);
            broadcast_join(LineJoin::Round);
            emit_prop_changed();
          });
      join_bevel->signal_toggled().connect(
          [this, obj, join_bevel, broadcast_join]() {
            if (m_loading || !join_bevel->get_active())
              return;
            Curvz::style::mutate_appearance(
                *obj, [](SceneNode &n) { n.stroke.join = LineJoin::Bevel; });
            push_inspector_command(obj);
            broadcast_join(LineJoin::Bevel);
            emit_prop_changed();
          });
    }
  };

  // ── Build Fill and Stroke sections ───────────────────────────────────────
  PaintEditor *fill_editor = nullptr;
  build_paint_row("Fill", obj->fill, false, &fill_editor);
  curvz::utils::set_name(fill_editor, "ins_fs_fill",
                         "inspector_fill_stroke_fill_editor");

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(6);
  sep->set_margin_bottom(2);
  outer_body->append(*sep);

  PaintEditor *stroke_editor = nullptr;
  build_paint_row("Stroke", obj->stroke.paint, true, &stroke_editor);
  curvz::utils::set_name(stroke_editor, "ins_fs_strk",
                         "inspector_fill_stroke_stroke_editor");

  // ── Opacity row (s108 m2) ────────────────────────────────────────────
  // Lives in Appearance per Illustrator/Affinity convention. Edits
  // route through Canvas::apply_opacity_to_selection so the multi-
  // select broadcast and group-flatten rule are applied uniformly with
  // macro replay. m_loading / gen guards mirror the rest of the panel.
  {
    auto *op_sep =
        Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    op_sep->set_margin_top(6);
    op_sep->set_margin_bottom(2);
    outer_body->append(*op_sep);

    auto *op_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    op_row->set_spacing(6);
    op_row->set_margin_start(8);
    op_row->set_margin_end(6);
    op_row->set_margin_top(2);
    op_row->set_margin_bottom(4);

    auto *op_lbl = Gtk::make_managed<Gtk::Label>("Opacity");
    op_lbl->add_css_class("prop-lbl");
    op_lbl->set_xalign(0.0f);
    op_row->append(*op_lbl);

    auto *sp_op =
        Gtk::make_managed<CurvzSpinButton>(SpinType::Percentage, m_canvas);
    curvz::utils::set_name(sp_op, "ins_fs_op",
                           "inspector_fill_stroke_opacity_spn");
    sp_op->with_value(obj->opacity * 100.0)->with_css("prop-width-entry");
    sp_op->set_width_chars(6);
    sp_op->set_hexpand(true);
    block_scroll(sp_op, [this] { emit_canvas_focus(); });
    op_row->append(*sp_op);
    outer_body->append(*op_row);

    uint32_t op_gen = m_build_gen;
    sp_op->on_changed([this, obj, op_gen](double pct) {
      if (op_gen != m_build_gen || m_loading)
        return;
      const double v = std::clamp(pct / 100.0, 0.0, 1.0);
      if (m_canvas_widget) {
        // Multi-select broadcast + group-flatten lives in Canvas. Inspector
        // does not write obj->opacity directly — keeps the rule in one
        // place and matches the macro replay path.
        m_canvas_widget->apply_opacity_to_selection(v);
      } else {
        // No canvas wired (placeholder branch): write primary directly.
        obj->opacity = v;
      }
      emit_prop_changed();
    });
  }
}

// ── update_visual_labels ─────────────────────────────────────────────────────
// Recompute Xv/Yv/Wv/Hv readout labels from m_current. Called from
// build_selection_section (initial population) and emit_prop_changed (live
// update after geometry / stroke edits) and sync_selection (live update
// during canvas drag).
//
// The labels show construction bbox padded by stroke width — see
// compute_visual_bbox. Hidden as a group when no leaf has a stroke.
void PropertiesPanel::update_visual_labels() {
  if (!m_sel_lbl_xv)
    return; // selection section not built yet
  if (!m_current || !m_canvas) {
    m_sel_lbl_xv->set_visible(false);
    m_sel_lbl_yv->set_visible(false);
    m_sel_lbl_wv->set_visible(false);
    m_sel_lbl_hv->set_visible(false);
    return;
  }

  // Collect leaves — same shape as build_selection_section's
  // collect_leaves. Anything with path geometry contributes.
  std::vector<SceneNode *> leaves;
  std::function<void(SceneNode *)> collect = [&](SceneNode *n) {
    if (!n)
      return;
    if (n->type == SceneNode::Type::Path && n->path &&
        !n->path->nodes.empty()) {
      leaves.push_back(n);
      return;
    }
    for (auto &c : n->children)
      collect(c.get());
    if (n->type == SceneNode::Type::ClipGroup && n->clip_shape)
      collect(n->clip_shape.get());
    if (n->type == SceneNode::Type::Blend) {
      collect(n->blend_source_a.get());
      collect(n->blend_source_b.get());
      for (auto &step : n->blend_cache)
        collect(step.get());
    }
    if (n->type == SceneNode::Type::Warp) {
      collect(n->warp_source.get());
      collect(n->warp_glyph_cache.get());
      collect(n->warp_cache.get());
    }
  };
  collect(m_current);

  if (leaves.empty()) {
    m_sel_lbl_xv->set_visible(false);
    m_sel_lbl_yv->set_visible(false);
    m_sel_lbl_wv->set_visible(false);
    m_sel_lbl_hv->set_visible(false);
    return;
  }

  double vx, vy, vw, vh;
  bool any_stroke = false;
  compute_visual_bbox(leaves, vx, vy, vw, vh, any_stroke);

  if (!any_stroke) {
    // Visual == construction; nothing useful to show.
    m_sel_lbl_xv->set_visible(false);
    m_sel_lbl_yv->set_visible(false);
    m_sel_lbl_wv->set_visible(false);
    m_sel_lbl_hv->set_visible(false);
    return;
  }

  // Convert doc-space → display-unit using each spinner's own
  // CurvzSpinButton::to_display(). This guarantees the visual labels
  // share precision and unit conversion with their editable siblings.
  // Y label shows the doc maxy (top of bbox in user-facing Y-up),
  // matching sp_y's convention.
  auto digits_for = [](CurvzSpinButton *sp) {
    return sp ? sp->get_digits() : 1;
  };
  auto fmt = [](double v, int d) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", d, v);
    return std::string(buf);
  };

  if (m_sel_sp_x) {
    double dx = m_sel_sp_x->to_display(vx);
    m_sel_lbl_xv->set_text("Xv " + fmt(dx, digits_for(m_sel_sp_x)));
  }
  if (m_sel_sp_y) {
    double dy = m_sel_sp_y->to_display(vy + vh); // doc maxy = top in Y-up
    m_sel_lbl_yv->set_text("Yv " + fmt(dy, digits_for(m_sel_sp_y)));
  }
  if (m_sel_sp_w) {
    double dw = m_sel_sp_w->to_display(vw);
    m_sel_lbl_wv->set_text("Wv " + fmt(dw, digits_for(m_sel_sp_w)));
  }
  if (m_sel_sp_h) {
    double dh = m_sel_sp_h->to_display(vh);
    m_sel_lbl_hv->set_text("Hv " + fmt(dh, digits_for(m_sel_sp_h)));
  }

  m_sel_lbl_xv->set_visible(true);
  m_sel_lbl_yv->set_visible(true);
  m_sel_lbl_wv->set_visible(true);
  m_sel_lbl_hv->set_visible(true);
}

// ── Legacy wrappers
// ───────────────────────────────────────────────────────────
void PropertiesPanel::sync_selection(SceneNode *obj) {
  if (!obj)
    return;

  // s122 m4: sync_selection is the lightweight "spin-buttons follow live
  // drag" path. It assumes the inspector was previously built for `obj`
  // (m_current == obj). If a structural op (boolean union, group, etc.)
  // has replaced selection with a fresh node and signal_document_changed
  // fires synchronously BEFORE the deferred refresh() rebuilds for the
  // new node, then m_current is a dangling pointer to a destroyed node.
  // update_visual_labels() walks m_current's children, segfaulting.
  // Bail here in that case — the deferred refresh() will populate
  // everything correctly when it runs.
  if (obj != m_current)
    return;

  // ── Refpt: separate spin pointers, no bbox ───────────────────────────
  // Live-update the ref-section X/Y spinners during refpt drag/nudge.
  // Refpts don't have path geometry so the bbox path below would no-op,
  // and the spin pointers there (m_sel_sp_*) are nullptr for a refpt
  // selection anyway.
  if (obj->is_ref() && m_ref_sp_x && m_ref_sp_y) {
    m_loading = true;
    m_ref_sp_x->set_internal_value(obj->ref_x);
    m_ref_sp_y->set_internal_value(obj->ref_y);
    m_loading = false;
    return;
  }

  // Lightweight update of X/Y/W/H spinners during object move.
  // Only works if build_selection_section has stored the adj pointers.
  if (!m_sel_sp_x || !m_sel_sp_y || !m_sel_sp_w || !m_sel_sp_h)
    return;

  // Recompute bbox — same logic as build_selection_section
  double minx = 1e18, miny = 1e18;
  double maxx = -1e18, maxy = -1e18;

  std::function<void(SceneNode *)> collect = [&](SceneNode *n) {
    if (!n)
      return;
    for (auto &c : n->children)
      collect(c.get());
    if (!n->path)
      return;
    expand_bbox_for_path(*n->path, minx, maxx, miny, maxy);
  };
  collect(obj);

  if (minx > 1e17)
    return; // no geometry

  double bw_doc = maxx - minx;
  double bh_doc = maxy - miny;

  m_loading = true;
  if (m_sel_sp_x)
    m_sel_sp_x->set_internal_value(minx);
  if (m_sel_sp_y)
    m_sel_sp_y->set_internal_value(maxy); // Y-up top = doc maxy
  if (m_sel_sp_w)
    m_sel_sp_w->set_internal_value(std::max(0.000001, bw_doc));
  if (m_sel_sp_h)
    m_sel_sp_h->set_internal_value(std::max(0.000001, bh_doc));
  m_loading = false;

  // Visual readouts ride along — drag-induced position/size changes feed
  // through the same path the spinners already updated above.
  update_visual_labels();
}

// ── Legacy wrappers
// ───────────────────────────────────────────────────────────
void PropertiesPanel::show_document_props(CanvasModel *canvas) {
  LOG_INFO("PropertiesPanel::show_document_props unit={}",
           canvas ? UnitSystem::label(canvas->display_unit) : "null");
  refresh(canvas, nullptr);
}
void PropertiesPanel::show_object_props(SceneNode *obj) {
  refresh(m_canvas, obj);
}
void PropertiesPanel::show_empty() { refresh(nullptr, nullptr); }

} // namespace Curvz
