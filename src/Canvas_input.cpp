#include "Canvas.hpp"
#include "CommandHistory.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"  // s116 m6 — m_project field reads workspace appearance
#include "CurvzSpinButton.hpp"
#include "MacroSystem.hpp"
#include "SvgParser.hpp"
#include "curvz_utils.hpp"  // S97 m2 — box_blur_argb32 for drop-shadow render
#include "color/SwatchLibrary.hpp"  // set_swatch_library + apply_swatch_to_selection
#include "color/FillStyleInterop.hpp"  // to_fillstyle — live-recolour walk (s70 M3)
#include "style/StyleInterop.hpp"  // mutate_appearance funnel for user-driven fill/stroke writes
#include "style/StyleLibrary.hpp"  // set_style_library + signal_style_changed (S78 m3d)
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/clipboard.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/pixbuf.h>
#include <gdkmm/rectangle.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/grid.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-font.h>
#include <sstream>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>  // s125 m1c — uint8_t for PNG IHDR parser
#include <cstring>  // s125 m1c — std::memcmp for PNG signature check
#include <ctime>    // s125 m1c — strftime/localtime for mtime in info dialog
#include <fstream>  // s125 m1c — std::ifstream for IHDR parser
#include <functional>
#include <glib.h> // g_uuid_string_random via generate_internal_id()
#include <glibmm/main.h>
#include <gtkmm/gestureclick.h>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Canvas_internal.hpp"

namespace Curvz {


void Canvas::set_active_tool(ActiveTool tool) {
  // Leaving pen tool — commit if enough nodes, otherwise cancel
  if (m_tool == ActiveTool::Pen && tool != ActiveTool::Pen) {
    if (m_pen_tool.has_wip && m_pen_tool.wip.nodes.size() >= 2) {
      commit_pen_path();
    } else {
      m_pen_tool.cancel();
    }
  }
  m_prev_tool = m_tool; // old tool, before we overwrite

  // Leaving Zoom — always reset toolbar icon before switching away
  if (m_tool == ActiveTool::Zoom && tool != ActiveTool::Zoom) {
    m_zoom_alt_prev = false;
    m_sig_zoom_alt.emit(false);
  }

  // Leaving Eyedropper — clear hover state so pointer doesn't dangle
  if (m_tool == ActiveTool::Eyedropper && tool != ActiveTool::Eyedropper) {
    m_eyedropper_hovered = nullptr;
  }

  // Leaving Text tool — commit any in-progress edit
  if (m_tool == ActiveTool::Text && tool != ActiveTool::Text) {
    if (m_text_editing)
      commit_text_edit();
  }

  m_tool = tool; // now set the new tool

  // Disarm any pending clip-pick — one arm = one selection-tool press.
  if (m_clip_pick_armed) {
    m_clip_pick_armed = false;
    m_clip_pick_selection.clear();
    LOG_INFO("Canvas: clip-pick disarmed on tool switch");
  }

  // Selection state management
  // Always clear secondary node selection on any tool switch
  m_selected2 = nullptr;
  m_selected_node2 = -1;
  // Clear multi-selection on tool switch — but NOT for Eyedropper (S66
  // Phase 3): the eyedropper's whole job is applying a sampled colour to
  // the current selection. Clearing it on entry makes every pick a no-op
  // against objects and only updates the Toolbar wells.
  if (tool != ActiveTool::Selection && tool != ActiveTool::Eyedropper) {
    m_selection.clear();
    m_move_snaps.clear();
    m_warp_env_move_snaps.clear();
  }

  if (tool == ActiveTool::Node && m_prev_tool != ActiveTool::Pen) {
    // Carry over a Path selection from Selection tool so the user doesn't
    // have to re-click after switching tools (S→N workflow).
    bool carried = m_selected && m_selected->type == SceneNode::Type::Path &&
                   m_selected->path && !m_selected->path->nodes.empty();
    if (carried) {
      m_selected_node = 0;
      m_node_selection.clear();
      m_node_selection.push_back({m_selected, 0});
      // s160 m2: m_selection was cleared at line 952 (tool != Selection
      // && tool != Eyedropper) but m_selected is preserved on this carry
      // branch — re-sync so SelectionContext sees the consistent pair.
      // (Audit had this as B; per-site re-verify upgraded to B+C — same
      // family as the 14687/10068 corrections caught in s159.)
      m_selection = {m_selected};
      notify_object_selection_changed();
      notify_node_selection_changed();
      m_sig_node_changed.emit(m_selected, m_selected_node);
    } else {
      m_selected = nullptr;
      m_selected_node = -1;
      notify_object_selection_changed();
      notify_node_selection_changed();
      m_sig_node_changed.emit(nullptr, -1);
    }
  } else if (tool == ActiveTool::Selection) {
    // Entering Selection tool — clear node index, keep object selection.
    // Always emit so the inspector rebuilds from Node→Selection layout.
    m_selected_node = -1;
    // Sync m_selection with m_selected
    if (m_selected && m_selection.empty())
      m_selection.push_back(m_selected);
    else if (!m_selected)
      m_selection.clear();
    notify_object_selection_changed();
  } else if (tool == ActiveTool::Eyedropper) {
    // S66 Phase 3 — preserve both primary and multi-selection. Node index
    // is meaningless for the eyedropper (it works on whole-object paint),
    // so clear that but leave m_selected alone. Emit so panels refresh
    // without losing the highlight.
    m_selected_node = -1;
    // Auto-sync m_selection ← m_selected when coming from a tool that
    // didn't maintain the multi-selection (e.g. Node, TextOnPath). The
    // commit loop iterates m_selection, so without this sync a click from
    // Eyedropper-after-Node would only update the Toolbar wells.
    if (m_selected && m_selection.empty())
      m_selection.push_back(m_selected);
    else if (!m_selected)
      m_selection.clear();
    notify_object_selection_changed();
  } else if (tool != ActiveTool::Node && tool != ActiveTool::TextOnPath) {
    m_selected = nullptr;
    m_selected_node = -1;
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_node_changed.emit(nullptr, -1);
  }

  // Clear corner tool state when leaving it
  if (m_prev_tool == ActiveTool::Corner && tool != ActiveTool::Corner) {
    m_corner_selection.clear();
    m_corner_rubber_active = false;
    m_sig_corner_sel_changed.emit(0);
    queue_draw();
  }

  // Cursor + zoom-alt sync for the newly active tool
  switch (tool) {
  case ActiveTool::Zoom:
    set_cursor(m_mod_alt ? "zoom-out" : "zoom-in");
    // Always emit so toolbar icon is set correctly on tool entry,
    // regardless of whether alt state changed.
    m_zoom_alt_prev = m_mod_alt;
    m_sig_zoom_alt.emit(m_mod_alt);
    break;
  case ActiveTool::Eyedropper:
    set_cursor("crosshair");
    break;
  case ActiveTool::Text:
    set_cursor("text");
    break;
  case ActiveTool::Pen:
  case ActiveTool::Rect:
  case ActiveTool::Ellipse:
  case ActiveTool::Line:
    set_cursor("crosshair");
    break;
  case ActiveTool::Measure:
    set_cursor("crosshair");
    break;
  case ActiveTool::TextOnPath:
    set_cursor("crosshair");
    break;
  case ActiveTool::Selection:
  case ActiveTool::Node:
  default:
    set_cursor("default");
    break;
  }

  // Entering Ruler tool — try to inherit a 2-node selection from Node tool
  if (tool == ActiveTool::Measure) {
    ruler_try_inherit_node_selection();
  }

  // Leaving Ruler tool — clear ruler node refs (pointers may dangle)
  if (m_prev_tool == ActiveTool::Measure && tool != ActiveTool::Measure) {
    m_ruler_node_a_obj = nullptr;
    m_ruler_node_a_idx = -1;
    m_ruler_node_b_obj = nullptr;
    m_ruler_node_b_idx = -1;
    m_ruler_labels.clear();
    if (m_ruler_toast_conn.connected())
      m_ruler_toast_conn.disconnect();
    m_ruler_toast_ms = 0;
  }

  // Entering / leaving TextOnPath tool
  if (tool == ActiveTool::TextOnPath) {
    m_top_phase = 0;
    m_top_text = nullptr;
    m_top_path_node = nullptr;
    m_top_dragging = false;
    // If there's already a linked text node selected, restore phase 2
    // so the user can drag the start point immediately without re-clicking.
    if (m_selected && m_selected->is_text() &&
        !m_selected->text_path_id.empty()) {
      SceneNode *guide = top_find_path_by_id(m_selected->text_path_id);
      if (guide) {
        m_top_text = m_selected;
        m_top_path_node = guide;
        m_top_phase = 2;
        LOG_DEBUG("set_active_tool(TOP): restored phase 2 for linked text '{}'",
                  m_selected->text_path_id);
      }
    }
  }
  if (m_prev_tool == ActiveTool::TextOnPath && tool != ActiveTool::TextOnPath) {
    m_top_phase = 0;
    m_top_text = nullptr;
    m_top_path_node = nullptr;
    m_top_dragging = false;
  }

  queue_draw();
}

// ── Alt forwarding from MainWindow (more reliable than canvas-local key ctrl)
// ─
void Canvas::notify_alt_pressed() {
  if (m_mod_alt)
    return; // already set
  m_mod_alt = true;
  if (m_tool == ActiveTool::Zoom) {
    set_cursor("zoom-out");
    if (!m_zoom_alt_prev) {
      m_zoom_alt_prev = true;
      m_sig_zoom_alt.emit(true);
    }
  }
}

void Canvas::notify_alt_released() {
  if (!m_mod_alt)
    return; // already clear
  m_mod_alt = false;
  if (m_tool == ActiveTool::Zoom) {
    set_cursor("zoom-in");
    if (m_zoom_alt_prev) {
      m_zoom_alt_prev = false;
      m_sig_zoom_alt.emit(false);
    }
  }
}

void Canvas::notify_space_pressed() {
  if (m_space_held)
    return;
  m_space_held = true;
  // Show a grab cursor whenever Space is down so the user knows pan is ready.
  // Don't change cursor if the Zoom tool is active — it owns its own cursor.
  if (m_tool != ActiveTool::Zoom)
    set_cursor("grab");
}

void Canvas::notify_space_released() {
  if (!m_space_held)
    return;
  m_space_held = false;
  m_space_panning = false;
  // Restore normal cursor for the active tool.
  if (m_tool == ActiveTool::Zoom)
    set_cursor(m_mod_alt ? "zoom-out" : "zoom-in");
  else if (m_tool == ActiveTool::Pen || m_tool == ActiveTool::Rect ||
           m_tool == ActiveTool::Ellipse || m_tool == ActiveTool::Line ||
           m_tool == ActiveTool::Eyedropper || m_tool == ActiveTool::Polygon ||
           m_tool == ActiveTool::Spiral)
    set_cursor("crosshair");
  else
    set_cursor("default");
}

void Canvas::notify_r_pressed() {
  // R toggles pivot mode on/off
  if (m_tool != ActiveTool::Selection || m_selection.empty())
    return;

  if (m_r_held) {
    // Second R press — exit pivot mode
    m_r_held = false;
    m_pivot_dragging = false;
    set_cursor("default");
    queue_draw();
    return;
  }

  m_r_held = true;
  // Default pivot to BBX center if not already set
  if (!m_has_custom_pivot) {
    bool found = false;
    double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    for (SceneNode *obj : m_selection) {
      auto bb = object_bbox(*obj);
      if (!bb)
        continue;
      if (!found) {
        bx1 = bb->x;
        by1 = bb->y;
        bx2 = bb->x + bb->w;
        by2 = bb->y + bb->h;
        found = true;
      } else {
        bx1 = std::min(bx1, bb->x);
        by1 = std::min(by1, bb->y);
        bx2 = std::max(bx2, bb->x + bb->w);
        by2 = std::max(by2, bb->y + bb->h);
      }
    }
    if (found) {
      m_custom_pivot_x = (bx1 + bx2) * 0.5;
      m_custom_pivot_y = (by1 + by2) * 0.5;
    }
  }
  set_cursor("crosshair");
  queue_draw();
}

void Canvas::notify_r_released() {
  // R is now a toggle — key release does nothing
  (void)0;
}

void Canvas::on_pivot_dialog(double doc_x, double doc_y) {
  m_custom_pivot_x = snap(doc_x);
  m_custom_pivot_y = snap(doc_y);
  m_has_custom_pivot = true;
  queue_draw();

  auto *win = dynamic_cast<Gtk::Window *>(get_root());
  if (!win)
    return;

  auto *dlg = new Gtk::Window();
  curvz::utils::set_name(dlg, "dlg_rfp", "rotate_from_point_dialog_root");
  dlg->set_title("Rotate from Point");
  dlg->set_transient_for(*win);
  curvz::utils::apply_motif_class_from_parent(*dlg, *win);  // s117 m18 v2
  dlg->set_modal(true);
  dlg->set_resizable(false);
  dlg->set_default_size(260, -1);

  auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(8);
  grid->set_column_spacing(12);
  grid->set_margin(16);
  grid->set_margin_bottom(8);

  auto make_lbl = [](const char *t) {
    auto *l = Gtk::make_managed<Gtk::Label>(t);
    l->set_halign(Gtk::Align::START);
    return l;
  };

  // csb handles unit display + Y-flip (PositionY stores doc-Y-down internally
  // and displays Y-up user space). Ruler origin = 0 so pivot coordinates are
  // shown in raw canvas space, matching prior behavior.
  const CanvasModel *cm = m_doc ? &m_doc->canvas : nullptr;
  auto *spin_x =
      Gtk::make_managed<CurvzSpinButton>(SpinType::PositionX, cm, 0.0);
  curvz::utils::set_name(spin_x, "dlg_rfp_x", "rotate_from_point_dialog_x_spn");
  spin_x->with_value(m_custom_pivot_x);
  spin_x->set_hexpand(true);

  auto *spin_y =
      Gtk::make_managed<CurvzSpinButton>(SpinType::PositionY, cm, 0.0);
  curvz::utils::set_name(spin_y, "dlg_rfp_y", "rotate_from_point_dialog_y_spn");
  spin_y->with_value(m_custom_pivot_y); // doc-Y-down; csb flips for display
  spin_y->set_hexpand(true);

  auto *spin_a = Gtk::make_managed<CurvzSpinButton>(SpinType::Angle, cm);
  curvz::utils::set_name(spin_a, "dlg_rfp_ang", "rotate_from_point_dialog_angle_spn");
  spin_a->with_value(0.0);
  spin_a->set_hexpand(true);

  grid->attach(*make_lbl("Pivot X"), 0, 0);
  grid->attach(*spin_x, 1, 0);
  if (auto *ul = spin_x->get_unit_label())
    grid->attach(*ul, 2, 0);
  grid->attach(*make_lbl("Pivot Y"), 0, 1);
  grid->attach(*spin_y, 1, 1);
  if (auto *ul = spin_y->get_unit_label())
    grid->attach(*ul, 2, 1);
  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  sep->set_margin_top(4);
  sep->set_margin_bottom(4);
  grid->attach(*sep, 0, 2, 3, 1);
  grid->attach(*make_lbl("Rotate °"), 0, 3);
  grid->attach(*spin_a, 1, 3);

  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
  btn_row->set_halign(Gtk::Align::END);
  btn_row->set_margin_start(16);
  btn_row->set_margin_end(16);
  btn_row->set_margin_bottom(16);
  auto *btn_cancel = Gtk::make_managed<Gtk::Button>("Cancel");
  auto *btn_apply = Gtk::make_managed<Gtk::Button>("Apply");
  curvz::utils::set_name(btn_cancel, "dlg_rfp_cnc", "rotate_from_point_dialog_cancel_btn");
  curvz::utils::set_name(btn_apply, "dlg_rfp_app", "rotate_from_point_dialog_apply_btn");
  btn_apply->add_css_class("suggested-action");
  btn_row->append(*btn_cancel);
  btn_row->append(*btn_apply);

  vbox->append(*grid);
  vbox->append(*btn_row);
  dlg->set_child(*vbox);
  spin_x->grab_focus();

  btn_cancel->signal_clicked().connect([dlg]() { dlg->close(); });

  btn_apply->signal_clicked().connect([this, dlg, spin_x, spin_y, spin_a]() {
    // csb internal = doc-Y-down coords already; no manual flip needed.
    m_custom_pivot_x = spin_x->get_internal_value();
    m_custom_pivot_y = spin_y->get_internal_value();
    m_has_custom_pivot = true;

    double angle_deg = spin_a->get_internal_value();
    if (m_doc && std::abs(angle_deg) > 0.0001) {
      double angle_rad = angle_deg * M_PI / 180.0;
      double px = m_custom_pivot_x, py = m_custom_pivot_y;
      double cos_a = std::cos(angle_rad), sin_a = std::sin(angle_rad);

      std::vector<SceneNode *> leaves, direct;
      for (SceneNode *obj : m_selection) {
        if (obj->is_path()) {
          std::vector<SceneNode *> tmp;
          // inline collect: paths only
          std::function<void(SceneNode *)> gather = [&](SceneNode *n) {
            if (n->is_path() && n->path)
              leaves.push_back(n);
            for (auto &ch : n->children)
              gather(ch.get());
          };
          gather(obj);
        } else if (obj->is_text() || obj->is_image()) {
          direct.push_back(obj);
        } else {
          std::function<void(SceneNode *)> gather = [&](SceneNode *n) {
            if (n->is_path() && n->path)
              leaves.push_back(n);
            for (auto &ch : n->children)
              gather(ch.get());
          };
          gather(obj);
        }
      }

      struct PSnap {
        SceneNode *obj;
        PathData before;
      };
      std::vector<PSnap> psnaps;
      for (SceneNode *leaf : leaves)
        if (leaf->path)
          psnaps.push_back({leaf, *leaf->path});
      struct TSnap {
        SceneNode *obj;
        double bx, by;
      };
      std::vector<TSnap> tsnaps;
      for (SceneNode *obj : direct)
        tsnaps.push_back({obj, obj->is_text() ? obj->text_x : obj->image_x,
                          obj->is_text() ? obj->text_y : obj->image_y});

      auto rot = [&](double &x, double &y) {
        double dx = x - px, dy = y - py;
        x = px + dx * cos_a - dy * sin_a;
        y = py + dx * sin_a + dy * cos_a;
      };

      for (SceneNode *leaf : leaves)
        if (leaf->path)
          for (auto &n : leaf->path->nodes) {
            rot(n.x, n.y);
            rot(n.cx1, n.cy1);
            rot(n.cx2, n.cy2);
          }
      for (SceneNode *obj : direct) {
        if (obj->is_text()) {
          rot(obj->text_x, obj->text_y);
        } else {
          double cx = obj->image_x + obj->image_w * 0.5;
          double cy = obj->image_y + obj->image_h * 0.5;
          rot(cx, cy);
          obj->image_x = cx - obj->image_w * 0.5;
          obj->image_y = cy - obj->image_h * 0.5;
        }
      }

      if (m_history) {
        auto comp = std::make_unique<CompositeCommand>("Rotate from point");
        for (auto &s : psnaps)
          comp->add(std::make_unique<EditPathCommand>(
              project(), s.obj->internal_id, s.before, *s.obj->path,
              "Rotate from point"));
        for (auto &s : tsnaps) {
          double ax = s.obj->is_text() ? s.obj->text_x : s.obj->image_x;
          double ay = s.obj->is_text() ? s.obj->text_y : s.obj->image_y;
          comp->add(
              std::make_unique<MoveObjectCommand>(
                  project(), s.obj->internal_id, s.bx, s.by, ax, ay));
        }
        if (!comp->steps.empty())
          m_history->push(std::move(comp));
      }
      m_sig_doc_changed.emit();
      queue_draw();
    }
    queue_draw();
    dlg->close();
  });

  dlg->signal_hide().connect([dlg]() { delete dlg; });
  dlg->show();
}

void Canvas::on_text_begin(double sx, double sy) {
  if (!m_doc)
    return;

  // Convert screen → doc space.
  double dx, dy;
  screen_to_doc(sx, sy, dx, dy);

  // Hit-test for an existing Text node first.
  SceneNode *hit = nullptr;
  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked)
      continue;
    for (int i = (int)layer->children.size() - 1; i >= 0; --i) {
      SceneNode *obj = layer->children[i].get();
      if (!obj->is_text())
        continue;
      // Simple bbox hit: use font_size as approximate height, width heuristic.
      double approx_w = obj->text_content.size() * obj->text_font_size * 0.6;
      double approx_h = obj->text_font_size * 1.4;
      double ox = obj->text_x;
      double oy = obj->text_y - approx_h;
      if (obj->text_anchor == "middle")
        ox -= approx_w * 0.5;
      if (obj->text_anchor == "end")
        ox -= approx_w;
      if (dx >= ox && dx <= ox + approx_w && dy >= oy && dy <= oy + approx_h) {
        hit = obj;
        break;
      }
    }
    if (hit)
      break;
  }

  if (hit) {
    // Edit existing text node — snapshot before-state for undo.
    m_text_editing = hit;
    m_text_is_new = false;
    m_text_snapshot = TextEditCommand::snapshot_before(project(), hit);
    m_text_has_snapshot = true;
    m_selected = hit;
    m_selection = {hit};  // s159 m2: sync for SelectionContext
    notify_object_selection_changed();
    // s168 m1 DIAG — STRIP after triage (re-added in m5; clobbered when
    // m4 overwrote Canvas_input.cpp from baseline)
    LOG_INFO("[IIDDIAG] TextEdit::snapshot_before  hit_iid='{}' "
             "snapshot.obj_iid='{}' name='{}' content='{}'",
             hit->internal_id, m_text_snapshot.obj_iid,
             hit->name, hit->text_content);
  } else {
    // Create a new text node at click position.
    auto obj = std::make_unique<SceneNode>();
    obj->type = SceneNode::Type::Text;
    obj->internal_id = generate_internal_id();
    obj->name = m_doc->next_default_name(CurvzDocument::NameKind::Text);
    // Defaults-application routes through the Style funnel per S74 m2.
    // For new objects the clear-bound_style step is a no-op, but going
    // through the funnel keeps the invariant "every writer outside
    // style-propagation takes this path" uniform.
    style::mutate_appearance(*obj, [this](SceneNode& n) {
      n.fill = m_def_fill;
      n.stroke = m_def_stroke;
      n.stroke.paint.type = FillStyle::Type::None;  // default: no stroke on text
    });
    obj->text_x = dx;
    obj->text_y = dy;
    obj->text_font_family = "Sans";
    obj->text_font_size = 24.0;
    obj->text_anchor = "start";
    obj->text_align = "left";

    SceneNode *layer = m_doc->active_layer();
    if (!layer)
      layer = m_doc->layers[0].get();
    layer->children.insert(layer->children.begin(), std::move(obj));
    m_text_editing = layer->children.front().get();
    m_text_is_new = true;
    m_text_has_snapshot = false;
    m_selected = m_text_editing;
    m_selection = {m_text_editing};  // s159 m2: sync for SelectionContext
    notify_object_selection_changed();
  }

  // Show and populate the entry.
  if (m_text_entry) {
    m_text_entry_conn_activate.disconnect();
    m_text_entry_conn_changed.disconnect();

    m_text_entry->set_text(m_text_editing->text_content);
    m_text_entry->set_visible(true);
    m_text_entry->grab_focus();
    m_text_entry->select_region(0, -1);

    position_text_entry();

    m_text_entry_conn_changed =
        m_text_entry->signal_changed().connect([this]() {
          if (m_text_editing) {
            m_text_editing->text_content = m_text_entry->get_text();
            queue_draw();
          }
        });
    m_text_entry_conn_activate = m_text_entry->signal_activate().connect(
        [this]() { commit_text_edit(); });
  }

  queue_draw();
}

void Canvas::commit_text_edit() {
  if (!m_text_editing)
    return;

  std::string content = m_text_entry ? m_text_entry->get_text() : "";

  if (content.empty() && m_text_is_new) {
    // Nothing typed — delete the placeholder node.
    cancel_text_edit();
    return;
  }

  m_text_editing->text_content = content;

  // Push undo command via TextEditCommand (full before/after snapshot).
  if (m_history) {
    if (!m_text_is_new && m_text_has_snapshot) {
      // Existing node edit.
      m_text_snapshot.record_after(m_text_editing);
      // s168 m1 DIAG — STRIP after triage (re-added in m5)
      LOG_INFO("[IIDDIAG] TextEdit::push  capturing iid='{}' "
               "obj_name='{}' obj_type={}  before='{}' after='{}'",
               m_text_snapshot.obj_iid,
               m_text_editing ? m_text_editing->name : std::string{"(null)"},
               m_text_editing ? (int)m_text_editing->type : -1,
               m_text_snapshot.before_content,
               m_text_snapshot.after_content);
      m_history->push(
          std::make_unique<TextEditCommand>(std::move(m_text_snapshot)));
      m_text_has_snapshot = false;
    } else if (m_text_is_new) {
      // New node — AddNodeCommand so Ctrl+Z deletes it.
      for (auto &layer : m_doc->layers) {
        for (auto &child : layer->children) {
          if (child.get() == m_text_editing) {
            m_history->push(std::make_unique<AddNodeCommand>(
                layer.get(), clone_node(*m_text_editing)));
            break;
          }
        }
      }
    }
  }

  if (m_text_entry)
    m_text_entry->set_visible(false);
  m_text_entry_conn_activate.disconnect();
  m_text_entry_conn_changed.disconnect();

  // Select the committed node so it's immediately moveable.
  SceneNode *committed = m_text_editing;
  m_selected = committed;
  m_selection = {committed};

  m_sig_doc_changed.emit();
  notify_object_selection_changed();
  m_text_editing = nullptr;
  m_text_is_new = false;
  // Stay in Text tool so user can click to add another text node,
  // but also switch to Selection so the committed node can be moved right away.
  // Emitting request_tool(Selection) mimics what place_shape_node does.
  m_sig_request_tool.emit(ActiveTool::Selection);
  queue_draw();
}

void Canvas::cancel_text_edit() {
  if (!m_text_editing)
    return;

  if (m_text_is_new) {
    // Remove the freshly-created node.
    for (auto &layer : m_doc->layers) {
      auto &ch = layer->children;
      auto it = std::find_if(ch.begin(), ch.end(),
                             [this](const std::unique_ptr<SceneNode> &n) {
                               return n.get() == m_text_editing;
                             });
      if (it != ch.end()) {
        ch.erase(it);
        break;
      }
    }
    m_selected = nullptr;
    // s159 m2: defensive cleanup. The freshly-created node was just
    // erased above; if it was in m_selection, that pointer is dangling.
    m_selection.clear();
    notify_object_selection_changed();
  }

  if (m_text_entry)
    m_text_entry->set_visible(false);
  m_text_entry_conn_activate.disconnect();
  m_text_entry_conn_changed.disconnect();

  m_text_editing = nullptr;
  m_text_is_new = false;
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── Input: scroll
// ─────────────────────────────────────────────────────────────
bool Canvas::on_scroll(double dx, double dy) {
  if (!m_doc)
    return false;
  // Read ctrl from the scroll event itself — reliable regardless of canvas
  // focus
  bool ctrl = m_mod_ctrl;
  if (m_scroll_ctrl) {
    auto ev = m_scroll_ctrl->get_current_event();
    if (ev) {
      auto state = ev->get_modifier_state();
      ctrl = (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    }
  }
  // Ctrl+scroll → zoom toward cursor, always
  if (ctrl) {
    zoom_toward(m_mouse_x, m_mouse_y, dy < 0 ? 1.1 : (1.0 / 1.1));
    return true;
  }
  // Scroll → pan (two-finger trackpad or scroll wheel)
  // dy is lines; multiply by a comfortable pixel step scaled by zoom
  constexpr double PAN_SPEED = 40.0;
  m_pan_x -= dx * PAN_SPEED;
  m_pan_y -= dy * PAN_SPEED;
  clamp_pan();
  queue_draw();
  return true;
}

// ── Input: middle-drag pan
// ────────────────────────────────────────────────────
void Canvas::on_pan_begin(double /*x*/, double /*y*/) {
  m_pan_drag_start_x = m_pan_x;
  m_pan_drag_start_y = m_pan_y;
}
void Canvas::on_pan_update(double dx, double dy) {
  m_pan_x = m_pan_drag_start_x + dx;
  m_pan_y = m_pan_drag_start_y + dy;
  clamp_pan();
  queue_draw();
}
void Canvas::on_pan_end(double /*dx*/, double /*dy*/) {}

// ── Input: left-drag — route by tool ─────────────────────────────────────────
void Canvas::on_draw_begin(double x, double y) {
  if (!m_doc)
    return;

  // Space+left-drag → pan regardless of active tool.
  // Capture pan origin and early-return — no tool action.
  if (m_space_held) {
    m_space_panning = true;
    m_space_pan_start_x = m_pan_x;
    m_space_pan_start_y = m_pan_y;
    // Reuse middle-drag fields so update/end arithmetic is identical.
    m_pan_drag_start_x = x;
    m_pan_drag_start_y = y;
    set_cursor("grabbing");
    return;
  }

  // ── Warp envelope drag (M4b) / pick (M4c-2) ─────────────────────────
  // Selection tool only. If primary selection is a Warp and the click
  // lands on an envelope anchor/handle, handle pick-set update then
  // snapshot pre-state and capture drag kind. Further routing (marquee,
  // object hit, etc.) is skipped for this press → release cycle.
  if (m_tool == ActiveTool::Selection && m_selected && m_selected->is_warp()) {
    sync_warp_env_picks_to_selection();
    WarpDragKind kind;
    bool is_top;
    int idx;
    if (hit_test_warp_envelope(x, y, *m_selected, kind, is_top, idx)) {
      // Map drag kind → pick-set part enum.
      EnvelopePart part =
          (kind == WarpDragKind::HandleIn)    ? EnvelopePart::HandleIn
          : (kind == WarpDragKind::HandleOut) ? EnvelopePart::HandleOut
                                              : EnvelopePart::Anchor;
      EnvelopePick pick{is_top, idx, part};
      // M4c-2: Shift+click → toggle pick in/out of set. No drag starts.
      if (m_mod_shift) {
        auto it =
            std::find(m_warp_env_picks.begin(), m_warp_env_picks.end(), pick);
        if (it != m_warp_env_picks.end())
          m_warp_env_picks.erase(it);
        else
          m_warp_env_picks.push_back(pick);
        m_warp_env_picks_owner = m_selected;
        queue_draw();
        return;
      }
      // M4c-2: Plain click. Two paths:
      //   (a) Hit is already in pick set with set size > 1 → start MULTI-
      //       drag. Pick set preserved, press point captured for delta.
      //   (b) Otherwise → replace pick set with just this hit, fall
      //       through to M4b single-drag setup.
      bool hit_in_set =
          (std::find(m_warp_env_picks.begin(), m_warp_env_picks.end(), pick) !=
           m_warp_env_picks.end());
      if (hit_in_set && m_warp_env_picks.size() > 1) {
        // Multi-drag path.
        m_warp_drag_is_multi = true;
        m_warp_drag_kind = kind; // anchor flag; not used for mutation
        m_warp_drag_is_top = is_top;
        m_warp_drag_idx = idx;
        m_warp_drag_pre_top = m_selected->warp_env_top;
        m_warp_drag_pre_bottom = m_selected->warp_env_bottom;
        m_warp_drag_pre_quality = m_selected->warp_quality;
        m_warp_drag_pre_preset_idx = m_selected->warp_preset_idx;  // s147 m2
        screen_to_doc(x, y, m_warp_drag_press_doc_x, m_warp_drag_press_doc_y);
        m_warp_drag_click_offset_x = 0.0;
        m_warp_drag_click_offset_y = 0.0;
        queue_draw();
        return;
      }
      // Single-drag path: replace set with just this element.
      m_warp_env_picks.clear();
      m_warp_env_picks.push_back(pick);
      m_warp_env_picks_owner = m_selected;
      m_warp_drag_is_multi = false;
      m_warp_drag_kind = kind;
      m_warp_drag_is_top = is_top;
      m_warp_drag_idx = idx;
      m_warp_drag_pre_top = m_selected->warp_env_top;
      m_warp_drag_pre_bottom = m_selected->warp_env_bottom;
      m_warp_drag_pre_quality = m_selected->warp_quality;
      m_warp_drag_pre_preset_idx = m_selected->warp_preset_idx;  // s147 m2
      // Compute click offset from the hit's doc-space center so first
      // motion doesn't jump. For anchors the offset is click-minus-
      // anchor; for handles it's click-minus-handle.
      double dx_doc, dy_doc;
      screen_to_doc(x, y, dx_doc, dy_doc);
      const PathData &env =
          is_top ? m_selected->warp_env_top : m_selected->warp_env_bottom;
      if (idx >= 0 && idx < (int)env.nodes.size()) {
        const BezierNode &n = env.nodes[idx];
        double target_x = n.x, target_y = n.y;
        if (kind == WarpDragKind::HandleIn) {
          target_x = n.cx1;
          target_y = n.cy1;
        }
        if (kind == WarpDragKind::HandleOut) {
          target_x = n.cx2;
          target_y = n.cy2;
        }
        m_warp_drag_click_offset_x = dx_doc - target_x;
        m_warp_drag_click_offset_y = dy_doc - target_y;
      }
      queue_draw();
      return;
    }
  }

  // SnR dialog pivot drag — pre-empts all tool logic while dialog is open.
  // Any click moves the refpt to that location and begins a drag; release
  // commits wherever the cursor is.  Works under any active tool (unlike
  // rotate-from-point which gates on Selection).
  if (m_sr_preview_active) {
    double dx, dy;
    screen_to_doc(x, y, dx, dy);
    m_sr_preview_x = dx;
    m_sr_preview_y = dy;
    m_sr_pivot_dragging = true;
    if (m_sr_pivot_change_cb)
      m_sr_pivot_change_cb(dx, dy);
    queue_draw();
    return;
  }

  // Guide construct mode — pre-empts tool logic.
  //   Phase 0: capture p1 (node-snap if within tolerance), advance to phase 1.
  //   Phase 1: capture p2 (node-snap), advance to phase 2, fire review cb.
  //   Phase 2: ignore clicks (dialog handles commit/cancel).
  if (m_guide_construct_active) {
    if (m_guide_construct_phase >= 2)
      return;
    double dx, dy;
    screen_to_doc(x, y, dx, dy);

    // Node snap — use the same helper the Ruler tool uses.
    const double tol = 8.0 / m_zoom;
    std::vector<std::pair<SceneNode *, int>> all_nodes;
    ruler_collect_all_path_nodes(all_nodes);
    double best_d = tol;
    for (auto &[obj, ni] : all_nodes) {
      const BezierNode &n = obj->path->nodes[ni];
      double d = std::hypot(n.x - dx, n.y - dy);
      if (d < best_d) {
        best_d = d;
        dx = n.x;
        dy = n.y;
      }
    }

    if (m_guide_construct_phase == 0) {
      m_guide_construct_p1_x = dx;
      m_guide_construct_p1_y = dy;
      m_guide_construct_preview_x = dx;
      m_guide_construct_preview_y = dy;
      m_guide_construct_phase = 1;
      queue_draw();
      return;
    }
    // Phase 1 → 2: capture p2 and hand control to the review dialog.
    m_guide_construct_p2_x = dx;
    m_guide_construct_p2_y = dy;
    m_guide_construct_phase = 2;
    queue_draw();
    if (m_guide_construct_review_cb)
      m_guide_construct_review_cb();
    return;
  }

  // R pivot mode + Selection tool: intercept clicks near the pivot crosshair
  // to move it. Clicks elsewhere fall through to normal handle/selection logic
  // so the rotate handles work as usual.
  if (m_r_held && m_tool == ActiveTool::Selection && !m_selection.empty()) {
    if (m_mod_ctrl) {
      // Ctrl+click anywhere → exact position dialog
      double dx, dy;
      screen_to_doc(x, y, dx, dy);
      on_pivot_dialog(dx, dy);
      return;
    }
    if (m_has_custom_pivot) {
      // Only grab the pivot if click is within ~12px of the crosshair
      double pvsx, pvsy;
      doc_to_screen(m_custom_pivot_x, m_custom_pivot_y, pvsx, pvsy);
      double dist = std::hypot(x - pvsx, y - pvsy);
      if (dist <= 12.0) {
        // Start pivot drag
        m_pivot_dragging = true;
        queue_draw();
        return;
      }
      // Not near pivot — fall through to normal handle/selection logic
    } else {
      // No pivot set yet — click sets it
      double dx, dy;
      screen_to_doc(x, y, dx, dy);
      m_custom_pivot_x = snap(dx);
      m_custom_pivot_y = snap(dy);
      m_has_custom_pivot = true;
      m_pivot_dragging = true;
      queue_draw();
      return;
    }
  }

  double dx, dy;
  screen_to_doc(x, y, dx, dy);

  if (m_tool == ActiveTool::Rect || m_tool == ActiveTool::Ellipse ||
      m_tool == ActiveTool::Polygon || m_tool == ActiveTool::Spiral) {
    m_drawing = true;
    m_draw_start_dx = snap(dx);
    m_draw_start_dy = snap(dy);
    m_draw_cur_dx = m_draw_start_dx;
    m_draw_cur_dy = m_draw_start_dy;
    m_draw_start_effective_dx = m_draw_start_dx;
    m_draw_start_effective_dy = m_draw_start_dy;
    m_poly_drag_angle = -M_PI * 0.5; // point-up default
    LOG_DEBUG("Draw begin at ({:.2f},{:.2f})", m_draw_start_dx,
              m_draw_start_dy);
  } else if (m_tool == ActiveTool::Line) {
    double px = snap(dx), py = snap(dy);
    // Apply 15° angle restriction when Shift held.
    // Compute fresh from the raw position so this works even if the motion
    // handler hasn't run between clicks (rapid clicking).
    if (m_mod_shift && m_line_tool.active()) {
      auto [lx, ly] = m_line_tool.points.back();
      double dw = px - lx, dh = py - ly;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        px = lx + len * std::cos(snapped);
        py = ly + len * std::sin(snapped);
      }
    }
    // Close snap: if near start, snap to it
    if (m_line_tool.active()) {
      double tol = 8.0 / m_zoom;
      auto [sx, sy] = m_line_tool.points[0];
      if (std::hypot(px - sx, py - sy) <= tol) {
        px = sx;
        py = sy;
        m_line_tool.close_snap = true;
      }
    }
    if (m_line_tool.close_snap && m_line_tool.points.size() >= 2) {
      // Closing click — commit closed path
      commit_line_path();
    } else {
      m_line_tool.points.push_back({px, py});
      m_line_tool.live_x = px;
      m_line_tool.live_y = py;
      LOG_DEBUG("LineTool: placed point ({:.2f},{:.2f}), total={}", px, py,
                m_line_tool.points.size());
    }
    queue_draw();
    return;
  } else if (m_tool == ActiveTool::Ref) {
    double px = snap(dx), py = snap(dy);
    // Hit test existing ref points in the RefLayer
    double tol = 8.0 / m_zoom;
    m_ref_selected = nullptr;
    SceneNode *rl = m_doc->ref_layer();
    if (rl && !rl->locked) {
      for (auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        if (std::hypot(px - child->ref_x, py - child->ref_y) <= tol) {
          m_ref_selected = child.get();
          m_ref_drag_ox = px - child->ref_x;
          m_ref_drag_oy = py - child->ref_y;
          // s168 m4: snapshot pre-drag position for RefMoveCommand push
          // at mouse-up. Without this, the drag mutates ref_x/ref_y in
          // place with no undo command — pre-existing gap diagnosed in
          // s168.
          m_ref_drag_orig_x = child->ref_x;
          m_ref_drag_orig_y = child->ref_y;
          break;
        }
      }
    }
    m_drawing = true;
    m_draw_start_dx = px;
    m_draw_start_dy = py;
    m_draw_cur_dx = px;
    m_draw_cur_dy = py;
    queue_draw();
    return;
  } else if (m_tool == ActiveTool::Node) {
    on_node_begin(x, y);
  } else if (m_tool == ActiveTool::Measure) {
    on_ruler_begin(x, y);
  } else if (m_tool == ActiveTool::TextOnPath) {
    on_top_begin(x, y);
  } else if (m_tool == ActiveTool::Corner) {
    on_corner_begin(x, y);
  } else if (m_tool == ActiveTool::Pen) {
    on_pen_begin(x, y);
  } else if (m_tool == ActiveTool::Selection) {
    on_select_begin(x, y);
  } else if (m_tool == ActiveTool::Zoom) {
    // Store screen-space anchor for marquee or click zoom
    m_zoom_anchor_x = x;
    m_zoom_anchor_y = y;
    m_drawing = true;  // reuse drawing flag to track drag
    m_draw_cur_dx = x; // repurpose as screen-space current end (not snapped)
    m_draw_cur_dy = y;
  } else if (m_tool == ActiveTool::Eyedropper) {
    // S66 — Phase 3 always-zoom loupe. Every click commits the centre
    // pixel of the magnified buffer sample (the pixel under the crosshair
    // in the loupe). Alt still controls the destination channel:
    //   Alt    -> apply to stroke.
    //   No Alt -> apply to fill.
    // Hit-test fallback retained only for the edge case where the buffer
    // isn't fresh yet (first activation with no hover tick).
    bool sampled_ok = false;
    FillStyle sampled;
    bool to_stroke = m_mod_alt;
    if (m_loupe_buffer_valid) {
      sampled.type = FillStyle::Type::Solid;
      sampled.r = m_loupe_buffer_r;
      sampled.g = m_loupe_buffer_g;
      sampled.b = m_loupe_buffer_b;
      sampled.a = m_loupe_buffer_a;
      sampled_ok = true;
    } else {
      SceneNode *hit = hit_test(dx, dy);
      if (hit) {
        sampled = m_mod_alt ? hit->stroke.paint : hit->fill;
        sampled_ok = true;
      }
    }

    if (sampled_ok) {
      // Apply to every selected object (undoable).
      for (SceneNode *obj : m_selection) {
        FillStyle fb = obj->fill;
        StrokeStyle sb = obj->stroke;
        FillStyle fa = fb;
        StrokeStyle sa = sb;
        if (to_stroke)
          sa.paint = sampled;
        else
          fa = sampled;
        // S82 m4f: capture pre-edit swatch ids; mutate via funnel which
        // clears them as a break-on-override side effect; then read post-
        // mutation ids for the after snapshot. Reordered from push-then-
        // mutate to mutate-then-push so the after snapshot is read from
        // the actual post-funnel state (matches the PropertiesPanel
        // broadcast pattern).
        // S92 m3: same shape for bound_style.
        std::string fsib = obj->fill_swatch_id;
        std::string ssib = obj->stroke_swatch_id;
        std::string bsb  = obj->bound_style;
        // Eyedropper is a user override — route through the funnel so any
        // Style/Swatch binding on this object is broken per addendum
        // invariant.
        style::mutate_appearance(*obj, [&](SceneNode& n) {
          n.fill = fa;
          n.stroke = sa;
        });
        std::string fsia = obj->fill_swatch_id;
        std::string ssia = obj->stroke_swatch_id;
        std::string bsa  = obj->bound_style;
        if (m_history) {
          // s168 m1 DIAG — STRIP after triage (re-added in m5)
          LOG_INFO("[IIDDIAG] EditAppearance::push (eyedropper) "
                   "iid='{}' obj_name='{}' obj_type={}",
                   obj->internal_id, obj->name, (int)obj->type);
          m_history->push(std::make_unique<EditAppearanceCommand>(
              project(), obj->internal_id, fb, sb, fa, sa,
              std::move(fsib), std::move(ssib),
              std::move(fsia), std::move(ssia),
              std::move(bsb), std::move(bsa),
              "Eyedropper"));
        }
      }

      // Notify MainWindow so it can sync the toolbar well.
      m_sig_eyedropper_pick.emit(sampled, to_stroke);

      if (!m_selection.empty()) {
        m_sig_doc_changed.emit();
        queue_draw();
      }
    }
    // Restore the previous tool (like Illustrator — one-shot pick).
    m_sig_request_tool.emit(m_prev_tool);
  } else if (m_tool == ActiveTool::Text) {
    on_text_begin(x, y);
  }
}

void Canvas::on_draw_update(double delta_x, double delta_y) {
  if (!m_doc)
    return;

  // Space pan — intercept before tool routing.
  if (m_space_panning) {
    m_pan_x = m_space_pan_start_x + (m_mouse_x - m_pan_drag_start_x);
    m_pan_y = m_space_pan_start_y + (m_mouse_y - m_pan_drag_start_y);
    clamp_pan();
    queue_draw();
    return;
  }

  // ── Warp envelope MULTI-drag (M4c-2c) ──────────────────────────────
  // Translate every element in m_warp_env_picks by cursor delta from
  // press point. Anchor picks carry both handles; standalone HandleIn/
  // HandleOut picks translate only the corresponding handle. If an
  // anchor's Anchor part is picked AND its handles are separately
  // picked, the handles are carried by the anchor — skip them to avoid
  // double-translation. Writes from snapshot + delta each motion event
  // to prevent accumulation.
  if (m_warp_drag_is_multi && m_selected && m_selected->is_warp()) {
    double mx_doc, my_doc;
    screen_to_doc(m_mouse_x, m_mouse_y, mx_doc, my_doc);
    double dx = mx_doc - m_warp_drag_press_doc_x;
    double dy = my_doc - m_warp_drag_press_doc_y;
    // Start from pre-drag snapshot — prevents cumulative error.
    m_selected->warp_env_top = m_warp_drag_pre_top;
    m_selected->warp_env_bottom = m_warp_drag_pre_bottom;
    // Flat independence: each picked element translates by the same
    // delta. Anchors translate x/y only (do NOT carry handles —
    // handles are independent unless separately picked). Handles
    // translate their own component.
    for (const auto &p : m_warp_env_picks) {
      PathData &env =
          p.is_top ? m_selected->warp_env_top : m_selected->warp_env_bottom;
      if (p.idx < 0 || p.idx >= (int)env.nodes.size())
        continue;
      BezierNode &n = env.nodes[p.idx];
      if (p.part == EnvelopePart::Anchor) {
        n.x += dx;
        n.y += dy;
        // Carry along any handle that was coincident with the anchor
        // in the pre-snapshot. A coincident handle has no independent
        // visual identity — stranding it at the old anchor position
        // produces visible "ghost dots" after the drag. Separated
        // handles are left alone (flat independence). Threshold
        // matches the one used by select_all for visibility gating.
        const PathData &pre =
            p.is_top ? m_warp_drag_pre_top : m_warp_drag_pre_bottom;
        if (p.idx < (int)pre.nodes.size()) {
          const BezierNode &prn = pre.nodes[p.idx];
          if (std::hypot(prn.cx1 - prn.x, prn.cy1 - prn.y) <= 1e-6) {
            n.cx1 += dx;
            n.cy1 += dy;
          }
          if (std::hypot(prn.cx2 - prn.x, prn.cy2 - prn.y) <= 1e-6) {
            n.cx2 += dx;
            n.cy2 += dy;
          }
        }
      } else if (p.part == EnvelopePart::HandleIn) {
        n.cx1 += dx;
        n.cy1 += dy;
      } else if (p.part == EnvelopePart::HandleOut) {
        n.cx2 += dx;
        n.cy2 += dy;
      }
    }
    m_selected->warp_cache_dirty = true;
    queue_draw();
    return;
  }

  // ── Warp envelope drag (M4b) ────────────────────────────────────────
  // Live writethrough to the envelope PathData. Anchor drags move the
  // anchor AND translate both handles by the same delta (so the tangent
  // shape is preserved). Handle drags move one handle independently by
  // default (M4c-2 revision: handles are independent from anchors and
  // from each other unless explicitly locked). Shift-drag re-enables
  // mirror-across-anchor for Smooth tangent continuity.
  if (m_warp_drag_kind != WarpDragKind::None && m_selected &&
      m_selected->is_warp()) {
    PathData &env = m_warp_drag_is_top ? m_selected->warp_env_top
                                       : m_selected->warp_env_bottom;
    if (m_warp_drag_idx < 0 || m_warp_drag_idx >= (int)env.nodes.size()) {
      // Drag target went away (undo mid-drag? shouldn't happen) — abort.
      m_warp_drag_kind = WarpDragKind::None;
      return;
    }
    BezierNode &n = env.nodes[m_warp_drag_idx];
    double mx_doc, my_doc;
    screen_to_doc(m_mouse_x, m_mouse_y, mx_doc, my_doc);
    double target_x = mx_doc - m_warp_drag_click_offset_x;
    double target_y = my_doc - m_warp_drag_click_offset_y;
    switch (m_warp_drag_kind) {
    case WarpDragKind::Anchor: {
      double dx = target_x - n.x;
      double dy = target_y - n.y;
      n.x += dx;
      n.y += dy;
      // Separated handles stay independent (flat independence). Coincident
      // handles (invisibly hiding under the anchor) ride along so they
      // don't get stranded when the anchor moves. Detected from pre-press
      // snapshot so jitter on live drag doesn't falsely separate them.
      const PathData &pre =
          m_warp_drag_is_top ? m_warp_drag_pre_top : m_warp_drag_pre_bottom;
      if (m_warp_drag_idx < (int)pre.nodes.size()) {
        const BezierNode &prn = pre.nodes[m_warp_drag_idx];
        if (std::hypot(prn.cx1 - prn.x, prn.cy1 - prn.y) <= 1e-6) {
          n.cx1 += dx;
          n.cy1 += dy;
        }
        if (std::hypot(prn.cx2 - prn.x, prn.cy2 - prn.y) <= 1e-6) {
          n.cx2 += dx;
          n.cy2 += dy;
        }
      }
      break;
    }
    case WarpDragKind::HandleIn: {
      n.cx1 = target_x;
      n.cy1 = target_y;
      if (m_mod_shift) {
        // Shift-drag: mirror cx2/cy2 across the anchor. Length matches
        // original cx2 distance, direction mirrors cx1 vector — classic
        // Smooth tangent continuity, opt-in.
        double vx = n.x - n.cx1;
        double vy = n.y - n.cy1;
        double v_len = std::hypot(vx, vy);
        double orig_cx2_dx = n.cx2 - n.x;
        double orig_cx2_dy = n.cy2 - n.y;
        double orig_len = std::hypot(orig_cx2_dx, orig_cx2_dy);
        if (v_len > 1e-9) {
          double scale = orig_len / v_len;
          n.cx2 = n.x + vx * scale;
          n.cy2 = n.y + vy * scale;
        }
      }
      break;
    }
    case WarpDragKind::HandleOut: {
      n.cx2 = target_x;
      n.cy2 = target_y;
      if (m_mod_shift) {
        double vx = n.x - n.cx2;
        double vy = n.y - n.cy2;
        double v_len = std::hypot(vx, vy);
        double orig_cx1_dx = n.cx1 - n.x;
        double orig_cx1_dy = n.cy1 - n.y;
        double orig_len = std::hypot(orig_cx1_dx, orig_cx1_dy);
        if (v_len > 1e-9) {
          double scale = orig_len / v_len;
          n.cx1 = n.x + vx * scale;
          n.cy1 = n.y + vy * scale;
        }
      }
      break;
    }
    case WarpDragKind::None:
      break;
    }
    m_selected->warp_cache_dirty = true;
    queue_draw();
    return;
  }

  // Pivot drag — update custom pivot position.
  if (m_pivot_dragging) {
    double dx, dy;
    screen_to_doc(m_mouse_x, m_mouse_y, dx, dy);
    m_custom_pivot_x = snap(dx);
    m_custom_pivot_y = snap(dy);
    queue_draw();
    return;
  }

  // SnR pivot drag — update SR preview pivot + fire callback to dialog.
  if (m_sr_pivot_dragging) {
    double dx, dy;
    screen_to_doc(m_mouse_x, m_mouse_y, dx, dy);
    m_sr_preview_x = dx;
    m_sr_preview_y = dy;
    if (m_sr_pivot_change_cb)
      m_sr_pivot_change_cb(dx, dy);
    queue_draw();
    return;
  }

  if (m_drawing && m_tool == ActiveTool::Zoom) {
    // Track current mouse in screen space for marquee preview
    m_draw_cur_dx = m_mouse_x;
    m_draw_cur_dy = m_mouse_y;
    queue_draw();
  } else if (m_drawing) {
    double ex, ey;
    screen_to_doc(m_mouse_x, m_mouse_y, ex, ey);
    m_draw_cur_dx = snap(ex);
    m_draw_cur_dy = snap(ey);
    // Shift/Alt constrain for Rect and Ellipse:
    //   Shift      — square (equal W and H)
    //   Alt        — draw from center (start point is center)
    //   Shift+Alt  — square from center
    if (m_tool == ActiveTool::Rect || m_tool == ActiveTool::Ellipse) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      if (m_mod_shift) {
        double sz = std::max(std::abs(dw), std::abs(dh));
        dw = std::copysign(sz, dw);
        dh = std::copysign(sz, dh);
        m_draw_cur_dx = m_draw_start_dx + dw;
        m_draw_cur_dy = m_draw_start_dy + dh;
      }
      if (m_mod_alt) {
        // Mirror cur through start — start becomes center
        m_draw_start_effective_dx =
            m_draw_start_dx - (m_draw_cur_dx - m_draw_start_dx);
        m_draw_start_effective_dy =
            m_draw_start_dy - (m_draw_cur_dy - m_draw_start_dy);
      } else {
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      }
    }

    // ── Polygon update ─────────────────────────────────────────────────
    // Alt  = draw from center (start is center, radius = distance to cur)
    // Shift = snap rotation to nearest 15°
    if (m_tool == ActiveTool::Polygon) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      // Compute angle from start to current for rotation
      double angle = std::atan2(dh, dw);
      if (m_mod_shift) {
        // Snap to nearest 15°
        double step = M_PI / 12.0;
        angle = std::round(angle / step) * step;
      }
      m_poly_drag_angle = angle - M_PI * 0.5; // offset so point is up at 0°
      if (m_mod_alt) {
        // Center = start, radius = distance to cur
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      } else {
        // Center = midpoint of drag
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      }
    }
    // ── Spiral update ──────────────────────────────────────────────────
    // Same drag convention as Polygon: drag distance = outer radius,
    // drag direction = rotation angle of outer tip.
    // Shift = snap rotation to 15°
    if (m_tool == ActiveTool::Spiral) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double angle = std::atan2(dh, dw);
      if (m_mod_shift) {
        double step = M_PI / 12.0;
        angle = std::round(angle / step) * step;
      }
      m_spiral_drag_angle = angle - M_PI * 0.5; // tip points up at 0°
    }

    // Shift constrain for Line: snap angle to nearest 45°
    if (m_tool == ActiveTool::Line && m_mod_shift) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        m_draw_cur_dx = m_draw_start_dx + len * std::cos(snapped);
        m_draw_cur_dy = m_draw_start_dy + len * std::sin(snapped);
      }
    }
    // Ref tool: drag moves an existing ref point
    if (m_tool == ActiveTool::Ref && m_ref_selected) {
      double ex, ey;
      screen_to_doc(m_mouse_x, m_mouse_y, ex, ey);
      m_ref_selected->ref_x = snap(ex) - m_ref_drag_ox;
      m_ref_selected->ref_y = snap(ey) - m_ref_drag_oy;
      // s177: drag changes position; the refpt's name is the
      // user's, untouched. Pre-s177 every drag stomped the name
      // with new "%.6f_%.6f" coords, throwing away any rename.
      m_sig_doc_changed.emit();
    }
    // Corner tool: update rubber-band endpoint
    if (m_tool == ActiveTool::Corner && m_corner_rubber_active) {
      m_corner_rubber_x1 = m_mouse_x;
      m_corner_rubber_y1 = m_mouse_y;
    }
    queue_draw();
  } else if (m_tool == ActiveTool::Pen) {
    double ddx, ddy;
    screen_to_doc(m_mouse_x, m_mouse_y, ddx, ddy);
    m_pen_tool.mods.alt = m_mod_alt;
    m_pen_tool.mods.shift = m_mod_shift;
    m_pen_tool.on_drag({ddx, ddy});
    queue_draw();
  } else if (m_tool == ActiveTool::Line && m_line_tool.active()) {
    double ex, ey;
    screen_to_doc(m_mouse_x, m_mouse_y, ex, ey);
    ex = snap(ex);
    ey = snap(ey);
    // 15° angle snap when Shift held (24 increments around the circle)
    if (m_mod_shift) {
      auto [lx, ly] = m_line_tool.points.back();
      double dw = ex - lx, dh = ey - ly;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        ex = lx + len * std::cos(snapped);
        ey = ly + len * std::sin(snapped);
      }
    }
    // Close snap check
    double tol = 8.0 / m_zoom;
    auto [sx, sy] = m_line_tool.points[0];
    m_line_tool.close_snap = (std::hypot(ex - sx, ey - sy) <= tol);
    if (m_line_tool.close_snap) {
      ex = sx;
      ey = sy;
    }
    m_line_tool.live_x = ex;
    m_line_tool.live_y = ey;
    queue_draw();
  } else if (m_tool == ActiveTool::Node) {
    on_node_update(delta_x, delta_y);
  } else if (m_tool == ActiveTool::Measure) {
    on_ruler_motion(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::TextOnPath) {
    // TOP drag does not set m_drawing, so m_draw_cur_dx/dy are never
    // updated in the m_drawing branch above. Convert mouse position fresh.
    double tdx, tdy;
    screen_to_doc(m_mouse_x, m_mouse_y, tdx, tdy);
    on_top_motion(tdx, tdy);
  } else if (m_tool == ActiveTool::Corner) {
    on_corner_motion(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::Selection) {
    on_select_update(delta_x, delta_y);
  }
}

void Canvas::on_draw_end(double delta_x, double delta_y) {
  if (!m_doc)
    return;

  // Space pan — finish without any tool action.
  if (m_space_panning) {
    m_space_panning = false;
    // Restore grab cursor (space still held) or the tool default.
    set_cursor(m_space_held ? "grab" : "default");
    return;
  }

  // ── Warp envelope drag commit (M4b single / M4c-2c multi) ──────────
  // Push EditWarpCommand with the captured pre-state and current
  // post-state. One command per drag — successive drags are distinct
  // undo steps. No time-coalescing; could add later.
  //
  // Multi-drag edge case: if the user clicked on a pick-set member but
  // didn't actually drag, the envelope is unchanged. Don't push a no-op
  // undo; instead collapse the pick set to just that element (matches
  // Illustrator/Affinity muscle memory: click-without-drag on a selected
  // item replaces selection with just that one).
  if (m_warp_drag_kind != WarpDragKind::None && m_selected &&
      m_selected->is_warp()) {
    // Detect no-motion by comparing envelopes to pre-snapshot.
    auto env_equal = [](const PathData &a, const PathData &b) {
      if (a.nodes.size() != b.nodes.size())
        return false;
      for (size_t i = 0; i < a.nodes.size(); ++i) {
        const auto &n1 = a.nodes[i];
        const auto &n2 = b.nodes[i];
        if (n1.x != n2.x || n1.y != n2.y || n1.cx1 != n2.cx1 ||
            n1.cy1 != n2.cy1 || n1.cx2 != n2.cx2 || n1.cy2 != n2.cy2)
          return false;
      }
      return true;
    };
    bool no_motion =
        env_equal(m_selected->warp_env_top, m_warp_drag_pre_top) &&
        env_equal(m_selected->warp_env_bottom, m_warp_drag_pre_bottom);
    if (m_warp_drag_is_multi && no_motion) {
      // Click-without-drag on a pick-set member → collapse set to just this.
      EnvelopePart part = (m_warp_drag_kind == WarpDragKind::HandleIn)
                              ? EnvelopePart::HandleIn
                          : (m_warp_drag_kind == WarpDragKind::HandleOut)
                              ? EnvelopePart::HandleOut
                              : EnvelopePart::Anchor;
      m_warp_env_picks.clear();
      m_warp_env_picks.push_back({m_warp_drag_is_top, m_warp_drag_idx, part});
      m_warp_env_picks_owner = m_selected;
    } else if (!no_motion) {
      // s147 m2: drag drift clears preset_idx — envelope no longer
      // matches the preset shape. Set live BEFORE snapshotting post,
      // so post_preset_idx (which we pass below as
      // m_selected->warp_preset_idx via the default field read) is
      // -1, and undo restores both the original envelope AND the
      // pre-drag preset label atomically.
      m_selected->warp_preset_idx = -1;
      if (m_history) {
        m_history->push(std::make_unique<EditWarpCommand>(
            project(), m_selected->internal_id,
            m_warp_drag_pre_top, m_warp_drag_pre_bottom,
            m_warp_drag_pre_quality, m_selected->warp_env_top,
            m_selected->warp_env_bottom, m_selected->warp_quality,
            m_warp_drag_pre_preset_idx, /*post_preset=*/-1));
      }
    }
    m_warp_drag_kind = WarpDragKind::None;
    m_warp_drag_idx = -1;
    m_warp_drag_is_multi = false;
    m_warp_drag_press_doc_x = 0.0;
    m_warp_drag_press_doc_y = 0.0;
    queue_draw();
    return;
  }

  // Pivot drag — finish; pivot is already set in m_custom_pivot_x/y.
  if (m_pivot_dragging) {
    m_pivot_dragging = false;
    set_cursor("crosshair"); // R still held
    queue_draw();
    return;
  }

  // SnR pivot drag — finish; SR preview already live.
  if (m_sr_pivot_dragging) {
    m_sr_pivot_dragging = false;
    queue_draw();
    return;
  }

  if (m_drawing && m_tool == ActiveTool::Zoom) {
    m_drawing = false;
    double ex = m_draw_cur_dx; // screen-space end (stored in draw_cur)
    double ey = m_draw_cur_dy;
    double dist = std::hypot(ex - m_zoom_anchor_x, ey - m_zoom_anchor_y);
    if (dist < 5.0) {
      // Ctrl+click — fit canvas to window
      if (m_mod_ctrl) {
        zoom_fit();
      } else {
        // Click — zoom in 2× (or out with Alt)
        double factor = m_mod_alt ? 0.5 : 2.0;
        zoom_toward(m_zoom_anchor_x, m_zoom_anchor_y, factor);
      }
    } else {
      // Marquee drag
      if (m_mod_alt) {
        // Alt+drag — zoom OUT: the marquee represents the area of the
        // screen into which the current canvas view should be shrunk.
        // Equivalent to zooming out by the ratio of viewport to marquee.
        double rw = std::abs(ex - m_zoom_anchor_x);
        double rh = std::abs(ey - m_zoom_anchor_y);
        double vw = static_cast<double>(get_width());
        double vh = static_cast<double>(get_height());
        if (rw > 4 && rh > 4) {
          double factor = std::min(rw / vw, rh / vh);
          double cx = (m_zoom_anchor_x + ex) * 0.5;
          double cy = (m_zoom_anchor_y + ey) * 0.5;
          zoom_toward(cx, cy, factor);
        }
      } else {
        // Normal drag — zoom IN to the marquee rect
        zoom_to_rect(m_zoom_anchor_x, m_zoom_anchor_y, ex, ey);
      }
    }
    queue_draw();
    return;
  }

  if (m_drawing && m_tool == ActiveTool::Line) {
    // Line tool no longer uses m_drawing — this branch is dead but kept for
    // safety
    m_drawing = false;
    queue_draw();
    return;
  }

  // Corner tool — end rubber-band or click, never commits a shape
  if (m_drawing && m_tool == ActiveTool::Corner) {
    m_drawing = false;
    on_corner_end(m_draw_cur_dx, m_draw_cur_dy);
    return;
  }

  // Ref tool: mouse-up — place new point or finish drag
  if (m_drawing && m_tool == ActiveTool::Ref) {
    m_drawing = false;
    double dx2, dy2;
    screen_to_doc(m_mouse_x, m_mouse_y, dx2, dy2);
    double px = snap(dx2), py = snap(dy2);

    if (m_ref_selected) {
      // Finished dragging existing ref point
      // s168 m4: push RefMoveCommand if the drag actually moved the
      // refpt. Pre-s168 this drag mutated ref_x/ref_y in place with
      // no undo entry — Ctrl+Z would skip past the drag entirely. The
      // 0.001 epsilon matches the path-move threshold at line ~3329.
      if (m_history) {
        double cur_x = m_ref_selected->ref_x;
        double cur_y = m_ref_selected->ref_y;
        if (std::abs(cur_x - m_ref_drag_orig_x) > 0.001 ||
            std::abs(cur_y - m_ref_drag_orig_y) > 0.001) {
          LOG_INFO("[IIDDIAG] RefMove::push (ref-tool drag) "
                   "iid='{}' obj_name='{}' obj_type={}  "
                   "before=({},{}) after=({},{})",
                   m_ref_selected->internal_id, m_ref_selected->name,
                   (int)m_ref_selected->type,
                   m_ref_drag_orig_x, m_ref_drag_orig_y, cur_x, cur_y);
          m_history->push(std::make_unique<RefMoveCommand>(
              project(), m_ref_selected->internal_id,
              m_ref_drag_orig_x, m_ref_drag_orig_y, cur_x, cur_y));
        }
      }
      m_ref_selected = nullptr;
      m_sig_doc_changed.emit();
    } else {
      // No existing ref hit — place new ref point on mouse-up
      double tol = 3.0 / m_zoom;
      bool is_click =
          (std::hypot(px - m_draw_start_dx, py - m_draw_start_dy) < tol);
      if (is_click && m_doc) {
        SceneNode *rl = m_doc->ensure_ref_layer();

        double rx = m_draw_start_dx, ry = m_draw_start_dy;

        auto ref = std::make_unique<SceneNode>();
        ref->type = SceneNode::Type::Ref;
        ref->id = next_id();
        // s177: route refpt naming through the doc-wide funnel —
        // "Ref 1", "Ref 2", ... — instead of the pre-s177
        // "%.6f_%.6f" coordinate-as-name. Coords as a name violated
        // the "no derived strings in user-facing UI" rule and were
        // stomped on every drag, throwing away any user rename.
        ref->name = m_doc->next_default_name(
            CurvzDocument::NameKind::Ref);
        ref->ref_x = rx;
        ref->ref_y = ry;

        rl->children.insert(rl->children.begin(), clone_node(*ref));
        m_selected = rl->children.front().get();
        m_selection = {m_selected};  // s159 m2: sync for SelectionContext

        if (m_history)
          m_history->push(std::make_unique<AddNodeCommand>(
              rl, clone_node(*rl->children.front())));

        notify_object_selection_changed();
        m_sig_doc_changed.emit();
        LOG_INFO("Ref point placed at ({:.4f},{:.4f})", rx, ry);
      }
    }
    queue_draw();
    return;
  }

  // Line tool double-click detection: on_draw_begin already placed a new point,
  // so back() is the just-placed point. A true double-click means the cursor is
  // near the PREVIOUS point (second-to-last), i.e. the user clicked the same
  // spot twice. We pop the duplicate and commit.
  if (m_tool == ActiveTool::Line && m_line_tool.points.size() >= 3) {
    double dx2, dy2;
    screen_to_doc(m_mouse_x, m_mouse_y, dx2, dy2);
    // Compare against second-to-last (the point placed on the previous click)
    auto [lx, ly] = m_line_tool.points[m_line_tool.points.size() - 2];
    double tol = 6.0 / m_zoom;
    if (std::hypot(dx2 - lx, dy2 - ly) <= tol) {
      // Double-click — remove the duplicate last point and commit open
      m_line_tool.points.pop_back();
      commit_line_path();
      return;
    }
  }

  if (m_drawing) {
    m_drawing = false;

    // Re-apply Shift/Alt constrains at commit (mirrors on_draw_update)
    if (m_tool == ActiveTool::Rect || m_tool == ActiveTool::Ellipse) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      if (m_mod_shift) {
        double sz = std::max(std::abs(dw), std::abs(dh));
        dw = std::copysign(sz, dw);
        dh = std::copysign(sz, dh);
        m_draw_cur_dx = m_draw_start_dx + dw;
        m_draw_cur_dy = m_draw_start_dy + dh;
      }
      if (m_mod_alt) {
        m_draw_start_effective_dx =
            m_draw_start_dx - (m_draw_cur_dx - m_draw_start_dx);
        m_draw_start_effective_dy =
            m_draw_start_dy - (m_draw_cur_dy - m_draw_start_dy);
      } else {
        m_draw_start_effective_dx = m_draw_start_dx;
        m_draw_start_effective_dy = m_draw_start_dy;
      }
    }

    // ── Polygon commit ─────────────────────────────────────────────────
    if (m_tool == ActiveTool::Polygon) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double radius = std::hypot(dw, dh);
      if (radius < 2.0) {
        queue_draw();
        return;
      }

      // Center: Alt = start is center, else start is edge of bounding circle
      double cx, cy;
      if (m_mod_alt) {
        cx = m_draw_start_dx;
        cy = m_draw_start_dy;
      } else {
        // Center is midpoint — drag defines radius and direction
        cx = m_draw_start_dx;
        cy = m_draw_start_dy;
      }

      int sides = m_poly_sides;
      double inflection = m_poly_inflection;

      // Snap inflection to perfect star ratio cos(π/sides)
      double perfect_star =
          (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                       : -1.0;
      if (perfect_star > 0.0 && std::abs(inflection - perfect_star) < 0.04)
        inflection = perfect_star;
      // Snap to full polygon
      if (inflection > 0.985)
        inflection = 1.0;

      SceneNode obj;
      obj.id = next_id();
      obj.internal_id = last_iid();
      obj.name = (inflection >= 1.0)
                     ? m_doc->next_default_name(CurvzDocument::NameKind::Polygon)
                     : m_doc->next_default_name(CurvzDocument::NameKind::Star);
      obj.type = SceneNode::Type::Path;
      obj.path = std::make_unique<PathData>(polygon_to_path(
          cx, cy, radius, sides, inflection, m_poly_drag_angle));
      style::mutate_appearance(obj, [this](SceneNode& n) {
        n.fill = m_def_fill;
        n.stroke = m_def_stroke;
      });

      if (!m_doc->layers.empty()) {
        SceneNode *layer = m_doc->active_layer();
        if (!layer)
          layer = m_doc->layers[0].get();
        layer->children.insert(layer->children.begin(), clone_node(obj));
        m_selected = layer->children.front().get();
        if (m_history) {
          auto cmd = std::make_unique<AddNodeCommand>(layer, clone_node(obj));
          m_history->push(std::move(cmd));
        }
      }
      m_selection.clear();
      if (m_selected)
        m_selection.push_back(m_selected);
      notify_object_selection_changed();
      m_sig_request_tool.emit(ActiveTool::Selection);
      m_sig_doc_changed.emit();
      queue_draw();
      LOG_INFO("Polygon placed: cx={:.1f} cy={:.1f} r={:.1f} sides={} "
               "inflect={:.3f}",
               cx, cy, radius, sides, inflection);
      return;
    }

    // ── Spiral commit ──────────────────────────────────────────────────
    if (m_tool == ActiveTool::Spiral) {
      double dw = m_draw_cur_dx - m_draw_start_dx;
      double dh = m_draw_cur_dy - m_draw_start_dy;
      double outer_r = std::hypot(dw, dh);
      if (outer_r < 2.0) {
        queue_draw();
        return;
      }

      double cx = m_draw_start_dx;
      double cy = m_draw_start_dy;
      double inner_r = outer_r * (m_spiral_inner / 100.0);

      SceneNode obj;
      obj.id = next_id();
      obj.internal_id = last_iid();
      obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Spiral);
      obj.type = SceneNode::Type::Path;
      obj.path = std::make_unique<PathData>(
          spiral_to_path(cx, cy, outer_r, inner_r, m_spiral_turns,
                         m_spiral_drag_angle));
      style::mutate_appearance(obj, [this](SceneNode& n) {
        n.fill = m_def_fill;
        n.stroke = m_def_stroke;
      });

      SceneNode *layer = m_doc->active_layer();
      if (!layer)
        layer = m_doc->layers[0].get();
      layer->children.insert(layer->children.begin(), clone_node(obj));
      m_selected = layer->children.front().get();
      if (m_history)
        m_history->push(
            std::make_unique<AddNodeCommand>(layer, clone_node(obj)));

      m_selection.clear();
      if (m_selected)
        m_selection.push_back(m_selected);
      notify_object_selection_changed();
      m_sig_request_tool.emit(ActiveTool::Selection);
      m_sig_doc_changed.emit();
      queue_draw();
      LOG_INFO(
          "Spiral placed: cx={:.1f} cy={:.1f} r={:.1f} turns={:.1f}",
          cx, cy, outer_r, m_spiral_turns);
      return;
    }

    double x1 = std::min(m_draw_start_effective_dx, m_draw_cur_dx);
    double y1 = std::min(m_draw_start_effective_dy, m_draw_cur_dy);
    double x2 = std::max(m_draw_start_effective_dx, m_draw_cur_dx);
    double y2 = std::max(m_draw_start_effective_dy, m_draw_cur_dy);
    double w = x2 - x1;
    double h = y2 - y1;

    // Minimum 1px in document space
    if (w < 0.5 || h < 0.5) {
      queue_draw();
      return;
    }

    SceneNode obj;
    obj.id = next_id();
    obj.internal_id = last_iid();

    obj.type = SceneNode::Type::Path;
    if (m_tool == ActiveTool::Rect) {
      obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Rectangle);
      obj.path = std::make_unique<PathData>(rect_to_path(x1, y1, w, h));
      LOG_INFO("Rect placed: ({:.1f},{:.1f}) {:.1f}x{:.1f}", x1, y1, w, h);
    } else {
      obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Ellipse);
      double ecx = x1 + w * 0.5, ecy = y1 + h * 0.5;
      obj.path = std::make_unique<PathData>(
          ellipse_to_path(ecx, ecy, w * 0.5, h * 0.5));
      LOG_INFO("Ellipse placed: cx={:.1f} cy={:.1f} rx={:.1f} ry={:.1f}", ecx,
               ecy, w * 0.5, h * 0.5);
    }

    // Apply project-wide defaults from ContextBar
    style::mutate_appearance(obj, [this](SceneNode& n) {
      n.fill = m_def_fill;
      n.stroke = m_def_stroke;
    });

    // Add to active layer via command
    if (!m_doc->layers.empty()) {
      SceneNode *layer = m_doc->active_layer();
      if (!layer)
        layer = m_doc->layers[0].get();
      // Pre-insert at front so new object appears at top of layer list
      layer->children.insert(layer->children.begin(), clone_node(obj));
      m_selected = layer->children.front().get();
      m_selection = {m_selected};  // s159 m2: sync for SelectionContext

      if (m_history) {
        auto cmd = std::make_unique<AddNodeCommand>(layer, clone_node(obj));
        // Don't call execute() — object already inserted above
        m_history->push(std::move(cmd));
      }
    }

    notify_object_selection_changed();
    m_sig_request_tool.emit(ActiveTool::Selection);
    m_sig_doc_changed.emit();
    queue_draw();
  } else if (m_tool == ActiveTool::Pen) {
    double ddx, ddy;
    screen_to_doc(m_mouse_x, m_mouse_y, ddx, ddy);
    m_pen_tool.on_release({ddx, ddy});
    if (m_pen_closing) {
      m_pen_closing = false;
      commit_pen_path();
    }
    queue_draw();
  } else if (m_tool == ActiveTool::Node) {
    on_node_end();
  } else if (m_tool == ActiveTool::Measure) {
    on_ruler_end(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::TextOnPath) {
    on_top_end(m_draw_cur_dx, m_draw_cur_dy);
  } else if (m_tool == ActiveTool::Selection) {
    on_select_end(0, 0);
  }
}

// ── Input: selection tool
// ─────────────────────────────────────────────────────
void Canvas::on_select_begin(double x, double y) {
  double dx, dy;
  screen_to_doc(x, y, dx, dy);

  // ── Clip-pick mode? Intercept before everything else ────────────────
  // When armed, any click is destined for clip-shape selection: either
  // we consume a Path/Compound and build the ClipGroup, or we cancel.
  // Either way we must NOT fall through to normal selection logic —
  // that would mutate m_selection before finish_clip_pick consults the
  // stashed snapshot.
  if (m_clip_pick_armed) {
    SceneNode *hit = hit_test(dx, dy);
    finish_clip_pick(hit);
    return;
  }

  // ── Refpt drag? Check before guide/handle/object — refpts aren't
  //   found by hit_test (skipped via is_special_layer in the regular
  //   layer scan), so without an explicit branch a click on a refpt
  //   falls through to "click on empty" and clears the selection.
  //   Tolerance 8 screen px matches the Ref tool's own hit-test.
  if (m_doc) {
    SceneNode *rl = m_doc->ref_layer();
    if (rl && rl->visible && !rl->locked) {
      SceneNode *hit_ref = nullptr;
      for (auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx, sy;
        doc_to_screen(child->ref_x, child->ref_y, sx, sy);
        if (std::hypot(x - sx, y - sy) <= 8.0) {
          hit_ref = child.get();
          break;
        }
      }
      if (hit_ref) {
        // Selection logic: if the hit refpt is already in m_selection
        // (marquee-then-drag), keep the whole selection so the drag
        // moves all selected refpts. Otherwise replace with single.
        bool already_in_sel = std::find(m_selection.begin(),
                                        m_selection.end(),
                                        hit_ref) != m_selection.end();
        if (!already_in_sel) {
          m_selection.clear();
          m_selection.push_back(hit_ref);
        }
        m_selected = hit_ref;
        // Clear guide selection — selecting a refpt is a foreground
        // pick, mirrors path-object selection's clear at line 8843.
        if (!m_guide_selection.empty()) {
          m_guide_selection.clear();
          m_sig_guide_selection_changed.emit(m_guide_selection);
        }
        // Snapshot refpts in selection for multi-refpt drag. Path /
        // text / image / warp snap lists stay empty — the
        // on_select_update branch checks m_ref_move_snaps separately.
        m_move_snaps.clear();
        m_text_move_snaps.clear();
        m_warp_env_move_snaps.clear();
        m_ref_move_snaps.clear();
        for (SceneNode *o : m_selection) {
          if (o && o->is_ref())
            m_ref_move_snaps.push_back({o, o->ref_x, o->ref_y});
        }
        m_moving = true;
        m_move_start_dx = dx;
        m_move_start_dy = dy;
        m_snap_bias_x = 0.0;
        m_snap_bias_y = 0.0;
        m_snap_x_locked = false;
        m_snap_y_locked = false;
        notify_object_selection_changed();
        queue_draw();
        return;
      }
    }
  }

  // ── Guide drag? Check before handle/object hit test ──────────────────
  if (m_guide_hovered && !m_guide_drag_active) {
    // Record press position — drag starts in on_select_update once
    // the mouse moves beyond GUIDE_DRAG_THRESHOLD_PX.
    m_guide_drag_node = m_guide_hovered;
    m_guide_press_x = x;
    m_guide_press_y = y;

    SceneNode *g = m_guide_hovered;
    bool already = std::find(m_guide_selection.begin(), m_guide_selection.end(),
                             g) != m_guide_selection.end();
    if (m_mod_ctrl) {
      // Ctrl+click: toggle membership
      if (already)
        m_guide_selection.erase(
            std::remove(m_guide_selection.begin(), m_guide_selection.end(), g),
            m_guide_selection.end());
      else
        m_guide_selection.push_back(g);
    } else {
      // Plain click: clear objects + replace guide selection
      if (!m_selection.empty()) {
        m_selection.clear();
        m_selected = nullptr;
        notify_object_selection_changed();
      }
      m_guide_selection = {g};
    }
    m_sig_guide_selection_changed.emit(m_guide_selection);
    queue_draw();
    return;
  }

  // ── Handle drag? Check before object hit test ────────────────────────
  // Only try handles when something is already selected and no shift.
  // M4c-1: Also suppressed when primary selection is a Warp — envelope
  // handles are the manipulation UI; bbox handles gone. Click falls
  // through to hit_test() for object-drag-translate.
  if (!m_selection.empty() && !m_mod_shift &&
      !(m_selected && m_selected->is_warp())) {
    HandleKind hk = handle_hit_test(x, y);
    // In pivot mode, corner scale handles become rotate handles
    if (m_r_held) {
      if (hk == HandleKind::NW)
        hk = HandleKind::RotateNW;
      else if (hk == HandleKind::NE)
        hk = HandleKind::RotateNE;
      else if (hk == HandleKind::SE)
        hk = HandleKind::RotateSE;
      else if (hk == HandleKind::SW)
        hk = HandleKind::RotateSW;
      // Edge-mid handles ignored in pivot mode
      else if (hk == HandleKind::N || hk == HandleKind::S ||
               hk == HandleKind::E || hk == HandleKind::W)
        hk = HandleKind::None;
    }
    if (hk != HandleKind::None) {
      // Compute union BBX of selection
      bool found = false;
      double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
      for (SceneNode *obj : m_selection) {
        auto bb = object_bbox(*obj);
        if (!bb)
          continue;
        if (!found) {
          bx1 = bb->x;
          by1 = bb->y;
          bx2 = bb->x + bb->w;
          by2 = bb->y + bb->h;
          found = true;
        } else {
          bx1 = std::min(bx1, bb->x);
          by1 = std::min(by1, bb->y);
          bx2 = std::max(bx2, bb->x + bb->w);
          by2 = std::max(by2, bb->y + bb->h);
        }
      }
      if (found) {
        m_handle_drag = hk;
        m_handle_start_bb = {bx1, by1, bx2 - bx1, by2 - by1};

        // Pivot = opposite corner (Alt = center)
        if (m_mod_alt) {
          m_handle_pivot_x = bx1 + (bx2 - bx1) * 0.5;
          m_handle_pivot_y = by1 + (by2 - by1) * 0.5;
        } else {
          switch (hk) {
          case HandleKind::NW:
            m_handle_pivot_x = bx2;
            m_handle_pivot_y = by2;
            break;
          case HandleKind::NE:
            m_handle_pivot_x = bx1;
            m_handle_pivot_y = by2;
            break;
          case HandleKind::SE:
            m_handle_pivot_x = bx1;
            m_handle_pivot_y = by1;
            break;
          case HandleKind::SW:
            m_handle_pivot_x = bx2;
            m_handle_pivot_y = by1;
            break;
          default:
            m_handle_pivot_x = (bx1 + bx2) * 0.5;
            m_handle_pivot_y = (by1 + by2) * 0.5;
            break;
          }
        }

        // Snapshot all leaves for undo
        m_scale_snaps.clear();
        m_image_transform_snaps.clear();
        for (SceneNode *obj : m_selection) {
          if (obj->is_image()) {
            m_image_transform_snaps.push_back({obj, obj->image_x, obj->image_y,
                                               obj->image_w, obj->image_h,
                                               obj->transform});
            continue;
          }
          std::vector<SceneNode *> leaves;
          collect_paths(obj, leaves);
          for (SceneNode *leaf : leaves) {
            if (leaf->path && !leaf->path->nodes.empty())
              m_scale_snaps.push_back({leaf, leaf->path->nodes, *leaf->path});
          }
        }

        // For edge-mid handles (N/S/E/W) record drag-start doc pos so we
        // can resolve scale-vs-skew intent once the cursor moves enough.
        bool is_edge_mid = (hk == HandleKind::N || hk == HandleKind::S ||
                            hk == HandleKind::E || hk == HandleKind::W);
        if (is_edge_mid) {
          m_skew_intent_locked = false;
          m_skew_is_skew = false;
          m_skew_start_dx = dx;
          m_skew_start_dy = dy;
        }

        // For rotate kinds, record the starting angle from pivot to cursor
        if (hk == HandleKind::RotateNW || hk == HandleKind::RotateNE ||
            hk == HandleKind::RotateSE || hk == HandleKind::RotateSW) {
          // Use custom pivot if set (rotate-from-point), otherwise
          // BBX center (or Alt = opposite corner).
          double rpx, rpy;
          if (m_has_custom_pivot) {
            rpx = m_custom_pivot_x;
            rpy = m_custom_pivot_y;
          } else if (m_mod_alt) {
            switch (hk) {
            case HandleKind::RotateNW:
              rpx = bx2;
              rpy = by2;
              break;
            case HandleKind::RotateNE:
              rpx = bx1;
              rpy = by2;
              break;
            case HandleKind::RotateSE:
              rpx = bx1;
              rpy = by1;
              break;
            case HandleKind::RotateSW:
              rpx = bx2;
              rpy = by1;
              break;
            default:
              rpx = (bx1 + bx2) * 0.5;
              rpy = (by1 + by2) * 0.5;
              break;
            }
          } else {
            rpx = (bx1 + bx2) * 0.5;
            rpy = (by1 + by2) * 0.5;
          }
          m_handle_pivot_x = rpx;
          m_handle_pivot_y = rpy;
          m_rotate_start_dx = dx;
          m_rotate_start_dy = dy;
        }

        m_moving = false;
        queue_draw();
        return;
      }
    }
  }

  SceneNode *hit = hit_test(dx, dy);

  // ── Ctrl+click: select-behind, wrap-around cycle within one layer ────
  // Build a cycle of all objects at the click point in m_selected's
  // parent layer, starting from the topmost (whatever hit_test returned)
  // and descending. Rotate one position forward from m_selected's slot
  // in the cycle — wrapping to the top if m_selected is at the bottom.
  //
  // Gate does NOT require hit == m_selected. After a previous Ctrl+click
  // rotation, m_selected may be the middle or bottom rect, but hit_test
  // still returns the topmost. Requiring equality would break repeated
  // Ctrl+clicks into a back-and-forth between top and second-from-top.
  //
  // Plain click on a selected object falls through to start a move.
  // Shift+click still does additive selection.
  // Alt is excluded because Ctrl+Alt+click is the align-anchor toggle
  // (handled below in the if(hit) block) — without this guard the cycle
  // would consume the click first when only one object is selected.
  if (m_mod_ctrl && !m_mod_shift && !m_mod_alt && hit && m_selected &&
      m_selection.size() == 1) {
    // Find m_selected's parent layer.
    SceneNode *sel_layer = nullptr;
    for (auto &layer : m_doc->layers) {
      for (auto &ch : layer->children) {
        if (ch.get() == m_selected) {
          sel_layer = layer.get();
          break;
        }
      }
      if (sel_layer)
        break;
    }
    if (sel_layer) {
      // Helper: is node a direct child of sel_layer?
      auto in_sel_layer = [&](SceneNode *n) -> bool {
        for (auto &ch : sel_layer->children)
          if (ch.get() == n)
            return true;
        return false;
      };

      // Start the cycle from the topmost hit. If that hit is outside
      // sel_layer, the cycle isn't meaningful for our selection — skip.
      std::vector<SceneNode *> cycle;
      if (in_sel_layer(hit)) {
        cycle.push_back(hit);
        SceneNode *prev = hit;
        while (SceneNode *n = hit_test_next(dx, dy, prev)) {
          if (!in_sel_layer(n))
            break; // layer boundary
          if (std::find(cycle.begin(), cycle.end(), n) != cycle.end())
            break; // cycle closed
          cycle.push_back(n);
          prev = n;
        }
      }

      // Rotate forward from m_selected's position (wrap to front if past
      // the end). If m_selected isn't in the cycle, fall back to the top.
      if (cycle.size() > 1) {
        auto it = std::find(cycle.begin(), cycle.end(), m_selected);
        if (it == cycle.end()) {
          hit = cycle.front();
        } else {
          auto next_it = std::next(it);
          if (next_it == cycle.end())
            next_it = cycle.begin();
          hit = *next_it;
        }
      }
      // If cycle.size() <= 1, nothing else to cycle to — hit stays.
    }
  }

  if (hit) {
    // ── Ctrl+Alt+click: toggle align anchor on hit object ──────────
    // Selection-time mark for the next Align op. Validator-on-read
    // (align_anchor()) clears it the moment the marked object leaves
    // m_selection, so we only need to set it here. Selects the object
    // first if it isn't already selected, so a single Ctrl+Alt+click
    // on an unselected object both selects AND anchors. Toggling: a
    // second Ctrl+Alt+click on the same anchored object clears the
    // anchor (object stays selected). Distribute ops ignore the anchor.
    //
    // Why not plain Alt+click: that's already taken by the Alt+drag
    // duplicate-in-place idiom (see m_alt_drag_dup below). Ctrl+Alt
    // pairs cleanly because the Ctrl+click select-behind cycle gates
    // on m_selection.size() == 1 and is naturally orthogonal here.
    if (m_mod_alt && m_mod_ctrl && !m_mod_shift) {
      // Add to selection if not already there.
      if (!is_selected(hit)) {
        m_selection.push_back(hit);
        m_selected = hit;
      }
      // Toggle anchor: if hit is already the anchor, clear; else set.
      // Read through the validator so a stale pointer (e.g. anchor
      // pointed to a deleted object) is normalised before compare.
      SceneNode *cur = align_anchor();
      m_align_anchor = (cur == hit) ? nullptr : hit;
      m_moving = false;
      m_move_snaps.clear();
      notify_object_selection_changed();
      queue_draw();
      return;
    }

    if (m_mod_shift) {
      // ── Shift+click: toggle object in/out of selection ────────────
      auto it = std::find(m_selection.begin(), m_selection.end(), hit);
      if (it != m_selection.end()) {
        m_selection.erase(it);
        m_selected = m_selection.empty() ? nullptr : m_selection.front();
      } else {
        m_selection.push_back(hit);
        m_selected = hit;
      }
      m_moving = false;
      m_move_snaps.clear();
      notify_object_selection_changed();
      queue_draw();
      return;
    }

    // ── Plain click: select this object ──────────────────────────────
    // Selecting an object clears guide selection
    if (!m_guide_selection.empty()) {
      m_guide_selection.clear();
      m_sig_guide_selection_changed.emit(m_guide_selection);
    }
    // If clicking an already-selected object in a multi-selection, keep
    // the selection and start moving; otherwise replace selection.
    bool already_selected = is_selected(hit);
    if (!already_selected) {
      m_selection.clear();
      m_selection.push_back(hit);
      // PTT coupled selection: also add the pair partner so both move together
      SceneNode *partner = top_pair_partner(hit);
      if (partner && !is_selected(partner))
        m_selection.push_back(partner);
      // New selection → reset custom pivot
      m_has_custom_pivot = false;
      m_pivot_dragging = false;
    }
    m_selected = hit;
    m_moving = true;
    m_move_start_dx = dx;
    m_move_start_dy = dy;
    m_snap_bias_x = 0.0;
    m_snap_bias_y = 0.0;
    m_snap_x_locked = false;
    m_snap_y_locked = false;

    // ── Alt+drag: duplicate in-place, then move the clones ───────────
    // The originals stay where they are; we immediately insert clones
    // at the same position and redirect the move to the clones.
    m_alt_drag_dup = false;
    if (m_mod_alt && !m_selection.empty()) {
      auto entries = collect_selection_entries(m_doc, m_selection);
      if (!entries.empty()) {
        // s170 m3 — iid-based capture: Entry stores parent_iid (string)
        // instead of a raw SceneNode*, push passes project() so
        // execute()/undo() can resolve via find_by_iid.
        std::vector<DuplicateCommand::Entry> cmd_entries;
        std::vector<SceneNode *> clone_sel;
        int id_counter = s_next_id;
        int shift = 0;
        for (auto &e : entries) {
          auto dup = clone_node(*e.node);
          freshen_ids(dup.get(), m_doc, id_counter);
          // No position offset — clone is placed exactly on top
          int ins = e.index + shift;
          auto snap = clone_node(*dup);
          clone_sel.push_back(dup.get());
          e.parent->children.insert(e.parent->children.begin() + ins,
                                    std::move(dup));
          cmd_entries.push_back({e.parent ? e.parent->internal_id : std::string(),
                                 std::move(snap), ins});
          ++shift;
        }
        s_next_id = id_counter;
        if (m_history)
          m_history->push(
              std::make_unique<DuplicateCommand>(project(), std::move(cmd_entries)));
        // Redirect selection to clones — the move will operate on them
        m_selection = clone_sel;
        m_selected = clone_sel.empty() ? nullptr : clone_sel[0];
        m_alt_drag_dup = true;
      }
    }

    // Snapshot all selected objects for multi-move
    m_move_snaps.clear();
    m_text_move_snaps.clear();
    m_warp_env_move_snaps.clear();
    // Helper — recursively find Warps in a subtree so nested Warps
    // (inside a selected Group/Compound) also have their envelopes
    // snapshotted for drag-translate.
    std::function<void(SceneNode *)> collect_warps = [&](SceneNode *n) {
      if (!n)
        return;
      if (n->is_warp()) {
        m_warp_env_move_snaps.push_back(
            {n, n->warp_env_top, n->warp_env_bottom});
      }
      for (auto &c : n->children)
        collect_warps(c.get());
      if (n->is_warp()) {
        // Don't recurse into warp_source/cache — they aren't Warps,
        // they're the source/derived geometry. (Nested Warps
        // are not supported at this time, but children-walk above
        // would catch them if/when they are.)
      }
    };
    for (SceneNode *obj : m_selection) {
      if (obj->is_text()) {
        m_text_move_snaps.push_back({obj, obj->text_x, obj->text_y});
        continue;
      }
      if (obj->is_image()) {
        m_text_move_snaps.push_back({obj, obj->image_x, obj->image_y});
        continue;
      }
      collect_warps(obj);
      std::vector<SceneNode *> leaves;
      collect_paths(obj, leaves);
      for (SceneNode *leaf : leaves) {
        if (!leaf->path->nodes.empty())
          m_move_snaps.push_back({leaf, leaf->path->nodes, *leaf->path});
      }
      // Nested Text/Image inside containers (Group/Compound/ClipGroup) —
      // collect_paths skips them by design (no Path geometry), so collect
      // them separately and snapshot their position fields for drag-move.
      if (obj->type == SceneNode::Type::Group ||
          obj->type == SceneNode::Type::Compound ||
          obj->type == SceneNode::Type::ClipGroup) {
        std::vector<SceneNode *> ti_leaves;
        collect_text_image_leaves(obj, ti_leaves);
        for (SceneNode *leaf : ti_leaves) {
          if (leaf->is_text())
            m_text_move_snaps.push_back({leaf, leaf->text_x, leaf->text_y});
          else if (leaf->is_image())
            m_text_move_snaps.push_back({leaf, leaf->image_x, leaf->image_y});
        }
      }
    }
    // Legacy single-object fields (used by undo in on_select_end)
    if (hit->type == SceneNode::Type::Path && hit->path &&
        !hit->path->nodes.empty()) {
      m_move_orig_nodes = hit->path->nodes;
      m_move_before_path = *hit->path;
    } else {
      m_move_orig_nodes.clear();
    }

    notify_object_selection_changed();
    LOG_DEBUG("Selected object {} (total selected: {})", hit->id,
              m_selection.size());

  } else {
    // M4c-2d: When primary is a Warp, click/Shift+click on empty starts
    // an envelope marquee (rubber-band anchor pick). Release handler
    // picks anchors inside. Plain → replaces set; Shift → additive.
    // Must run before the general "Shift+click-empty does nothing" rule
    // so Shift+marquee can start.
    if (m_selected && m_selected->is_warp()) {
      m_marquee_active = true;
      m_marquee_start_dx = dx;
      m_marquee_start_dy = dy;
      m_marquee_cur_dx = dx;
      m_marquee_cur_dy = dy;
      m_warp_env_marquee_additive = m_mod_shift;
      queue_draw();
      return;
    }

    if (m_mod_shift) {
      // Shift+click on empty — keep existing selection, do nothing
      queue_draw();
      return;
    }

    // ── Click on empty: clear selection and guide selection, start marquee
    if (!m_guide_selection.empty()) {
      m_guide_selection.clear();
      m_sig_guide_selection_changed.emit(m_guide_selection);
    }
    m_selected = nullptr;
    m_selection.clear();
    m_moving = false;
    m_move_snaps.clear();
    m_warp_env_move_snaps.clear();
    m_move_orig_nodes.clear();
    // Deselect → reset custom pivot
    m_has_custom_pivot = false;
    m_pivot_dragging = false;
    m_marquee_active = true;
    m_marquee_start_dx = dx;
    m_marquee_start_dy = dy;
    m_marquee_cur_dx = dx;
    m_marquee_cur_dy = dy;
    notify_object_selection_changed();
  }
  queue_draw();
}

void Canvas::on_select_update(double /*dx*/, double /*dy*/) {
  // ── Guide drag ────────────────────────────────────────────────────────
  // If a guide node is armed (press recorded) but drag not yet active,
  // start the drag once the mouse moves beyond the threshold.
  if (!m_guide_drag_active && m_guide_drag_node) {
    double dist =
        std::hypot(m_mouse_x - m_guide_press_x, m_mouse_y - m_guide_press_y);
    if (dist >= GUIDE_DRAG_THRESHOLD_PX)
      m_guide_drag_active = true;
  }
  if (m_guide_drag_active && m_guide_drag_node) {
    if (m_guide_drag_node->locked) {
      // Guide is locked — swallow the drag with no position change.
      queue_draw();
      return;
    }
    double cur_dx, cur_dy;
    screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);
    // Axis-aligned guides: slide along the varying axis only so the
    // perpendicular position is preserved.  Angled guides: move the whole
    // anchor to follow the cursor (anchor slides along the perpendicular).
    if (m_guide_drag_node->guide_is_horizontal()) {
      m_guide_drag_node->guide_y = cur_dy;
    } else if (m_guide_drag_node->guide_is_vertical()) {
      m_guide_drag_node->guide_x = cur_dx;
    } else {
      m_guide_drag_node->guide_x = cur_dx;
      m_guide_drag_node->guide_y = cur_dy;
    }
    queue_draw();
    return;
  }

  // ── Handle drag ───────────────────────────────────────────────────────
  if (m_handle_drag != HandleKind::None) {
    double cur_dx, cur_dy;
    screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);

    // ── Rotate drag ───────────────────────────────────────────────────
    bool is_rotate = (m_handle_drag == HandleKind::RotateNW ||
                      m_handle_drag == HandleKind::RotateNE ||
                      m_handle_drag == HandleKind::RotateSE ||
                      m_handle_drag == HandleKind::RotateSW);
    if (is_rotate) {
      // Pivot is locked at drag-start (m_handle_pivot_x/y set in
      // on_select_begin). Recomputing live would cause a jump if Alt is toggled
      // mid-drag.
      const double px = m_handle_pivot_x;
      const double py = m_handle_pivot_y;

      double start_angle =
          std::atan2(m_rotate_start_dy - py, m_rotate_start_dx - px);
      double cur_angle = std::atan2(cur_dy - py, cur_dx - px);
      double delta = cur_angle - start_angle;

      // Shift = snap to 15° increments
      if (m_mod_shift) {
        constexpr double SNAP = M_PI / 12.0; // 15°
        delta = std::round(delta / SNAP) * SNAP;
      }

      m_last_rotate_angle_deg = delta * (180.0 / M_PI);

      double cosA = std::cos(delta);
      double sinA = std::sin(delta);

      for (auto &snap : m_scale_snaps) {
        if (!snap.obj->path)
          continue;
        auto &nodes = snap.obj->path->nodes;
        const auto &orig = snap.orig_nodes;
        if (nodes.size() != orig.size())
          continue;
        for (size_t i = 0; i < nodes.size(); ++i) {
          auto rot = [&](double ox, double oy, double &rx, double &ry) {
            double rx0 = ox - px, ry0 = oy - py;
            rx = px + rx0 * cosA - ry0 * sinA;
            ry = py + rx0 * sinA + ry0 * cosA;
          };
          rot(orig[i].x, orig[i].y, nodes[i].x, nodes[i].y);
          rot(orig[i].cx1, orig[i].cy1, nodes[i].cx1, nodes[i].cy1);
          rot(orig[i].cx2, orig[i].cy2, nodes[i].cx2, nodes[i].cy2);
        }
      }
      // Rotate image nodes — rotate centre point, update transform matrix
      for (auto &isnap : m_image_transform_snaps) {
        auto &obj = *isnap.obj;
        // Image centre in doc space
        double icx = isnap.orig_x + isnap.orig_w * 0.5;
        double icy = isnap.orig_y + isnap.orig_h * 0.5;
        // Rotate centre around pivot
        double rx0 = icx - px, ry0 = icy - py;
        double new_cx = px + rx0 * cosA - ry0 * sinA;
        double new_cy = py + rx0 * sinA + ry0 * cosA;
        obj.image_x = new_cx - isnap.orig_w * 0.5;
        obj.image_y = new_cy - isnap.orig_h * 0.5;
        obj.image_w = isnap.orig_w;
        obj.image_h = isnap.orig_h;
        // Accumulate rotation in transform: R_new = R(delta) * R_orig
        // Store as rotation angle in transform
        // (a=cos,b=sin,c=-sin,d=cos,e=0,f=0) relative to image centre (applied
        // during draw around image centre)
        double orig_angle =
            std::atan2(isnap.orig_transform.b, isnap.orig_transform.a);
        double new_angle = orig_angle + delta;
        obj.transform.a = std::cos(new_angle);
        obj.transform.b = std::sin(new_angle);
        obj.transform.c = -std::sin(new_angle);
        obj.transform.d = std::cos(new_angle);
        obj.transform.e = 0.0;
        obj.transform.f = 0.0;
      }
      queue_draw();
      return;
    }

    // ── Scale / Skew drag (edge-mid handles N/S/E/W) ─────────────────
    bool is_edge_mid =
        (m_handle_drag == HandleKind::N || m_handle_drag == HandleKind::S ||
         m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W);

    if (is_edge_mid && !m_skew_intent_locked) {
      // Resolve intent once the drag exceeds 12px from start (screen space).
      // Require a 2:1 dominance ratio before committing — pure jitter near
      // the axis boundary stays ambiguous and keeps waiting.
      double dsx, dsy, ssx, ssy;
      doc_to_screen(cur_dx, cur_dy, dsx, dsy);
      doc_to_screen(m_skew_start_dx, m_skew_start_dy, ssx, ssy);
      double adx = std::abs(dsx - ssx);
      double ady = std::abs(dsy - ssy);
      double total = adx + ady;
      if (total > 12.0) {
        // E/W: ady dominant → skew, adx dominant → scale
        // N/S: adx dominant → skew, ady dominant → scale
        bool vert_dominant = (ady > adx * 1.5);
        bool horiz_dominant = (adx > ady * 1.5);
        bool resolved =
            (m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W)
                ? (vert_dominant || horiz_dominant)
                : (horiz_dominant || vert_dominant);
        if (resolved) {
          m_skew_intent_locked = true;
          if (m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W)
            m_skew_is_skew = vert_dominant;
          else
            m_skew_is_skew = horiz_dominant;
        }
      }
    }

    // ── Skew drag ─────────────────────────────────────────────────────
    if (is_edge_mid && m_skew_intent_locked && m_skew_is_skew) {
      const BBoxF &sb = m_handle_start_bb;
      double shear = 0.0;
      bool horiz_shear =
          (m_handle_drag == HandleKind::N || m_handle_drag == HandleKind::S);

      if (horiz_shear) {
        // N/S horizontal drag → horizontal shear: x' = x + (y - anchor_y) * k
        // Default: opposite edge is anchor (stays fixed).
        // Alt: BBX center is anchor (symmetric shear from middle).
        double anchor_y;
        double span;
        if (m_mod_alt) {
          anchor_y = sb.y + sb.h * 0.5;
          span = sb.h * 0.5; // half-height: each edge moves half as much
        } else {
          // N dragged: bottom edge locked (sb.y + sb.h)
          // S dragged: top edge locked (sb.y)
          anchor_y = (m_handle_drag == HandleKind::N) ? sb.y + sb.h : sb.y;
          span = sb.h;
        }
        if (std::abs(span) > 1e-6)
          shear = -(cur_dx - m_skew_start_dx) / span;
        for (auto &snap : m_scale_snaps) {
          if (!snap.obj->path)
            continue;
          auto &nodes = snap.obj->path->nodes;
          const auto &orig = snap.orig_nodes;
          if (nodes.size() != orig.size())
            continue;
          for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].x = orig[i].x + (orig[i].y - anchor_y) * shear;
            nodes[i].y = orig[i].y;
            nodes[i].cx1 = orig[i].cx1 + (orig[i].cy1 - anchor_y) * shear;
            nodes[i].cy1 = orig[i].cy1;
            nodes[i].cx2 = orig[i].cx2 + (orig[i].cy2 - anchor_y) * shear;
            nodes[i].cy2 = orig[i].cy2;
          }
        }
      } else {
        // E/W vertical drag → vertical shear: y' = y + (x - anchor_x) * k
        // Default: opposite edge is anchor (stays fixed).
        // Alt: BBX center is anchor (symmetric shear from middle).
        double anchor_x;
        double span;
        if (m_mod_alt) {
          anchor_x = sb.x + sb.w * 0.5;
          span = sb.w * 0.5;
        } else {
          // E dragged: left edge locked (sb.x)
          // W dragged: right edge locked (sb.x + sb.w)
          anchor_x = (m_handle_drag == HandleKind::E) ? sb.x : sb.x + sb.w;
          span = sb.w;
        }
        if (std::abs(span) > 1e-6)
          shear = (cur_dy - m_skew_start_dy) / span;
        for (auto &snap : m_scale_snaps) {
          if (!snap.obj->path)
            continue;
          auto &nodes = snap.obj->path->nodes;
          const auto &orig = snap.orig_nodes;
          if (nodes.size() != orig.size())
            continue;
          for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].x = orig[i].x;
            nodes[i].y = orig[i].y + (orig[i].x - anchor_x) * shear;
            nodes[i].cx1 = orig[i].cx1;
            nodes[i].cy1 = orig[i].cy1 + (orig[i].cx1 - anchor_x) * shear;
            nodes[i].cx2 = orig[i].cx2;
            nodes[i].cy2 = orig[i].cy2 + (orig[i].cx2 - anchor_x) * shear;
          }
        }
      }
      // Skew image nodes via transform matrix shear
      for (auto &isnap : m_image_transform_snaps) {
        auto &obj = *isnap.obj;
        obj.image_x = isnap.orig_x;
        obj.image_y = isnap.orig_y;
        obj.image_w = isnap.orig_w;
        obj.image_h = isnap.orig_h;
        // Preserve any existing rotation, add shear
        double ca = isnap.orig_transform.a, sa = isnap.orig_transform.b;
        if (std::abs(ca) < 1e-10 && std::abs(sa) < 1e-10) {
          ca = 1.0;
          sa = 0.0;
        }
        if (horiz_shear) {
          // Horizontal shear: x' = x + k*y
          obj.transform.a = ca;
          obj.transform.b = sa;
          obj.transform.c = -sa + shear * ca;
          obj.transform.d = ca + shear * sa;
        } else {
          // Vertical shear: y' = y + k*x
          obj.transform.a = ca + shear * (-sa);
          obj.transform.b = sa + shear * ca;
          obj.transform.c = -sa;
          obj.transform.d = ca;
        }
        obj.transform.e = 0.0;
        obj.transform.f = 0.0;
      }
      queue_draw();
      return;
    }

    // ── Scale drag ────────────────────────────────────────────────────
    const BBoxF &sb = m_handle_start_bb;

    // Avoid degenerate BBX
    if (sb.w < 1e-6 || sb.h < 1e-6)
      return;

    // Recompute pivot live so Alt can be toggled mid-drag.
    // Alt = scale from center; default = opposite edge/corner.
    double px, py;
    if (m_mod_alt) {
      px = sb.x + sb.w * 0.5;
      py = sb.y + sb.h * 0.5;
    } else {
      switch (m_handle_drag) {
      case HandleKind::NW:
        px = sb.x + sb.w;
        py = sb.y + sb.h;
        break;
      case HandleKind::NE:
        px = sb.x;
        py = sb.y + sb.h;
        break;
      case HandleKind::SE:
        px = sb.x;
        py = sb.y;
        break;
      case HandleKind::SW:
        px = sb.x + sb.w;
        py = sb.y;
        break;
      // Edge mids: pivot is opposite edge midpoint
      case HandleKind::N:
        px = sb.x + sb.w * 0.5;
        py = sb.y + sb.h;
        break;
      case HandleKind::S:
        px = sb.x + sb.w * 0.5;
        py = sb.y;
        break;
      case HandleKind::E:
        px = sb.x;
        py = sb.y + sb.h * 0.5;
        break;
      case HandleKind::W:
        px = sb.x + sb.w;
        py = sb.y + sb.h * 0.5;
        break;
      default:
        px = sb.x + sb.w * 0.5;
        py = sb.y + sb.h * 0.5;
        break;
      }
    }

    // Original handle position and constrained scale axes.
    // Edge mids scale one axis only (sx or sy = 1.0 on the fixed axis).
    double orig_hx = 0, orig_hy = 0;
    bool scale_x = true, scale_y = true;
    switch (m_handle_drag) {
    case HandleKind::NW:
      orig_hx = sb.x;
      orig_hy = sb.y;
      break;
    case HandleKind::NE:
      orig_hx = sb.x + sb.w;
      orig_hy = sb.y;
      break;
    case HandleKind::SE:
      orig_hx = sb.x + sb.w;
      orig_hy = sb.y + sb.h;
      break;
    case HandleKind::SW:
      orig_hx = sb.x;
      orig_hy = sb.y + sb.h;
      break;
    case HandleKind::N:
      orig_hx = sb.x + sb.w * 0.5;
      orig_hy = sb.y;
      scale_x = false;
      break;
    case HandleKind::S:
      orig_hx = sb.x + sb.w * 0.5;
      orig_hy = sb.y + sb.h;
      scale_x = false;
      break;
    case HandleKind::E:
      orig_hx = sb.x + sb.w;
      orig_hy = sb.y + sb.h * 0.5;
      scale_y = false;
      break;
    case HandleKind::W:
      orig_hx = sb.x;
      orig_hy = sb.y + sb.h * 0.5;
      scale_y = false;
      break;
    default:
      return;
    }

    double dx_from_pivot = orig_hx - px;
    double dy_from_pivot = orig_hy - py;

    // ── Snap the dragged handle (S98) ────────────────────────────────────
    // The handle's destination in doc space is (cur_dx, cur_dy). We snap
    // that point to nearest grid/guide/margin/ref via snap_x / snap_y
    // BEFORE deriving sx / sy, so the snap target ends up exactly on
    // the dragged edge. Pivot (px, py) is unaffected — it's the anchor.
    //
    // Per-axis: edge-mid handles (N/S/E/W) only scale one axis; only
    // snap the axis that's actually moving. Shift (uniform scale) is
    // handled below — we snap each axis independently here, then collapse
    // to a single scale factor under Shift.
    //
    // Tolerance matches the single-point snap idiom in snap_x / snap_y
    // (12 px); snap_move's engage/release hysteresis isn't a fit because
    // there's no equivalent of a "moving away from snapped" continuous
    // delta in scale — each frame is a fresh snap candidate.
    double snap_dx = cur_dx;
    double snap_dy = cur_dy;
    if (scale_x) snap_dx = snap_x(cur_dx, /*tolerance_px=*/12.0);
    if (scale_y) snap_dy = snap_y(cur_dy, /*tolerance_px=*/12.0);

    // Compute raw scale on each active axis, from the snapped destination.
    double sx = 1.0, sy = 1.0;
    if (scale_x && std::abs(dx_from_pivot) > 1e-6)
      sx = (snap_dx - px) / dx_from_pivot;
    if (scale_y && std::abs(dy_from_pivot) > 1e-6)
      sy = (snap_dy - py) / dy_from_pivot;

    // Shift on corner handles = uniform scale (largest delta wins).
    // After snap each axis may have its own target; "largest delta wins"
    // picks one, which is the right behaviour — the user gets a snap on
    // the dominant axis, with the other axis matching that scale.
    if (m_mod_shift && scale_x && scale_y) {
      double s = (std::abs(sx - 1.0) >= std::abs(sy - 1.0)) ? sx : sy;
      sx = sy = s;
    }

    // Apply to all leaves from their original snapshots
    for (auto &snap : m_scale_snaps) {
      if (!snap.obj->path)
        continue;
      auto &nodes = snap.obj->path->nodes;
      const auto &orig = snap.orig_nodes;
      if (nodes.size() != orig.size())
        continue;
      for (size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].x = px + (orig[i].x - px) * sx;
        nodes[i].y = py + (orig[i].y - py) * sy;
        nodes[i].cx1 = px + (orig[i].cx1 - px) * sx;
        nodes[i].cy1 = py + (orig[i].cy1 - py) * sy;
        nodes[i].cx2 = px + (orig[i].cx2 - px) * sx;
        nodes[i].cy2 = py + (orig[i].cy2 - py) * sy;
      }
    }
    // Scale image nodes — move top-left and resize
    for (auto &isnap : m_image_transform_snaps) {
      auto &obj = *isnap.obj;
      // Original corners
      double x1 = isnap.orig_x, y1 = isnap.orig_y;
      double x2 = x1 + isnap.orig_w, y2 = y1 + isnap.orig_h;
      // Scale corners from pivot
      double nx1 = px + (x1 - px) * sx;
      double ny1 = py + (y1 - py) * sy;
      double nx2 = px + (x2 - px) * sx;
      double ny2 = py + (y2 - py) * sy;
      obj.image_x = std::min(nx1, nx2);
      obj.image_y = std::min(ny1, ny2);
      obj.image_w = std::abs(nx2 - nx1);
      obj.image_h = std::abs(ny2 - ny1);
      // Preserve existing transform rotation/shear — only position/size change
      obj.transform = isnap.orig_transform;
    }
    queue_draw();
    return;
  }

  // ── Marquee update ────────────────────────────────────────────────────
  if (m_marquee_active) {
    double cx, cy;
    screen_to_doc(m_mouse_x, m_mouse_y, cx, cy);
    m_marquee_cur_dx = cx;
    m_marquee_cur_dy = cy;
    queue_draw();
    return;
  }

  if (!m_moving || m_selection.empty())
    return;

  double cur_dx, cur_dy;
  screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);

  double raw_dx = cur_dx - m_move_start_dx;
  double raw_dy = cur_dy - m_move_start_dy;
  auto [delta_x, delta_y] = snap_move(raw_dx, raw_dy);

  // Move all selected objects by the same delta
  for (auto &snap_data : m_move_snaps) {
    SceneNode *obj = snap_data.obj;
    if (obj->type != SceneNode::Type::Path || !obj->path)
      continue;
    if (snap_data.orig_nodes.size() != obj->path->nodes.size())
      continue;
    for (size_t i = 0; i < obj->path->nodes.size(); ++i) {
      auto &nd = obj->path->nodes[i];
      const auto &orig = snap_data.orig_nodes[i];
      nd.x = orig.x + delta_x;
      nd.y = orig.y + delta_y;
      nd.cx1 = orig.cx1 + delta_x;
      nd.cy1 = orig.cy1 + delta_y;
      nd.cx2 = orig.cx2 + delta_x;
      nd.cy2 = orig.cy2 + delta_y;
    }
  }
  // Move Text and Image nodes
  for (auto &tsnap : m_text_move_snaps) {
    if (tsnap.obj->is_image()) {
      tsnap.obj->image_x = tsnap.orig_x + delta_x;
      tsnap.obj->image_y = tsnap.orig_y + delta_y;
    } else {
      tsnap.obj->text_x = tsnap.orig_x + delta_x;
      tsnap.obj->text_y = tsnap.orig_y + delta_y;
    }
  }
  // Move ref points — same per-point translation as text/image.
  for (auto &rsnap : m_ref_move_snaps) {
    if (rsnap.obj && rsnap.obj->is_ref()) {
      rsnap.obj->ref_x = rsnap.orig_x + delta_x;
      rsnap.obj->ref_y = rsnap.orig_y + delta_y;
    }
  }
  // Translate Warp envelopes alongside source geometry so the whole
  // Warp moves as a unit (envelope + source + derived caches).
  // C++17: structured bindings can't be captured by lambdas, so copy
  // delta_x/delta_y into plain locals for the inner translate lambda.
  const double dxl = delta_x;
  const double dyl = delta_y;
  for (auto &wsnap : m_warp_env_move_snaps) {
    SceneNode *w = wsnap.warp;
    if (!w || !w->is_warp())
      continue;
    w->warp_env_top = wsnap.orig_env_top;
    w->warp_env_bottom = wsnap.orig_env_bottom;
    auto translate_env = [dxl, dyl](PathData &env) {
      for (auto &n : env.nodes) {
        n.x += dxl;
        n.y += dyl;
        n.cx1 += dxl;
        n.cy1 += dyl;
        n.cx2 += dxl;
        n.cy2 += dyl;
      }
    };
    translate_env(w->warp_env_top);
    translate_env(w->warp_env_bottom);
    w->warp_cache_dirty = true;
  }
  queue_draw();
}

void Canvas::on_select_end(double /*dx*/, double /*dy*/) {
  // ── Guide drag end ────────────────────────────────────────────────────
  if (m_guide_drag_node) {
    if (m_guide_drag_active && !m_guide_drag_node->locked) {
      // Actual drag — commit position (new-model only).
      double cur_dx, cur_dy;
      screen_to_doc(m_mouse_x, m_mouse_y, cur_dx, cur_dy);
      if (m_guide_drag_node->guide_is_horizontal()) {
        m_guide_drag_node->guide_y = cur_dy;
      } else if (m_guide_drag_node->guide_is_vertical()) {
        m_guide_drag_node->guide_x = cur_dx;
      } else {
        m_guide_drag_node->guide_x = cur_dx;
        m_guide_drag_node->guide_y = cur_dy;
      }
      m_sig_doc_changed.emit();
    }
    // Either way, clear armed state
    m_guide_drag_node = nullptr;
    m_guide_drag_active = false;
    queue_draw();
    return;
  }

  // ── Handle scale drag end — push undo ─────────────────────────────────
  if (m_handle_drag != HandleKind::None) {
    bool was_rotate = (m_handle_drag == HandleKind::RotateNW ||
                       m_handle_drag == HandleKind::RotateNE ||
                       m_handle_drag == HandleKind::RotateSE ||
                       m_handle_drag == HandleKind::RotateSW);
    bool was_skew = m_skew_intent_locked && m_skew_is_skew;
    m_handle_drag = HandleKind::None;
    m_skew_intent_locked = false;
    m_skew_is_skew = false;
    if (m_history) {
      std::vector<ScaleObjectsCommand::LeafSnap> snaps;
      for (auto &snap : m_scale_snaps) {
        SceneNode *obj = snap.obj;
        if (!obj->path)
          continue;
        bool changed = false;
        for (size_t i = 0;
             i < obj->path->nodes.size() && i < snap.orig_nodes.size(); ++i) {
          if (std::abs(obj->path->nodes[i].x - snap.orig_nodes[i].x) > 1e-6 ||
              std::abs(obj->path->nodes[i].y - snap.orig_nodes[i].y) > 1e-6) {
            changed = true;
            break;
          }
        }
        if (changed)
          snaps.push_back({obj->internal_id, snap.before_path, *obj->path});
      }
      std::string desc = was_rotate
                             ? "Rotate object"
                             : (was_skew ? "Skew object" : "Scale object");
      if (!snaps.empty())
        m_history->push(std::make_unique<ScaleObjectsCommand>(
            project(), std::move(snaps), std::move(desc)));
      // Image transform undo
      std::vector<ScaleImageCommand::Snap> img_snaps;
      for (auto &isnap : m_image_transform_snaps) {
        auto &obj = *isnap.obj;
        bool changed =
            std::abs(obj.image_x - isnap.orig_x) > 1e-6 ||
            std::abs(obj.image_y - isnap.orig_y) > 1e-6 ||
            std::abs(obj.image_w - isnap.orig_w) > 1e-6 ||
            std::abs(obj.image_h - isnap.orig_h) > 1e-6 ||
            std::abs(obj.transform.a - isnap.orig_transform.a) > 1e-6 ||
            std::abs(obj.transform.b - isnap.orig_transform.b) > 1e-6;
        if (changed)
          img_snaps.push_back({obj.internal_id, isnap.orig_x, isnap.orig_y,
                               isnap.orig_w, isnap.orig_h,
                               isnap.orig_transform, obj.image_x, obj.image_y,
                               obj.image_w, obj.image_h, obj.transform});
      }
      if (!img_snaps.empty())
        m_history->push(
            std::make_unique<ScaleImageCommand>(
                project(), std::move(img_snaps), desc));
    }
    // ── Capture scale factor before clearing snaps ────────────────────
    double rec_scale_x = 1.0, rec_scale_y = 1.0;
    if (!was_rotate && !was_skew && !m_scale_snaps.empty()) {
      // Compute from first path snap: ratio of after-bbox to before-bbox
      double bx1b = 1e9, by1b = 1e9, bx2b = -1e9, by2b = -1e9;
      double bx1a = 1e9, by1a = 1e9, bx2a = -1e9, by2a = -1e9;
      for (auto &sn : m_scale_snaps) {
        for (auto &nd : sn.before_path.nodes) {
          bx1b = std::min(bx1b, nd.x);
          by1b = std::min(by1b, nd.y);
          bx2b = std::max(bx2b, nd.x);
          by2b = std::max(by2b, nd.y);
        }
        if (sn.obj->path)
          for (auto &nd : sn.obj->path->nodes) {
            bx1a = std::min(bx1a, nd.x);
            by1a = std::min(by1a, nd.y);
            bx2a = std::max(bx2a, nd.x);
            by2a = std::max(by2a, nd.y);
          }
      }
      double wb = bx2b - bx1b, hb = by2b - by1b;
      double wa = bx2a - bx1a, ha = by2a - by1a;
      if (wb > 1e-6)
        rec_scale_x = wa / wb;
      if (hb > 1e-6)
        rec_scale_y = ha / hb;
    }

    m_scale_snaps.clear();
    m_image_transform_snaps.clear();
    m_sig_doc_changed.emit();
    notify_object_selection_changed();
    queue_draw();
    // ── Record macro step ─────────────────────────────────────────────
    if (was_rotate) {
      if (std::abs(m_last_rotate_angle_deg) > 0.001) {
        MacroStep s;
        s.op = MacroStep::Op::Rotate;
        s.angle_deg = m_last_rotate_angle_deg;
        s.pivot_x = m_handle_pivot_x;
        s.pivot_y = m_handle_pivot_y;
        s.pivot_is_explicit = true;
        record_step_if_recording(s);
      }
    } else if (!was_skew) {
      if (std::abs(rec_scale_x - 1.0) > 0.001 ||
          std::abs(rec_scale_y - 1.0) > 0.001) {
        MacroStep s;
        s.op = MacroStep::Op::Scale;
        s.scale_x = rec_scale_x;
        s.scale_y = rec_scale_y;
        record_step_if_recording(s);
      }
    }
    return;
  }

  // ── Marquee end — select all intersecting objects ─────────────────────
  if (m_marquee_active) {
    m_marquee_active = false;
    double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx);
    double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy);
    double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx);
    double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy);

    // M4c-2d: Warp-primary marquee — pick envelope anchors inside rect.
    // Handles NOT picked by marquee (per spec — must be Shift-clicked).
    if (m_selected && m_selected->is_warp()) {
      bool additive = m_warp_env_marquee_additive;
      m_warp_env_marquee_additive = false;
      if (x2 - x1 > 1.0 && y2 - y1 > 1.0) {
        // Meaningful drag → pick anchors inside rect.
        if (!additive) {
          m_warp_env_picks.clear();
        }
        m_warp_env_picks_owner = m_selected;
        auto scan_env = [&](const PathData &env, bool is_top) {
          auto try_add = [&](EnvelopePick pk) {
            if (std::find(m_warp_env_picks.begin(), m_warp_env_picks.end(),
                          pk) == m_warp_env_picks.end())
              m_warp_env_picks.push_back(pk);
          };
          for (int i = 0; i < (int)env.nodes.size(); ++i) {
            const BezierNode &n = env.nodes[i];
            // Anchor
            if (n.x >= x1 && n.x <= x2 && n.y >= y1 && n.y <= y2)
              try_add({is_top, i, EnvelopePart::Anchor});
            // HandleIn — only pick if visually distinct from anchor
            // (coincident handles are not drawn and can't be aimed at).
            if (std::hypot(n.cx1 - n.x, n.cy1 - n.y) > 1e-6 && n.cx1 >= x1 &&
                n.cx1 <= x2 && n.cy1 >= y1 && n.cy1 <= y2)
              try_add({is_top, i, EnvelopePart::HandleIn});
            // HandleOut — same rule
            if (std::hypot(n.cx2 - n.x, n.cy2 - n.y) > 1e-6 && n.cx2 >= x1 &&
                n.cx2 <= x2 && n.cy2 >= y1 && n.cy2 <= y2)
              try_add({is_top, i, EnvelopePart::HandleOut});
          }
        };
        scan_env(m_selected->warp_env_top, true);
        scan_env(m_selected->warp_env_bottom, false);
      } else {
        // Click-without-drag. Plain click clears picks; Shift click
        // leaves picks alone (matches spec: click-empty clears set,
        // Shift+click-empty is a no-op).
        if (!additive && !m_warp_env_picks.empty()) {
          m_warp_env_picks.clear();
        }
      }
      queue_draw();
      return;
    }

    // Only select if marquee has meaningful size
    if (x2 - x1 > 1.0 && y2 - y1 > 1.0 && m_doc) {
      m_selection.clear();
      // Scan regular layers
      for (auto &layer : m_doc->layers) {
        if (!layer->visible || layer->locked || layer->is_special_layer())
          continue;
        for (auto &obj_uptr : layer->children) {
          SceneNode &obj = *obj_uptr;
          // s125 m1b: the marquee was previously gated to Path-only, which
          // silently filtered out Compounds, Groups, Text, Images, Blends,
          // Warps, ClipGroups — every other user-visible node type. The
          // bug surfaced as "marquee can't select compound objects" but
          // affected all non-Path types. Mirror hit_test's selectable set:
          // these are the node types that count as a single user-visible
          // object (the parent — not its inner children — is what the
          // marquee adds to selection, matching how hit_test returns the
          // Compound/Group/Blend/Warp itself, never its inner nodes).
          // Locked objects (per-object, not per-layer) are still selectable
          // — locked means "no move/delete," not "invisible to selection,"
          // matching the existing RefLayer pass below and how Illustrator
          // and Affinity behave.
          const bool selectable =
              obj.type == SceneNode::Type::Path     ||
              obj.type == SceneNode::Type::Compound ||
              obj.type == SceneNode::Type::Group    ||
              obj.type == SceneNode::Type::ClipGroup||
              obj.type == SceneNode::Type::Text     ||
              obj.type == SceneNode::Type::Image    ||
              obj.type == SceneNode::Type::Blend    ||
              obj.type == SceneNode::Type::Warp;
          if (!selectable)
            continue;
          if (obj.type == SceneNode::Type::Path && !obj.path)
            continue;  // malformed path — nothing to bbox
          if (!obj.visible)
            continue;  // respect per-object visibility (matches hit_test)
          auto bb = object_bbox(obj);
          if (!bb)
            continue;
          if (bb->x < x2 && bb->x + bb->w > x1 && bb->y < y2 &&
              bb->y + bb->h > y1)
            m_selection.push_back(&obj);
        }
      }
      // Also scan RefLayer for ref points inside marquee —
      // selectable even when locked (locked = no move/delete, not no select)
      SceneNode *rl = m_doc->ref_layer();
      if (rl && rl->visible) {
        for (auto &child : rl->children) {
          if (!child->is_ref())
            continue;
          if (child->ref_x >= x1 && child->ref_x <= x2 && child->ref_y >= y1 &&
              child->ref_y <= y2)
            m_selection.push_back(child.get());
        }
      }
      m_selected = m_selection.empty() ? nullptr : m_selection.front();
      // PTT coupled selection: ensure both members of any PTT pair are
      // included if either was caught by the marquee.
      std::vector<SceneNode *> partners;
      for (SceneNode *obj : m_selection) {
        SceneNode *p = top_pair_partner(obj);
        if (p && !is_selected(p) &&
            std::find(partners.begin(), partners.end(), p) == partners.end())
          partners.push_back(p);
      }
      for (SceneNode *p : partners)
        m_selection.push_back(p);
      notify_object_selection_changed();
      LOG_DEBUG("Marquee selected {} objects", m_selection.size());
    }
    queue_draw();
    return;
  }

  if (m_moving) {
    m_moving = false;
    m_snap_bias_x = 0.0;
    m_snap_bias_y = 0.0;
    m_snap_x_locked = false;
    m_snap_y_locked = false;

    // Push undo command for each moved object
    if (m_history) {
      for (auto &snap_data : m_move_snaps) {
        SceneNode *obj = snap_data.obj;
        if (!obj->path || snap_data.orig_nodes.empty())
          continue;
        bool moved = false;
        for (size_t i = 0;
             i < obj->path->nodes.size() && i < snap_data.orig_nodes.size();
             ++i) {
          if (std::abs(obj->path->nodes[i].x - snap_data.orig_nodes[i].x) >
                  0.001 ||
              std::abs(obj->path->nodes[i].y - snap_data.orig_nodes[i].y) >
                  0.001) {
            moved = true;
            break;
          }
        }
        if (moved) {
          PathData after = *obj->path;
          m_history->push(std::make_unique<EditPathCommand>(
              project(), obj->internal_id, snap_data.before_path,
              std::move(after), "Move object"));
        }
      }
      // Text and Image node moves
      for (auto &tsnap : m_text_move_snaps) {
        double cur_x =
            tsnap.obj->is_image() ? tsnap.obj->image_x : tsnap.obj->text_x;
        double cur_y =
            tsnap.obj->is_image() ? tsnap.obj->image_y : tsnap.obj->text_y;
        if (std::abs(cur_x - tsnap.orig_x) > 0.001 ||
            std::abs(cur_y - tsnap.orig_y) > 0.001) {
          m_history->push(std::make_unique<MoveObjectCommand>(
              project(), tsnap.obj->internal_id,
              tsnap.orig_x, tsnap.orig_y, cur_x, cur_y));
        }
      }
      // Warp envelope moves — push EditWarpCommand per moved Warp.
      for (auto &wsnap : m_warp_env_move_snaps) {
        SceneNode *w = wsnap.warp;
        if (!w || !w->is_warp())
          continue;
        bool moved = false;
        if (!w->warp_env_top.nodes.empty() &&
            !wsnap.orig_env_top.nodes.empty()) {
          moved = std::abs(w->warp_env_top.nodes[0].x -
                           wsnap.orig_env_top.nodes[0].x) > 0.001 ||
                  std::abs(w->warp_env_top.nodes[0].y -
                           wsnap.orig_env_top.nodes[0].y) > 0.001;
        }
        if (moved) {
          // s147 m2: translation moves envelope but not warp_source —
          // the renderer's coordinate frame and the preset's natural
          // frame (source bbox) diverge. Picking the same preset
          // again afterwards would teleport the envelope back over
          // the source, which is visually surprising. Treat
          // translation as drift: clear preset_idx so the inspector
          // honestly shows (Custom). Capture pre_preset_idx before
          // clearing so undo restores both position AND preset label.
          int pre_preset = w->warp_preset_idx;
          w->warp_preset_idx = -1;
          m_history->push(std::make_unique<EditWarpCommand>(
              project(), w->internal_id,
              wsnap.orig_env_top, wsnap.orig_env_bottom, w->warp_quality,
              w->warp_env_top, w->warp_env_bottom, w->warp_quality,
              pre_preset, /*post_preset=*/-1));
        }
      }
      // s168 m4: refpt moves — push RefMoveCommand per moved ref. The
      // multi-object drag pattern matches the path / text / warp
      // pushes above (one command per moved object, not a Composite —
      // pre-existing pattern, see line ~3320). 0.001 epsilon matches
      // the path move threshold.
      for (auto &rsnap : m_ref_move_snaps) {
        if (!rsnap.obj || !rsnap.obj->is_ref())
          continue;
        double cur_x = rsnap.obj->ref_x;
        double cur_y = rsnap.obj->ref_y;
        if (std::abs(cur_x - rsnap.orig_x) > 0.001 ||
            std::abs(cur_y - rsnap.orig_y) > 0.001) {
          LOG_INFO("[IIDDIAG] RefMove::push (selection drag) "
                   "iid='{}' obj_name='{}' obj_type={}  "
                   "before=({},{}) after=({},{})",
                   rsnap.obj->internal_id, rsnap.obj->name,
                   (int)rsnap.obj->type,
                   rsnap.orig_x, rsnap.orig_y, cur_x, cur_y);
          m_history->push(std::make_unique<RefMoveCommand>(
              project(), rsnap.obj->internal_id,
              rsnap.orig_x, rsnap.orig_y, cur_x, cur_y));
        }
      }
    }
    // ── Record macro step ─────────────────────────────────────────────
    {
      // Compute delta from first moved path snap (or text/image snap)
      double rdx = 0.0, rdy = 0.0;
      bool found = false;
      for (auto &snap : m_move_snaps) {
        if (!snap.obj->path || snap.orig_nodes.empty() ||
            snap.obj->path->nodes.empty())
          continue;
        rdx = snap.obj->path->nodes[0].x - snap.orig_nodes[0].x;
        rdy = snap.obj->path->nodes[0].y - snap.orig_nodes[0].y;
        found = true;
        break;
      }
      if (!found) {
        for (auto &tsnap : m_text_move_snaps) {
          double cx =
              tsnap.obj->is_image() ? tsnap.obj->image_x : tsnap.obj->text_x;
          double cy =
              tsnap.obj->is_image() ? tsnap.obj->image_y : tsnap.obj->text_y;
          rdx = cx - tsnap.orig_x;
          rdy = cy - tsnap.orig_y;
          found = true;
          break;
        }
      }
      if (!found) {
        for (auto &rsnap : m_ref_move_snaps) {
          if (!rsnap.obj || !rsnap.obj->is_ref())
            continue;
          rdx = rsnap.obj->ref_x - rsnap.orig_x;
          rdy = rsnap.obj->ref_y - rsnap.orig_y;
          found = true;
          break;
        }
      }
      if (found && (std::abs(rdx) > 0.001 || std::abs(rdy) > 0.001)) {
        MacroStep step;
        step.op = MacroStep::Op::Move;
        step.dx = rdx;
        step.dy = rdy;
        record_step_if_recording(step);
      }
    }
    m_move_snaps.clear();
    m_text_move_snaps.clear();
    m_warp_env_move_snaps.clear();
    m_ref_move_snaps.clear();
    m_move_orig_nodes.clear();
    m_sig_doc_changed.emit();
    notify_object_selection_changed();
    LOG_DEBUG("Move end ({} objects)", m_selection.size());
  }
}

// ── Input: motion
// ─────────────────────────────────────────────────────────────
void Canvas::on_motion(double x, double y) {
  m_mouse_x = x;
  m_mouse_y = y;
  double doc_x, doc_y;
  screen_to_doc(x, y, doc_x, doc_y);
  m_cursor_doc_x = doc_x;
  m_cursor_doc_y = doc_y;
  m_sig_cursor.emit(doc_x, doc_y);

  // Guide construct live preview — snap-aware.  Phase 1 follows mouse from
  // the captured p1.  Phase 2 is locked (dialog is open).  Phase 0 just
  // shows the cursor.
  if (m_guide_construct_active && m_guide_construct_phase == 1) {
    double sx = doc_x, sy = doc_y;
    const double tol = 8.0 / m_zoom;
    std::vector<std::pair<SceneNode *, int>> all_nodes;
    ruler_collect_all_path_nodes(all_nodes);
    double best_d = tol;
    for (auto &[obj, ni] : all_nodes) {
      const BezierNode &n = obj->path->nodes[ni];
      double d = std::hypot(n.x - doc_x, n.y - doc_y);
      if (d < best_d) {
        best_d = d;
        sx = n.x;
        sy = n.y;
      }
    }
    m_guide_construct_preview_x = sx;
    m_guide_construct_preview_y = sy;
    queue_draw();
  }

  // ── Guide hover hit-test — runs for all tools ─────────────────────────
  // Test within 5px screen tolerance. Update m_guide_hovered and cursor.
  // Only active when the guide layer is visually above (lower index = on top)
  // the current active layer — if guides are below other layers they are
  // obscured and should not grab the cursor.
  //
  // S66 — Phase 3: skip entirely for Eyedropper. Eyedropper doesn't
  // interact with guides, and the guide-hovered path early-returns out
  // of on_motion, which freezes the loupe buffer and the crosshair cursor
  // whenever the mouse passes within 5px of a guide — the "eyedropper
  // snaps to guide" bug.
  if (m_doc && !m_guide_drag_active && m_tool != ActiveTool::Eyedropper) {
    const SceneNode *gl = m_doc->guide_layer();
    SceneNode *prev_hovered = m_guide_hovered;
    m_guide_hovered = nullptr;

    // Find guide layer index (lower index = drawn on top)
    int guide_idx = -1;
    for (int i = 0; i < (int)m_doc->layers.size(); ++i)
      if (m_doc->layers[i]->is_guide_layer()) {
        guide_idx = i;
        break;
      }

    // Only hover-test when guide layer is on top of (index ≤) active layer
    bool guide_on_top =
        (guide_idx >= 0 && guide_idx <= m_doc->active_layer_index);

    if (gl && gl->visible && !gl->locked && guide_on_top) {
      static constexpr double GUIDE_HIT_PX = 5.0;
      const double ox = doc_origin_x();
      const double oy = doc_origin_y();
      for (auto &child : gl->children) {
        if (!child->is_guide())
          continue;
        double dist;
        if (child->guide_is_horizontal()) {
          double sy = child->guide_y * m_zoom + oy;
          dist = std::abs(y - sy);
        } else if (child->guide_is_vertical()) {
          double sx = child->guide_x * m_zoom + ox;
          dist = std::abs(x - sx);
        } else {
          // Angled guide — perpendicular distance from mouse point to the
          // infinite line through (guide_x, guide_y) at angle guide_angle.
          double ax_s = child->guide_x * m_zoom + ox;
          double ay_s = child->guide_y * m_zoom + oy;
          double a = child->guide_angle * M_PI / 180.0;
          double dxu = std::cos(a);
          double dyu = std::sin(a);
          // Perp distance = |(p - a) × dir| where dir is unit.
          double vx = x - ax_s;
          double vy = y - ay_s;
          dist = std::abs(vx * dyu - vy * dxu);
        }
        if (dist <= GUIDE_HIT_PX) {
          m_guide_hovered = child.get();
          break;
        }
      }
    }
    if (m_guide_hovered != prev_hovered)
      queue_draw();
    if (m_guide_hovered) {
      // Cursor hint: row/col-resize for axis-aligned, default "move" for
      // angled (no single axis meaningful).
      if (m_guide_hovered->guide_is_horizontal())
        set_cursor("row-resize");
      else if (m_guide_hovered->guide_is_vertical())
        set_cursor("col-resize");
      else
        set_cursor("move");
      return; // suppress other cursor logic
    }
    // In node mode, no guide hovered — ensure cursor stays default
    if (m_tool == ActiveTool::Node && prev_hovered && !m_guide_hovered)
      set_cursor("default");
  }

  // ── Eyedropper hover tracking
  // ─────────────────────────────────────────────────────
  if (m_tool == ActiveTool::Eyedropper) {
    // S66 — Phase 3: re-assert the crosshair cursor each tick so nothing
    // upstream (e.g. a future guide/handle/hit-test cursor hint) can leave
    // the cursor stuck on a non-crosshair icon while the eyedropper is
    // active.
    set_cursor("crosshair");
    SceneNode *prev_eye = m_eyedropper_hovered;
    m_eyedropper_hovered = hit_test(doc_x, doc_y);
    if (m_eyedropper_hovered) {
      m_eyedropper_hovered_color = m_mod_alt
                                       ? m_eyedropper_hovered->stroke.paint
                                       : m_eyedropper_hovered->fill;
    }
    // S66 — Phase 3: loupe is always-zoom. Refresh the buffer sample
    // every hover tick so the magnified view follows the cursor.
    refresh_loupe_buffer();
    if (m_eyedropper_hovered != prev_eye)
      queue_draw();
    queue_draw(); // always redraw so loupe follows cursor smoothly
    return;
  }

  // ── Selection tool: cursor changes on handle proximity ────────────────
  if (m_tool == ActiveTool::Selection) {
    // M4c-1: When primary selection is a Warp, bbox handles are gone —
    // don't run the bbox handle cursor resolver (it would otherwise hand
    // out stale nw-resize/grab cursors where bbox handles used to be).
    // Envelope handles draw themselves; cursor stays default over them.
    if (m_selected && m_selected->is_warp()) {
      set_cursor("default");
    } else if (m_handle_drag != HandleKind::None) {
      // During an active edge-mid drag: swap to grab once skew is locked
      bool is_edge_mid =
          (m_handle_drag == HandleKind::N || m_handle_drag == HandleKind::S ||
           m_handle_drag == HandleKind::E || m_handle_drag == HandleKind::W);
      if (is_edge_mid && m_skew_intent_locked && m_skew_is_skew)
        set_cursor("grab");
    } else {
      HandleKind hk = handle_hit_test(x, y);
      if (m_r_held) {
        // Pivot mode cursors:
        // Near pivot crosshair → move cursor (can drag pivot)
        // Near corner handles → grab cursor (can rotate)
        // Elsewhere → crosshair (click sets new pivot)
        bool near_pivot = false;
        if (m_has_custom_pivot) {
          double pvsx, pvsy;
          doc_to_screen(m_custom_pivot_x, m_custom_pivot_y, pvsx, pvsy);
          near_pivot = std::hypot(x - pvsx, y - pvsy) <= 12.0;
        }
        bool is_corner =
            (hk == HandleKind::NW || hk == HandleKind::NE ||
             hk == HandleKind::SE || hk == HandleKind::SW ||
             hk == HandleKind::RotateNW || hk == HandleKind::RotateNE ||
             hk == HandleKind::RotateSE || hk == HandleKind::RotateSW);
        if (near_pivot)
          set_cursor("move");
        else if (is_corner)
          set_cursor("grab");
        else
          set_cursor("crosshair");
      } else {
        switch (hk) {
        case HandleKind::NW:
        case HandleKind::SE:
          set_cursor("nw-resize");
          break;
        case HandleKind::NE:
        case HandleKind::SW:
          set_cursor("ne-resize");
          break;
        case HandleKind::N:
        case HandleKind::S:
          set_cursor("n-resize");
          break;
        case HandleKind::E:
        case HandleKind::W:
          set_cursor("e-resize");
          break;
        case HandleKind::RotateNW:
        case HandleKind::RotateNE:
        case HandleKind::RotateSE:
        case HandleKind::RotateSW:
          set_cursor("grab");
          break;
        default:
          set_cursor("default");
          break;
        }
      }
    }
  }
  if (m_tool == ActiveTool::Pen) {
    PenModifiers mods;
    mods.alt = m_mod_alt;
    mods.shift = m_mod_shift;
    // Snap motion to nearest node on any existing path (not WIP)
    // Only snap when starting a new stroke (no WIP) to avoid
    // the rubber-band locking to the path being drawn.
    double pen_snap_x = doc_x, pen_snap_y = doc_y;
    if (!m_pen_tool.has_wip && m_doc) {
      static constexpr double NODE_SNAP_PX = 6.0;
      double best_d2 = NODE_SNAP_PX * NODE_SNAP_PX;
      for (auto &layer : m_doc->layers) {
        if (!layer->visible || layer->locked || layer->is_special_layer())
          continue;
        for (auto &obj_uptr : layer->children) {
          SceneNode &obj = *obj_uptr;
          if (obj.type != SceneNode::Type::Path || !obj.path)
            continue;
          for (const auto &nd : obj.path->nodes) {
            double sx, sy;
            doc_to_screen(nd.x, nd.y, sx, sy);
            double ddx = x - sx, ddy = y - sy;
            double d2 = ddx * ddx + ddy * ddy;
            if (d2 < best_d2) {
              best_d2 = d2;
              pen_snap_x = nd.x;
              pen_snap_y = nd.y;
            }
          }
        }
      }
      // Guide snap — only if node snap didn't already lock
      if (pen_snap_x == doc_x)
        pen_snap_x = snap_x(doc_x);
      if (pen_snap_y == doc_y)
        pen_snap_y = snap_y(doc_y);
    }
    m_pen_tool.on_motion({pen_snap_x, pen_snap_y}, mods, m_zoom);
    queue_draw();
  } else if (m_tool == ActiveTool::Line && m_line_tool.active()) {
    double ex = snap(doc_x), ey = snap(doc_y);
    // 15° angle snap when Shift held
    if (m_mod_shift && !m_line_tool.points.empty()) {
      auto [lx, ly] = m_line_tool.points.back();
      double dw = ex - lx, dh = ey - ly;
      double len = std::hypot(dw, dh);
      if (len > 0.001) {
        double angle = std::atan2(dh, dw);
        double snapped = std::round(angle / (M_PI / 12.0)) * (M_PI / 12.0);
        ex = lx + len * std::cos(snapped);
        ey = ly + len * std::sin(snapped);
      }
    }
    // Close-snap check
    double tol = 8.0 / m_zoom;
    auto [sx, sy] = m_line_tool.points[0];
    m_line_tool.close_snap = (std::hypot(ex - sx, ey - sy) <= tol);
    if (m_line_tool.close_snap) {
      ex = sx;
      ey = sy;
    }
    m_line_tool.live_x = ex;
    m_line_tool.live_y = ey;
    queue_draw();
  } else if (m_tool == ActiveTool::Zoom) {
    // Flip cursor and notify toolbar when Alt state changes
    set_cursor(m_mod_alt ? "zoom-out" : "zoom-in");
    if (m_mod_alt != m_zoom_alt_prev) {
      m_zoom_alt_prev = m_mod_alt;
      m_sig_zoom_alt.emit(m_mod_alt);
    }
  } else if (m_tool == ActiveTool::Ref && m_doc) {
    // Hover hit-test for ref points in the RefLayer
    SceneNode *prev_hovered = m_ref_hovered;
    m_ref_hovered = nullptr;
    SceneNode *rl = m_doc->ref_layer();
    if (rl && !rl->locked && rl->visible) {
      for (auto &child : rl->children) {
        if (!child->is_ref())
          continue;
        double sx, sy;
        doc_to_screen(child->ref_x, child->ref_y, sx, sy);
        if (std::hypot(x - sx, y - sy) <= 8.0) {
          m_ref_hovered = child.get();
          break;
        }
      }
    }
    set_cursor(m_ref_hovered ? "move" : "crosshair");
    queue_draw();
  }
}

// ── Pen tool click (via drag with tiny delta treated as click)
// ────────────────
void Canvas::on_pen_begin(double x, double y) {
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  m_draw_start_dx = dx;
  m_draw_start_dy = dy;
  // Update modifier state before press
  m_pen_tool.mods.alt = m_mod_alt;
  m_pen_tool.mods.shift = m_mod_shift;

  // ── Continue path ─────────────────────────────────────────────────────
  // If no WIP is active and the click lands on the tail or head of an
  // existing open path, load that path and continue drawing from that end.
  if (!m_pen_tool.has_wip && m_doc) {
    double tol = PenTool::CLOSE_RADIUS_PX / m_zoom;
    Vec2 click{dx, dy};
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode &obj = *obj_uptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;
        if (obj.path->closed)
          continue;
        if (obj.path->nodes.size() < 2)
          continue;

        BezierNode &head = obj.path->nodes.front();
        BezierNode &tail = obj.path->nodes.back();
        Vec2 head_pos{head.x, head.y};
        Vec2 tail_pos{tail.x, tail.y};

        bool near_tail = click.dist(tail_pos) <= tol;
        bool near_head = click.dist(head_pos) <= tol;

        if (!near_tail && !near_head)
          continue;

        // Save before-state for undo (we'll replace the object's path)
        PathData before = *obj.path;
        m_continue_target = &obj;
        m_continue_before = before;

        // Load path into WIP
        BezierPath bp = BezierPath::from_path_data(*obj.path);

        if (near_head && !near_tail) {
          // Continuing from head — reverse so tail is the open end
          bp.reverse();
        }

        m_pen_tool.wip = bp;
        m_pen_tool.has_wip = true;
        m_pen_tool.state = PenTool::State::Placing;
        m_pen_tool.drag_node_idx = (int)bp.nodes.size() - 1;
        m_pen_tool.live_in_valid = false;

        // Remove the object from the document — it will be re-committed
        // with the extra nodes when the pen path is finished
        for (auto &l2 : m_doc->layers) {
          auto it = std::find_if(l2->children.begin(), l2->children.end(),
                                 [&obj](const std::unique_ptr<SceneNode> &o) {
                                   return o.get() == &obj;
                                 });
          if (it != l2->children.end()) {
            l2->children.erase(it);
            break;
          }
        }
        m_selected = nullptr;
        // s159 m2: defensive cleanup. The just-erased child may have
        // been in m_selection; clear it so SelectionContext doesn't
        // walk a dangling pointer in build_object_info.
        m_selection.clear();
        notify_object_selection_changed();
        m_sig_doc_changed.emit();
        queue_draw();
        LOG_INFO("PenTool: continuing path from {}",
                 near_tail ? "tail" : "head");
        return; // don't fall through to on_press
      }
    }
  }

  // ── Pen node snap ─────────────────────────────────────────────────────
  // Snap click to nearest node within NODE_SNAP_PX screen pixels.
  // Comparison in screen space so tolerance is zoom-independent.
  if (!m_pen_tool.has_wip && m_doc) {
    static constexpr double NODE_SNAP_PX = 6.0;
    double best_d2 = NODE_SNAP_PX * NODE_SNAP_PX;
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode &obj = *obj_uptr;
        if (obj.type != SceneNode::Type::Path || !obj.path)
          continue;
        for (const auto &nd : obj.path->nodes) {
          double sx, sy;
          doc_to_screen(nd.x, nd.y, sx, sy);
          double ddx = x - sx, ddy = y - sy;
          double d2 = ddx * ddx + ddy * ddy;
          if (d2 < best_d2) {
            best_d2 = d2;
            dx = nd.x;
            dy = nd.y;
          }
        }
      }
    }
  }

  bool done = m_pen_tool.on_press({dx, dy}, m_zoom);
  if (done) {
    // Close detected — don't commit yet, allow drag to set node 0 in-handle
    m_pen_closing = true;
  }
  queue_draw();
}

void Canvas::place_ref_at_display(double ux, double uy) {
  if (!m_doc)
    return;
  double doc_x = ux + m_doc->ruler_origin_x;
  double doc_y = m_doc->canvas_height() - (uy + m_doc->ruler_origin_y);
  double rx = snap(doc_x), ry = snap(doc_y);

  SceneNode *rl = m_doc->ensure_ref_layer();

  auto ref = std::make_unique<SceneNode>();
  ref->type = SceneNode::Type::Ref;
  ref->id = next_id();
  // s177 m6: route through name funnel — same as the click-creation
  // path. Pre-s177 this was a coordinate-as-name string that violated
  // the "no derived strings in user-facing UI" rule.
  ref->name = m_doc->next_default_name(
      CurvzDocument::NameKind::Ref);
  ref->ref_x = rx;
  ref->ref_y = ry;

  rl->children.insert(rl->children.begin(), clone_node(*ref));
  m_selected = rl->children.front().get();
  m_selection = {m_selected};  // s159 m2: sync for SelectionContext

  if (m_history)
    m_history->push(std::make_unique<AddNodeCommand>(
        rl, clone_node(*rl->children.front())));

  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("Ref placed via popover at doc ({:.4f},{:.4f})", rx, ry);
}

// ── Precise placement helpers (Ctrl+click popovers)
// ─────────────────────────── All coordinates arrive in display space (Y-up,
// ruler-origin-relative). Convert to doc space (Y-down, absolute) before
// placing.

void Canvas::place_rect_precise(double ux, double uy, double w, double h) {
  if (!m_doc || w < 0.001 || h < 0.001)
    return;
  // ux,uy = top-left corner in display space (Y-up)
  // Doc space: x = ux + ruler_ox, y_bottom = canvas_h - (uy + ruler_oy)
  // y_top (doc, Y-down) = y_bottom - h
  double doc_x = ux + m_doc->ruler_origin_x;
  double doc_yb = m_doc->canvas_height() - (uy + m_doc->ruler_origin_y);
  double doc_y = doc_yb - h; // top-left corner in Y-down space

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Rectangle);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(rect_to_path(doc_x, doc_y, w, h));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Rect placed via popover: x={:.2f} y={:.2f} w={:.2f} h={:.2f}",
           doc_x, doc_y, w, h);
}

void Canvas::place_ellipse_precise(double ucx, double ucy, double rx,
                                   double ry) {
  if (!m_doc || rx < 0.001 || ry < 0.001)
    return;
  double doc_cx = ucx + m_doc->ruler_origin_x;
  double doc_cy = m_doc->canvas_height() - (ucy + m_doc->ruler_origin_y);

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Ellipse);
  obj.type = SceneNode::Type::Path;
  obj.path =
      std::make_unique<PathData>(ellipse_to_path(doc_cx, doc_cy, rx, ry));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO(
      "Ellipse placed via popover: cx={:.2f} cy={:.2f} rx={:.2f} ry={:.2f}",
      doc_cx, doc_cy, rx, ry);
}

void Canvas::place_polygon_precise(double cx, double cy, double radius,
                                   int sides, double inflection,
                                   double angle_rad) {
  if (!m_doc || radius < 0.001 || sides < 3)
    return;
  double doc_cx = cx + m_doc->ruler_origin_x;
  double doc_cy = m_doc->canvas_height() - (cy + m_doc->ruler_origin_y);

  // Snap inflection
  double perfect_star =
      (sides >= 5) ? std::cos(2.0 * M_PI / sides) / std::cos(M_PI / sides)
                   : -1.0;
  if (perfect_star > 0.0 && std::abs(inflection - perfect_star) < 0.04)
    inflection = perfect_star;
  if (inflection > 0.985)
    inflection = 1.0;

  m_poly_sides = sides;
  m_poly_inflection = inflection;

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = (inflection >= 1.0)
                 ? m_doc->next_default_name(CurvzDocument::NameKind::Polygon)
                 : m_doc->next_default_name(CurvzDocument::NameKind::Star);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(
      polygon_to_path(doc_cx, doc_cy, radius, sides, inflection, angle_rad));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Polygon placed via popover: cx={:.2f} cy={:.2f} r={:.2f} sides={} "
           "inflect={:.3f}",
           doc_cx, doc_cy, radius, sides, inflection);
}

void Canvas::place_spiral_precise(double cx, double cy, double outer_r,
                                  double inner_r, double turns,
                                  double angle_rad) {
  if (!m_doc || outer_r < 0.001)
    return;
  double doc_cx = cx + m_doc->ruler_origin_x;
  double doc_cy = m_doc->canvas_height() - (cy + m_doc->ruler_origin_y);

  m_spiral_turns = turns;
  m_spiral_inner = (outer_r > 0.0) ? (inner_r / outer_r * 100.0) : 0.0;

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Spiral);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(spiral_to_path(
      doc_cx, doc_cy, outer_r, inner_r, turns, angle_rad));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Spiral placed via popover: cx={:.2f} cy={:.2f} r={:.2f} "
           "turns={:.1f}",
           doc_cx, doc_cy, outer_r, turns);
}

void Canvas::place_line_precise(double ux1, double uy1, double ux2,
                                double uy2) {
  if (!m_doc)
    return;
  double doc_x1 = ux1 + m_doc->ruler_origin_x;
  double doc_y1 = m_doc->canvas_height() - (uy1 + m_doc->ruler_origin_y);
  double doc_x2 = ux2 + m_doc->ruler_origin_x;
  double doc_y2 = m_doc->canvas_height() - (uy2 + m_doc->ruler_origin_y);

  PathData pd;
  pd.closed = false;
  for (auto [px, py] : std::initializer_list<std::pair<double, double>>{
           {doc_x1, doc_y1}, {doc_x2, doc_y2}}) {
    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = px;
    n.cy1 = py;
    n.cx2 = px;
    n.cy2 = py;
    n.type = BezierNode::Type::Corner;
    pd.nodes.push_back(n);
  }

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Line);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(std::move(pd));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  place_shape_node(std::move(obj));
  LOG_INFO("Line placed via popover: ({:.2f},{:.2f})→({:.2f},{:.2f})", doc_x1,
           doc_y1, doc_x2, doc_y2);
}

void Canvas::place_text_precise(double ux, double uy, const std::string &family,
                                double size, bool bold, bool italic,
                                const std::string &anchor,
                                const std::string &align) {
  if (!m_doc)
    return;
  // Convert display (user) space → doc space (Y-down Cairo coords)
  double doc_x = ux + m_doc->ruler_origin_x;
  double doc_y = m_doc->canvas_height() - (uy + m_doc->ruler_origin_y);

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = generate_internal_id();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Text);
  obj.type = SceneNode::Type::Text;
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke.paint.type = FillStyle::Type::None;
  });
  obj.text_x = doc_x;
  obj.text_y = doc_y;
  obj.text_font_family = family.empty() ? "Sans" : family;
  obj.text_font_size = size > 0 ? size : 24.0;
  obj.text_bold = bold;
  obj.text_italic = italic;
  obj.text_anchor = anchor.empty() ? "start" : anchor;
  obj.text_align = align.empty() ? "left" : align;
  obj.text_content = ""; // user types content via the floating entry

  // Insert into scene first so on_text_begin can find and edit it
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(),
                         std::make_unique<SceneNode>(std::move(obj)));
  SceneNode *inserted = layer->children.front().get();
  m_selected = inserted;
  m_selection = {inserted};

  // Switch to Text tool and open the inline editor on the new node
  m_sig_request_tool.emit(ActiveTool::Text);
  Glib::signal_idle().connect_once([this, inserted]() {
    m_text_editing = inserted;
    m_text_is_new = true;
    if (m_text_entry) {
      m_text_entry_conn_activate.disconnect();
      m_text_entry_conn_changed.disconnect();
      m_text_entry->set_text("");
      m_text_entry->set_visible(true);
      m_text_entry->grab_focus();
      position_text_entry();
      m_text_entry_conn_changed =
          m_text_entry->signal_changed().connect([this]() {
            if (m_text_editing) {
              m_text_editing->text_content = m_text_entry->get_text();
              queue_draw();
            }
          });
      m_text_entry_conn_activate = m_text_entry->signal_activate().connect(
          [this]() { commit_text_edit(); });
    }
    notify_object_selection_changed();
    m_sig_doc_changed.emit();
    queue_draw();
  });
  LOG_INFO("Text placed via popover at ({:.2f},{:.2f}) family={} size={:.1f}",
           doc_x, doc_y, family, size);
}

// Shared: insert a new shape node at the front of the active layer with undo
void Canvas::place_shape_node(SceneNode obj) {
  if (!m_doc)
    return;
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();
  m_selection = {m_selected};  // s159 m2: sync for SelectionContext
  if (m_history)
    m_history->push(
        std::make_unique<AddNodeCommand>(layer, clone_node(*m_selected)));
  notify_object_selection_changed();
  m_sig_request_tool.emit(ActiveTool::Selection);
  m_sig_doc_changed.emit();
  queue_draw();
}

// ── Guide construct (two-point guide creation) ─────────────────────────────
void Canvas::begin_guide_construct() {
  m_guide_construct_active = true;
  m_guide_construct_phase = 0;
  m_guide_construct_perpendicular = false;
  queue_draw();
}

void Canvas::cancel_guide_construct() {
  m_guide_construct_active = false;
  m_guide_construct_phase = 0;
  queue_draw();
}

void Canvas::set_guide_construct_perpendicular(bool on) {
  if (!m_guide_construct_active)
    return;
  m_guide_construct_perpendicular = on;
  queue_draw();
}

void Canvas::set_guide_construct_review_callback(GuideConstructReviewCb cb) {
  m_guide_construct_review_cb = std::move(cb);
}

bool Canvas::commit_guide_construct() {
  if (!m_guide_construct_active || m_guide_construct_phase < 2 || !m_doc)
    return false;

  SceneNode *gl = m_doc->ensure_guide_layer();
  if (!gl || gl->locked) {
    m_guide_construct_active = false;
    m_guide_construct_phase = 0;
    queue_draw();
    return false;
  }
  if (!gl->visible)
    gl->visible = true;

  // Anchor = midpoint of p1/p2.  Angle = atan2(Δy, Δx); +90 for perpendicular.
  const double cx = (m_guide_construct_p1_x + m_guide_construct_p2_x) * 0.5;
  const double cy = (m_guide_construct_p1_y + m_guide_construct_p2_y) * 0.5;
  const double dx = m_guide_construct_p2_x - m_guide_construct_p1_x;
  const double dy = m_guide_construct_p2_y - m_guide_construct_p1_y;
  double angle_deg = std::atan2(dy, dx) * 180.0 / M_PI;
  if (m_guide_construct_perpendicular)
    angle_deg += 90.0;
  // Normalize into (-180, 180] for cleanliness.
  while (angle_deg > 180.0)
    angle_deg -= 360.0;
  while (angle_deg <= -180.0)
    angle_deg += 360.0;

  auto g = std::make_unique<SceneNode>();
  g->type = SceneNode::Type::Guide;
  g->guide_x = cx;
  g->guide_y = cy;
  g->guide_angle = angle_deg;
  gl->children.push_back(std::move(g));

  m_guide_construct_active = false;
  m_guide_construct_phase = 0;
  m_sig_doc_changed.emit();
  queue_draw();
  return true;
}

void Canvas::commit_line_path() {
  if (m_line_tool.points.size() < 2) {
    m_line_tool.reset();
    queue_draw();
    return;
  }

  PathData pd;
  pd.closed = m_line_tool.close_snap; // closed if snapped to start

  for (auto [px, py] : m_line_tool.points) {
    BezierNode n;
    n.x = px;
    n.y = py;
    n.cx1 = px;
    n.cy1 = py;
    n.cx2 = px;
    n.cy2 = py;
    n.type = BezierNode::Type::Corner;
    pd.nodes.push_back(n);
  }

  // If closing, the last point equals first — don't duplicate it
  if (pd.closed && pd.nodes.size() >= 2) {
    auto &first = pd.nodes.front();
    auto &last = pd.nodes.back();
    if (std::hypot(last.x - first.x, last.y - first.y) < 0.001)
      pd.nodes.pop_back();
  }

  SceneNode obj;
  obj.id = next_id();
  obj.internal_id = last_iid();
  obj.name = m_doc->next_default_name(CurvzDocument::NameKind::Line);
  obj.type = SceneNode::Type::Path;
  obj.path = std::make_unique<PathData>(std::move(pd));
  FillStyle no_fill;
  no_fill.type = FillStyle::Type::None;
  style::mutate_appearance(obj, [this, &no_fill](SceneNode& n) {
    n.fill = no_fill;
    n.stroke = m_def_stroke;
  });

  if (!m_doc || m_doc->layers.empty()) {
    m_line_tool.reset();
    return;
  }
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();

  if (m_history)
    m_history->push(std::make_unique<AddNodeCommand>(layer, clone_node(obj)));

  m_line_tool.reset();
  m_selection = {m_selected};  // s159 m2: sync for SelectionContext
  notify_object_selection_changed();
  m_sig_request_tool.emit(ActiveTool::Selection);
  m_sig_doc_changed.emit();
  queue_draw();
  LOG_INFO("LineTool: committed {} points, closed={}", obj.path->nodes.size(),
           obj.path->closed);
}

void Canvas::cancel_line_path() {
  if (!m_line_tool.active())
    return;
  m_line_tool.reset();
  notify_object_selection_changed();
  queue_draw();
  LOG_INFO("LineTool: cancelled");
}

void Canvas::commit_pen_path() {
  auto pd = m_pen_tool.finish();
  if (!pd)
    return;

  SceneNode obj;
  obj.type = SceneNode::Type::Path;
  obj.id = "obj" + std::to_string(++s_next_id_pen);
  obj.internal_id = generate_internal_id();
  obj.path = std::make_unique<PathData>(std::move(*pd));
  style::mutate_appearance(obj, [this](SceneNode& n) {
    n.fill = m_def_fill;
    n.stroke = m_def_stroke;
  });

  if (!m_doc || m_doc->layers.empty())
    return;
  SceneNode *layer = m_doc->active_layer();
  if (!layer)
    layer = m_doc->layers[0].get();
  layer->children.insert(layer->children.begin(), clone_node(obj));
  m_selected = layer->children.front().get();

  if (m_history) {
    auto cmd = std::make_unique<AddNodeCommand>(layer, clone_node(obj));
    m_history->push(std::move(cmd));
  }

  // Clear continue-path state
  m_continue_target = nullptr;

  m_selection = {m_selected};  // s159 m2: sync for SelectionContext
  notify_object_selection_changed();
  m_sig_doc_changed.emit();
  LOG_INFO("PenTool: committed path '{}'", m_selected->id);
}

void Canvas::cancel_pen_path() {
  if (!m_pen_tool.has_wip)
    return;

  // If we were continuing an existing path, restore it
  if (m_continue_target && m_doc) {
    for (auto &layer : m_doc->layers) {
      // The target was erased — put it back
      auto restored = clone_node(*m_continue_target);
      *restored->path = m_continue_before;
      layer->children.insert(layer->children.begin(), std::move(restored));
      m_selected = layer->children.front().get();
      m_selection = {m_selected};  // s159 m2: sync for SelectionContext
      break;
    }
    m_continue_target = nullptr;
    notify_object_selection_changed();
    m_sig_doc_changed.emit();
  }

  m_pen_tool.cancel();
  m_pen_closing = false;
  if (!m_continue_target)
    notify_object_selection_changed();
  queue_draw();
  LOG_INFO("PenTool: cancelled WIP path");
}

bool Canvas::selection_tool_key(guint keyval, bool shift, bool ctrl, bool alt) {
  if (m_tool != ActiveTool::Selection)
    return false;
  if (!m_selected)
    return false;

  // ── M4c-2c: Warp envelope pick-set nudge ─────────────────────────────
  // When a Warp is primary and the pick set is non-empty, arrow keys
  // translate every pick by a step (1 px / Shift=8 / Alt=32). Matches
  // object nudge conventions. Ctrl reserved for arrange. One Edit-
  // WarpCommand per keypress → distinct undo steps. Skip handle picks
  // whose parent anchor is also picked (anchor carries handles).
  if (m_selected->is_warp() && !m_warp_env_picks.empty() && !ctrl) {
    double ndx = 0, ndy = 0;
    switch (keyval) {
    case GDK_KEY_Left:
      ndx = -1;
      break;
    case GDK_KEY_Right:
      ndx = 1;
      break;
    case GDK_KEY_Up:
      ndy = -1;
      break;
    case GDK_KEY_Down:
      ndy = 1;
      break;
    default:
      return false;
    }
    double step_px = alt ? 32.0 : (shift ? 8.0 : 1.0);
    double step = step_px / m_zoom;
    ndx *= step;
    ndy *= step;
    // Snapshot pre-state for undo.
    PathData pre_top = m_selected->warp_env_top;
    PathData pre_bottom = m_selected->warp_env_bottom;
    int pre_quality = m_selected->warp_quality;
    // Flat independence: anchors translate x/y only (do NOT carry
    // handles); handles translate their own component. Each picked
    // element moves by the same delta.
    for (const auto &p : m_warp_env_picks) {
      PathData &env =
          p.is_top ? m_selected->warp_env_top : m_selected->warp_env_bottom;
      if (p.idx < 0 || p.idx >= (int)env.nodes.size())
        continue;
      BezierNode &n = env.nodes[p.idx];
      if (p.part == EnvelopePart::Anchor) {
        n.x += ndx;
        n.y += ndy;
        // Carry coincident handles — see multi-drag rationale.
        const PathData &pre = p.is_top ? pre_top : pre_bottom;
        if (p.idx < (int)pre.nodes.size()) {
          const BezierNode &prn = pre.nodes[p.idx];
          if (std::hypot(prn.cx1 - prn.x, prn.cy1 - prn.y) <= 1e-6) {
            n.cx1 += ndx;
            n.cy1 += ndy;
          }
          if (std::hypot(prn.cx2 - prn.x, prn.cy2 - prn.y) <= 1e-6) {
            n.cx2 += ndx;
            n.cy2 += ndy;
          }
        }
      } else if (p.part == EnvelopePart::HandleIn) {
        n.cx1 += ndx;
        n.cy1 += ndy;
      } else if (p.part == EnvelopePart::HandleOut) {
        n.cx2 += ndx;
        n.cy2 += ndy;
      }
    }
    m_selected->warp_cache_dirty = true;
    if (m_history) {
      // s147 m2: arrow-key nudge of envelope handles is drift —
      // capture pre-preset before clearing, push the swap atomically.
      int pre_preset = m_selected->warp_preset_idx;
      m_selected->warp_preset_idx = -1;
      m_history->push(std::make_unique<EditWarpCommand>(
          project(), m_selected->internal_id,
          pre_top, pre_bottom, pre_quality,
          m_selected->warp_env_top, m_selected->warp_env_bottom,
          m_selected->warp_quality,
          pre_preset, /*post_preset=*/-1));
    }
    queue_draw();
    return true;
  }

  // Collect all leaf paths — works for Path, Group, and Compound
  std::vector<SceneNode *> leaves;
  for (SceneNode *obj : m_selection)
    collect_paths(obj, leaves);

  // Text/Image live outside the path-leaf tree — they have their own
  // position fields (text_x/text_y, image_x/image_y) rather than
  // path->nodes. Gather them separately so keyboard nudge works for
  // mixed and pure-text/image selections, and for containers
  // (Group/Compound/ClipGroup) that nest Text or Image children.
  std::vector<SceneNode *> text_nodes;
  std::vector<SceneNode *> image_nodes;
  std::vector<SceneNode *> ref_nodes;
  for (SceneNode *obj : m_selection) {
    if (!obj)
      continue;
    if (obj->is_text()) {
      text_nodes.push_back(obj);
      continue;
    }
    if (obj->is_image()) {
      image_nodes.push_back(obj);
      continue;
    }
    if (obj->is_ref()) {
      ref_nodes.push_back(obj);
      continue;
    }
    if (obj->type == SceneNode::Type::Group ||
        obj->type == SceneNode::Type::Compound ||
        obj->type == SceneNode::Type::ClipGroup) {
      std::vector<SceneNode *> ti_leaves;
      collect_text_image_leaves(obj, ti_leaves);
      for (SceneNode *leaf : ti_leaves) {
        if (leaf->is_text())
          text_nodes.push_back(leaf);
        else if (leaf->is_image())
          image_nodes.push_back(leaf);
      }
    }
  }

  if (leaves.empty() && text_nodes.empty() && image_nodes.empty() &&
      ref_nodes.empty())
    return false;

  // ── Arrow-key nudge — moves all nodes by the same delta ───────────────
  double ndx = 0, ndy = 0;
  switch (keyval) {
  case GDK_KEY_Left:
    ndx = -1;
    break;
  case GDK_KEY_Right:
    ndx = 1;
    break;
  case GDK_KEY_Up:
    ndy = -1;
    break;
  case GDK_KEY_Down:
    ndy = 1;
    break;
  default:
    return false;
  }

  double screen_px = alt ? 32.0 : (shift ? 8.0 : 2.0);
  if (ctrl)
    return false; // Ctrl+arrow reserved for arrange
  double step = screen_px / m_zoom;
  ndx *= step;
  ndy *= step;

  // One-shot snap — populate m_move_snaps + m_ref_move_snaps from current
  // positions, run snap_move, then immediately clear lock state. Snaps to
  // any active snap class (guides / grid / margins). Path leaves and refpts
  // contribute their positions; text/image are not snapped via nudge today
  // (m_text_move_snaps stays empty — separate work if needed).
  if (m_doc && m_doc->snap.enabled &&
      (m_doc->snap.snap_guides || m_doc->snap.snap_grid ||
       m_doc->snap.snap_margins)) {
    m_move_snaps.clear();
    m_ref_move_snaps.clear();
    for (SceneNode *leaf : leaves)
      if (leaf->path && !leaf->path->nodes.empty())
        m_move_snaps.push_back({leaf, leaf->path->nodes, *leaf->path});
    for (SceneNode *r : ref_nodes)
      m_ref_move_snaps.push_back({r, r->ref_x, r->ref_y});
    m_snap_x_locked = false;
    m_snap_y_locked = false;
    auto [snapped_dx, snapped_dy] = snap_move(ndx, ndy);
    ndx = snapped_dx;
    ndy = snapped_dy;
    m_move_snaps.clear();
    m_ref_move_snaps.clear();
    m_snap_x_locked = false;
    m_snap_y_locked = false;
  }

  std::vector<PathData> befores, afters;
  for (SceneNode *leaf : leaves)
    befores.push_back(*leaf->path);
  // s168 m4: parallel before/after snapshots for refpts so nudge can
  // push RefMoveCommands. Same coalesce pattern as the path nudge
  // below — within a 1500ms window, patch the last command's after
  // rather than pushing a new one.
  struct RefBefore { std::string iid; double bx, by; };
  std::vector<RefBefore> ref_befores;
  for (SceneNode *r : ref_nodes)
    ref_befores.push_back({r->internal_id, r->ref_x, r->ref_y});

  for (SceneNode *leaf : leaves) {
    for (auto &nd : leaf->path->nodes) {
      nd.x += ndx;
      nd.y += ndy;
      nd.cx1 += ndx;
      nd.cy1 += ndy;
      nd.cx2 += ndx;
      nd.cy2 += ndy;
    }
  }
  for (SceneNode *leaf : leaves)
    afters.push_back(*leaf->path);

  // Text nudge — match the drag-move convention in on_select_update
  // (see m_text_move_snaps loop near line ~6670): text_x/text_y are
  // updated with the same signs as path node deltas, regardless of what
  // the "Y-up" comment on the field declaration suggests. If down-arrow
  // moves text upward after this lands, invert ndy for text.
  for (SceneNode *t : text_nodes) {
    t->text_x += ndx;
    t->text_y += ndy;
  }
  // Image: stored Y-down (doc space) like paths — add directly.
  for (SceneNode *im : image_nodes) {
    im->image_x += ndx;
    im->image_y += ndy;
  }
  // s168 m4: refpt nudge now pushes RefMoveCommand and coalesces with
  // the same 1500ms window as path nudges below. Pre-s168 this drag
  // mutated ref_x/ref_y in place with no undo.
  for (SceneNode *r : ref_nodes) {
    r->ref_x += ndx;
    r->ref_y += ndy;
  }

  if (m_history) {
    using clock = std::chrono::steady_clock;
    auto now = clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - m_nudge_last_time)
                       .count();
    bool same_window = (m_nudge_last_obj == m_selected) && (elapsed < 1500);
    m_nudge_last_obj = m_selected;
    m_nudge_last_time = now;

    if (!same_window) {
      for (int i = 0; i < (int)leaves.size(); ++i)
        m_history->push(std::make_unique<EditPathCommand>(
            project(), leaves[i]->internal_id,
            std::move(befores[i]), std::move(afters[i]),
            "Nudge object"));
      // s168 m4: push RefMoveCommand for each ref nudged, mirroring the
      // path branch above. Each ref gets its own command — multi-ref
      // nudge produces N commands per nudge step (matches path branch).
      for (size_t i = 0; i < ref_nodes.size() && i < ref_befores.size(); ++i) {
        SceneNode *r = ref_nodes[i];
        LOG_INFO("[IIDDIAG] RefMove::push (nudge) "
                 "iid='{}' obj_name='{}' obj_type={}  "
                 "before=({},{}) after=({},{})",
                 r->internal_id, r->name, (int)r->type,
                 ref_befores[i].bx, ref_befores[i].by,
                 r->ref_x, r->ref_y);
        m_history->push(std::make_unique<RefMoveCommand>(
            project(), ref_befores[i].iid,
            ref_befores[i].bx, ref_befores[i].by,
            r->ref_x, r->ref_y));
      }
    } else {
      for (SceneNode *leaf : leaves)
        if (auto *cmd =
                dynamic_cast<EditPathCommand *>(m_history->last_command()))
          if (cmd->obj_iid == leaf->internal_id)
            cmd->after = *leaf->path;
      // s168 m4: same coalesce shape for refs — patch the last command's
      // after if iid matches. The "last command" check is single-shot
      // (matches the path branch's pattern) — for multi-ref nudge in a
      // continuation, only the most-recently-pushed RefMoveCommand gets
      // its after patched. Pre-existing pattern; revisit if multi-ref
      // continuation produces visible undo drift.
      for (SceneNode *r : ref_nodes)
        if (auto *cmd =
                dynamic_cast<RefMoveCommand *>(m_history->last_command()))
          if (cmd->node_iid == r->internal_id) {
            cmd->after_x = r->ref_x;
            cmd->after_y = r->ref_y;
          }
    }
  }

  m_sig_doc_changed.emit();
  queue_draw();
  // Record nudge as a Move step. Consecutive nudges within the coalesce
  // window will be folded by the macro editor later.
  if (std::abs(ndx) > 0.001 || std::abs(ndy) > 0.001) {
    MacroStep ms;
    ms.op = MacroStep::Op::Move;
    ms.dx = ndx;
    ms.dy = ndy;
    record_step_if_recording(ms);
  }
  return true;
}

bool Canvas::node_tool_key(guint keyval, bool shift, bool ctrl, bool alt) {
  if (m_tool != ActiveTool::Node)
    return false;
  if (!m_selected || m_selected->type != SceneNode::Type::Path ||
      !m_selected->path)
    return false;

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  int n = (int)bp.nodes.size();
  if (n == 0)
    return false;

  // ── Delete selected node(s) ───────────────────────────────────────────
  // s163 m4: cross-path n-order delete — restored.
  //
  // Pre-s163 the handler operated on m_selected_node only, which was a
  // regression from the original behavior where Delete walked m_node_selection
  // and removed every selected node, even across multiple paths. The
  // single-node behavior surfaced as "select 5 nodes, press Delete, only one
  // disappears." This restores the n-order behavior.
  //
  // Per-path independence: each path's deletes are local. We group
  // m_node_selection by path, walk each group's indices descending (so
  // intra-group delete doesn't shift later targets), call bp.delete_node(idx)
  // per node — which preserves the shape-fitting handle reshaping at each
  // neighbor pair via the existing least-squares fit in BezierPath. One
  // EditPathCommand per affected path; if a path's deletes consume all its
  // nodes, push DeleteObjectCommand for that path's parent layer instead.
  // The whole press wraps in CompositeCommand so a single Ctrl+Z undoes it.
  //
  // Pre-flight Blend rejection runs across every affected path. If any are
  // blend sources the press is rejected cleanly with the existing message —
  // partial delete with some-paths-skipped is worse than a clean reject.
  //
  // Fallback: if m_node_selection is empty but m_selected_node >= 0 (single
  // click hadn't populated multi-select for some reason), synthesize a
  // single-entry list. Preserves the historical single-node case.
  if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) {
    // Build the (path → indices) groups, preserving first-seen order of
    // paths so undo grouping is deterministic. Synthesize a single entry
    // from m_selected/m_selected_node when m_node_selection is empty.
    std::vector<std::pair<SceneNode *, std::vector<int>>> groups;
    auto add_entry = [&](SceneNode *p, int idx) {
      if (!p || !p->path)
        return;
      const int pn = (int)p->path->nodes.size();
      if (idx < 0 || idx >= pn)
        return;
      for (auto &g : groups) {
        if (g.first == p) {
          // Dedup within a path
          for (int existing : g.second)
            if (existing == idx)
              return;
          g.second.push_back(idx);
          return;
        }
      }
      groups.push_back({p, {idx}});
    };
    if (!m_node_selection.empty()) {
      for (const auto &ns : m_node_selection)
        add_entry(ns.obj, ns.node_idx);
    } else if (m_selected_node >= 0) {
      add_entry(m_selected, m_selected_node);
    }

    if (groups.empty())
      return false;  // nothing to delete, fall through

    // Pre-flight: reject the press if any affected path is a Blend source.
    // Node-count locks per Blend's matched-A/B requirement; partial delete
    // would silently violate it.
    for (const auto &g : groups) {
      if (find_blend_owner(g.first)) {
        LOG_INFO("NodeTool: delete-node rejected — path is a Blend source "
                 "(node count is locked)");
        m_sig_show_message.emit(
            "Blend",
            "One or more selected nodes belong to a path that is part of a "
            "Blend. The Blend's node count is locked. Release the Blend first "
            "to edit node counts.");
        return true;
      }
    }

    if (!m_doc)
      return false;

    // Sort each group's indices descending so the delete loop's
    // BezierPath::delete_node(idx) calls don't shift targets we haven't
    // visited yet.
    for (auto &g : groups)
      std::sort(g.second.begin(), g.second.end(), std::greater<int>());

    // Compose into a single undoable unit.
    auto composite = std::make_unique<CompositeCommand>(
        groups.size() == 1 ? std::string("Delete node")
                            : std::string("Delete nodes"));

    // Track whether the primary-selected object survives. If its path
    // gets fully consumed we need to clear m_selected too.
    bool primary_erased = false;

    for (auto &g : groups) {
      SceneNode *obj = g.first;
      const std::vector<int> &indices = g.second;
      const int orig_n = (int)obj->path->nodes.size();
      const bool consumes_all = ((int)indices.size() >= orig_n);

      if (consumes_all) {
        // All of this path's nodes are slated for delete → erase the
        // whole object. Mirrors the historical n==1 branch but generalised
        // to multi-node-consume-all. Snapshot before the erase so undo
        // can re-insert at the original index.
        SceneNode *parent_layer = nullptr;
        int orig_index = -1;
        for (auto &layer : m_doc->layers) {
          for (int i = 0; i < (int)layer->children.size(); ++i) {
            if (layer->children[i].get() == obj) {
              parent_layer = layer.get();
              orig_index = i;
              break;
            }
          }
          if (parent_layer)
            break;
        }
        if (parent_layer && orig_index >= 0) {
          auto snap = clone_node(*obj);
          composite->add(std::make_unique<DeleteObjectCommand>(
              parent_layer, std::move(snap), orig_index));
          // Mutate now so subsequent groups see consistent state. Match
          // the existing single-node-erase pattern: scrub Canvas state
          // pointing at the about-to-be-freed object before the erase.
          scrub_node_refs(obj);
          if (m_selected == obj)
            primary_erased = true;
          parent_layer->children.erase(parent_layer->children.begin() +
                                        orig_index);
        } else {
          // Object not at a top-level layer — nested in a Group/Compound.
          // Leave it intact; falling through to the per-node path would
          // empty the path's nodes vector and produce a degenerate node-
          // less Path, which other code may not handle. Safer to skip
          // this group with a log line. The user can ungroup/release-
          // compound first if they want to delete the contained path.
          LOG_INFO("NodeTool: skipping consume-all delete on nested path "
                   "'{}' — not at top-level layer; ungroup first.",
                   obj->id);
        }
        continue;
      }

      // Partial delete — apply per-node delete in descending order.
      PathData before_pd = *obj->path;
      BezierPath bp = BezierPath::from_path_data(before_pd);
      for (int idx : indices)
        bp.delete_node(idx);
      *obj->path = bp.to_path_data();

      composite->add(std::make_unique<EditPathCommand>(
          project(), obj->internal_id, std::move(before_pd), *obj->path,
          indices.size() == 1 ? "Delete node" : "Delete nodes"));
    }

    // Push the composite. Empty-step guards below — a press where every
    // group was a skipped nested consume-all leaves an empty composite;
    // skip the push so the redo stack isn't cleared spuriously.
    if (m_history && !composite->steps.empty())
      m_history->push(std::move(composite));

    // Selection state cleanup. Clear node selection wholesale; survivors
    // can re-select if they want to continue editing. m_selected also
    // clears if its object got erased.
    m_node_selection.clear();
    m_selected_node = -1;
    if (primary_erased) {
      m_selected = nullptr;
      m_selection.clear();
    }
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_doc_changed.emit();
    m_sig_node_changed.emit(m_selected, m_selected_node);
    queue_draw();
    LOG_INFO("NodeTool: deleted nodes across {} path(s)", groups.size());
    return true;
  }

  // ── Cycle selection with Tab / Shift+Tab ──────────────────────────────
  // Ctrl-gated: Ctrl+Tab / Ctrl+Shift+Tab are reserved for document
  // navigation at the window level (s108 m7). Bare Tab / Shift+Tab
  // remains the node-cycle binding when the Node tool is active.
  if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) && !ctrl) {
    if (m_selected_node < 0) {
      m_selected_node = 0;
    } else if (shift) {
      m_selected_node = (m_selected_node - 1 + n) % n;
    } else {
      m_selected_node = (m_selected_node + 1) % n;
    }
    queue_draw();
    LOG_DEBUG("NodeTool: selected node {}/{}", m_selected_node, n);
    m_sig_node_changed.emit(m_selected, m_selected_node);
    return true;
  }

  // ── Context-aware Join / Close (J) ───────────────────────────────────
  // Priority:
  //   1. m_selected2 set → cross-path join of two open paths
  //   2. Two endpoints of the same open path in m_node_selection → close it
  //   3. Single open path (any endpoint selected, or none) → close/open toggle
  //   4. Anything else → show a helpful error dialog
  if (keyval == GDK_KEY_j || keyval == GDK_KEY_J) {
    // Node-count lock: J closes/opens single paths (no count change) OR
    // joins two paths (count change). Either way, the closed-flag and
    // node-count of A/B must stay matched. Reject if the primary path
    // is a Blend source.
    if (find_blend_owner(m_selected)) {
      LOG_INFO("NodeTool: J rejected — path is a Blend source");
      m_sig_show_message.emit(
          "Blend",
          "This path is part of a Blend; its node count and open/closed "
          "state are locked. Release the Blend first.");
      return true;
    }

    // ── Pre-flight: resolve (base, append) from current state ───────────
    // s157: identity-resolved from m_node_selection, NOT from m_selected.
    //
    // Rule: base = the path whose selected node is its TAIL (end). If no
    // entry has an end selected, fall back to the first start-selected
    // entry (the splice math will reverse it). Append = the next entry on
    // a different open path.
    //
    // Why this matters: after Break-Break-Break the user can have multiple
    // paths with identical names ("Rectangle 1") that visually overlap.
    // m_selected reflects whichever pointer the click hit-test resolved
    // to, which is not always the path the user thought they clicked. But
    // the endpoint nodes the user clicked into m_node_selection are
    // unambiguous — those are the geometric points the user picked. So
    // we trust m_node_selection over m_selected for cross-path Join.
    //
    // Resolution order:
    //   a) Two endpoint entries on different open paths in m_node_selection
    //      → end-selected wins as base; if none, first start-selected wins
    //      and gets reversed later.
    //   b) One endpoint on m_selected, autoscan for nearest other endpoint
    //      → m_selected is base, autoscan picks append.
    //
    // Branch (a) overwrites m_selected/m_selected_node from its result —
    // this is the surgical fix for the wrong-endpoint bug. m_selected2 is
    // always set by exactly one of these branches.
    if (!m_selected->path->closed && (!m_selected2 || !m_selected2->path)) {

      // ── a) Two endpoints in m_node_selection on different open paths ──
      // Rule: base is the path whose selected endpoint is its TAIL (end).
      // If no entry in m_node_selection has an end selected, fall back to
      // the first start-selected entry (the splice math will reverse it
      // before appending). Append is the next entry on a different path.
      //
      // This is the surgical fix for the wrong-endpoint bug after
      // Break-Break-Break: identity-resolved by endpoint role in the
      // selection, not by m_selected (which may have been wrong).
      if (!m_selected2) {
        SceneNode *base_obj = nullptr;
        int base_node = -1;
        SceneNode *app_obj = nullptr;
        int app_node = -1;

        // Pass 1: find the first end-selected entry → base.
        for (const auto &ns : m_node_selection) {
          if (!ns.obj || !ns.obj->path || ns.obj->path->closed)
            continue;
          int nn = (int)ns.obj->path->nodes.size();
          if (nn >= 1 && ns.node_idx == nn - 1) {
            base_obj = ns.obj;
            base_node = ns.node_idx;
            break;
          }
        }
        // Fallback: no end-selected entry — take the first start-selected.
        if (!base_obj) {
          for (const auto &ns : m_node_selection) {
            if (!ns.obj || !ns.obj->path || ns.obj->path->closed)
              continue;
            if (ns.node_idx == 0) {
              base_obj = ns.obj;
              base_node = 0;
              break;
            }
          }
        }

        // Pass 2: find the next endpoint entry on a different path → append.
        if (base_obj) {
          for (const auto &ns : m_node_selection) {
            if (!ns.obj || ns.obj == base_obj || !ns.obj->path ||
                ns.obj->path->closed)
              continue;
            int nn = (int)ns.obj->path->nodes.size();
            if (ns.node_idx == 0 || ns.node_idx == nn - 1) {
              app_obj = ns.obj;
              app_node = ns.node_idx;
              break;
            }
          }
        }

        if (base_obj && app_obj) {
          // Identity-resolve from m_node_selection, overriding whatever
          // m_selected pointed at (which may have been wrong after
          // Break-Break-Break or other identity-confusing operations).
          m_selected = base_obj;
          m_selected_node = base_node;
          m_selected2 = app_obj;
          m_selected_node2 = app_node;
        }
      }

      // ── b) Auto-scan: find nearest endpoint on any other open path ────
      // Reached when m_node_selection didn't yield two paths — i.e. user
      // clicked once and pressed J without shift-clicking. m_selected is
      // unambiguously the user's intent here (single click = no identity
      // confusion).
      if (!m_selected2 && m_doc) {
        int n1 = (int)m_selected->path->nodes.size();
        int active_ep = -1;
        if (m_selected_node == 0 || m_selected_node == n1 - 1)
          active_ep = m_selected_node;
        if (active_ep >= 0) {
          const BezierNode &my_ep = m_selected->path->nodes[active_ep];
          double best_d2 = 1e18;
          SceneNode *best_obj = nullptr;
          int best_node = -1;

          for (auto &layer : m_doc->layers) {
            if (!layer->visible || layer->locked || layer->is_special_layer())
              continue;
            for (auto &uptr : layer->children) {
              SceneNode *other = uptr.get();
              if (other == m_selected || !other->path ||
                  other->path->closed || other->path->nodes.empty())
                continue;
              int nn = (int)other->path->nodes.size();
              for (int ei = 0; ei < 2; ++ei) {
                int ni = (ei == 0) ? 0 : nn - 1;
                const BezierNode &ep = other->path->nodes[ni];
                double d2 = Vec2{ep.x - my_ep.x, ep.y - my_ep.y}.length();
                d2 = d2 * d2;
                if (d2 < best_d2) {
                  best_d2 = d2;
                  best_obj = other;
                  best_node = ni;
                }
              }
            }
          }

          if (best_obj) {
            m_selected2 = best_obj;
            m_selected_node2 = best_node;
          }
        }
      }
    }

    // ── 1. Cross-path join: secondary path queued ─────────────────────
    if (m_selected2 && m_selected2->path && m_selected_node2 >= 0) {

      // Validate m_selected2 is still live in the document
      SceneNode *layer2_ptr = nullptr;
      std::unique_ptr<SceneNode> *obj2_uptr = nullptr;
      if (m_doc) {
        for (auto &layer : m_doc->layers) {
          for (auto &uptr : layer->children) {
            if (uptr.get() == m_selected2) {
              layer2_ptr = layer.get();
              obj2_uptr = &uptr;
              break;
            }
          }
          if (layer2_ptr)
            break;
        }
      }
      if (!layer2_ptr) {
        // m_selected2 is dangling — clear and fall through to single-path logic
        m_selected2 = nullptr;
        m_selected_node2 = -1;
        LOG_INFO(
            "NodeTool: join — secondary path no longer in document, cleared");
        // fall through: skip the rest of this block, handled below
      } else {

        // Both paths must be open
        if (m_selected->path->closed || m_selected2->path->closed) {
          std::string reason;
          if (m_selected->path->closed && m_selected2->path->closed)
            reason = "Both paths are already closed.\n\nJoin only works "
                     "between two open paths.";
          else
            reason = "One of the selected paths is already closed.\n\nJoin "
                     "only works between two open paths.";
          m_sig_show_message.emit("Cannot Join Paths", reason);
          LOG_INFO("NodeTool: join — one or both paths are closed");
          return true;
        }

        int n1 = (int)m_selected->path->nodes.size();
        int n2 = (int)m_selected2->path->nodes.size();

        // Validate endpoint selection — each selected node must be an endpoint
        bool s1_is_head = (m_selected_node == 0);
        bool s1_is_tail = (m_selected_node == n1 - 1);
        bool s2_is_head = (m_selected_node2 == 0);
        bool s2_is_tail = (m_selected_node2 == n2 - 1);

        if (!(s1_is_head || s1_is_tail) || !(s2_is_head || s2_is_tail)) {
          m_sig_show_message.emit(
              "Cannot Join Paths",
              "Select an endpoint node on each path before joining.\n\n"
              "Only the first or last node of an open path can be joined.");
          LOG_INFO("NodeTool: join — selected nodes are not endpoints");
          return true;
        }

        // ── s156 algorithm restructure (preserved from s156) ─────────────
        // Doc-tree consistency rule: never write merged geometry into the
        // surviving SceneNode while the other SceneNode wrapper still
        // exists in the tree. Subscribers waking up on prop_changed/
        // doc_changed signals during that window would see two objects
        // with one logically already merged into the other — an
        // unrepresentable state that crashed the renderer in s155.
        //
        // Order:
        //   1. Compute merged BezierPath in a local
        //   2. Snapshot base's BEFORE state (for undo)
        //   3. Snapshot append's full SceneNode (for undo)
        //   4. Erase append from its layer (and scrub all Canvas state
        //      pointing at it via scrub_node_refs)
        //   5. Write merged data into base
        //   6. Push undo command (composite: edit base + restore append)
        //   7. Emit signals
        //
        // ── s157 algorithm rephrase ──────────────────────────────────────
        // Identity rule (decided by pre-flight, not here):
        //   base = the path whose selected endpoint is its tail (end). If
        //   no entry has an end selected, the first start-selected entry
        //   is taken as base and reversed below.
        //   append = the next entry on a different open path.
        //
        // m_selected = base SceneNode (its identity survives the join).
        // m_selected2 = append SceneNode (its identity is consumed).
        //
        // Geometry rule (here):
        //   - if base's selected is its head (start-fallback case),
        //     reverse base so the splice point is its tail
        //   - if append's selected is its tail, reverse append so its
        //     head leads into the splice
        //   - splice base.tail → append.head, weld coincident or bridge
        //   - if the result's start meets its end, close the path
        //
        // The four user-facing cases — end+start, end+end, start+start,
        // start+end — all collapse to this single pipeline, with
        // identity decided by which endpoint role each path contributed.

        // ── 1. Compute merged BezierPath ─────────────────────────────────
        BezierPath base = BezierPath::from_path_data(*m_selected->path);
        BezierPath app = BezierPath::from_path_data(*m_selected2->path);

        // Reorient base so its selected endpoint is the tail
        if (s1_is_head)
          base.reverse();
        // Reorient append so its selected endpoint is the head
        if (s2_is_tail)
          app.reverse();

        // Splice point: base.tail() ↔ app.front()
        double tol = 6.0 / m_zoom;
        double weld_d = Vec2{base.nodes.back().x - app.nodes.front().x,
                             base.nodes.back().y - app.nodes.front().y}
                            .length();

        if (weld_d <= tol) {
          // Coincident — drop base.tail, weld cleanly into app.head
          // (matches s156 convention; either direction loses one
          // "endpoint-at-self" handle pair, neither crashes).
          base.nodes.pop_back();
        } else {
          // Not coincident — straighten handles to bridge with a line
          base.nodes.back().cx2 = base.nodes.back().x;
          base.nodes.back().cy2 = base.nodes.back().y;
          app.nodes.front().cx1 = app.nodes.front().x;
          app.nodes.front().cy1 = app.nodes.front().y;
        }

        // Append append's nodes to base
        for (auto &nd : app.nodes)
          base.nodes.push_back(nd);

        // ── s157: closed-loop fall-out ───────────────────────────────────
        // If after splicing the new last node coincides with the first,
        // collapse it and close. This is the case where the user joined
        // two halves of what was originally one closed shape — today's
        // code always produced an open path here; now it closes naturally.
        base.closed = false;
        if (base.nodes.size() >= 3) {
          double end_d = Vec2{base.nodes.back().x - base.nodes.front().x,
                              base.nodes.back().y - base.nodes.front().y}
                             .length();
          if (end_d <= tol) {
            // Last node welds into first — drop it, set closed
            base.nodes.pop_back();
            base.closed = true;
          }
        }

        PathData merged_pd = base.to_path_data();

        // ── 2. Snapshot base's BEFORE state for undo ─────────────────────
        PathData before_base = *m_selected->path;

        // ── 3. Snapshot append for undo + remember its layer slot ────────
        int app_idx = 0;
        for (int i = 0; i < (int)layer2_ptr->children.size(); ++i) {
          if (&layer2_ptr->children[i] == obj2_uptr) {
            app_idx = i;
            break;
          }
        }

        auto app_snap = clone_node(**obj2_uptr);

        // ── 4. Scrub Canvas state pointing at append, then erase it ──────
        // Order matters: scrub references BEFORE erase so no read of dead
        // memory is even possible. m_selected stays = base (joined_obj).
        SceneNode *joined_obj = m_selected;
        SceneNode *erased_obj = obj2_uptr->get(); // freed by erase below

        // m_selected2 / m_selected_node2 are scrubbed by scrub_node_refs.
        // Clear node selection wholesale here (separately from scrub) —
        // Join consumes it. Even surviving entries on base are semantically
        // stale (base may have been reversed and node indices no longer
        // point where the user clicked).
        m_node_selection.clear();
        scrub_node_refs(erased_obj);

        auto erase_it = std::find_if(
            layer2_ptr->children.begin(), layer2_ptr->children.end(),
            [obj2_uptr](const std::unique_ptr<SceneNode> &o) {
              return &o == obj2_uptr;
            });
        if (erase_it == layer2_ptr->children.end()) {
          LOG_INFO("NodeTool: join — erase target not found, bailing");
          return true;
        }
        layer2_ptr->children.erase(erase_it);
        // erased_obj is now a dangling pointer. DO NOT deref it. We only
        // use it for pointer-equality checks (safe — the bit pattern is
        // fine even if the memory is gone). Treat as tombstone marker.
        (void)erased_obj;

        // ── 5. NOW write merged data into the surviving base ─────────────
        *joined_obj->path = std::move(merged_pd);
        // Park selected node at the welded splice point (was base.tail
        // before the append; for closed loops, end-meets-start collapsed
        // so we sit at node 0).
        m_selected_node = joined_obj->path->closed
                              ? 0
                              : (int)joined_obj->path->nodes.size() - 1;

        // ── 6. Composite undo: restores base's data + re-inserts append ──
        if (m_history) {
          auto composite = std::make_unique<CompositeCommand>("Join paths");
          composite->add(std::make_unique<EditPathCommand>(
              project(), joined_obj->internal_id,
              std::move(before_base), *joined_obj->path,
              "Join paths"));
          composite->add(std::make_unique<DeleteObjectCommand>(
              layer2_ptr, std::move(app_snap), app_idx));
          m_history->push(std::move(composite));
        }

        // ── 7. Emit signals — subscribers see a clean, consistent tree ──
        notify_object_selection_changed();
        notify_node_selection_changed();
        m_sig_node_changed.emit(joined_obj, m_selected_node);
        m_sig_doc_changed.emit();
        queue_draw();
        LOG_INFO("NodeTool: joined paths — {} nodes, closed={}",
                 joined_obj->path->nodes.size(), joined_obj->path->closed);
        return true;

      } // end else (layer2_ptr valid)
    } // end if (m_selected2)

    // ── 2. Same-path two-endpoint join ───────────────────────────────
    // If exactly two endpoints of the *same* open path are selected via
    // m_node_selection (head=0 and tail=n-1), close the path directly.
    {
      // Collect endpoint indices selected on this path
      bool has_head = false, has_tail = false;
      int last_idx = n - 1;
      for (const auto &ns : m_node_selection) {
        if (ns.obj == m_selected) {
          if (ns.node_idx == 0)
            has_head = true;
          if (ns.node_idx == last_idx)
            has_tail = true;
        }
      }
      // Also count m_selected_node itself
      if (m_selected_node == 0)
        has_head = true;
      if (m_selected_node == last_idx)
        has_tail = true;

      if (has_head && has_tail && !bp.closed && n >= 2) {
        PathData before_pd = *m_selected->path;
        const BezierNode &head = bp.nodes[0];
        const BezierNode &tail = bp.nodes.back();
        double tol_doc = 6.0 / m_zoom;
        double d = Vec2{tail.x - head.x, tail.y - head.y}.length();
        if (d <= tol_doc) {
          // Coincident — weld tail into head, close cleanly
          bp.nodes.pop_back();
        } else {
          // Distant — bridge with straight segment
          int last = (int)bp.nodes.size() - 1;
          bp.nodes.back().cx2 = bp.nodes.back().x;
          bp.nodes.back().cy2 = bp.nodes.back().y;
          bp.nodes.front().cx1 = bp.nodes.front().x;
          bp.nodes.front().cy1 = bp.nodes.front().y;
          bp.recompute_join_handles(0);
          bp.recompute_join_handles(last);
        }
        bp.closed = true;
        *m_selected->path = bp.to_path_data();
        m_selected_node = -1;
        m_node_selection.clear();
        if (m_history)
          m_history->push(std::make_unique<EditPathCommand>(
              project(), m_selected->internal_id,
              std::move(before_pd), *m_selected->path,
              "Close path"));
        notify_object_selection_changed();
        notify_node_selection_changed();
        m_sig_doc_changed.emit();
        queue_draw();
        LOG_INFO("NodeTool: closed path via two-endpoint selection");
        return true;
      }
    }

    // ── 3. Single path: close / open toggle ──────────────────────────
    // Works on any open or closed path regardless of which node is selected.
    if (n < 2 && !bp.closed) {
      m_sig_show_message.emit(
          "Cannot Close Path",
          "A path needs at least 2 nodes before it can be closed.");
      return true;
    }

    PathData before_pd = *m_selected->path;
    bool was_closed = bp.closed;

    if (bp.closed) {
      // Opening: duplicate node 0 and append as new tail so the closing
      // segment becomes explicit and no geometry is lost.
      BezierNode tail = bp.nodes[0];
      tail.cx2 = tail.x;
      tail.cy2 = tail.y;
      LOG_INFO("J open: node0=({:.2f},{:.2f}) tail=({:.2f},{:.2f})",
               bp.nodes[0].x, bp.nodes[0].y, tail.x, tail.y);
      bp.nodes.push_back(tail);
      bp.closed = false;
    } else {
      // Closing: weld if endpoints are coincident, else bridge.
      const BezierNode &head = bp.nodes[0];
      const BezierNode &tail = bp.nodes.back();
      double tol_doc = 6.0 / m_zoom;
      double d = Vec2{tail.x - head.x, tail.y - head.y}.length();
      if (d <= tol_doc) {
        bp.nodes.pop_back();
      } else {
        int last = (int)bp.nodes.size() - 1;
        bp.nodes.back().cx2 = bp.nodes.back().x;
        bp.nodes.back().cy2 = bp.nodes.back().y;
        bp.nodes.front().cx1 = bp.nodes.front().x;
        bp.nodes.front().cy1 = bp.nodes.front().y;
        bp.recompute_join_handles(0);
        bp.recompute_join_handles(last);
      }
      bp.closed = true;
    }
    *m_selected->path = bp.to_path_data();
    m_selected_node = -1;
    if (m_history)
      m_history->push(std::make_unique<EditPathCommand>(
          project(), m_selected->internal_id,
          std::move(before_pd), *m_selected->path,
          was_closed ? "Open path" : "Close path"));
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("NodeTool: path {} → {}", m_selected->id,
             bp.closed ? "closed" : "open");
    return true;
  }

  // ── Break path at selected node (B) ──────────────────────────────────
  // Routing is path-state aware:
  //   • Closed path → open the loop at the selected node (one path remains)
  //   • Open path  → split into two paths at the selected node
  // Pre-S100 m3 the handler always called open_selected_at_node, which is
  // the closed-path opener; on open paths it returned silently and the user
  // saw nothing happen. Symptom name: "B sometimes does nothing." Fixed by
  // dispatching on bp.closed at the call site (the handler already has bp).
  if (!ctrl && !shift && !alt && (keyval == GDK_KEY_b || keyval == GDK_KEY_B)) {
    if (m_selected_node < 0 || m_selected_node >= n) {
      m_sig_show_message.emit(
          "Cannot Break Path",
          "Select a node first, then press B to break the path at that node.");
      return true;
    }
    if (find_blend_owner(m_selected)) {
      LOG_INFO("NodeTool: break-path rejected — path is a Blend source");
      m_sig_show_message.emit(
          "Blend", "This path is part of a Blend; its structure is locked. "
                   "Release the Blend first.");
      return true;
    }
    if (bp.closed) {
      open_selected_at_node();
    } else {
      split_selected_at_node();
    }
    return true;
  }

  // ── Node type hotkeys ─────────────────────────────────────────────────
  // A = Symmetric, M = sMooth, C = Cusp, K = corner (Kink)
  // One key per type — aliases (S for Symmetric, Q for Corner) were removed
  // because they shadowed global shortcuts (S → Selection tool,
  // Q → toggle snap) making those global keys unreachable when a node
  // was selected. Node-tool keys only act when no modifier is held.
  if (!ctrl && !shift && m_selected_node >= 0 && m_selected_node < n) {
    BezierNode::Type new_type;
    bool type_key = true;
    switch (keyval) {
    case GDK_KEY_a:
    case GDK_KEY_A:
      new_type = BezierNode::Type::Symmetric;
      break;
    case GDK_KEY_m:
    case GDK_KEY_M:
      new_type = BezierNode::Type::Smooth;
      break;
    case GDK_KEY_c:
    case GDK_KEY_C:
      new_type = BezierNode::Type::Cusp;
      break;
    case GDK_KEY_k:
    case GDK_KEY_K:
      new_type = BezierNode::Type::Corner;
      break;
    default:
      type_key = false;
      break;
    }
    if (type_key) {
      PathData before = *m_selected->path;
      // Apply to all nodes in m_node_selection (or just m_selected_node)
      if (!m_node_selection.empty()) {
        for (const auto &ns : m_node_selection) {
          if (ns.obj == m_selected && ns.node_idx >= 0 &&
              ns.node_idx < (int)bp.nodes.size())
            bp.set_node_type(ns.node_idx, new_type);
        }
      } else {
        bp.set_node_type(m_selected_node, new_type);
      }
      *m_selected->path = bp.to_path_data();
      if (m_history)
        m_history->push(std::make_unique<EditPathCommand>(
            project(), m_selected->internal_id,
            std::move(before), *m_selected->path, "Set node type"));
      LOG_INFO("NodeTool: node {} type → {}", m_selected_node, (int)new_type);
      m_sig_doc_changed.emit();
      m_sig_node_changed.emit(m_selected, m_selected_node);
      queue_draw();
      return true;
    }
  }

  // ── Reverse path direction (R) ────────────────────────────────────────
  if (keyval == GDK_KEY_r || keyval == GDK_KEY_R) {
    PathData before = *m_selected->path;
    bp.reverse();
    // After reversal the selected node index maps to (n-1-idx)
    if (m_selected_node >= 0)
      m_selected_node = n - 1 - m_selected_node;
    *m_selected->path = bp.to_path_data();
    PathData after = *m_selected->path;
    if (m_history)
      m_history->push(std::make_unique<EditPathCommand>(
          project(), m_selected->internal_id,
          std::move(before), std::move(after), "Reverse path"));
    m_sig_doc_changed.emit();
    queue_draw();
    LOG_INFO("NodeTool: reversed path '{}'", m_selected->id);
    return true;
  }

  // ── Arrow-key nudge ───────────────────────────────────────────────────
  // Zoom-relative: small=2px, medium=8px, large=32px screen pixels → doc units
  {
    double ndx = 0, ndy = 0;
    switch (keyval) {
    case GDK_KEY_Left:
      ndx = -1;
      break;
    case GDK_KEY_Right:
      ndx = 1;
      break;
    case GDK_KEY_Up:
      ndy = -1;
      break;
    case GDK_KEY_Down:
      ndy = 1;
      break;
    default:
      break;
    }
    if (ndx != 0.0 || ndy != 0.0) {
      double screen_px = alt ? 32.0 : (shift ? 8.0 : 2.0);
      if (ctrl)
        return false; // Ctrl+arrow reserved for arrange
      double step = screen_px / m_zoom;
      ndx *= step;
      ndy *= step;

      PathData before = *m_selected->path;

      if (m_selected_node >= 0 && m_selected_node < n) {
        // Move primary node
        BezierNode &nd = bp.nodes[m_selected_node];
        nd.x += ndx;
        nd.y += ndy;
        nd.cx1 += ndx;
        nd.cy1 += ndy;
        nd.cx2 += ndx;
        nd.cy2 += ndy;
        if (m_selected_node > 0 || bp.closed)
          bp.recompute_cusp_handles((m_selected_node - 1 + n) % n);
        bp.recompute_cusp_handles(m_selected_node);
        if (m_selected_node < n - 1 || bp.closed)
          bp.recompute_cusp_handles((m_selected_node + 1) % n);
      }
      *m_selected->path = bp.to_path_data();

      // Move all other nodes in m_node_selection
      for (const auto &ns : m_node_selection) {
        if (!ns.obj || !ns.obj->path)
          continue;
        if (ns.obj == m_selected && ns.node_idx == m_selected_node)
          continue;
        if (ns.node_idx < 0 || ns.node_idx >= (int)ns.obj->path->nodes.size())
          continue;
        BezierNode &nd = ns.obj->path->nodes[ns.node_idx];
        nd.x += ndx;
        nd.y += ndy;
        nd.cx1 += ndx;
        nd.cy1 += ndy;
        nd.cx2 += ndx;
        nd.cy2 += ndy;
      }
      PathData after = *m_selected->path;
      if (m_history) {
        using clock = std::chrono::steady_clock;
        auto now = clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - m_nudge_last_time)
                           .count();
        bool same_window = (m_nudge_last_obj == m_selected) && (elapsed < 1500);
        m_nudge_last_obj = m_selected;
        m_nudge_last_time = now;

        if (!same_window) {
          m_history->push(std::make_unique<EditPathCommand>(
              project(), m_selected->internal_id,
              std::move(before), std::move(after), "Nudge node"));
        } else {
          if (auto *cmd =
                  dynamic_cast<EditPathCommand *>(m_history->last_command()))
            if (cmd->obj_iid == m_selected->internal_id)
              cmd->after = after;
        }
      }
      m_sig_doc_changed.emit();
      queue_draw();
      LOG_DEBUG("NodeTool: nudge node {} by ({:.2f},{:.2f})", m_selected_node,
                ndx, ndy);
      return true;
    }
  }

  return false;
}

// ── Node editor
// ───────────────────────────────────────────────────────────────
void Canvas::on_node_begin(double x, double y) {
  if (!m_doc)
    return;
  // Record press position for dead-zone; drag hasn't started yet
  m_node_press_x = x;
  m_node_press_y = y;
  m_node_drag_started = false;

  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  Vec2 doc_pos{dx, dy};

  // ── Shift+click: toggle any node on any path in/out of multi-selection ─
  if (m_mod_shift && m_selected) {
    double tol_doc = 8.0 / m_zoom;

    std::function<bool(SceneNode *)> shift_scan = [&](SceneNode *node) -> bool {
      if (node->type == SceneNode::Type::Path && node->path) {
        for (int i = 0; i < (int)node->path->nodes.size(); ++i) {
          Vec2 np{node->path->nodes[i].x, node->path->nodes[i].y};
          if (np.dist(doc_pos) <= tol_doc) {
            auto it =
                std::find_if(m_node_selection.begin(), m_node_selection.end(),
                             [&](const NodeSel &ns) {
                               return ns.obj == node && ns.node_idx == i;
                             });
            if (it != m_node_selection.end()) {
              m_node_selection.erase(it);
              // If we just deselected the primary node, point to another
              if (m_selected_node == i && node == m_selected) {
                if (!m_node_selection.empty()) {
                  for (const auto &ns2 : m_node_selection) {
                    if (ns2.obj == m_selected) {
                      m_selected_node = ns2.node_idx;
                      break;
                    }
                  }
                }
              }
            } else {
              m_node_selection.push_back({node, i});
              if (node != m_selected) {
                m_selected = node;
                m_selection = {node};  // s159 m2: sync for SelectionContext
                m_selected_node = i;
                m_node_drag_kind = HitResult::Kind::Node;
              } else {
                m_selected_node = i;
                m_node_drag_kind = HitResult::Kind::Node;
              }
            }
            // s159 m2: refresh SelectionContext for both worlds.
            // Object-side mask compare suppresses no-op emits in the
            // deselect / same-path branches.
            notify_object_selection_changed();
            notify_node_selection_changed();
            m_sig_node_changed.emit(m_selected, m_selected_node);
            queue_draw();
            return true;
          }
        }
      } else if (node->type == SceneNode::Type::Group ||
                 node->type == SceneNode::Type::Compound) {
        for (auto &child : node->children)
          if (shift_scan(child.get()))
            return true;
      }
      return false;
    };

    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children)
        if (shift_scan(obj_uptr.get()))
          return;
    }
    // Shift+click missed all nodes — fall through to path body hit
  }

  // 1. Try to hit the currently selected path first
  if (m_selected && m_selected->type == SceneNode::Type::Path &&
      m_selected->path) {
    BezierPath bp = BezierPath::from_path_data(*m_selected->path);

    if (m_mod_shift) {
      // Already handled above — this branch only reached if no node was hit
      // Fall through to step 2 for path body shift+click
    }

    HitResult hit = bp.hit_test(doc_pos, m_zoom);
    if (hit.kind != HitResult::Kind::None) {
      m_node_drag_kind = hit.kind;
      m_selected_node = hit.node_index;
      m_cycle_last_pos = doc_pos;
      m_cycle_index = 0;

      // Only clear multi-selection if clicking a node NOT already in it
      bool already_in_selection = std::any_of(
          m_node_selection.begin(), m_node_selection.end(),
          [&](const NodeSel &ns) {
            return ns.obj == m_selected && ns.node_idx == hit.node_index;
          });
      if (!already_in_selection) {
        m_node_selection.clear();
        if (hit.kind == HitResult::Kind::Node)
          m_node_selection.push_back({m_selected, hit.node_index});
      }

      if (hit.kind == HitResult::Kind::OnCurve) {
        // Node-count lock: same reasoning as delete — A and B of a Blend
        // must have matching node counts.
        if (find_blend_owner(m_selected)) {
          LOG_INFO("NodeTool: insert-node rejected — path is a Blend source");
          m_sig_show_message.emit(
              "Blend",
              "This path is part of a Blend and its node count is locked. "
              "Release the Blend first to edit node counts.");
          queue_draw();
          return;
        }
        m_node_selection.clear(); // inserting always clears
        PathData before = *m_selected->path;
        bp.insert_node_at(hit.segment_index, hit.t);
        PathData after = bp.to_path_data();
        *m_selected->path = after;
        m_selected_node = hit.segment_index + 1;
        if (m_history)
          m_history->push(std::make_unique<EditPathCommand>(
              project(), m_selected->internal_id,
              std::move(before), std::move(after), "Insert node"));
        m_sig_doc_changed.emit();
      }
      // s160 m2: m_node_selection may have mutated above (clear+push at
      // 14820-14823, or clear at 14837 for OnCurve insert). Refresh
      // SelectionContext for both worlds before the node-changed emit.
      // Object side is unchanged in this branch — mask compare suppresses
      // a spurious emit; cost is one cheap recompute.
      notify_object_selection_changed();
      notify_node_selection_changed();
      m_sig_node_changed.emit(m_selected, m_selected_node);
      queue_draw();
      return;
    }
  }

  // 2. No hit on current selection — scan all OTHER path objects for the
  // closest hit Descends into Group and Compound children to find leaf paths.
  SceneNode *best_obj = nullptr;
  HitResult best_hit;
  best_hit.distance = 1e9;

  std::function<void(SceneNode *)> scan_node = [&](SceneNode *node) {
    if (node->type == SceneNode::Type::Path && node->path) {
      if (node == m_selected)
        return; // already checked in step 1
      BezierPath bp = BezierPath::from_path_data(*node->path);
      HitResult hit = bp.hit_test(doc_pos, m_zoom);
      if (hit.kind != HitResult::Kind::None &&
          hit.distance < best_hit.distance) {
        best_hit = hit;
        best_obj = node;
      }
    } else if (node->type == SceneNode::Type::Group ||
               node->type == SceneNode::Type::Compound) {
      for (auto &child : node->children)
        scan_node(child.get());
    }
  };

  for (auto &layer : m_doc->layers) {
    if (!layer->visible || layer->locked || layer->is_special_layer())
      continue;
    for (auto &obj_uptr : layer->children)
      scan_node(obj_uptr.get());
  }

  if (best_obj) {
    if (m_mod_shift && m_selected && best_obj != m_selected) {
      // ── Shift+click on different path body: add all its nodes ────────
      bool already_added = std::any_of(
          m_node_selection.begin(), m_node_selection.end(),
          [best_obj](const NodeSel &ns) { return ns.obj == best_obj; });
      if (already_added) {
        // Remove all nodes of this path
        m_node_selection.erase(std::remove_if(m_node_selection.begin(),
                                              m_node_selection.end(),
                                              [best_obj](const NodeSel &ns) {
                                                return ns.obj == best_obj;
                                              }),
                               m_node_selection.end());
      } else {
        // Add all nodes of this path
        for (int i = 0; i < (int)best_obj->path->nodes.size(); ++i)
          m_node_selection.push_back({best_obj, i});
      }
      LOG_DEBUG("NodeTool: shift+path '{}' → {} nodes in selection",
                best_obj->id, m_node_selection.size());
      // s160 m2: this branch mutated m_node_selection (add or remove all
      // nodes of best_obj) but had no signal at all pre-m2. Refresh
      // SelectionContext so node-side mask is fresh; object side unchanged.
      notify_object_selection_changed();
      notify_node_selection_changed();
      queue_draw();
      return;
    }

    // ── Plain click on unselected path — select it ────────────────────
    m_node_selection.clear();
    m_selected = best_obj;
    m_node_drag_kind = best_hit.kind == HitResult::Kind::OnCurve
                           ? HitResult::Kind::None
                           : best_hit.kind;
    m_selected_node =
        best_hit.kind == HitResult::Kind::OnCurve ? -1 : best_hit.node_index;
    LOG_DEBUG("NodeTool: new obj='{}' node={} kind={}", m_selected->id,
              m_selected_node, (int)m_node_drag_kind);
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_node_changed.emit(m_selected, m_selected_node);
  } else {
    LOG_DEBUG("NodeTool: clicked empty space — start marquee");
    m_selected = nullptr;
    m_selected2 = nullptr;
    m_selected_node = -1;
    m_selected_node2 = -1;
    m_node_selection.clear();
    m_node_drag_kind = HitResult::Kind::None;
    // Start node marquee
    m_marquee_active = true;
    m_marquee_start_dx = doc_pos.x;
    m_marquee_start_dy = doc_pos.y;
    m_marquee_cur_dx = doc_pos.x;
    m_marquee_cur_dy = doc_pos.y;
    // s160 m2: audit re-verification — this site was tentatively classified
    // E (refresh-side-effect) in s159 with a TODO claiming "selection state
    // isn't actually cleared at this site." That claim is empirically wrong:
    // m_selected, m_selected2, m_selected_node, m_selected_node2, and
    // m_node_selection are all wiped at lines 14953-14957 above. This is a
    // real selection-clear emit. Migrating to the helpers — SelectionContext
    // mask compare suppresses no-op emits when the prior state was already
    // empty, so this is safe under all entry conditions.
    notify_object_selection_changed();
    notify_node_selection_changed();
    m_sig_node_changed.emit(nullptr, -1);
  }
  queue_draw();
}

void Canvas::on_node_update(double /*delta_x*/, double /*delta_y*/) {
  // ── Node marquee update ───────────────────────────────────────────────
  if (m_marquee_active) {
    double cx, cy;
    screen_to_doc(m_mouse_x, m_mouse_y, cx, cy);
    m_marquee_cur_dx = cx;
    m_marquee_cur_dy = cy;
    queue_draw();
    return;
  }

  if (!m_selected || m_selected->type != SceneNode::Type::Path ||
      !m_selected->path)
    return;
  if (m_selected_node < 0)
    return;
  if (m_node_drag_kind == HitResult::Kind::None)
    return;

  // Dead-zone: suppress movement until cursor has travelled enough from press
  if (!m_node_drag_started) {
    double dist =
        std::hypot(m_mouse_x - m_node_press_x, m_mouse_y - m_node_press_y);
    if (dist < NODE_DRAG_THRESHOLD_PX)
      return;
    m_node_drag_started = true;
    // Snapshot before-state for undo
    m_node_drag_before = *m_selected->path;
    m_node_drag_before_multi.clear();
    for (const auto &ns : m_node_selection) {
      if (!ns.obj || !ns.obj->path || ns.obj == m_selected)
        continue;
      bool already = std::any_of(
          m_node_drag_before_multi.begin(), m_node_drag_before_multi.end(),
          [&](const auto &p) { return p.first == ns.obj; });
      if (!already)
        m_node_drag_before_multi.push_back({ns.obj, *ns.obj->path});
    }
  }

  double dx, dy;
  screen_to_doc(m_mouse_x, m_mouse_y, dx, dy);
  Vec2 doc_pos{dx, dy};

  BezierPath bp = BezierPath::from_path_data(*m_selected->path);
  if (m_selected_node >= (int)bp.nodes.size())
    return;

  // Snapshot before move to detect actual change
  const BezierNode &nb = bp.nodes[m_selected_node];
  double pre_x = nb.x, pre_y = nb.y;
  double pre_ix = nb.cx1, pre_iy = nb.cy1;
  double pre_ox = nb.cx2, pre_oy = nb.cy2;

  // ── Endpoint snap ─────────────────────────────────────────────────────
  // When dragging an endpoint of an open path, snap to nearby endpoints
  // of other open paths. This sets up the merge point for J.
  m_snap_target_obj = nullptr;
  m_snap_target_end = -1;

  bool is_endpoint =
      !m_selected->path->closed && m_node_drag_kind == HitResult::Kind::Node &&
      (m_selected_node == 0 ||
       m_selected_node == (int)m_selected->path->nodes.size() - 1);

  if (is_endpoint && m_doc) {
    static constexpr double ENDPOINT_SNAP_PX = 10.0; // screen pixels
    double best_d2 = ENDPOINT_SNAP_PX * ENDPOINT_SNAP_PX;

    // ── Same path: snap tail→head or head→tail (closing gesture) ─────
    {
      int n_nodes = (int)m_selected->path->nodes.size();
      int other_end_idx = (m_selected_node == 0) ? n_nodes - 1 : 0;
      if (n_nodes >= 2) {
        const BezierNode &other_ep = m_selected->path->nodes[other_end_idx];
        double sx, sy;
        doc_to_screen(other_ep.x, other_ep.y, sx, sy);
        double ddx = m_mouse_x - sx, ddy = m_mouse_y - sy;
        double d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2) {
          best_d2 = d2;
          m_snap_target_obj = m_selected;
          m_snap_target_end = (other_end_idx == 0) ? 0 : 1;
          doc_pos = Vec2{other_ep.x, other_ep.y};
        }
      }
    }

    // ── Other paths: snap to their endpoints ──────────────────────────
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode &other = *obj_uptr;
        if (&other == m_selected)
          continue;
        if (other.type != SceneNode::Type::Path || !other.path)
          continue;
        if (other.path->closed)
          continue;
        if (other.path->nodes.empty())
          continue;

        for (int end = 0; end < 2; ++end) {
          const BezierNode &ep =
              end == 0 ? other.path->nodes.front() : other.path->nodes.back();
          double sx, sy;
          doc_to_screen(ep.x, ep.y, sx, sy);
          double ddx = m_mouse_x - sx, ddy = m_mouse_y - sy;
          double d2 = ddx * ddx + ddy * ddy;
          if (d2 < best_d2) {
            best_d2 = d2;
            m_snap_target_obj = &other;
            m_snap_target_end = end;
            doc_pos = Vec2{ep.x, ep.y};
          }
        }
      }
    }
  }

  // ── Shift-axis constraint (Node + Handle drags) ───────────────────────
  // Anchor drag: lock to dominant axis from the pre-drag anchor position
  // (Affinity convention — shift = "I want a straight line").
  // Handle drag: lock to dominant axis from the ANCHOR. The anchor is the
  // natural reference because a handle's role is to point a direction
  // outward from its anchor; H/V-locking that direction is the predictable
  // analogue of the anchor's axis-lock. (Affinity uses 45° increments for
  // handles instead — flagged as a follow-up if we want that fidelity.)
  // Applied AFTER endpoint-snap so shift overrides snap.
  //
  // Snap on the LOCKED axis is bypassed below for the Node case — a
  // horizontal-lock with a horizontal guide near the start Y would
  // otherwise snap-pull the lock off-axis. Handles don't go through
  // snap_x/y in the existing code, so no snap-bypass plumbing for them.
  bool shift_lock_x = false; // true ⇒ X is locked, snap_x bypassed
  bool shift_lock_y = false; // true ⇒ Y is locked, snap_y bypassed
  if (m_mod_shift &&
      m_selected_node >= 0 &&
      m_selected_node < (int)m_node_drag_before.nodes.size() &&
      (m_node_drag_kind == HitResult::Kind::Node ||
       m_node_drag_kind == HitResult::Kind::HandleIn ||
       m_node_drag_kind == HitResult::Kind::HandleOut)) {
    const BezierNode &orig = m_node_drag_before.nodes[m_selected_node];
    // Reference is the anchor position for ALL three drag kinds. For
    // anchor drag this is the pre-drag anchor; for handle drag this is
    // the live anchor (handles never alter their own anchor, so the
    // pre-drag value is still the current value).
    double cdx = doc_pos.x - orig.x;
    double cdy = doc_pos.y - orig.y;
    if (std::abs(cdx) >= std::abs(cdy)) {
      doc_pos.y = orig.y;        // horizontal lock
      shift_lock_y = true;
    } else {
      doc_pos.x = orig.x;        // vertical lock
      shift_lock_x = true;
    }
  }

  switch (m_node_drag_kind) {
  case HitResult::Kind::Node:
    bp.move_node(m_selected_node,
                 {shift_lock_x ? doc_pos.x : snap_x(doc_pos.x),
                  shift_lock_y ? doc_pos.y : snap_y(doc_pos.y)});
    break;
  case HitResult::Kind::HandleIn:
    bp.move_handle_in(m_selected_node, doc_pos);
    break;
  case HitResult::Kind::HandleOut:
    bp.move_handle_out(m_selected_node, doc_pos);
    break;
  default:
    break;
  }
  *m_selected->path = bp.to_path_data();

  // ── Apply same delta to all other nodes in m_node_selection ──────────
  if (m_node_drag_kind == HitResult::Kind::Node && !m_node_selection.empty()) {
    const BezierNode &moved = m_selected->path->nodes[m_selected_node];
    double delta_x = moved.x - pre_x;
    double delta_y = moved.y - pre_y;
    for (const auto &ns : m_node_selection) {
      if (!ns.obj || !ns.obj->path)
        continue;
      if (ns.obj == m_selected && ns.node_idx == m_selected_node)
        continue;
      if (ns.node_idx < 0 || ns.node_idx >= (int)ns.obj->path->nodes.size())
        continue;
      BezierNode &nd = ns.obj->path->nodes[ns.node_idx];
      nd.x += delta_x;
      nd.y += delta_y;
      nd.cx1 += delta_x;
      nd.cy1 += delta_y;
      nd.cx2 += delta_x;
      nd.cy2 += delta_y;
    }
  }

  // Only emit if any coordinate actually changed
  const BezierNode &na = bp.nodes[m_selected_node];
  bool changed =
      (std::abs(na.x - pre_x) > 1e-9 || std::abs(na.y - pre_y) > 1e-9 ||
       std::abs(na.cx1 - pre_ix) > 1e-9 || std::abs(na.cy1 - pre_iy) > 1e-9 ||
       std::abs(na.cx2 - pre_ox) > 1e-9 || std::abs(na.cy2 - pre_oy) > 1e-9);
  if (changed)
    m_sig_node_changed.emit(m_selected, m_selected_node);
  queue_draw();
}

void Canvas::on_node_end() {
  // ── Node marquee end — select all nodes within rect ───────────────────
  if (m_marquee_active) {
    m_marquee_active = false;
    double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx);
    double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy);
    double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx);
    double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy);

    if (x2 - x1 > 1.0 && y2 - y1 > 1.0 && m_doc) {
      m_node_selection.clear();
      m_selected = nullptr;
      m_selected_node = -1;

      for (auto &layer : m_doc->layers) {
        if (!layer->visible || layer->locked || layer->is_special_layer())
          continue;
        for (auto &obj_uptr : layer->children) {
          SceneNode &obj = *obj_uptr;
          if (obj.type != SceneNode::Type::Path || !obj.path)
            continue;

          auto bb = object_bbox(obj);
          if (!bb)
            continue;
          bool path_hit = (bb->x < x2 && bb->x + bb->w > x1 && bb->y < y2 &&
                           bb->y + bb->h > y1);
          if (!path_hit)
            continue;

          if (!m_selected)
            m_selected = &obj;

          for (int i = 0; i < (int)obj.path->nodes.size(); ++i) {
            const BezierNode &nd = obj.path->nodes[i];
            if (nd.x >= x1 && nd.x <= x2 && nd.y >= y1 && nd.y <= y2) {
              m_node_selection.push_back({&obj, i});
              if (m_selected_node < 0 && &obj == m_selected)
                m_selected_node = i;
            }
          }
        }
      }

      if (m_selected) {
        notify_object_selection_changed();
        notify_node_selection_changed();
        m_sig_node_changed.emit(m_selected, m_selected_node);
      }
    }
    queue_draw();
    return;
  }

  if (m_node_drag_kind != HitResult::Kind::None && m_node_drag_started) {
    // Push undo for the drag
    if (m_history && m_selected && m_selected->path) {
      if (m_node_drag_before_multi.empty()) {
        // Single path drag
        m_history->push(std::make_unique<EditPathCommand>(
            project(), m_selected->internal_id,
            m_node_drag_before, *m_selected->path, "Move node"));
      } else {
        // Multi-path drag — use ScaleObjectsCommand to bundle all snaps
        std::vector<ScaleObjectsCommand::LeafSnap> snaps;
        snaps.push_back({m_selected->internal_id, m_node_drag_before, *m_selected->path});
        for (auto &[obj, before] : m_node_drag_before_multi)
          if (obj->path)
            snaps.push_back({obj->internal_id, before, *obj->path});
        m_history->push(std::make_unique<ScaleObjectsCommand>(
            project(), std::move(snaps), "Move nodes"));
      }
    }
    m_sig_doc_changed.emit();
    m_sig_node_changed.emit(m_selected, m_selected_node);

    // ── Auto-set secondary selection when endpoint snapped ────────────
    if (m_snap_target_obj && m_snap_target_obj->path &&
        m_snap_target_obj != m_selected) {
      m_selected2 = m_snap_target_obj;
      m_selected_node2 = (m_snap_target_end == 0)
                             ? 0
                             : (int)m_snap_target_obj->path->nodes.size() - 1;
      LOG_DEBUG("NodeTool: snap ended — secondary set to obj='{}' node={}",
                m_selected2->id, m_selected_node2);
    }
  }
  m_node_drag_kind = HitResult::Kind::None;
  m_node_drag_started = false;
}

void Canvas::apply_node_edit(int node_idx, double x, double y, double cx1,
                             double cy1, double cx2, double cy2) {
  if (!m_selected || !m_selected->path)
    return;
  if (node_idx < 0 || node_idx >= (int)m_selected->path->nodes.size())
    return;

  // Never overwrite path data during an active drag — on_node_update owns
  // the path during a drag and reads from *m_selected->path at the start of
  // every frame. A write here would corrupt the next frame's starting state.
  if (m_node_drag_started)
    return;

  const BezierNode &prev = m_selected->path->nodes[node_idx];
  LOG_DEBUG(
      "apply_node_edit: node={} type={} x={:.2f}→{:.2f} y={:.2f}→{:.2f} "
      "cx1={:.2f}→{:.2f} cy1={:.2f}→{:.2f} cx2={:.2f}→{:.2f} cy2={:.2f}→{:.2f}",
      node_idx, (int)prev.type, prev.x, x, prev.y, y, prev.cx1, cx1, prev.cy1,
      cy1, prev.cx2, cx2, prev.cy2, cy2);

  BezierNode &n = m_selected->path->nodes[node_idx];
  n.x = x;
  n.y = y;
  n.cx1 = cx1;
  n.cy1 = cy1;
  n.cx2 = cx2;
  n.cy2 = cy2;

  queue_draw();
}

void Canvas::on_corner_begin(double x, double y) {
  if (!m_doc)
    return;

  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  Vec2 doc_pos{dx, dy};
  double tol = 8.0 / m_zoom;

  // Try to hit a Corner/Cusp node
  SceneNode *hit_obj = nullptr;
  int hit_idx = -1;
  double hit_d2 = tol * tol;

  foreach_corner_node(m_doc, [&](SceneNode *obj, int i) {
    Vec2 np{obj->path->nodes[i].x, obj->path->nodes[i].y};
    double d2 = np.dist_sq(doc_pos);
    if (d2 < hit_d2) {
      hit_d2 = d2;
      hit_obj = obj;
      hit_idx = i;
    }
    return false; // keep scanning for nearest
  });

  if (hit_obj) {
    if (m_mod_shift) {
      // Shift+click: toggle
      auto it =
          std::find_if(m_corner_selection.begin(), m_corner_selection.end(),
                       [&](const CornerSel &cs) {
                         return cs.obj == hit_obj && cs.node_idx == hit_idx;
                       });
      if (it != m_corner_selection.end())
        m_corner_selection.erase(it);
      else
        m_corner_selection.push_back({hit_obj, hit_idx});
    } else {
      // Plain click: select only this node (unless it was already selected)
      bool already = corner_sel_contains(hit_obj, hit_idx);
      if (!already) {
        m_corner_selection.clear();
        m_corner_selection.push_back({hit_obj, hit_idx});
      }
    }
    m_corner_rubber_active = false;
  } else {
    // Missed all nodes — start rubber-band (clears selection unless Shift)
    if (!m_mod_shift)
      m_corner_selection.clear();
    m_corner_rubber_active = true;
    m_corner_rubber_x0 = x;
    m_corner_rubber_y0 = y;
    m_corner_rubber_x1 = x;
    m_corner_rubber_y1 = y;
  }

  m_drawing = true; // enable drag gesture routing
  m_sig_corner_sel_changed.emit((int)m_corner_selection.size());
  queue_draw();
}

void Canvas::on_corner_motion(double /*x*/, double /*y*/) {
  if (!m_doc)
    return;
  if (m_corner_rubber_active) {
    m_corner_rubber_x1 = m_mouse_x;
    m_corner_rubber_y1 = m_mouse_y;
    queue_draw();
  }
}

void Canvas::on_corner_end(double /*x*/, double /*y*/) {
  if (!m_doc)
    return;

  if (m_corner_rubber_active) {
    // Finalise rubber-band: select all Corner/Cusp nodes inside the rect
    m_corner_rubber_active = false;

    double sx0 = std::min(m_corner_rubber_x0, m_mouse_x);
    double sy0 = std::min(m_corner_rubber_y0, m_mouse_y);
    double sx1 = std::max(m_corner_rubber_x0, m_mouse_x);
    double sy1 = std::max(m_corner_rubber_y0, m_mouse_y);

    foreach_corner_node(m_doc, [&](SceneNode *obj, int i) {
      double sx, sy;
      doc_to_screen(obj->path->nodes[i].x, obj->path->nodes[i].y, sx, sy);
      if (sx >= sx0 && sx <= sx1 && sy >= sy0 && sy <= sy1) {
        if (!corner_sel_contains(obj, i))
          m_corner_selection.push_back({obj, i});
      }
      return false;
    });

    m_sig_corner_sel_changed.emit((int)m_corner_selection.size());
    queue_draw();
  }
}

void Canvas::on_ruler_begin(double x, double y) {
  if (!m_doc)
    return;
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  // dy is in Cairo doc space (Y-down); convert to user space (Y-up)
  double ux = dx;
  double uy = m_doc->canvas_height() - dy;

  double tol = 8.0 / m_zoom;

  // ── Click-to-copy hit test first ───────────────────────────────────────
  // S89: m_ruler_labels carries hit-test rects from BOTH the live ruler
  // triangle (when A/B are picked) AND saved measurements drawn by the
  // persistent overlay (gated on ruler_active in the persistent draw
  // block). So a non-empty labels vector means there's something
  // clickable; we don't need to pre-check live A/B presence.
  if (!m_ruler_labels.empty()) {
    for (auto &lbl : m_ruler_labels) {
      if (x >= lbl.sx && x <= lbl.sx + lbl.sw && y >= lbl.sy &&
          y <= lbl.sy + lbl.sh) {

        // S89: single-click on any label always copies the full
        // structured block (x1,y1 / x2,y2 / Δx,Δy / distance / angles).
        // Alt-modifier no longer distinguishes — both produce the same
        // payload. The label that was clicked is recorded in copy_value
        // by the renderer (set to the structured block uniformly).
        std::string to_copy = lbl.copy_value;
        // Copy to clipboard via Gdk::ContentProvider (GTK4 API)
        auto disp = get_display();
        if (disp) {
          auto clip = disp->get_clipboard();
          if (clip) {
            Glib::Value<Glib::ustring> val;
            val.init(Glib::Value<Glib::ustring>::value_type());
            val.set(Glib::ustring(to_copy));
            clip->set_content(Gdk::ContentProvider::create(val));
          }
        }
        m_ruler_toast_text = "Copied measurement data";
        m_ruler_toast_x = lbl.sx + lbl.sw * 0.5;
        m_ruler_toast_y = lbl.sy - 6;
        m_ruler_toast_ms = 1500;
        if (m_ruler_toast_conn.connected())
          m_ruler_toast_conn.disconnect();
        m_ruler_toast_conn = Glib::signal_timeout().connect(
            [this]() -> bool {
              m_ruler_toast_ms -= 50;
              if (m_ruler_toast_ms <= 0) {
                m_ruler_toast_ms = 0;
              }
              queue_draw();
              return m_ruler_toast_ms > 0;
            },
            50);

        // S89: "Delete on copy" applies only to TRANSIENT measurements —
        // i.e. the live A/B picks shown by the ruler tool overlay. Saved
        // entries on the measure layer are NEVER auto-deleted by copy
        // (permanent until manually × deleted from inspector or layers).
        // When measure_save_to_layer is ON, completion auto-saves and the
        // live A/B are reset at save-time, so by the time the user sees a
        // label to copy, the underlying entry is a saved one — destruct
        // doesn't apply. When OFF, the live measurement is transient and
        // destruct dismisses it from canvas.
        if (m_doc->measure_destruct_after_copy &&
            !m_doc->measure_save_to_layer) {
          // Reset ruler session state so the next click starts a fresh
          // pick — same as hitting Space. Done inline rather than calling
          // ruler_clear() so the "Copied …" toast stays visible.
          m_ruler_node_a_obj = nullptr;
          m_ruler_node_a_idx = -1;
          m_ruler_node_b_obj = nullptr;
          m_ruler_node_b_idx = -1;
          m_ruler_labels.clear();
        }
        queue_draw();
        return;
      }
    }
  }

  // ── Endpoint hit test — pick closest node OR refpt within tolerance ───
  // S89: refpts joined the candidate set. ruler_endpoint_pos resolves
  // both kinds to a doc-space position uniformly.
  std::vector<std::pair<SceneNode *, int>> all_nodes;
  ruler_collect_all_endpoints(all_nodes);

  SceneNode *hit_obj = nullptr;
  int hit_idx = -1;
  double best_d = tol;

  for (auto &[obj, ni] : all_nodes) {
    double ex, ey;
    if (!ruler_endpoint_pos(obj, ni, ex, ey))
      continue;
    double d = std::hypot(ex - dx, ey - dy);
    if (d < best_d) {
      best_d = d;
      hit_obj = obj;
      hit_idx = ni;
    }
  }

  if (!hit_obj) {
    // Clicked empty space — start marquee to box-select nodes
    m_marquee_active = true;
    m_marquee_start_dx = dx;
    m_marquee_start_dy = dy;
    m_marquee_cur_dx = dx;
    m_marquee_cur_dy = dy;
    return;
  }

  // Shift+click — add/remove from the two-node selection
  if (m_mod_shift) {
    // If this node is already A or B, deselect it
    bool was_a =
        (hit_obj == m_ruler_node_a_obj && hit_idx == m_ruler_node_a_idx);
    bool was_b =
        (hit_obj == m_ruler_node_b_obj && hit_idx == m_ruler_node_b_idx);
    if (was_a) {
      m_ruler_node_a_obj = m_ruler_node_b_obj;
      m_ruler_node_a_idx = m_ruler_node_b_idx;
      m_ruler_node_b_obj = nullptr;
      m_ruler_node_b_idx = -1;
    } else if (was_b) {
      m_ruler_node_b_obj = nullptr;
      m_ruler_node_b_idx = -1;
    } else {
      // Promote A→B, set new as A
      m_ruler_node_b_obj = m_ruler_node_a_obj;
      m_ruler_node_b_idx = m_ruler_node_a_idx;
      m_ruler_node_a_obj = hit_obj;
      m_ruler_node_a_idx = hit_idx;
      // S89: shift+click that lands a fresh {A,B} pair is a completion
      // event — auto-save if the doc flag is on. Helper resets A/B on save.
      ruler_try_auto_save();
    }
  } else {
    // Plain click — set as A, clear B
    m_ruler_node_a_obj = hit_obj;
    m_ruler_node_a_idx = hit_idx;
    m_ruler_node_b_obj = nullptr;
    m_ruler_node_b_idx = -1;
  }
  queue_draw();
}

void Canvas::on_ruler_motion(double /*x*/, double /*y*/) {
  // Marquee redraws are handled by on_draw_update writing m_marquee_cur_*
  queue_draw();
}

void Canvas::on_ruler_end(double /*x*/, double /*y*/) {
  if (!m_doc)
    return;
  if (!m_marquee_active)
    return;

  m_marquee_active = false;
  double x1 = std::min(m_marquee_start_dx, m_marquee_cur_dx);
  double y1 = std::min(m_marquee_start_dy, m_marquee_cur_dy);
  double x2 = std::max(m_marquee_start_dx, m_marquee_cur_dx);
  double y2 = std::max(m_marquee_start_dy, m_marquee_cur_dy);

  if (x2 - x1 < 1.0 || y2 - y1 < 1.0) {
    queue_draw();
    return;
  }

  // Collect endpoints inside marquee rect (doc space, Y-down). S89: refpts
  // are picked alongside path nodes — same kind logic as the click hit test.
  std::vector<std::pair<SceneNode *, int>> all_nodes;
  ruler_collect_all_endpoints(all_nodes);
  std::vector<std::pair<SceneNode *, int>> inside;
  for (auto &[obj, ni] : all_nodes) {
    double ex, ey;
    if (!ruler_endpoint_pos(obj, ni, ex, ey))
      continue;
    if (ex >= x1 && ex <= x2 && ey >= y1 && ey <= y2)
      inside.push_back({obj, ni});
  }

  if (inside.size() == 0) {
    queue_draw();
    return;
  }
  if (inside.size() == 1) {
    m_ruler_node_a_obj = inside[0].first;
    m_ruler_node_a_idx = inside[0].second;
    m_ruler_node_b_obj = nullptr;
    m_ruler_node_b_idx = -1;
    queue_draw();
    return;
  }
  if (inside.size() == 2) {
    m_ruler_node_a_obj = inside[0].first;
    m_ruler_node_a_idx = inside[0].second;
    m_ruler_node_b_obj = inside[1].first;
    m_ruler_node_b_idx = inside[1].second;
    // S89: marquee-with-2 is a single user gesture that completes the pair.
    // Auto-save if flag is on. Helper resets A/B on save.
    ruler_try_auto_save();
    queue_draw();
    return;
  }

  // >2 nodes — inform user
  if (auto *win = dynamic_cast<Gtk::Window *>(get_root()))
    curvz::utils::show_alert(
        *win, "Too many nodes selected",
        "Only 2 nodes can be measured at a time. Please marquee "
        "exactly 2 nodes.");
  queue_draw();
}

void Canvas::on_top_begin(double x, double y) {
  if (!m_doc)
    return;
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  double tol = 10.0 / m_zoom;
  LOG_DEBUG("on_top_begin: phase={} screen=({:.1f},{:.1f}) doc=({:.1f},{:.1f})",
            m_top_phase, x, y, dx, dy);

  // Phase 2: check if clicking the offset drag handle OR anywhere on the guide
  // path
  if (m_top_phase == 2 && m_top_text && !m_top_text->text_path_id.empty()) {
    SceneNode *guide = top_find_path_by_id(m_top_text->text_path_id);
    if (guide && guide->path) {
      BezierPath bp = BezierPath::from_path_data(*guide->path);
      std::vector<double> arc_table;
      double total = build_arc_table(bp, arc_table);

      // Hit-test the I-beam handle (tight tolerance).
      // flip=true: I-beam is at the mirrored arc position.
      double ibeam_arc = m_top_text->text_path_flip
                             ? total - m_top_text->text_path_offset
                             : m_top_text->text_path_offset;
      Vec2 pos;
      double angle;
      if (path_point_at(bp, arc_table, total, ibeam_arc, pos, angle)) {
        double hsx, hsy;
        doc_to_screen(pos.x, pos.y, hsx, hsy);
        double dist = std::hypot(x - hsx, y - hsy);
        LOG_DEBUG("on_top_begin phase2: ibeam screen=({:.1f},{:.1f}) "
                  "dist={:.1f} tol={:.1f}",
                  hsx, hsy, dist, tol + 6.0);
        if (dist < tol + 6.0) {
          LOG_DEBUG("on_top_begin phase2: HIT ibeam — starting drag");
          m_top_dragging = true;
          m_top_drag_start_off = m_top_text->text_path_offset;
          m_top_drag_start_x = x;
          m_top_drag_start_y = y;
          return;
        }
      }
      // Also hit-test the full path stroke
      HitResult hr = bp.hit_test({dx, dy}, m_zoom, 10.0);
      LOG_DEBUG("on_top_begin phase2: path hit_test kind={}", (int)hr.kind);
      if (hr.kind != HitResult::Kind::None) {
        LOG_DEBUG("on_top_begin phase2: HIT path stroke — starting drag");
        m_top_dragging = true;
        m_top_drag_start_off = m_top_text->text_path_offset;
        m_top_drag_start_x = x;
        m_top_drag_start_y = y;
        return;
      }
    }
    // Phase 2 miss: click hit neither I-beam nor guide path stroke.
    // Stay in phase 2 — a miss is a no-op, not a reason to lose the selection.
    return;
  }

  // Right-click is handled by on_top_rclick — skip here
  // Phase 0: pick a text node
  if (m_top_phase == 0) {
    // Hit-test text nodes
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode *obj = obj_uptr.get();
        if (!obj->is_text())
          continue;

        bool hit = false;
        if (!obj->text_path_id.empty()) {
          // Linked text: hit-test the guide path stroke so the user
          // can click anywhere along the path to re-select the link.
          SceneNode *guide = top_find_path_by_id(obj->text_path_id);
          if (guide && guide->path) {
            BezierPath bp = BezierPath::from_path_data(*guide->path);
            HitResult hr = bp.hit_test({dx, dy}, m_zoom, 10.0);
            hit = (hr.kind != HitResult::Kind::None);
          }
        } else {
          // Unlinked text: use approx bounding box
          double approx_w =
              obj->text_content.size() * obj->text_font_size * 0.6;
          double approx_h = obj->text_font_size * 1.4;
          double tx = obj->text_x;
          double ty = obj->text_y;
          if (obj->text_anchor == "middle")
            tx -= approx_w * 0.5;
          if (obj->text_anchor == "end")
            tx -= approx_w;
          hit = (dx >= tx - 4 && dx <= tx + approx_w + 4 &&
                 dy >= ty - approx_h && dy <= ty + 4);
        }

        if (hit) {
          m_top_text = obj;
          // If already linked, jump straight to phase 2
          if (!obj->text_path_id.empty()) {
            m_top_path_node = top_find_path_by_id(obj->text_path_id);
            m_top_phase = 2;
          } else {
            m_top_phase = 1;
            m_top_path_node = nullptr;
          }
          m_selected = obj;
          m_selection = {obj};  // s159 m2: keep m_selection in sync for SelectionContext
          notify_object_selection_changed();
          queue_draw();
          return;
        }
      }
    }
    // Missed — reset
    m_top_text = nullptr;
    m_top_phase = 0;
    queue_draw();
    return;
  }

  // Phase 0: also check if user clicked a bare path (no pre-selected text)
  // → create a new text node linked to that path and enter edit mode.
  if (m_top_phase == 0) {
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode *obj = obj_uptr.get();
        if (!obj->is_path() || !obj->path)
          continue;
        BezierPath bp = BezierPath::from_path_data(*obj->path);
        HitResult hr = bp.hit_test({dx, dy}, m_zoom, 8.0);
        if (hr.kind != HitResult::Kind::None) {
          // Ensure path has stable internal_id
          if (obj->internal_id.empty())
            obj->internal_id = generate_internal_id();
          // Find path start in doc space to position text anchor
          Vec2 path_start{obj->path->nodes.front().x,
                          obj->path->nodes.front().y};
          // Create new text node
          auto tn = std::make_unique<SceneNode>();
          tn->type = SceneNode::Type::Text;
          tn->internal_id = generate_internal_id();
          tn->id =
              "text_" + std::to_string(reinterpret_cast<uintptr_t>(tn.get()));
          tn->name = m_doc->next_default_name(CurvzDocument::NameKind::Text);
          tn->text_content = "";
          tn->text_x = path_start.x;
          tn->text_y = path_start.y;
          tn->text_font_family = "Sans";
          tn->text_font_size = 24.0;
          style::mutate_appearance(*tn, [](SceneNode& n) {
            n.fill.type = FillStyle::Type::CurrentColor;
            n.stroke.paint.type = FillStyle::Type::None;
          });
          tn->text_path_id = obj->internal_id;
          tn->text_path_offset = 0.0;
          tn->text_path_flip = false;
          SceneNode *tn_ptr = tn.get();
          // Place in active layer
          SceneNode *al = m_doc->active_layer();
          if (al)
            al->children.push_back(std::move(tn));
          // Enter text editing mode
          m_top_text = tn_ptr;
          m_top_path_node = obj;
          m_top_phase = 2;
          // Enter text editing mode (same as Text tool activation)
          m_text_editing = tn_ptr;
          m_text_is_new = true;
          if (m_text_entry) {
            m_text_entry_conn_activate.disconnect();
            m_text_entry_conn_changed.disconnect();
            m_text_entry->set_text("");
            m_text_entry->set_visible(true);
            m_text_entry->grab_focus();
            position_text_entry();
            m_text_entry_conn_changed =
                m_text_entry->signal_changed().connect([this]() {
                  if (m_text_editing)
                    m_text_editing->text_content = m_text_entry->get_text();
                  queue_draw();
                });
            m_text_entry_conn_activate =
                m_text_entry->signal_activate().connect([this, tn_ptr, obj]() {
                  // Save TOP state before commit_text_edit, which
                  // emits request_tool(Selection) internally and
                  // will cause set_active_tool(TextOnPath) to reset.
                  SceneNode *saved_text = tn_ptr;
                  SceneNode *saved_path_node = obj;
                  commit_text_edit();
                  // Return to TextOnPath tool, then immediately
                  // restore phase 2 so inspector sees the text node.
                  set_active_tool(ActiveTool::TextOnPath);
                  m_top_text = saved_text;
                  m_top_path_node = saved_path_node;
                  m_top_phase = 2;
                  m_selected = saved_text;
                  notify_object_selection_changed();
                  queue_draw();
                });
          }
          queue_draw();
          return;
        }
      }
    }
  }

  // Phase 1: pick a path node to link
  if (m_top_phase == 1 && m_top_text) {
    // Hit-test path objects
    for (auto &layer : m_doc->layers) {
      if (!layer->visible || layer->locked || layer->is_special_layer())
        continue;
      for (auto &obj_uptr : layer->children) {
        SceneNode *obj = obj_uptr.get();
        if (!obj->is_path() || !obj->path)
          continue;
        if (obj == m_top_text)
          continue;
        BezierPath bp = BezierPath::from_path_data(*obj->path);
        HitResult hr = bp.hit_test({dx, dy}, m_zoom, 12.0);
        if (hr.kind != HitResult::Kind::None) {
          // Check multi-line warning
          const std::string &tc = m_top_text->text_content;
          if (tc.find('\n') != std::string::npos) {
            if (auto *win = dynamic_cast<Gtk::Window *>(get_root()))
              curvz::utils::show_alert(
                  *win, "Text on path supports single-line text only.",
                  "Only the first line of text will be used. "
                  "Please edit the text to a single line.");
          }
          // Ensure path has stable internal_id
          if (obj->internal_id.empty())
            obj->internal_id = generate_internal_id();
          // s175 m2: ensure text node has stable internal_id too — the
          // migrated LinkTextToPathCommand captures m_top_text's iid for
          // resolution at undo/redo time. Symmetric with the path-side
          // ensure above.
          if (m_top_text->internal_id.empty())
            m_top_text->internal_id = generate_internal_id();
          // Push undo command before mutating
          if (m_history) {
            m_history->push(std::make_unique<LinkTextToPathCommand>(
                project(), m_top_text->internal_id,
                m_top_text->text_path_id, // before (empty or old)
                m_top_text->text_path_offset, m_top_text->text_path_flip,
                m_top_text->text_x, m_top_text->text_y, // before x/y
                obj->internal_id,                       // after
                0.0, false, m_top_text->text_x,
                m_top_text->text_y)); // after x/y (unchanged)
          }
          m_top_text->text_path_id = obj->internal_id;
          m_top_text->text_path_offset = 0.0;
          m_top_text->text_path_flip = false;
          m_top_path_node = obj;
          m_top_phase = 2;
          // Keep text node selected so inspector shows text panel
          m_selected = m_top_text;
          m_selection = {m_top_text};  // s159 m2: keep m_selection in sync for SelectionContext
          notify_object_selection_changed();
          // Persist the link immediately — ensures data-curvz-iid is
          // written to SVG before next load, so text_path_id survives
          // restarts without UUID re-assignment breaking the link.
          m_sig_doc_changed.emit();
          queue_draw();
          return;
        }
      }
    }
  }
}

void Canvas::on_top_motion(double x, double y) {
  if (!m_top_dragging || !m_top_text) {
    queue_draw();
    return;
  }

  // x, y are already doc-space coordinates (m_draw_cur_dx/dy passed by caller).
  SceneNode *guide = top_find_path_by_id(m_top_text->text_path_id);
  if (guide && guide->path) {
    BezierPath bp = BezierPath::from_path_data(*guide->path);
    std::vector<double> arc_table;
    double total = build_arc_table(bp, arc_table);

    // Sample path at fine intervals to find nearest arc position
    const int samples = 256;
    double best_dist2 = 1e18;
    double best_arc = m_top_text->text_path_offset;
    for (int i = 0; i <= samples; ++i) {
      double arc = total * i / (double)samples;
      Vec2 pt;
      double ang;
      if (path_point_at(bp, arc_table, total, arc, pt, ang)) {
        double d2 = (pt.x - x) * (pt.x - x) + (pt.y - y) * (pt.y - y);
        if (d2 < best_dist2) {
          best_dist2 = d2;
          best_arc = arc;
        }
      }
    }
    m_top_text->text_path_offset = std::max(
        0.0, std::min(m_top_text->text_path_flip ? total - best_arc : best_arc,
                      total));
  } else {
    // Fallback: horizontal drag delta
    double ddx = x - m_top_drag_start_x;
    m_top_text->text_path_offset = std::max(0.0, m_top_drag_start_off + ddx);
  }
  queue_draw();
}

void Canvas::on_top_end(double /*x*/, double /*y*/) {
  if (m_top_dragging)
    m_sig_doc_changed.emit(); // persist the new offset
  m_top_dragging = false;
}

void Canvas::on_top_rclick(double x, double y) {
  if (!m_doc)
    return;
  LOG_DEBUG("on_top_rclick: top_text={} top_phase={}", (void *)m_top_text,
            m_top_phase);

  // If we have a currently selected text node, release it via the proper
  // pathway so text_x/text_y is repositioned and undo works correctly.
  if (m_top_text && !m_top_text->text_path_id.empty()) {
    set_selection_single(m_top_text);
    release_text_from_path();
    return;
  }

  // Fallback: hit-test any text node with text_path_id
  double dx, dy;
  screen_to_doc(x, y, dx, dy);
  for (auto &layer : m_doc->layers) {
    if (!layer->visible)
      continue;
    for (auto &obj_uptr : layer->children) {
      SceneNode *obj = obj_uptr.get();
      if (!obj->is_text() || obj->text_path_id.empty())
        continue;
      SceneNode *guide = top_find_path_by_id(obj->text_path_id);
      if (guide && guide->path) {
        BezierPath bp = BezierPath::from_path_data(*guide->path);
        HitResult hr = bp.hit_test({dx, dy}, m_zoom, 20.0);
        if (hr.kind != HitResult::Kind::None) {
          set_selection_single(obj);
          release_text_from_path();
          return;
        }
      }
    }
  }
}

// ── Guide drag from ruler ────────────────────────────────────────────────────
void Canvas::begin_guide_drag(double doc_pos, bool horizontal) {
  if (!m_doc)
    return;
  SceneNode *gl = m_doc->ensure_guide_layer();
  // If guide layer was somehow saved as invisible, make it visible now
  // (creating a guide implies the user wants to see guides)
  if (!gl->visible)
    gl->visible = true;
  if (gl->locked)
    return;
  LOG_DEBUG("Canvas::begin_guide_drag pos={:.2f} horiz={}", doc_pos,
            horizontal);

  // Create guide node and start dragging it.  Anchor = canvas-center-on-line;
  // angle = 0 (H) or 90 (V).
  auto guide = std::make_unique<SceneNode>();
  guide->type = SceneNode::Type::Guide;
  const double cw_doc = m_doc->canvas_width();
  const double ch_doc = m_doc->canvas_height();
  if (horizontal) {
    guide->guide_x = cw_doc * 0.5;
    guide->guide_y = doc_pos;
    guide->guide_angle = 0.0;
  } else {
    guide->guide_x = doc_pos;
    guide->guide_y = ch_doc * 0.5;
    guide->guide_angle = 90.0;
  }

  m_guide_drag_node = guide.get();
  gl->children.push_back(std::move(guide));
  m_guide_drag_active = true;
  queue_draw();
}

void Canvas::update_guide_drag(double doc_pos) {
  if (!m_guide_drag_active || !m_guide_drag_node)
    return;
  // Ruler-originated drag: adjust along the varying axis.
  if (m_guide_drag_node->guide_is_horizontal()) {
    m_guide_drag_node->guide_y = doc_pos;
  } else if (m_guide_drag_node->guide_is_vertical()) {
    m_guide_drag_node->guide_x = doc_pos;
  }
  queue_draw();
}

void Canvas::end_guide_drag(double doc_pos) {
  LOG_DEBUG("Canvas::end_guide_drag pos={:.2f} active={}", doc_pos,
            m_guide_drag_active);
  if (!m_guide_drag_active || !m_guide_drag_node)
    return;
  if (m_guide_drag_node->guide_is_horizontal()) {
    m_guide_drag_node->guide_y = doc_pos;
  } else if (m_guide_drag_node->guide_is_vertical()) {
    m_guide_drag_node->guide_x = doc_pos;
  }
  m_guide_drag_node = nullptr;
  m_guide_drag_active = false;
  m_sig_doc_changed.emit();
  queue_draw();
}

void Canvas::cancel_guide_drag() {
  if (!m_guide_drag_active || !m_guide_drag_node || !m_doc)
    return;
  // Remove the guide that was being created
  SceneNode *gl = m_doc->guide_layer();
  if (gl) {
    auto it = std::find_if(gl->children.begin(), gl->children.end(),
                           [this](const std::unique_ptr<SceneNode> &c) {
                             return c.get() == m_guide_drag_node;
                           });
    if (it != gl->children.end())
      gl->children.erase(it);
  }
  m_guide_drag_node = nullptr;
  m_guide_drag_active = false;
  queue_draw();
}

} // namespace Curvz
