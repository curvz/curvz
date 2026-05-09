#include "LayersPanel.hpp"
#include "Canvas.hpp"
#include "ColorPickerPopover.hpp"
#include "CommandHistory.hpp"  // s171 m1 — push AddLayerCommand / DeleteLayerCommand
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"    // s171 m1 — required for command construction
#include "MacroSystem.hpp"
#include "SceneNode.hpp"
#include "UnitSystem.hpp"
#include "curvz_utils.hpp"  // s121 m2: curvz::utils::set_name
#include <algorithm>
#include <cairomm/cairomm.h>
#include <cmath>
#include <cstdio>
#include <functional>
#include <gdkmm/contentprovider.h>
#include <gdkmm/rgba.h>
#include <glibmm/main.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/popover.h>
#include <gtkmm/revealer.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

namespace Curvz {

// ── S96 m5 — indent depth → pixels ───────────────────────────────────
// Each nesting level adds 16 px of left margin to a row. Beyond depth
// kIndentDepthCap the visual signal "this is deeply nested" is already
// fully conveyed and further indent just wastes horizontal space — so
// we clamp before multiplying. Belt-and-braces with the AUTOMATIC
// horizontal scroll policy: scroll handles the overflow case if rows
// still exceed the viewport for any other reason.
static constexpr int kIndentStep      = 16;
static constexpr int kIndentDepthCap  = 10;

static inline int indent_px(int depth) {
    if (depth < 0) depth = 0;
    if (depth > kIndentDepthCap) depth = kIndentDepthCap;
    return depth * kIndentStep;
}

// ── Palette
// ───────────────────────────────────────────────────────────────────
const char *LayersPanel::palette(int i) {
  static const char *P[] = {"",        "#e34c26", "#e8a838", "#f0d020",
                            "#38a838", "#3584e4", "#9b59b6", "#e84393"};
  return P[i % PALETTE_SIZE];
}

static bool parse_hex(const std::string &hex, double &r, double &g, double &b) {
  if (hex.size() != 7 || hex[0] != '#')
    return false;
  auto h = [&](int pos) -> int {
    char c = hex[pos];
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return 0;
  };
  r = (h(1) * 16 + h(2)) / 255.0;
  g = (h(3) * 16 + h(4)) / 255.0;
  b = (h(5) * 16 + h(6)) / 255.0;
  return true;
}

// ── Constructor
// ───────────────────────────────────────────────────────────────
LayersPanel::LayersPanel() : Gtk::Box(Gtk::Orientation::VERTICAL) {
  curvz::utils::set_name(*this, "lp", "layers_panel_root");
  add_css_class("layers-panel");

  m_toolbar.set_margin_start(8);
  m_toolbar.set_margin_end(4);
  m_toolbar.set_margin_top(6);
  m_toolbar.set_margin_bottom(4);
  // m_title.set_text("Layers");
  m_title.set_text("  ");
  m_title.set_xalign(0.0f);
  m_title.set_hexpand(true);
  m_title.add_css_class("panel-title");
  m_btn_add.set_icon_name("list-add-symbolic");
  m_btn_add.set_has_frame(false);
  m_btn_add.set_tooltip_text("Add layer");
  curvz::utils::set_name(m_btn_add, "lp_add", "layers_panel_add_btn");
  m_btn_add.signal_clicked().connect(
      sigc::mem_fun(*this, &LayersPanel::on_add_layer));
  m_btn_delete.set_icon_name("list-remove-symbolic");
  m_btn_delete.set_has_frame(false);
  m_btn_delete.set_tooltip_text("Delete layer");
  curvz::utils::set_name(m_btn_delete, "lp_del", "layers_panel_delete_btn");
  m_btn_delete.signal_clicked().connect(
      sigc::mem_fun(*this, &LayersPanel::on_delete_layer));
  m_toolbar.append(m_title);
  m_toolbar.append(m_btn_add);
  m_toolbar.append(m_btn_delete);
  append(m_toolbar);
  append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));

  m_scroll.set_child(m_content);
  // S96 m5 — horizontal scroll policy AUTOMATIC (was NEVER).
  //
  // With NEVER, when the content's natural width exceeded the viewport
  // (deep group nesting at indent * 16 px per level — 53-deep nests
  // alone produced 848 px of left margin) the ScrolledWindow could not
  // show a horizontal scrollbar, so it propagated the natural width
  // upward and the whole right pane grew to fit. AUTOMATIC bounds the
  // panel at its allocated width and shows a horizontal scrollbar when
  // content overflows, which makes "deeply nested SVG → wide pane" a
  // structurally impossible bug class regardless of input.
  m_scroll.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
  m_scroll.set_vexpand(true);
  append(m_scroll);

  // Rebuild the macro area when the manager's data changes
  // (star toggles, rename, delete, new recording steps, etc.)
  m_macro_mgr_conn = MacroManager::instance().signal_changed().connect(
      [this]() {
        Glib::signal_idle().connect_once([this]() { rebuild(); });
      });

  // Layer-colour picker popover. One instance, reused across every
  // layer-dot click. Lives until the LayersPanel itself is destroyed.
  m_color_popover.attach(*this);
}

// ── Public
// ────────────────────────────────────────────────────────────────────
void LayersPanel::set_document(CurvzDocument *doc) {
  m_doc = doc;
  m_active_layer = doc ? doc->active_layer_index : 0;
  m_canvas_selection.clear();
  rebuild();
}

void LayersPanel::refresh() {
  // s171 m1 fix2 — re-sync m_active_layer from the doc before rebuild.
  // Without this, undo of a layer add/delete restores doc->active_layer_index
  // but the panel's local m_active_layer is stale (still holds the post-op
  // value from the original push), so rebuild() highlights the wrong row.
  // set_document does the same sync; refresh now matches its shape.
  if (m_doc)
    m_active_layer = m_doc->active_layer_index;
  Glib::signal_idle().connect_once([this]() { rebuild(); });
}

void LayersPanel::set_canvas_selection(
    const std::vector<SceneNode *> &selection) {
  m_canvas_selection = selection;

  // Update active layer to follow the selection. The loop walks every
  // top-level node, including special layers (RefLayer, GuideLayer,
  // MeasureLayer, ...) — the prior is_layer()-only filter meant a refpt
  // selected on canvas could not advance m_active_layer to the References
  // layer. Special layers already accept m_active_layer in their click
  // handlers (see add_ref_layer_row name click), so this is safe.
  if (!selection.empty() && m_doc) {
    SceneNode *sel = selection.front();
    for (int i = 0; i < (int)m_doc->layers.size(); ++i) {
      auto &layer = m_doc->layers[i];
      std::function<bool(SceneNode *)> contains = [&](SceneNode *n) -> bool {
        if (n == sel)
          return true;
        for (auto &ch : n->children)
          if (contains(ch.get()))
            return true;
        return false;
      };
      bool found = false;
      for (auto &child : layer->children)
        if (contains(child.get())) {
          found = true;
          break;
        }
      if (found) {
        if (m_active_layer != i) {
          m_active_layer = i;
          m_doc->active_layer_index = i;
          m_sig_layer_selected.emit(i);
        }
        break;
      }
    }
  }

  refresh_highlights();
}

void LayersPanel::set_guide_selection(const std::vector<SceneNode *> &sel) {
  LOG_DEBUG("LayersPanel::set_guide_selection size={}", sel.size());
  if (m_guide_selection == sel) {
    LOG_DEBUG("LayersPanel::set_guide_selection — same, skip");
    return;
  }
  m_guide_selection = sel;
  LOG_DEBUG(
      "LayersPanel::set_guide_selection — scheduling double-idle rebuild");
  Glib::signal_idle().connect_once([this]() {
    LOG_DEBUG("LayersPanel::set_guide_selection — idle1 firing, queuing idle2");
    Glib::signal_idle().connect_once([this]() {
      LOG_DEBUG(
          "LayersPanel::set_guide_selection — idle2 firing, calling rebuild()");
      rebuild();
      LOG_DEBUG("LayersPanel::set_guide_selection — rebuild() done");
    });
  });
}

// ── Highlight refresh — update CSS without full rebuild
// ───────────────────────
void LayersPanel::refresh_highlights() {
  for (auto &re : m_row_entries) {
    bool active_layer =
        (re.layer_idx == m_active_layer && re.obj_idx < 0 && re.obj == nullptr);
    bool canvas_sel =
        (re.obj != nullptr &&
         std::find(m_canvas_selection.begin(), m_canvas_selection.end(),
                   re.obj) != m_canvas_selection.end());
    bool selected = active_layer || canvas_sel ||
                    (m_panel_selection.count({re.layer_idx, re.obj_idx}) > 0);
    if (re.widget) {
      if (selected)
        re.widget->add_css_class("layer-active");
      else
        re.widget->remove_css_class("layer-active");
    }
  }
}

// ── Collapse helpers
// ──────────────────────────────────────────────────────────

// Stable persistence keys. Layer uses "L:" + internal_id, falling back to
// "LN:" + name when internal_id isn't populated (older files loaded before
// we start emitting layer iids). Groups use "G:" + internal_id — groups
// already round-trip their iid in SVG.
std::string LayersPanel::layer_state_key(const SceneNode &layer) {
  if (!layer.internal_id.empty())
    return std::string("L:") + layer.internal_id;
  if (!layer.name.empty())
    return std::string("LN:") + layer.name;
  return std::string("L:?"); // degenerate — unlikely, but groups them all
}

std::string LayersPanel::group_state_key(const SceneNode &group) {
  if (!group.internal_id.empty())
    return std::string("G:") + group.internal_id;
  return std::string("G:?");
}

bool LayersPanel::lookup_expanded(const std::string &key,
                                  bool default_expanded) const {
  auto it = m_collapse_state.find(key);
  return (it != m_collapse_state.end()) ? it->second : default_expanded;
}

// s112 — live read of layer lock state. Used by every click and DnD gate
// so toggling lock mid-session takes effect immediately without forcing
// a panel rebuild.
bool LayersPanel::layer_at_locked(int layer_idx) const {
  if (!m_doc)
    return false;
  if (layer_idx < 0 || layer_idx >= (int)m_doc->layers.size())
    return false;
  return m_doc->layers[layer_idx]->locked;
}

// s172 m4 — push helpers for layer state-mutations. Resolve the layer
// iid at push time (live read of doc->layers[layer_idx]->internal_id),
// then build and push the command. Both helpers follow the same
// mutate-then-push convention as the s171 m1+m2 layer commands —
// callers do the direct mutation first, then call the helper to record
// it. push() does NOT call execute(); it only appends to the undo
// stack. Silent no-op when m_history/m_project aren't wired (test
// harnesses, partial init) or the layer's iid is empty (degenerate /
// pre-iid legacy doc).
//
// before_*/after_* are passed in as plain values, not snapshotted from
// the live layer — the caller has already mutated, so reading the live
// state would give us "after" twice. Caller responsibility to capture
// `before` before mutating, then call the helper with both.
void LayersPanel::push_edit_layer_string_field(int layer_idx,
                                               bool is_color,
                                               const std::string& before_str,
                                               const std::string& after_str) {
  if (!m_history || !m_project) return;
  if (!m_doc) return;
  if (layer_idx < 0 || layer_idx >= (int)m_doc->layers.size()) return;
  if (!m_doc->layers[layer_idx]) return;
  std::string layer_iid = m_doc->layers[layer_idx]->internal_id;
  if (layer_iid.empty()) {
    LOG_WARN("[IIDDIAG] EditLayerField: layer at idx={} has empty iid, "
             "skipping command push", layer_idx);
    return;
  }
  using Field = EditLayerFieldCommand::Field;
  auto cmd = std::make_unique<EditLayerFieldCommand>(
      m_project,
      std::move(layer_iid),
      is_color ? Field::Color : Field::Name,
      before_str,
      after_str,
      /*before_bool=*/false,
      /*after_bool=*/false);
  m_history->push(std::move(cmd));
}

void LayersPanel::push_edit_layer_bool_field(int layer_idx,
                                             bool is_locked,
                                             bool before_bool,
                                             bool after_bool) {
  if (!m_history || !m_project) return;
  if (!m_doc) return;
  if (layer_idx < 0 || layer_idx >= (int)m_doc->layers.size()) return;
  if (!m_doc->layers[layer_idx]) return;
  std::string layer_iid = m_doc->layers[layer_idx]->internal_id;
  if (layer_iid.empty()) {
    LOG_WARN("[IIDDIAG] EditLayerField: layer at idx={} has empty iid, "
             "skipping command push", layer_idx);
    return;
  }
  using Field = EditLayerFieldCommand::Field;
  auto cmd = std::make_unique<EditLayerFieldCommand>(
      m_project,
      std::move(layer_iid),
      is_locked ? Field::Locked : Field::Visible,
      /*before_str=*/std::string{},
      /*after_str=*/std::string{},
      before_bool,
      after_bool);
  m_history->push(std::move(cmd));
}

void LayersPanel::write_expanded(const std::string &key, bool expanded) {
  m_collapse_state[key] = expanded;
}

LayersPanel::CollapseEntry *LayersPanel::find_collapse(int layer_idx) {
  for (auto &e : m_collapse_entries)
    if (e.group_node == nullptr && e.layer_idx == layer_idx)
      return &e;
  return nullptr;
}

LayersPanel::CollapseEntry *LayersPanel::find_collapse_group(SceneNode *group) {
  for (auto &e : m_collapse_entries)
    if (e.group_node == group)
      return &e;
  return nullptr;
}

void LayersPanel::toggle_layer(int layer_idx) {
  auto *e = find_collapse(layer_idx);
  if (!e)
    return;
  e->expanded = !e->expanded;
  if (!e->state_key.empty())
    write_expanded(e->state_key, e->expanded);
  if (e->revealer)
    e->revealer->set_reveal_child(e->expanded);
  if (e->arrow)
    e->arrow->set_label(e->expanded ? "▾" : "▸");
}

void LayersPanel::toggle_group(SceneNode *group) {
  auto *e = find_collapse_group(group);
  if (!e)
    return;
  e->expanded = !e->expanded;
  if (!e->state_key.empty())
    write_expanded(e->state_key, e->expanded);
  if (e->revealer)
    e->revealer->set_reveal_child(e->expanded);
  if (e->arrow)
    e->arrow->set_label(e->expanded ? "▾" : "▸");
}

// ── Drop zone
// ─────────────────────────────────────────────────────────────────
LayersPanel::DropZone LayersPanel::compute_drop_zone(Gtk::Widget *w, double y,
                                                     bool is_layer) const {
  int h = w->get_height();
  if (h <= 0)
    return DropZone::After;
  if (is_layer) {
    if (y < h * 0.25)
      return DropZone::Before;
    if (y > h * 0.75)
      return DropZone::After;
    return DropZone::Inside;
  }
  return (y < h * 0.5) ? DropZone::Before : DropZone::After;
}

void LayersPanel::apply_drop_highlight(Gtk::Widget *w, DropZone zone) {
  if (m_drop_hl.widget == w && m_drop_hl.zone == zone)
    return;
  clear_drop_highlight();
  m_drop_hl = {w, zone};
  if (!w)
    return;
  if (zone == DropZone::Before)
    w->add_css_class("layer-drop-top");
  else if (zone == DropZone::After)
    w->add_css_class("layer-drop-bottom");
  else if (zone == DropZone::Inside)
    w->add_css_class("layer-drop-into");
}

void LayersPanel::clear_drop_highlight() {
  if (m_drop_hl.widget) {
    m_drop_hl.widget->remove_css_class("layer-drop-top");
    m_drop_hl.widget->remove_css_class("layer-drop-bottom");
    m_drop_hl.widget->remove_css_class("layer-drop-into");
  }
  m_drop_hl = {};
}

// ── DnD setup — layer row
// ─────────────────────────────────────────────────────
void LayersPanel::setup_layer_drag(Gtk::Widget *w, int layer_idx) {
  auto src = Gtk::DragSource::create();
  src->set_actions(Gdk::DragAction::MOVE);
  src->signal_prepare().connect(
      [this, layer_idx](double,
                        double) -> Glib::RefPtr<Gdk::ContentProvider> {
        // s112 — locked layer cannot be reordered via DnD.
        if (layer_at_locked(layer_idx))
          return {};
        Glib::Value<int> val;
        val.init(G_TYPE_INT);
        val.set(encode_payload(0, layer_idx, 0));
        return Gdk::ContentProvider::create(val);
      },
      false);
  src->signal_drag_begin().connect([w](const Glib::RefPtr<Gdk::Drag> &) {
    w->add_css_class("layer-dragging");
  });
  src->signal_drag_end().connect([w](const Glib::RefPtr<Gdk::Drag> &, bool) {
    w->remove_css_class("layer-dragging");
  });
  w->add_controller(src);
}

void LayersPanel::setup_layer_drop(Gtk::Widget *w, int layer_idx) {
  auto tgt = Gtk::DropTarget::create(G_TYPE_INT, Gdk::DragAction::MOVE);

  tgt->signal_enter().connect(
      [this, w](double, double y) {
        apply_drop_highlight(w, compute_drop_zone(w, y, true));
        return Gdk::DragAction::MOVE;
      },
      false);
  tgt->signal_motion().connect(
      [this, w](double, double y) {
        apply_drop_highlight(w, compute_drop_zone(w, y, true));
        return Gdk::DragAction::MOVE;
      },
      false);
  tgt->signal_leave().connect([this]() { clear_drop_highlight(); });

  tgt->signal_drop().connect(
      [this, w, layer_idx](const Glib::ValueBase &val, double,
                           double y) -> bool {
        clear_drop_highlight();
        if (!m_doc || m_rebuilding)
          return false;
        // Validate layer_idx is still in range (may be stale after prior DnD)
        if (layer_idx < 0 || layer_idx >= (int)m_doc->layers.size())
          return false;

        // Clear canvas selection before mutating layer structure — prevents
        // set_canvas_selection from chasing stale pointers after the move.
        m_canvas_selection.clear();
        int payload = g_value_get_int(val.gobj());
        int type, src_li, src_oi;
        decode_payload(payload, type, src_li, src_oi);
        // s112 — refuse all drops involving locked layers. For type 0
        // (layer-rearrange) this is a defensive check (drag was already
        // gated); for type 1 (path-into-layer) we additionally refuse if
        // the *source* layer is locked (children can't leave) or the
        // destination layer is locked (children can't arrive).
        if (type == 0 && layer_at_locked(src_li))
          return false;
        if (type == 1 && (layer_at_locked(src_li) || layer_at_locked(layer_idx)))
          return false;
        int n = (int)m_doc->layers.size();
        DropZone zone = compute_drop_zone(w, y, true);

        if (type == 0) {
          if (src_li == layer_idx)
            return false;
          int src_row = n - 1 - src_li;
          int dst_row = n - 1 - layer_idx;
          int insert_before_row =
              (zone == DropZone::After) ? dst_row + 1 : dst_row;
          if (insert_before_row == src_row || insert_before_row == src_row + 1)
            return false;

          // s172 m3 — capture pre-op layer iid sequence and active index
          // before the rearrange. iids_before is the doc->layers order
          // *as-is* (no row->index conversion needed: we're capturing the
          // canonical doc-level vector ordering, which is what the
          // command applies in both directions).
          std::vector<std::string> iids_before;
          int active_before = m_doc->active_layer_index;
          if (m_history && m_project) {
            iids_before.reserve(n);
            for (auto& ly : m_doc->layers)
              iids_before.push_back(ly ? ly->internal_id : std::string{});
          }

          std::vector<int> order;
          order.reserve(n);
          for (int r = 0; r < n; ++r) {
            if (r == src_row)
              continue;
            if (r == insert_before_row)
              order.push_back(src_row);
            order.push_back(r);
          }
          if (insert_before_row >= n)
            order.push_back(src_row);
          std::vector<std::unique_ptr<SceneNode>> new_layers;
          new_layers.reserve(n);
          for (int i = (int)order.size() - 1; i >= 0; --i)
            new_layers.push_back(std::move(m_doc->layers[(n - 1) - order[i]]));
          m_doc->layers = std::move(new_layers);
          int new_row = (int)(std::find(order.begin(), order.end(), src_row) -
                              order.begin());
          m_active_layer = (n - 1) - new_row;
          m_doc->active_layer_index = m_active_layer;
          m_doc->invalidate_iid_index();

          // s172 m3 — capture post-op iid sequence and push the command.
          // Anchor is the first non-empty iid in iids_after; layers don't
          // disappear during reorder so iids_after has the same set as
          // iids_before, just permuted. Push only if both before and
          // after sequences are non-empty (degenerate doc safety).
          if (m_history && m_project && !iids_before.empty()) {
            std::vector<std::string> iids_after;
            iids_after.reserve(m_doc->layers.size());
            for (auto& ly : m_doc->layers)
              iids_after.push_back(ly ? ly->internal_id : std::string{});

            std::string anchor_iid;
            for (const auto& iid : iids_after) {
              if (!iid.empty()) {
                anchor_iid = iid;
                break;
              }
            }
            if (!anchor_iid.empty()) {
              auto cmd = std::make_unique<ReorderLayersCommand>(
                  m_project,
                  std::move(anchor_iid),
                  std::move(iids_before),
                  std::move(iids_after),
                  active_before,
                  m_doc->active_layer_index);
              m_history->push(std::move(cmd));
            } else {
              LOG_WARN("[IIDDIAG] ReorderLayers: no anchor iid found, "
                       "skipping command push");
            }
          }
        } else {
          if (src_li == layer_idx)
            return false;
          if (src_li < 0 || src_li >= n)
            return false;
          if (src_oi < 0 ||
              src_oi >= (int)m_doc->layers[src_li]->children.size())
            return false;
          // Never drop artwork into special layers
          if (m_doc->layers[layer_idx]->is_guide_layer())
            return false;
          if (m_doc->layers[layer_idx]->is_ref_layer())
            return false;
          if (m_doc->layers[layer_idx]->is_grid_layer())
            return false;
          if (m_doc->layers[layer_idx]->is_margin_layer())
            return false;
          // Pre-mutation check: never drag ref points out of their ref
          // layer. Read without taking ownership so we can bail before
          // the move. (The original `auto obj = std::move(...)` happened
          // before the is_ref check; if is_ref returned true the obj
          // was already moved-out, leaving a hole. Reordering the check
          // here is a small safety improvement.)
          if (m_doc->layers[src_li]->children[src_oi]->is_ref())
            return false;

          // s171 m2 — capture iids and indices before the mutation, so
          // the command sees the pre-op state. Mutate the doc directly
          // (matching pre-s171 behaviour and the established
          // mutate-then-push pattern), then push the command.
          std::string src_layer_iid = m_doc->layers[src_li]->internal_id;
          std::string dst_layer_iid = m_doc->layers[layer_idx]->internal_id;
          std::string obj_iid       = m_doc->layers[src_li]->children[src_oi]->internal_id;
          int saved_src_index       = src_oi;
          int saved_dst_index       = (int)m_doc->layers[layer_idx]->children.size();

          // Snapshot the object before move-out so the command holds a
          // stable clone (every iid preserved).
          std::unique_ptr<SceneNode> snap_for_cmd;
          if (m_history && m_project
              && !src_layer_iid.empty() && !dst_layer_iid.empty()
              && !obj_iid.empty()) {
            snap_for_cmd = clone_node(*m_doc->layers[src_li]->children[src_oi]);
          }

          auto obj = std::move(m_doc->layers[src_li]->children[src_oi]);
          m_doc->layers[src_li]->children.erase(
              m_doc->layers[src_li]->children.begin() + src_oi);
          m_doc->layers[layer_idx]->children.push_back(std::move(obj));
          m_doc->invalidate_iid_index();

          if (snap_for_cmd) {
            auto cmd = std::make_unique<MoveObjectToLayerCommand>(
                m_project,
                std::move(src_layer_iid),
                std::move(dst_layer_iid),
                std::move(obj_iid),
                std::move(snap_for_cmd),
                saved_src_index,
                saved_dst_index);
            m_history->push(std::move(cmd));
          }
        }

        m_sig_layer_selected.emit(m_active_layer);
        m_sig_layer_changed.emit();
        // If destination layer is hidden, deselect on canvas
        if (type == 1 && !m_doc->layers[layer_idx]->visible) {
          m_canvas_selection.clear();
          m_sig_object_selected.emit(nullptr);
        }
        Glib::signal_idle().connect_once([this]() { rebuild(); });
        return true;
      },
      false);

  w->add_controller(tgt);
}

// ── DnD setup — path row ─────────────────────────────────────────────────────
void LayersPanel::setup_path_drag(Gtk::Widget *w, int layer_idx, int obj_idx) {
  auto src = Gtk::DragSource::create();
  src->set_actions(Gdk::DragAction::MOVE);
  src->signal_prepare().connect(
      [this, layer_idx, obj_idx](
          double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
        // s112 — child of locked layer cannot be dragged out.
        if (layer_at_locked(layer_idx))
          return {};
        Glib::Value<int> val;
        val.init(G_TYPE_INT);
        val.set(encode_payload(1, layer_idx, obj_idx));
        return Gdk::ContentProvider::create(val);
      },
      false);
  src->signal_drag_begin().connect([w](const Glib::RefPtr<Gdk::Drag> &) {
    w->add_css_class("layer-dragging");
  });
  src->signal_drag_end().connect([w](const Glib::RefPtr<Gdk::Drag> &, bool) {
    w->remove_css_class("layer-dragging");
  });
  w->add_controller(src);
}

void LayersPanel::setup_path_drop(Gtk::Widget *w, int layer_idx, int obj_idx) {
  auto tgt = Gtk::DropTarget::create(G_TYPE_INT, Gdk::DragAction::MOVE);

  tgt->signal_enter().connect(
      [this, w](double, double y) {
        apply_drop_highlight(w, compute_drop_zone(w, y, false));
        return Gdk::DragAction::MOVE;
      },
      false);
  tgt->signal_motion().connect(
      [this, w](double, double y) {
        apply_drop_highlight(w, compute_drop_zone(w, y, false));
        return Gdk::DragAction::MOVE;
      },
      false);
  tgt->signal_leave().connect([this]() { clear_drop_highlight(); });

  tgt->signal_drop().connect(
      [this, w, layer_idx, obj_idx](const Glib::ValueBase &val, double,
                                    double y) -> bool {
        clear_drop_highlight();
        if (!m_doc || m_rebuilding)
          return false;
        if (layer_idx < 0 || layer_idx >= (int)m_doc->layers.size())
          return false;
        m_canvas_selection.clear();
        int payload = g_value_get_int(val.gobj());
        int type, src_li, src_oi;
        decode_payload(payload, type, src_li, src_oi);
        if (type != 1)
          return false;
        // s112 — refuse drops where the source or destination layer is locked.
        if (layer_at_locked(src_li) || layer_at_locked(layer_idx))
          return false;
        int n = (int)m_doc->layers.size();
        if (src_li < 0 || src_li >= n)
          return false;
        // Never drop artwork into special layers
        if (m_doc->layers[layer_idx]->is_guide_layer())
          return false;
        if (m_doc->layers[layer_idx]->is_ref_layer())
          return false;
        if (m_doc->layers[layer_idx]->is_grid_layer())
          return false;
        if (m_doc->layers[layer_idx]->is_margin_layer())
          return false;
        auto &src_layer = m_doc->layers[src_li];
        if (src_oi < 0 || src_oi >= (int)src_layer->children.size())
          return false;
        // Never drag ref points out of their layer
        if (src_layer->children[src_oi]->is_ref())
          return false;

        DropZone zone = compute_drop_zone(w, y, false);
        // Object rows are displayed in reverse array order (index 0 = bottom of
        // panel). "Before" in display (top half of row) = visually above =
        // higher array index. "After"  in display (bottom half)     = visually
        // below = lower array index. Invert zone so insert_at math works
        // correctly.
        if (zone == DropZone::Before)
          zone = DropZone::After;
        else if (zone == DropZone::After)
          zone = DropZone::Before;

        if (src_li == layer_idx) {
          if (src_oi == obj_idx)
            return false;
          auto &children = m_doc->layers[layer_idx]->children;
          auto moving = std::move(children[src_oi]);
          children.erase(children.begin() + src_oi);
          int insert_at = obj_idx;
          if (src_oi < obj_idx)
            insert_at--;
          if (zone == DropZone::After)
            insert_at++;
          insert_at = std::max(0, std::min(insert_at, (int)children.size()));
          children.insert(children.begin() + insert_at, std::move(moving));
        } else {
          // s171 m2 — cross-layer move via path-row drop. Capture iids
          // and indices before the mutation; mutate; then push the
          // command. Same pattern as the layer-row drop site above.
          std::string src_layer_iid = src_layer->internal_id;
          std::string dst_layer_iid = m_doc->layers[layer_idx]->internal_id;
          std::string obj_iid       = src_layer->children[src_oi]->internal_id;
          int saved_src_index       = src_oi;
          int saved_dst_index       = (zone == DropZone::After) ? obj_idx + 1 : obj_idx;
          // Clamp dst to live size (the live insertion below clamps too).
          saved_dst_index = std::max(0, std::min(saved_dst_index,
              (int)m_doc->layers[layer_idx]->children.size()));

          std::unique_ptr<SceneNode> snap_for_cmd;
          if (m_history && m_project
              && !src_layer_iid.empty() && !dst_layer_iid.empty()
              && !obj_iid.empty()) {
            snap_for_cmd = clone_node(*src_layer->children[src_oi]);
          }

          auto obj = std::move(src_layer->children[src_oi]);
          src_layer->children.erase(src_layer->children.begin() + src_oi);
          auto &dst = m_doc->layers[layer_idx]->children;
          int insert_at = (zone == DropZone::After) ? obj_idx + 1 : obj_idx;
          insert_at = std::max(0, std::min(insert_at, (int)dst.size()));
          dst.insert(dst.begin() + insert_at, std::move(obj));
          m_doc->invalidate_iid_index();

          if (snap_for_cmd) {
            auto cmd = std::make_unique<MoveObjectToLayerCommand>(
                m_project,
                std::move(src_layer_iid),
                std::move(dst_layer_iid),
                std::move(obj_iid),
                std::move(snap_for_cmd),
                saved_src_index,
                saved_dst_index);
            m_history->push(std::move(cmd));
          }
        }

        m_sig_layer_changed.emit();
        // If moved to a different hidden layer, deselect on canvas
        if (src_li != layer_idx && !m_doc->layers[layer_idx]->visible) {
          m_canvas_selection.clear();
          m_sig_object_selected.emit(nullptr);
        }
        Glib::signal_idle().connect_once([this]() { rebuild(); });
        return true;
      },
      false);

  w->add_controller(tgt);
}

// ── add_layer_row
// ─────────────────────────────────────────────────────────────
void LayersPanel::add_layer_row(int i, Gtk::Box *parent) {
  SceneNode &layer = *m_doc->layers[i];

  std::string skey = layer_state_key(layer);
  bool expanded = lookup_expanded(skey, false);
  if (auto *e = find_collapse(i))
    expanded = e->expanded;

  auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(4);
  hdr->set_margin_end(6);
  hdr->set_margin_top(2);
  hdr->set_margin_bottom(2);
  hdr->add_css_class("layer-header");
  hdr->set_focusable(true);
  if (i == m_active_layer)
    hdr->add_css_class("layer-active");

  // Track for highlight refresh
  m_row_entries.push_back({i, -1, nullptr, hdr});

  // ── Arrow — use Button so it reliably captures clicks
  auto *arrow_btn = Gtk::make_managed<Gtk::Button>();
  arrow_btn->set_has_frame(false);
  arrow_btn->set_label(expanded ? "▾" : "▸");
  arrow_btn->set_size_request(20, -1);
  arrow_btn->add_css_class("part-arrow");
  arrow_btn->signal_clicked().connect([this, i]() { toggle_layer(i); });
  hdr->append(*arrow_btn);

  // Store arrow ptr for toggle_layer to update label
  // We'll update CollapseEntry to store the button
  // ── Color dot
  auto *color_dot = Gtk::make_managed<Gtk::DrawingArea>();
  color_dot->set_size_request(10, 10);
  color_dot->set_can_target(true);
  color_dot->set_tooltip_text("Layer color — click to cycle");
  std::string cur_color = layer.color;
  color_dot->set_draw_func(
      [cur_color](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        double r = 0.35, g = 0.35, b = 0.35;
        if (!cur_color.empty())
          parse_hex(cur_color, r, g, b);
        cr->arc(w * 0.5, h * 0.5, w * 0.5 - 0.5, 0, 2 * M_PI);
        cr->set_source_rgb(r, g, b);
        cr->fill_preserve();
        cr->set_source_rgba(1, 1, 1, 0.15);
        cr->set_line_width(0.5);
        cr->stroke();
      });
  auto dot_click = Gtk::GestureClick::create();
  dot_click->set_button(1);
  dot_click->signal_pressed().connect([this, i, color_dot](int, double, double) {
    // Read current layer colour as the popover's starting point.
    double r = 0.35, g = 0.35, b = 0.35;
    if (!m_doc->layers[i]->color.empty())
      parse_hex(m_doc->layers[i]->color, r, g, b);
    color::Color initial(r, g, b, 1.0);

    // The popover is parented to `*this` (attached in ctor) so it
    // outlives this row's rebuild. The dot is only the visual anchor;
    // when rebuild() tears down `color_dot`, the popover stays up
    // pointing at the rect we captured at open time.
    m_color_popover.open(
        *color_dot, initial, /*with_alpha=*/false,
        [this, i](const color::Color& c) {
          if (i >= (int)m_doc->layers.size()) return;
          std::string hex = color::to_hex(c);
          if (hex.size() > 7) hex.resize(7);
          // s172 m4 — capture before, mutate, push.
          std::string before = m_doc->layers[i]->color;
          m_doc->layers[i]->color = hex;
          if (before != hex) {
            push_edit_layer_string_field(i, /*is_color=*/true, before, hex);
          }
          m_sig_layer_changed.emit();
          Glib::signal_idle().connect_once([this]() { rebuild(); });
        });
  });
  color_dot->add_controller(dot_click);
  hdr->append(*color_dot);

  // ── Visibility — symbolic icon so it follows CSS color and respects
  // Curvz motif. S117 m12: was Gtk::Picture loading a hard-coded
  // light/dark SVG pair, which was selected once at app startup based
  // on GTK's `gtk-application-prefer-dark-theme` setting — NOT Curvz's
  // per-project motif. That's why layer eyes failed to honour motif
  // while every other eye in the panel (References, Grid, Margins,
  // Guides, Measurement) did. They all use set_icon_name on a
  // symbolic; we now match.
  bool vis = layer.visible;
  auto *vis_btn = Gtk::make_managed<Gtk::Button>();
  vis_btn->set_has_frame(false);
  vis_btn->set_icon_name("curvz-eye-symbolic");
  vis_btn->set_tooltip_text(vis ? "Hide layer" : "Show layer");
  if (!vis)
    vis_btn->add_css_class("layer-hidden-btn");
  vis_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->visible;
    m_doc->layers[i]->visible = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/false, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*vis_btn);

  // ── Name — single click activates, no rebuild; double-click renames
  auto *name_lbl = Gtk::make_managed<Gtk::Label>(layer.name);
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  if (!layer.visible)
    name_lbl->add_css_class("layer-hidden-text");

  auto name_click = Gtk::GestureClick::create();
  name_click->set_button(1);
  name_click->signal_pressed().connect(
      [this, i, name_lbl, hdr, name_click](int n_press, double, double) {
        if (m_rebuilding)
          return;
        // Always update active layer — no rebuild, just refresh highlights
        m_active_layer = i;
        m_doc->active_layer_index = i;
        m_sig_layer_selected.emit(i);
        refresh_highlights();

        bool shift_held = false;
        {
          auto *g = dynamic_cast<Gtk::GestureClick *>(name_click.get());
          if (g) {
            auto evt = g->get_last_event(g->get_current_sequence());
            if (evt) {
              shift_held =
                  (evt->get_modifier_state() & Gdk::ModifierType::SHIFT_MASK) !=
                  Gdk::ModifierType::NO_MODIFIER_MASK;
            }
          }
        }

        if (n_press >= 2 && !shift_held) {
          // Rename inline
          auto *entry = Gtk::make_managed<CurvzEntry>();
          curvz::utils::set_name(entry, "lp_re_lyr", "layers_panel_rename_layer_entry");
          entry->set_text(m_doc->layers[i]->name);
          entry->set_hexpand(true);
          entry->add_css_class("prop-entry");
          hdr->remove(*name_lbl);
          hdr->insert_child_after(*entry, *hdr->get_first_child());
          entry->grab_focus();
          entry->select_region(0, -1);
          entry->on_commit([this, i, entry]() {
            std::string nm = entry->get_text();
            LOG_INFO("[IIDDIAG] LayerRename: on_commit fired idx={} text='{}' "
                     "m_history={} m_project={}",
                     i, nm,
                     m_history ? "yes" : "NO",
                     m_project ? "yes" : "NO");
            if (!nm.empty()) {
              // Route user-typed names through the uniqueness funnel so
              // hand-typed collisions get suffixed (e.g. typing "Background"
              // when another node already owns that name yields
              // "Background (2)"). Self-exclude so renaming-to-current-name
              // doesn't falsely collide with itself.
              SceneNode* self = m_doc->layers[i].get();
              // s172 m4 — capture before, mutate, push (only if name
              // actually changed after uniquify; typing the current
              // name back in shouldn't add an empty undo step).
              std::string before = m_doc->layers[i]->name;
              std::string after  = m_doc->uniquify_name(nm, self);
              m_doc->layers[i]->name = after;
              if (before != after) {
                push_edit_layer_string_field(i, /*is_color=*/false, before, after);
              }
            }
            m_sig_layer_changed.emit();
            Glib::signal_idle().connect_once([this]() { rebuild(); });
          });
        }
      });
  name_lbl->add_controller(name_click);
  hdr->append(*name_lbl);

  // ── Lock
  auto *lock_btn = Gtk::make_managed<Gtk::Button>();
  lock_btn->set_has_frame(false);
  // lock_btn->set_icon_name(layer.locked ? "changes-prevent-symbolic"
  //                                      : "changes-allow-symbolic");
  lock_btn->set_icon_name(layer.locked ? "curvz-locked-symbolic"
                                       : "curvz-unlocked-symbolic");
  lock_btn->set_tooltip_text(layer.locked ? "Unlock" : "Lock");
  if (layer.locked)
    lock_btn->add_css_class("layer-locked-btn");
  lock_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->locked;
    m_doc->layers[i]->locked = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/true, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*lock_btn);

  // ── Object count
  char cnt[12];
  snprintf(cnt, sizeof(cnt), "%d", (int)layer.children.size());
  auto *count_lbl = Gtk::make_managed<Gtk::Label>(cnt);
  count_lbl->set_size_request(20, -1);
  count_lbl->set_xalign(1.0f);
  count_lbl->add_css_class("dim-label");
  count_lbl->set_tooltip_text("Objects");
  hdr->append(*count_lbl);

  card->append(*hdr);

  setup_layer_drag(hdr, i);
  setup_layer_drop(hdr, i);

  // ── Revealer
  auto *revealer = Gtk::make_managed<Gtk::Revealer>();
  revealer->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);
  revealer->set_transition_duration(150);
  revealer->set_reveal_child(expanded);

  auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  for (int j = 0; j < (int)layer.children.size(); ++j)
    add_child_row(layer.children[j].get(), i, 1, inner);
  revealer->set_child(*inner);
  card->append(*revealer);
  parent->append(*card);

  // Update collapse entry
  m_collapse_entries.erase(
      std::remove_if(m_collapse_entries.begin(), m_collapse_entries.end(),
                     [i](const CollapseEntry &e) {
                       return e.group_node == nullptr && e.layer_idx == i;
                     }),
      m_collapse_entries.end());
  m_collapse_entries.push_back({i, nullptr, revealer, arrow_btn, expanded, skey});
}

// ── add_path_row
// ──────────────────────────────────────────────────────────────
void LayersPanel::add_path_row(int layer_idx, int obj_idx, Gtk::Box *parent) {
  SceneNode &obj = *m_doc->layers[layer_idx]->children[obj_idx];

  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  row->set_spacing(4);
  row->set_margin_start(20);
  row->set_margin_end(6);
  row->set_margin_top(2);
  row->set_margin_bottom(2);
  row->add_css_class("path-row");

  // Highlight if canvas-selected
  if (std::find(m_canvas_selection.begin(), m_canvas_selection.end(), &obj) !=
      m_canvas_selection.end())
    row->add_css_class("layer-active");

  // Track for refresh
  m_row_entries.push_back({layer_idx, obj_idx, &obj, row});

  // ── Grip
  auto *grip = Gtk::make_managed<Gtk::Label>("⠿");
  grip->add_css_class("dim-label");
  row->append(*grip);

  // ── Type label — uniform text, not unicode symbols
  const char *type_str = "Path";
  if (obj.type == SceneNode::Type::Text) {
    type_str = "Text";
  } else if (obj.type == SceneNode::Type::Image) {
    type_str = "Image";
  } else if (obj.path && !obj.path->node_sets.empty()) {
    auto k = obj.path->node_sets[0].kind;
    if (k == NodeSet::Kind::Rect)
      type_str = "Rect";
    else if (k == NodeSet::Kind::Ellipse)
      type_str = "Oval";
  }
  auto *type_lbl = Gtk::make_managed<Gtk::Label>(type_str);
  type_lbl->add_css_class("dim-label");
  type_lbl->set_size_request(36, -1); // wider for "Image"
  type_lbl->set_xalign(0.0f);
  row->append(*type_lbl);

  // ── Name — prefer name over id; double-click to rename
  std::string display_name = obj.name.empty() ? obj.id : obj.name;
  if (display_name.empty())
    display_name = obj.is_text() ? "Text" : "path";
  auto *name_lbl = Gtk::make_managed<Gtk::Label>(display_name);
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  name_lbl->set_ellipsize(Pango::EllipsizeMode::END);
  // s112 — grey the name when the parent layer is locked, signalling
  // that the row is inert (click and DnD are gated).
  if (layer_at_locked(layer_idx)) {
    name_lbl->add_css_class("layer-locked-text");
    type_lbl->add_css_class("layer-locked-text");
  }

  auto name_click = Gtk::GestureClick::create();
  name_click->set_button(1);

  name_click->signal_pressed().connect([this, layer_idx, obj_idx, name_lbl,
                                        type_lbl, row, name_click](
                                           int n_press, double, double) {
    if (m_rebuilding)
      return;
    // s112 — locked layer: child rows are non-selectable. Layer row itself
    // remains clickable so the lock button stays reachable.
    if (layer_at_locked(layer_idx))
      return;
    // Single click — select on canvas
    m_active_layer = layer_idx;
    m_doc->active_layer_index = layer_idx;
    m_sig_layer_selected.emit(layer_idx);
    if (layer_idx < (int)m_doc->layers.size() &&
        obj_idx < (int)m_doc->layers[layer_idx]->children.size()) {
      SceneNode *clicked = m_doc->layers[layer_idx]->children[obj_idx].get();
      // Check Shift via the GestureClick's current event state
      auto *gesture = dynamic_cast<Gtk::GestureClick *>(name_click.get());
      bool shift_held = false;
      if (gesture) {
        auto evt = gesture->get_last_event(gesture->get_current_sequence());
        if (evt) {
          auto mods = evt->get_modifier_state();
          shift_held = (mods & Gdk::ModifierType::SHIFT_MASK) !=
                       Gdk::ModifierType::NO_MODIFIER_MASK;
        }
      }
      if (shift_held) {
        auto it = std::find(m_canvas_selection.begin(),
                            m_canvas_selection.end(), clicked);
        if (it != m_canvas_selection.end())
          m_canvas_selection.erase(it);
        else
          m_canvas_selection.push_back(clicked);
        m_sig_multi_selected.emit(m_canvas_selection);
      } else {
        m_canvas_selection = {clicked};
        m_sig_object_selected.emit(clicked);
      }
    }
    refresh_highlights();

    // Double-click — rename inline (not when shift-clicking to multi-select).
    // shift_held is read inside the if block above; re-read here for the check.
    {
      auto *g2 = dynamic_cast<Gtk::GestureClick *>(name_click.get());
      bool sh2 = false;
      if (g2) {
        auto ev2 = g2->get_last_event(g2->get_current_sequence());
        if (ev2)
          sh2 = (ev2->get_modifier_state() & Gdk::ModifierType::SHIFT_MASK) !=
                Gdk::ModifierType::NO_MODIFIER_MASK;
      }
      if (n_press >= 2 && !sh2 && layer_idx < (int)m_doc->layers.size() &&
          obj_idx < (int)m_doc->layers[layer_idx]->children.size()) {
        auto *entry = Gtk::make_managed<CurvzEntry>();
        curvz::utils::set_name(entry, "lp_re_obj", "layers_panel_rename_object_entry");
        auto &obj_ref = *m_doc->layers[layer_idx]->children[obj_idx];
        entry->set_text(obj_ref.name);
        entry->set_hexpand(true);
        entry->set_max_length(0);
        entry->set_width_chars(16); // force usable width
        entry->add_css_class("layer-name-entry");
        row->remove(*type_lbl);
        row->remove(*name_lbl);
        row->append(*entry);
        entry->grab_focus();
        entry->select_region(0, -1);

        // Shared commit — save and rebuild, guarded against double-fire
        auto committed = std::make_shared<bool>(false);
        auto do_commit = [this, layer_idx, obj_idx, entry, committed]() {
          if (*committed)
            return;
          *committed = true;
          if (layer_idx < (int)m_doc->layers.size() &&
              obj_idx < (int)m_doc->layers[layer_idx]->children.size()) {
            const std::string typed = entry->get_text();
            if (!typed.empty()) {
              SceneNode* self =
                  m_doc->layers[layer_idx]->children[obj_idx].get();
              m_doc->layers[layer_idx]->children[obj_idx]->name =
                  m_doc->uniquify_name(typed, self);
            }
          }
          m_sig_layer_changed.emit();
          Glib::signal_idle().connect_once([this]() { rebuild(); });
        };

        // Return key via CurvzEntry
        entry->on_commit(do_commit);

        // Focus-out (click elsewhere / Tab) also commits
        auto focus_ctrl = Gtk::EventControllerFocus::create();
        focus_ctrl->signal_leave().connect([do_commit]() { do_commit(); });
        entry->add_controller(focus_ctrl);
      } // end double-click rename
    } // end extra brace
  });
  name_lbl->add_controller(name_click);
  row->append(*name_lbl);

  setup_path_drag(row, layer_idx, obj_idx);
  setup_path_drop(row, layer_idx, obj_idx);

  parent->append(*row);
}

// ── add_child_row — dispatches to path or group row ──────────────────────────
void LayersPanel::add_child_row(SceneNode *obj, int layer_idx, int indent,
                                Gtk::Box *parent) {
  if (!obj)
    return;

  // DIAG S52 — trace dispatch for ClipGroup children issue. Remove when
  // Group-inside-ClipGroup expansion is confirmed working.
  LOG_INFO("LayersPanel::add_child_row type={} name='{}' indent={} parent={}",
           (int)obj->type, obj->name, indent, (void *)parent);

  // Ref points are rendered by add_ref_layer_row — never via this dispatch.
  // Defensive guard: if a Ref somehow appears under a non-ref layer, skip.
  if (obj->type == SceneNode::Type::Ref)
    return;


  if (obj->type == SceneNode::Type::Group ||
      obj->type == SceneNode::Type::Compound ||
      obj->type == SceneNode::Type::ClipGroup ||
      obj->type == SceneNode::Type::Blend ||
      obj->type == SceneNode::Type::Warp) {
    add_group_row(obj, layer_idx, indent, parent);
    return;
  }
  // Path — find obj_idx within layer for DnD, then build row
  for (int j = 0; j < (int)m_doc->layers[layer_idx]->children.size(); ++j) {
    if (m_doc->layers[layer_idx]->children[j].get() == obj) {
      add_path_row(layer_idx, j, parent);
      // Override indent — path row defaults to margin_start(20) for depth 1
      // For deeper nesting, find the last appended widget and adjust
      if (indent > 1) {
        if (auto *last = parent->get_last_child())
          last->set_margin_start(indent_px(indent));
      }
      return;
    }
  }
  // Object is inside a group — search groups recursively.
  //   The previous form only looked one level deep: direct layer
  //   children that were Group/Compound. That missed Paths nested
  //   inside Groups-within-Groups (and now Groups-within-ClipGroups).
  //   This walker descends through any container-like node and fires
  //   the row-building callback when it finds `obj`.
  auto is_container = [](const SceneNode *n) {
    return n && (n->type == SceneNode::Type::Group ||
                 n->type == SceneNode::Type::Compound ||
                 n->type == SceneNode::Type::ClipGroup ||
                 n->type == SceneNode::Type::Blend ||
                 n->type == SceneNode::Type::Warp);
  };
  bool found = false;
  std::function<void(const SceneNode *)> walk = [&](const SceneNode *node) {
    if (found || !is_container(node))
      return;
    for (int j = 0; j < (int)node->children.size(); ++j) {
      SceneNode *ch = node->children[j].get();
      if (ch == obj) {
        found = true;
        // ── Inline path-row render (was the body of the old one-level
        //   loop). Kept verbatim to minimise diff; the only difference
        //   is the enclosing control flow.
        auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
          row->set_spacing(4);
          row->set_margin_start(indent_px(indent));
          row->set_margin_end(6);
          row->set_margin_top(2);
          row->set_margin_bottom(2);
          row->add_css_class("path-row");
          if (std::find(m_canvas_selection.begin(), m_canvas_selection.end(),
                        obj) != m_canvas_selection.end())
            row->add_css_class("layer-active");
          m_row_entries.push_back({layer_idx, -1, obj, row});

          const char *type_str = "Path";
          if (obj->type == SceneNode::Type::Text) {
            type_str = "Text";
          } else if (obj->type == SceneNode::Type::Image) {
            type_str = "Image";
          } else if (obj->path && !obj->path->node_sets.empty()) {
            auto k = obj->path->node_sets[0].kind;
            if (k == NodeSet::Kind::Rect)
              type_str = "Rect";
            else if (k == NodeSet::Kind::Ellipse)
              type_str = "Oval";
          }
          auto *type_lbl = Gtk::make_managed<Gtk::Label>(type_str);
          type_lbl->add_css_class("dim-label");
          type_lbl->set_size_request(32, -1);
          type_lbl->set_xalign(0.0f);
          row->append(*type_lbl);

          std::string display_name = obj->name.empty() ? obj->id : obj->name;
          if (display_name.empty())
            display_name = "path";
          auto *name_lbl = Gtk::make_managed<Gtk::Label>(display_name);
          name_lbl->set_xalign(0.0f);
          name_lbl->set_hexpand(true);
          name_lbl->add_css_class("prop-lbl");
          name_lbl->set_ellipsize(Pango::EllipsizeMode::END);
          // s112 — grey when parent layer is locked.
          if (layer_at_locked(layer_idx)) {
            name_lbl->add_css_class("layer-locked-text");
            type_lbl->add_css_class("layer-locked-text");
          }

          auto click = Gtk::GestureClick::create();
          click->set_button(1);
          click->signal_pressed().connect([this, obj, layer_idx, row, type_lbl, name_lbl,
                                           click](int n_press, double, double) {
            if (m_rebuilding)
              return;
            // s112 — locked layer: child rows are non-selectable.
            if (layer_at_locked(layer_idx))
              return;
            auto *g = dynamic_cast<Gtk::GestureClick *>(click.get());
            bool shift_held = false;
            if (g) {
              auto evt = g->get_last_event(g->get_current_sequence());
              if (evt)
                shift_held = (evt->get_modifier_state() &
                              Gdk::ModifierType::SHIFT_MASK) !=
                             Gdk::ModifierType::NO_MODIFIER_MASK;
            }
            if (shift_held) {
              auto it = std::find(m_canvas_selection.begin(),
                                  m_canvas_selection.end(), obj);
              if (it != m_canvas_selection.end())
                m_canvas_selection.erase(it);
              else
                m_canvas_selection.push_back(obj);
              m_sig_multi_selected.emit(m_canvas_selection);
            } else {
              m_canvas_selection = {obj};
              m_sig_object_selected.emit(obj);
            }
            refresh_highlights();

            // Double-click — rename inline
            if (n_press >= 2 && !shift_held) {
              auto *entry = Gtk::make_managed<CurvzEntry>();
              curvz::utils::set_name(entry, "lp_re_chd", "layers_panel_rename_child_entry");
              entry->set_text(obj->name);
              entry->set_hexpand(true);
              entry->set_max_length(0);
              entry->set_width_chars(16);
              entry->add_css_class("layer-name-entry");
              row->remove(*type_lbl);
              row->remove(*name_lbl);
              row->append(*entry);
              entry->grab_focus();
              entry->select_region(0, -1);

              auto committed = std::make_shared<bool>(false);
              auto do_commit = [this, obj, entry, committed]() {
                if (*committed)
                  return;
                *committed = true;
                const std::string typed = entry->get_text();
                if (!typed.empty())
                  obj->name = m_doc->uniquify_name(typed, obj);
                m_sig_layer_changed.emit();
                Glib::signal_idle().connect_once([this]() { rebuild(); });
              };
              entry->on_commit(do_commit);
              auto focus_ctrl = Gtk::EventControllerFocus::create();
              focus_ctrl->signal_leave().connect(
                  [do_commit]() { do_commit(); });
              entry->add_controller(focus_ctrl);
            }
          });
          name_lbl->add_controller(click);
          row->append(*name_lbl);
          parent->append(*row);
          return;
      }
      // Recurse into nested containers.
      if (is_container(ch))
        walk(ch);
      if (found) return;
    }
  };
  for (auto &layer : m_doc->layers) {
    for (auto &child : layer->children) {
      walk(child.get());
      if (found) break;
    }
    if (found) break;
  }
}

// ── add_group_row — collapsible group with children
// ───────────────────────────
void LayersPanel::add_group_row(SceneNode *group, int layer_idx, int indent,
                                Gtk::Box *parent) {
  std::string skey = group ? group_state_key(*group) : std::string("G:?");
  bool expanded = lookup_expanded(skey, false);
  if (auto *e = find_collapse_group(group))
    expanded = e->expanded;

  auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(indent_px(indent));
  hdr->set_margin_end(6);
  hdr->set_margin_top(2);
  hdr->set_margin_bottom(2);
  hdr->add_css_class("path-row");

  // Highlight if canvas-selected
  if (std::find(m_canvas_selection.begin(), m_canvas_selection.end(), group) !=
      m_canvas_selection.end())
    hdr->add_css_class("layer-active");

  m_row_entries.push_back({layer_idx, -1, group, hdr});

  // ── Arrow
  auto *arrow_btn = Gtk::make_managed<Gtk::Button>();
  arrow_btn->set_has_frame(false);
  arrow_btn->set_label(expanded ? "▾" : "▸");
  arrow_btn->set_size_request(20, -1);
  arrow_btn->add_css_class("part-arrow");
  arrow_btn->signal_clicked().connect([this, group]() { toggle_group(group); });
  hdr->append(*arrow_btn);

  // ── Group/Compound/ClipGroup/Blend/Warp icon
  const char *icon_char;
  if (group->type == SceneNode::Type::Compound)       icon_char = "◉";
  else if (group->type == SceneNode::Type::ClipGroup) icon_char = "✂";
  else if (group->type == SceneNode::Type::Blend)     icon_char = "⥂";
  else if (group->type == SceneNode::Type::Warp)      icon_char = "∿";
  else                                                 icon_char = "⬚";
  auto *icon_lbl = Gtk::make_managed<Gtk::Label>(icon_char);
  icon_lbl->add_css_class("dim-label");
  icon_lbl->set_size_request(20, -1);
  hdr->append(*icon_lbl);

  // ── Name
  std::string display_name = group->name.empty() ? group->id : group->name;
  if (display_name.empty()) {
    if (group->type == SceneNode::Type::Compound)       display_name = "Compound";
    else if (group->type == SceneNode::Type::ClipGroup) display_name = "Clip";
    else if (group->type == SceneNode::Type::Blend)     display_name = "Blend";
    else if (group->type == SceneNode::Type::Warp)      display_name = "Warp";
    else                                                 display_name = "Group";
  }
  auto *name_lbl = Gtk::make_managed<Gtk::Label>(display_name);
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  name_lbl->set_ellipsize(Pango::EllipsizeMode::END);
  // s112 — grey when parent layer is locked.
  if (layer_at_locked(layer_idx)) {
    name_lbl->add_css_class("layer-locked-text");
    icon_lbl->add_css_class("layer-locked-text");
  }
  auto name_click = Gtk::GestureClick::create();
  name_click->set_button(1);
  name_click->signal_pressed().connect(
      [this, group, layer_idx, name_lbl, icon_lbl, hdr, name_click](int n_press, double,
                                                         double) {
        if (m_rebuilding)
          return;
        // s112 — locked layer: child rows are non-selectable.
        if (layer_at_locked(layer_idx))
          return;
        auto *gesture3 = dynamic_cast<Gtk::GestureClick *>(name_click.get());
        bool shift_held = false;
        if (gesture3) {
          auto evt = gesture3->get_last_event(gesture3->get_current_sequence());
          if (evt)
            shift_held =
                (evt->get_modifier_state() & Gdk::ModifierType::SHIFT_MASK) !=
                Gdk::ModifierType::NO_MODIFIER_MASK;
        }
        m_sig_object_selected.emit(group);
        if (shift_held) {
          auto it = std::find(m_canvas_selection.begin(),
                              m_canvas_selection.end(), group);
          if (it != m_canvas_selection.end())
            m_canvas_selection.erase(it);
          else
            m_canvas_selection.push_back(group);
          m_sig_multi_selected.emit(m_canvas_selection);
        } else {
          m_canvas_selection = {group};
          m_sig_object_selected.emit(group);
        }
        refresh_highlights();

        // Double-click — rename inline (not when shift-clicking to
        // multi-select)
        if (n_press >= 2 && !shift_held) {
          auto *entry = Gtk::make_managed<CurvzEntry>();
          curvz::utils::set_name(entry, "lp_re_grp", "layers_panel_rename_group_entry");
          entry->set_text(group->name);
          entry->set_hexpand(true);
          entry->set_max_length(0);
          entry->set_width_chars(16);
          entry->add_css_class("layer-name-entry");
          hdr->remove(*icon_lbl);
          hdr->remove(*name_lbl);
          hdr->append(*entry);
          entry->grab_focus();
          entry->select_region(0, -1);

          auto committed = std::make_shared<bool>(false);
          auto do_commit = [this, group, entry, committed]() {
            if (*committed)
              return;
            *committed = true;
            const std::string typed = entry->get_text();
            if (!typed.empty())
              group->name = m_doc->uniquify_name(typed, group);
            m_sig_layer_changed.emit();
            Glib::signal_idle().connect_once([this]() { rebuild(); });
          };

          entry->on_commit(do_commit);

          auto focus_ctrl = Gtk::EventControllerFocus::create();
          focus_ctrl->signal_leave().connect([do_commit]() { do_commit(); });
          entry->add_controller(focus_ctrl);
        }
      });
  name_lbl->add_controller(name_click);
  hdr->append(*name_lbl);

  // ── Child count — ClipGroup counts its clip shape too (it occupies
  //   a row in the expanded view, just like a real child).
  //   Blend: always 3 rows — A, "Steps (N)" pseudo, B.
  //   Warp: always 3 rows — Source, Top envelope, Bottom envelope.
  char cnt[12];
  int child_count = (int)group->children.size();
  if (group->type == SceneNode::Type::ClipGroup && group->clip_shape)
    child_count += 1;
  if (group->type == SceneNode::Type::Blend)
    child_count = 3;
  if (group->type == SceneNode::Type::Warp)
    child_count = 3;
  snprintf(cnt, sizeof(cnt), "%d", child_count);
  auto *count_lbl = Gtk::make_managed<Gtk::Label>(cnt);
  count_lbl->set_size_request(20, -1);
  count_lbl->set_xalign(1.0f);
  count_lbl->add_css_class("dim-label");
  hdr->append(*count_lbl);

  card->append(*hdr);

  // ── DnD: top-level group-like rows reuse the path-row drag/drop pump
  //   so Compound/Group/ClipGroup/Blend/Warp can be reordered within a
  //   layer and moved between layers. Wired only when this group is a
  //   direct child of m_doc->layers[layer_idx] — nested groups have no
  //   stable obj_idx in the path-payload and (consistent with nested
  //   paths) are not draggable from within their parent.
  if (layer_idx >= 0 && layer_idx < (int)m_doc->layers.size()) {
    auto &siblings = m_doc->layers[layer_idx]->children;
    int top_obj_idx = -1;
    for (int j = 0; j < (int)siblings.size(); ++j) {
      if (siblings[j].get() == group) {
        top_obj_idx = j;
        break;
      }
    }
    if (top_obj_idx >= 0) {
      setup_path_drag(hdr, layer_idx, top_obj_idx);
      setup_path_drop(hdr, layer_idx, top_obj_idx);
    }
  }

  // ── Right-click on a Blend's header row → Rebuild context menu ───────
  // Only Blends get the popover; other container types (Group/Compound/
  // ClipGroup) fall through to existing behaviour (nothing, currently).
  if (group->type == SceneNode::Type::Blend) {
    auto rclick = Gtk::GestureClick::create();
    rclick->set_button(3);
    rclick->signal_pressed().connect(
        [this, group, hdr](int, double x, double y) {
          if (m_rebuilding) return;
          auto *popover = Gtk::make_managed<Gtk::Popover>();
          popover->set_parent(*hdr);
          popover->set_has_arrow(true);
          auto *box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
          box->set_spacing(2);
          box->set_margin_start(4);
          box->set_margin_end(4);
          box->set_margin_top(4);
          box->set_margin_bottom(4);
          auto *btn = Gtk::make_managed<Gtk::Button>("Rebuild Blend Steps");
          btn->set_has_frame(false);
          btn->signal_clicked().connect([this, group, popover]() {
            m_sig_rebuild_blend.emit(group);
            popover->popdown();
          });
          box->append(*btn);
          popover->set_child(*box);
          Gdk::Rectangle rect((int)x, (int)y, 1, 1);
          popover->set_pointing_to(rect);
          popover->popup();
        });
    hdr->add_controller(rclick);
  }

  // ── Revealer for children
  auto *revealer = Gtk::make_managed<Gtk::Revealer>();
  revealer->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);
  revealer->set_transition_duration(150);
  revealer->set_reveal_child(expanded);

  auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  // Match the layer-level iteration direction: children[0] is the visually
  // topmost object (rendered last in draw_object's group branch), and the
  // panel should present it at the top of the group's expanded area.
  //
  // ClipGroup: show clip_shape as the first (topmost) pseudo-child,
  // then the actual clipped children. The clip_shape is a distinct
  // SceneNode held in its own slot, but for panel purposes it's just
  // another node to render a row for — add_child_row resolves parents
  // by scanning the document tree, and since clip_shape lives on the
  // ClipGroup rather than in `children`, a straight add_child_row may
  // have trouble. For Stage 3.5 we emit a minimal inline row for it —
  // enough to show name + select it — without going through the full
  // path-row plumbing (which expects an index into parent->children).
  if (group->type == SceneNode::Type::ClipGroup && group->clip_shape) {
    SceneNode *cs = group->clip_shape.get();
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(4);
    row->set_margin_start(indent_px(indent + 1));
    row->set_margin_end(6);
    row->set_margin_top(2);
    row->set_margin_bottom(2);
    row->add_css_class("path-row");
    if (std::find(m_canvas_selection.begin(), m_canvas_selection.end(), cs) !=
        m_canvas_selection.end())
      row->add_css_class("layer-active");
    m_row_entries.push_back({layer_idx, -1, cs, row});

    auto *badge = Gtk::make_managed<Gtk::Label>("✂");
    badge->add_css_class("dim-label");
    badge->set_size_request(20, -1);
    row->append(*badge);

    std::string cs_name = cs->name.empty() ? cs->id : cs->name;
    if (cs_name.empty()) cs_name = "Clip Path";
    auto *lbl = Gtk::make_managed<Gtk::Label>(cs_name);
    lbl->set_xalign(0.0f);
    lbl->set_hexpand(true);
    lbl->add_css_class("prop-lbl");
    lbl->set_ellipsize(Pango::EllipsizeMode::END);

    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect(
        [this, cs, layer_idx](int /*n*/, double, double) {
          if (m_rebuilding) return;
          // s112 — locked layer: child rows are non-selectable.
          if (layer_at_locked(layer_idx)) return;
          m_canvas_selection = {cs};
          m_sig_object_selected.emit(cs);
          refresh_highlights();
        });
    lbl->add_controller(click);
    row->append(*lbl);
    inner->append(*row);
  }

  // ── Blend: three pseudo-rows in panel order B → Steps → A.
  //   Panel convention is [0]=top-of-z, so the row listed first in the
  //   panel should be the visually-topmost thing. Blend render order is
  //   A (bottom) → cache → B (top), so B goes first in the expanded
  //   panel, then a non-expandable "Steps (N)" pseudo-row, then A.
  //
  //   Rows are clickable (select the source) and name-editable via
  //   double-click in a future pass. Deleting A or B via the Layer
  //   panel dissolves the Blend — handled in Canvas::delete_selected
  //   via find_blend_owner (M2 add).
  //
  //   A/B do NOT live in group->children — they're in dedicated slots,
  //   so we emit inline rows rather than calling add_child_row (which
  //   expects an index into a parent's children vector).
  if (group->type == SceneNode::Type::Blend) {
    auto emit_source_row = [&](SceneNode *src, const char *badge_char,
                               const std::string &default_name,
                               bool locked_node_count) {
      if (!src) return;
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->set_spacing(4);
      row->set_margin_start(indent_px(indent + 1));
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);
      row->add_css_class("path-row");
      if (std::find(m_canvas_selection.begin(), m_canvas_selection.end(),
                    src) != m_canvas_selection.end())
        row->add_css_class("layer-active");
      m_row_entries.push_back({layer_idx, -1, src, row});

      auto *badge = Gtk::make_managed<Gtk::Label>(badge_char);
      badge->add_css_class("dim-label");
      badge->set_size_request(20, -1);
      row->append(*badge);

      std::string nm = src->name.empty() ? src->id : src->name;
      if (nm.empty()) nm = default_name;
      if (locked_node_count) nm += "  (locked count)";
      auto *lbl = Gtk::make_managed<Gtk::Label>(nm);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      lbl->add_css_class("prop-lbl");
      lbl->set_ellipsize(Pango::EllipsizeMode::END);

      auto click = Gtk::GestureClick::create();
      click->set_button(1);
      click->signal_pressed().connect(
          [this, src, layer_idx](int /*n*/, double, double) {
            if (m_rebuilding) return;
            // s112 — locked layer: child rows are non-selectable.
            if (layer_at_locked(layer_idx)) return;
            m_canvas_selection = {src};
            m_sig_object_selected.emit(src);
            refresh_highlights();
          });
      lbl->add_controller(click);
      row->append(*lbl);
      inner->append(*row);
    };

    // B on top (z-order top)
    emit_source_row(group->blend_source_b.get(), "B", "Source B", true);

    // Steps pseudo-row — non-expandable, non-selectable-for-edit.
    //   Label shows "Steps (N)" where N is the current step count.
    //   Click is a no-op for M2 (the pseudo-row is a display artefact;
    //   in M3 it'll gain a spinner for live step count adjustment).
    {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->set_spacing(4);
      row->set_margin_start(indent_px(indent + 1));
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);
      row->add_css_class("path-row");
      auto *badge = Gtk::make_managed<Gtk::Label>("⋯");
      badge->add_css_class("dim-label");
      badge->set_size_request(20, -1);
      row->append(*badge);

      char buf[48];
      snprintf(buf, sizeof(buf), "Steps (%d)", group->blend_steps);
      auto *lbl = Gtk::make_managed<Gtk::Label>(buf);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      lbl->add_css_class("prop-lbl");
      lbl->add_css_class("dim-label");
      row->append(*lbl);
      inner->append(*row);
    }

    // A at bottom (z-order bottom)
    emit_source_row(group->blend_source_a.get(), "A", "Source A", true);
  }

  // ── Warp: three pseudo-rows in panel order Source → Top env → Bottom env.
  //   Panel convention is [0]=top-of-z. The Warp itself has no internal
  //   z-order to speak of (warp_cache is what renders), so the row
  //   ordering here is for readability: the clickable Source row goes
  //   first because that's the user-authored input and the most common
  //   selection target from the tree. The two envelope rows are
  //   non-clickable display-only in M1 (envelopes are PathData, not
  //   SceneNodes, so there's nothing for click to select) — M3's
  //   WarpDialog is the editor.
  //
  //   Deleting the source via the Layer panel dissolves the Warp —
  //   handled in Canvas::delete_selected via find_warp_owner (M3 add).
  if (group->type == SceneNode::Type::Warp) {
    // Source row — clickable, opens selection like any other path/group.
    if (group->warp_source) {
      SceneNode *src = group->warp_source.get();
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->set_spacing(4);
      row->set_margin_start(indent_px(indent + 1));
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);
      row->add_css_class("path-row");
      if (std::find(m_canvas_selection.begin(), m_canvas_selection.end(),
                    src) != m_canvas_selection.end())
        row->add_css_class("layer-active");
      m_row_entries.push_back({layer_idx, -1, src, row});

      auto *badge = Gtk::make_managed<Gtk::Label>("S");
      badge->add_css_class("dim-label");
      badge->set_size_request(20, -1);
      row->append(*badge);

      std::string nm = src->name.empty() ? src->id : src->name;
      if (nm.empty()) nm = "Warp Source";
      auto *lbl = Gtk::make_managed<Gtk::Label>(nm);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      lbl->add_css_class("prop-lbl");
      lbl->set_ellipsize(Pango::EllipsizeMode::END);

      auto click = Gtk::GestureClick::create();
      click->set_button(1);
      click->signal_pressed().connect(
          [this, src, layer_idx](int /*n*/, double, double) {
            if (m_rebuilding) return;
            // s112 — locked layer: child rows are non-selectable.
            if (layer_at_locked(layer_idx)) return;
            m_canvas_selection = {src};
            m_sig_object_selected.emit(src);
            refresh_highlights();
          });
      lbl->add_controller(click);
      row->append(*lbl);
      inner->append(*row);
    }

    // Envelope pseudo-rows — display-only, no click handler.
    auto emit_env_row = [&](const char *badge_char, const char *name,
                            int anchor_count) {
      auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      row->set_spacing(4);
      row->set_margin_start(indent_px(indent + 1));
      row->set_margin_end(6);
      row->set_margin_top(2);
      row->set_margin_bottom(2);
      row->add_css_class("path-row");
      auto *badge = Gtk::make_managed<Gtk::Label>(badge_char);
      badge->add_css_class("dim-label");
      badge->set_size_request(20, -1);
      row->append(*badge);
      char buf[64];
      snprintf(buf, sizeof(buf), "%s (%d anchor%s)", name, anchor_count,
               anchor_count == 1 ? "" : "s");
      auto *lbl = Gtk::make_managed<Gtk::Label>(buf);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      lbl->add_css_class("prop-lbl");
      lbl->add_css_class("dim-label");
      row->append(*lbl);
      inner->append(*row);
    };
    emit_env_row("T", "Top envelope",
                 (int)group->warp_env_top.nodes.size());
    emit_env_row("B", "Bottom envelope",
                 (int)group->warp_env_bottom.nodes.size());
  }

  for (int j = 0; j < (int)group->children.size(); ++j)
    add_child_row(group->children[j].get(), layer_idx, indent + 1, inner);
  revealer->set_child(*inner);
  card->append(*revealer);
  parent->append(*card);

  // Update collapse entry for this group
  m_collapse_entries.erase(std::remove_if(m_collapse_entries.begin(),
                                          m_collapse_entries.end(),
                                          [group](const CollapseEntry &e) {
                                            return e.group_node == group;
                                          }),
                           m_collapse_entries.end());
  m_collapse_entries.push_back(
      {layer_idx, group, revealer, arrow_btn, expanded, skey});
}

// ── add_ref_layer_row
// ────────────────────────────────────────────────────────── Renders the
// References layer row: fixed label, eye, lock, DnD for z-order, children
// listed as coordinate name rows (selectable, no DnD out).
void LayersPanel::add_ref_layer_row(int i, Gtk::Box *parent) {
  SceneNode &layer = *m_doc->layers[i];

  std::string skey = layer_state_key(layer);
  bool expanded = lookup_expanded(skey, false);
  if (auto *e = find_collapse(i))
    expanded = e->expanded;

  auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(4);
  hdr->set_margin_end(6);
  hdr->set_margin_top(2);
  hdr->set_margin_bottom(2);
  hdr->add_css_class("layer-header");
  hdr->set_focusable(true);

  m_row_entries.push_back({i, -1, nullptr, hdr});

  // ── Arrow
  auto *arrow_btn = Gtk::make_managed<Gtk::Button>();
  arrow_btn->set_has_frame(false);
  arrow_btn->set_label(expanded ? "▾" : "▸");
  arrow_btn->set_size_request(20, -1);
  arrow_btn->add_css_class("part-arrow");
  arrow_btn->signal_clicked().connect([this, i]() { toggle_layer(i); });
  hdr->append(*arrow_btn);

  // ── Ref color dot — clickable, opens color picker
  auto *color_dot = Gtk::make_managed<Gtk::DrawingArea>();
  color_dot->set_size_request(12, 12);
  color_dot->set_valign(Gtk::Align::CENTER);
  color_dot->set_can_target(true);
  color_dot->set_tooltip_text("Reference point color — click to change");

  // Parse current color from layer->color hex string
  auto parse_ref_color = [](const std::string &hex, double &r, double &g,
                            double &b) {
    r = 0.85;
    g = 0.10;
    b = 0.75; // default magenta
    if (hex.size() == 7 && hex[0] == '#') {
      auto hv = [&](int i) { return std::stoi(hex.substr(i, 2), nullptr, 16); };
      r = hv(1) / 255.0;
      g = hv(3) / 255.0;
      b = hv(5) / 255.0;
    }
  };

  color_dot->set_draw_func(
      [this, i, parse_ref_color](const Cairo::RefPtr<Cairo::Context> &cr, int w,
                                 int h) {
        double r, g, b;
        std::string col =
            (i < (int)m_doc->layers.size()) ? m_doc->layers[i]->color : "";
        parse_ref_color(col, r, g, b);
        cr->arc(w / 2.0, h / 2.0, w / 2.0 - 0.5, 0, 2 * M_PI);
        cr->set_source_rgb(r, g, b);
        cr->fill_preserve();
        cr->set_source_rgba(1, 1, 1, 0.15);
        cr->set_line_width(0.5);
        cr->stroke();
      });

  auto ref_dot_click = Gtk::GestureClick::create();
  ref_dot_click->set_button(1);
  ref_dot_click->signal_pressed().connect(
      [this, i, color_dot, parse_ref_color](int, double, double) {
        double r, g, b;
        std::string col =
            (i < (int)m_doc->layers.size()) ? m_doc->layers[i]->color : "";
        parse_ref_color(col, r, g, b);
        color::Color initial(r, g, b, 1.0);

        m_color_popover.open(
            *color_dot, initial, /*with_alpha=*/false,
            [this, i](const color::Color& c) {
              if (i >= (int)m_doc->layers.size()) return;
              std::string hex = color::to_hex(c);
              if (hex.size() > 7) hex.resize(7);
              // s172 m4 — capture before, mutate, push.
              std::string before = m_doc->layers[i]->color;
              m_doc->layers[i]->color = hex;
              if (before != hex) {
                push_edit_layer_string_field(i, /*is_color=*/true, before, hex);
              }
              m_sig_layer_changed.emit();
              Glib::signal_idle().connect_once([this]() { rebuild(); });
            });
      });
  color_dot->add_controller(ref_dot_click);
  hdr->append(*color_dot);

  // ── Visibility
  auto *vis_btn = Gtk::make_managed<Gtk::Button>();
  vis_btn->set_has_frame(false);
  vis_btn->set_icon_name("curvz-eye-symbolic");
  vis_btn->set_tooltip_text(layer.visible ? "Hide references"
                                          : "Show references");
  if (!layer.visible)
    vis_btn->add_css_class("layer-hidden-btn");
  vis_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->visible;
    m_doc->layers[i]->visible = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/false, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*vis_btn);

  // ── Name (fixed, click activates the ref layer — see add_guide_layer_row
  //    for the rationale around active_layer_index gating in canvas-side
  //    hit tests.)
  auto *name_lbl = Gtk::make_managed<Gtk::Label>("References");
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  if (!layer.visible)
    name_lbl->add_css_class("layer-hidden-text");
  auto name_click = Gtk::GestureClick::create();
  name_click->set_button(1);
  name_click->signal_pressed().connect(
      [this, i](int, double, double) {
        if (m_rebuilding)
          return;
        m_active_layer = i;
        m_doc->active_layer_index = i;
        m_sig_layer_selected.emit(i);
        refresh_highlights();
      });
  name_lbl->add_controller(name_click);
  hdr->append(*name_lbl);

  // ── Lock
  auto *lock_btn = Gtk::make_managed<Gtk::Button>();
  lock_btn->set_has_frame(false);
  lock_btn->set_icon_name(layer.locked ? "curvz-locked-symbolic"
                                       : "curvz-unlocked-symbolic");
  // lock_btn->set_icon_name(layer.locked ? "changes-prevent-symbolic"
  // : "changes-allow-symbolic");
  lock_btn->set_tooltip_text(layer.locked ? "Unlock references"
                                          : "Lock references");
  if (layer.locked)
    lock_btn->add_css_class("layer-locked-btn");
  lock_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->locked;
    m_doc->layers[i]->locked = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/true, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*lock_btn);

  // ── Count (after lock, matching normal layer order)
  char cnt[12];
  snprintf(cnt, sizeof(cnt), "%d", (int)layer.children.size());
  auto *count_lbl = Gtk::make_managed<Gtk::Label>(cnt);
  count_lbl->set_size_request(20, -1);
  count_lbl->set_xalign(1.0f);
  count_lbl->add_css_class("dim-label");
  count_lbl->set_tooltip_text("Ref points");
  hdr->append(*count_lbl);

  card->append(*hdr);

  // Layer can be DnD'd for z-order
  setup_layer_drag(hdr, i);
  setup_layer_drop(hdr, i);

  // ── Revealer — ref children listed by coordinate name
  auto *revealer = Gtk::make_managed<Gtk::Revealer>();
  revealer->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);
  revealer->set_transition_duration(150);
  revealer->set_reveal_child(expanded);

  auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  bool rl_locked = layer.locked;

  for (int j = (int)layer.children.size() - 1; j >= 0; --j) {
    SceneNode *r = layer.children[j].get();
    if (!r->is_ref())
      continue;

    bool is_sel =
        std::find(m_canvas_selection.begin(), m_canvas_selection.end(), r) !=
        m_canvas_selection.end();

    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(4);
    row->set_margin_start(20);
    row->set_margin_end(6);
    row->set_margin_top(1);
    row->set_margin_bottom(1);
    row->set_focusable(
        true); // always focusable — locked means no move/delete, not no select
    if (is_sel)
      row->add_css_class("layer-active");

    // Register for highlight sync
    m_row_entries.push_back({i, j, r, row});

    // ⊕ type label
    auto *type_lbl = Gtk::make_managed<Gtk::Label>("⊕");
    type_lbl->add_css_class("dim-label");
    type_lbl->set_size_request(16, -1);
    row->append(*type_lbl);

    // Coordinate name
    std::string display = r->name.empty() ? r->id : r->name;
    auto *name_r = Gtk::make_managed<Gtk::Label>(display);
    name_r->set_xalign(0.0f);
    name_r->set_hexpand(true);
    name_r->add_css_class("prop-lbl");
    name_r->set_ellipsize(Pango::EllipsizeMode::END);
    row->append(*name_r);

    // Click on the name label — single-click selects, double-click renames.
    // Mirror of the path-row pattern at add_path_row (~line 1117). Controller
    // is on the label, not the row, so n_press click-counting works for the
    // double-click branch. Locked refpts can still be selected (locked = no
    // move/delete, not no select).
    {
      auto click = Gtk::GestureClick::create();
      click->set_button(1);
      // Capture i (layer_idx) and j (obj_idx) from the enclosing for-loop;
      // also row, type_lbl, name_r so the rename branch can swap widgets.
      click->signal_pressed().connect([this, i, j, r, row, type_lbl, name_r,
                                       click](int n_press, double, double) {
        if (m_rebuilding)
          return;
        // Single-click: select on canvas. Shift extends multi-selection.
        Gdk::ModifierType mod = click->get_current_event_state();
        bool shift =
            (mod & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
        if (shift) {
          auto it = std::find(m_canvas_selection.begin(),
                              m_canvas_selection.end(), r);
          if (it != m_canvas_selection.end())
            m_canvas_selection.erase(it);
          else
            m_canvas_selection.push_back(r);
          m_sig_multi_selected.emit(m_canvas_selection);
        } else {
          m_canvas_selection = {r};
          m_sig_object_selected.emit(r);
        }
        Glib::signal_idle().connect_once([this]() {
          Glib::signal_idle().connect_once([this]() { refresh_highlights(); });
        });

        // Double-click: rename inline (skip when shift-extending).
        if (n_press >= 2 && !shift && i < (int)m_doc->layers.size() &&
            j < (int)m_doc->layers[i]->children.size() &&
            m_doc->layers[i]->children[j].get() == r) {
          auto *entry = Gtk::make_managed<CurvzEntry>();
          curvz::utils::set_name(entry, "lp_re_ref",
                                 "layers_panel_rename_ref_entry");
          entry->set_text(r->name);
          entry->set_hexpand(true);
          entry->set_max_length(0);
          entry->set_width_chars(16);
          entry->add_css_class("layer-name-entry");
          // Swap visible widgets for the entry, mirroring path-row.
          row->remove(*type_lbl);
          row->remove(*name_r);
          row->append(*entry);
          entry->grab_focus();
          entry->select_region(0, -1);

          // Shared commit — guarded against double-fire from Return + focus-out.
          // s145: refpt names always non-empty; auto-fill via uniquify_name
          // routes empty input to a generated default through the funnel.
          auto committed = std::make_shared<bool>(false);
          auto do_commit = [this, i, j, r, entry, committed]() {
            if (*committed)
              return;
            *committed = true;
            if (i < (int)m_doc->layers.size() &&
                j < (int)m_doc->layers[i]->children.size() &&
                m_doc->layers[i]->children[j].get() == r) {
              const std::string typed = entry->get_text();
              if (!typed.empty()) {
                r->name = m_doc->uniquify_name(typed, r);
              }
            }
            m_sig_layer_changed.emit();
            Glib::signal_idle().connect_once([this]() { rebuild(); });
          };
          entry->on_commit(do_commit);
          auto focus_ctrl = Gtk::EventControllerFocus::create();
          focus_ctrl->signal_leave().connect([do_commit]() { do_commit(); });
          entry->add_controller(focus_ctrl);
        }
      });
      name_r->add_controller(click);
    }
    inner->append(*row);
  }

  revealer->set_child(*inner);
  card->append(*revealer);
  parent->append(*card);

  // Store collapse state
  m_collapse_entries.erase(
      std::remove_if(m_collapse_entries.begin(), m_collapse_entries.end(),
                     [i](const CollapseEntry &e) {
                       return e.layer_idx == i && !e.group_node;
                     }),
      m_collapse_entries.end());
  m_collapse_entries.push_back({i, nullptr, revealer, arrow_btn, expanded, skey});
}

// ── add_measure_layer_row
// ───────────────────────────────────────────────────── Renders the special
// Measurements layer row: green dot, eye, lock, name, count badge, and a
// revealer listing each measurement by name.
void LayersPanel::add_measure_layer_row(int i, Gtk::Box *parent) {
  SceneNode &layer = *m_doc->layers[i];

  std::string skey = layer_state_key(layer);
  bool expanded = lookup_expanded(skey, false);
  if (auto *e = find_collapse(i))
    expanded = e->expanded;

  auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(4);
  hdr->set_margin_end(6);
  hdr->set_margin_top(2);
  hdr->set_margin_bottom(2);
  // S89: match guides / references / grid / margins headers, was diverging
  // (layer-row + pan-*-symbolic icons) which rendered with extra indent and
  // the wrong chevron icon depending on theme. Canonical pattern is:
  //   class "layer-header" + text-label part-arrow button + toggle_layer(i).
  hdr->add_css_class("layer-header");
  hdr->set_focusable(true);

  m_row_entries.push_back({i, -1, nullptr, hdr});

  // ── Arrow (text label, like guides / refs / grid)
  auto *toggle = Gtk::make_managed<Gtk::Button>();
  toggle->set_has_frame(false);
  toggle->set_label(expanded ? "\u25be" : "\u25b8");  // ▾ ▸
  toggle->set_size_request(20, -1);
  toggle->add_css_class("part-arrow");
  toggle->signal_clicked().connect([this, i]() { toggle_layer(i); });
  hdr->append(*toggle);

  // ── Green dot (static, no colour dialog)
  auto *dot = Gtk::make_managed<Gtk::DrawingArea>();
  dot->set_size_request(14, 14);
  dot->set_draw_func([](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
    cr->arc(w / 2.0, h / 2.0, w / 2.0 - 0.5, 0, 2 * M_PI);
    cr->set_source_rgb(0.133, 0.773, 0.369); // #22C55E
    cr->fill_preserve();
    cr->set_source_rgba(1, 1, 1, 0.15);
    cr->set_line_width(0.5);
    cr->stroke();
  });
  hdr->append(*dot);

  // ── Visibility
  auto *vis_btn = Gtk::make_managed<Gtk::Button>();
  vis_btn->set_has_frame(false);
  vis_btn->set_icon_name("curvz-eye-symbolic");
  vis_btn->set_tooltip_text(layer.visible ? "Hide measurements"
                                          : "Show measurements");
  if (!layer.visible)
    vis_btn->add_css_class("layer-hidden-btn");
  vis_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->visible;
    m_doc->layers[i]->visible = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/false, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*vis_btn);

  // ── Name
  auto *name_lbl = Gtk::make_managed<Gtk::Label>("Measurements");
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  if (!layer.visible)
    name_lbl->add_css_class("layer-hidden-text");
  hdr->append(*name_lbl);

  // ── Lock
  auto *lock_btn = Gtk::make_managed<Gtk::Button>();
  lock_btn->set_has_frame(false);
  // lock_btn->set_icon_name(layer.locked ? "changes-prevent-symbolic"
  //                                      : "changes-allow-symbolic");
  lock_btn->set_icon_name(layer.locked ? "curvz-locked-symbolic"
                                       : "curvz-unlocked-symbolic");
  lock_btn->set_tooltip_text(layer.locked ? "Unlock measurements"
                                          : "Lock measurements");
  if (layer.locked)
    lock_btn->add_css_class("layer-locked-btn");
  lock_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->locked;
    m_doc->layers[i]->locked = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/true, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*lock_btn);

  // ── Count badge
  int mcount = 0;
  for (auto &ch : layer.children)
    if (ch->is_measurement())
      ++mcount;
  char cnt[12];
  snprintf(cnt, sizeof(cnt), "%d", mcount);
  auto *count_lbl = Gtk::make_managed<Gtk::Label>(cnt);
  count_lbl->set_size_request(20, -1);
  count_lbl->set_xalign(1.0f);
  count_lbl->add_css_class("dim-label");
  count_lbl->set_tooltip_text("Measurements");
  hdr->append(*count_lbl);

  card->append(*hdr);
  setup_layer_drag(hdr, i);
  setup_layer_drop(hdr, i);

  // ── Revealer — list measurements by name
  auto *revealer = Gtk::make_managed<Gtk::Revealer>();
  revealer->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);
  revealer->set_transition_duration(150);
  revealer->set_reveal_child(expanded);

  auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

  // S89: format helpers — synthesise display name and distance string in
  // the active doc unit. Captured by per-row lambdas so a doc-unit change
  // re-renders correctly on the next rebuild without any stored state.
  Unit dunit = m_doc->canvas.display_unit;
  const char *dunit_label = UnitSystem::label(dunit);
  auto fmt_n_disp = [&](double v) -> std::string {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4g", UnitSystem::from_px(v, dunit));
    return buf;
  };
  auto derive_name = [&](const SceneNode *mn) -> std::string {
    return std::string("[") + fmt_n_disp(mn->measure_x1) + ", " +
           fmt_n_disp(mn->measure_y1) + " \u2192 " +
           fmt_n_disp(mn->measure_x2) + ", " +
           fmt_n_disp(mn->measure_y2) + " " + dunit_label + "]";
  };

  for (int j = (int)layer.children.size() - 1; j >= 0; --j) {
    SceneNode *mn = layer.children[j].get();
    if (!mn->is_measurement())
      continue;

    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(4);
    row->set_margin_start(20);
    row->set_margin_end(6);
    row->set_margin_top(2);
    row->set_margin_bottom(2);
    row->add_css_class("path-row");

    // ── Grip (matches add_path_row visual rhythm)
    auto *grip = Gtk::make_managed<Gtk::Label>("\u2833");
    grip->add_css_class("dim-label");
    row->append(*grip);

    // ── Type indicator — ruler icon (was "Measure" text token in v2;
    // user prefers the icon to match the original v1 design). 12px to
    // sit comfortably with the 14×14 dot/eye/grip rhythm of the row.
    auto *type_ico = Gtk::make_managed<Gtk::Image>();
    type_ico->set_from_icon_name("curvz-ruler-symbolic");
    type_ico->set_pixel_size(12);
    type_ico->add_css_class("dim-label");
    row->append(*type_ico);

    // ── Per-entry visibility toggle (eye)
    // Mirrors the layer-row eye pattern: single icon, toggle adds/removes
    // the layer-hidden-btn class to indicate the off state.
    auto *vis_btn = Gtk::make_managed<Gtk::Button>();
    vis_btn->set_has_frame(false);
    vis_btn->set_icon_name("curvz-eye-symbolic");
    vis_btn->set_tooltip_text(mn->visible ? "Hide measurement"
                                          : "Show measurement");
    if (!mn->visible)
      vis_btn->add_css_class("layer-hidden-btn");
    vis_btn->signal_clicked().connect([this, mn]() {
      mn->visible = !mn->visible;
      m_sig_layer_changed.emit();
      Glib::signal_idle().connect_once([this]() { rebuild(); });
    });
    row->append(*vis_btn);

    // ── Derived name — [x,y → x,y unit] in active doc unit
    std::string display_name = derive_name(mn);
    auto *lbl = Gtk::make_managed<Gtk::Label>(display_name);
    lbl->set_xalign(0.0f);
    lbl->set_hexpand(true);
    lbl->add_css_class("prop-lbl");
    lbl->set_ellipsize(Pango::EllipsizeMode::END);
    if (!mn->visible)
      lbl->add_css_class("layer-hidden-text");
    row->append(*lbl);

    // ── Copy button — clipboards the full structured block (matches the
    // canvas single-click and the inspector's copy button). Captured by
    // value so the lambda survives a sibling delete that invalidates `mn`.
    std::string copy_payload = Canvas::format_structured_block_for(
        m_doc, mn->measure_x1, mn->measure_y1,
        mn->measure_x2, mn->measure_y2);
    auto *copy_btn = Gtk::make_managed<Gtk::Button>();
    copy_btn->set_has_frame(false);
    copy_btn->set_icon_name("edit-copy-symbolic");
    copy_btn->set_tooltip_text("Copy measurement data");
    copy_btn->add_css_class("dim-label");
    copy_btn->signal_clicked().connect([this, copy_payload]() {
      auto disp = get_display();
      if (!disp) return;
      auto clip = disp->get_clipboard();
      if (!clip) return;
      Glib::Value<Glib::ustring> val;
      val.init(Glib::Value<Glib::ustring>::value_type());
      val.set(Glib::ustring(copy_payload));
      clip->set_content(Gdk::ContentProvider::create(val));
    });
    row->append(*copy_btn);

    // Delete button
    auto *del_btn = Gtk::make_managed<Gtk::Button>();
    del_btn->set_has_frame(false);
    del_btn->set_icon_name("edit-delete-symbolic");
    del_btn->set_tooltip_text("Delete measurement");
    del_btn->add_css_class("dim-label");
    del_btn->signal_clicked().connect([this, i, j]() {
      if (i < (int)m_doc->layers.size()) {
        auto &children = m_doc->layers[i]->children;
        // j is reversed index (we iterate backwards), but we stored real j
        if (j >= 0 && j < (int)children.size())
          children.erase(children.begin() + j);
      }
      m_sig_layer_changed.emit();
      Glib::signal_idle().connect_once([this]() { rebuild(); });
    });
    row->append(*del_btn);

    inner->append(*row);
  }

  revealer->set_child(*inner);
  card->append(*revealer);

  // Update collapse entry — same pattern as add_layer_row
  m_collapse_entries.erase(
      std::remove_if(m_collapse_entries.begin(), m_collapse_entries.end(),
                     [i](const CollapseEntry &e) {
                       return e.group_node == nullptr && e.layer_idx == i;
                     }),
      m_collapse_entries.end());
  m_collapse_entries.push_back({i, nullptr, revealer, toggle, expanded, skey});

  parent->append(*card);
}

// ── add_grid_layer_row / add_margin_layer_row
// ─────────────────────────────────────────────

void LayersPanel::add_grid_layer_row(int i, Gtk::Box *parent) {
  SceneNode &layer = *m_doc->layers[i];
  auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(40);
  hdr->set_margin_end(29);
  hdr->set_margin_top(2);
  hdr->set_margin_bottom(2);
  hdr->add_css_class("layer-row");

  // Colour dot — shows actual grid color, click to change
  auto *dot = Gtk::make_managed<Gtk::DrawingArea>();
  dot->set_size_request(12, 12);
  dot->set_valign(Gtk::Align::CENTER);
  dot->set_can_target(true);
  dot->set_tooltip_text("Grid color — click to change");
  dot->set_draw_func([this, i](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
    auto *g = m_doc->layers[i].get();
    double r = g->grid_color_r, gv = g->grid_color_g,
           b = g->grid_color_b, a = g->grid_color_a;
    cr->arc(w / 2.0, h / 2.0, w / 2.0 - 0.5, 0, 2 * M_PI);
    cr->set_source_rgba(r, gv, b, a);
    cr->fill_preserve();
    cr->set_source_rgba(1, 1, 1, 0.15);
    cr->set_line_width(0.5);
    cr->stroke();
  });
  auto dot_click = Gtk::GestureClick::create();
  dot_click->set_button(1);
  dot_click->signal_pressed().connect([this, i, dot](int, double, double) {
    auto *g = m_doc->layers[i].get();
    color::Color initial(g->grid_color_r, g->grid_color_g,
                         g->grid_color_b, g->grid_color_a);

    m_color_popover.open(
        *dot, initial, /*with_alpha=*/true,
        [this, i, dot](const color::Color& c) {
          if (i >= (int)m_doc->layers.size()) return;
          auto *g = m_doc->layers[i].get();
          g->grid_color_r = c.r;
          g->grid_color_g = c.g;
          g->grid_color_b = c.b;
          g->grid_color_a = c.a;
          dot->queue_draw();
          m_sig_layer_changed.emit();
        });
  });
  dot->add_controller(dot_click);
  hdr->append(*dot);

  // Visibility
  auto *vis_btn = Gtk::make_managed<Gtk::Button>();
  vis_btn->set_has_frame(false);
  vis_btn->set_icon_name("curvz-eye-symbolic");
  vis_btn->set_tooltip_text(layer.visible ? "Hide grid" : "Show grid");
  if (!layer.visible)
    vis_btn->add_css_class("layer-hidden-btn");
  vis_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->visible;
    m_doc->layers[i]->visible = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/false, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*vis_btn);

  // Name
  auto *name_lbl = Gtk::make_managed<Gtk::Label>("Grid");
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  if (!layer.visible)
    name_lbl->add_css_class("layer-hidden-text");
  hdr->append(*name_lbl);

  // Lock
  auto *lock_btn = Gtk::make_managed<Gtk::Button>();
  lock_btn->set_has_frame(false);
  lock_btn->set_icon_name(layer.locked ? "curvz-locked-symbolic"
                                       : "curvz-unlocked-symbolic");
  lock_btn->set_tooltip_text(layer.locked ? "Unlock grid" : "Lock grid");
  if (layer.locked)
    lock_btn->add_css_class("layer-locked-btn");
  lock_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->locked;
    m_doc->layers[i]->locked = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/true, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*lock_btn);

  card->append(*hdr);
  if (i == m_active_layer)
    card->add_css_class("layer-row-active");

  setup_layer_drag(hdr, i);
  setup_layer_drop(hdr, i);

  // Use TARGET phase so the gesture only fires when clicking the row
  // background — not when clicking child buttons (eye, lock)
  auto click = Gtk::GestureClick::create();
  click->set_propagation_phase(Gtk::PropagationPhase::TARGET);
  click->signal_pressed().connect([this, i](int, double, double) {
    m_active_layer = i;
    m_doc->active_layer_index = i;
    m_sig_layer_selected.emit(i);
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->add_controller(click);
  parent->append(*card);
}

// Margins Layer
// _____________________________________
void LayersPanel::add_margin_layer_row(int i, Gtk::Box *parent) {
  SceneNode &layer = *m_doc->layers[i];
  auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(40);
  hdr->set_margin_end(29);
  hdr->set_margin_top(2);
  hdr->set_margin_bottom(2);
  hdr->add_css_class("layer-row");

  // Colour dot — shows actual margin color, click to change
  auto *dot = Gtk::make_managed<Gtk::DrawingArea>();
  dot->set_size_request(12, 12);
  dot->set_valign(Gtk::Align::CENTER);
  dot->set_can_target(true);
  dot->set_tooltip_text("Margin color — click to change");
  dot->set_draw_func([this, i](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
    auto *m = m_doc->layers[i].get();
    double r = m->margin_color_r, gv = m->margin_color_g,
           b = m->margin_color_b, a = m->margin_color_a;
    cr->arc(w / 2.0, h / 2.0, w / 2.0 - 0.5, 0, 2 * M_PI);
    cr->set_source_rgba(r, gv, b, a);
    cr->fill_preserve();
    cr->set_source_rgba(1, 1, 1, 0.15);
    cr->set_line_width(0.5);
    cr->stroke();
  });
  auto dot_click = Gtk::GestureClick::create();
  dot_click->set_button(1);
  dot_click->signal_pressed().connect([this, i, dot](int, double, double) {
    auto *m = m_doc->layers[i].get();
    color::Color initial(m->margin_color_r, m->margin_color_g,
                         m->margin_color_b, m->margin_color_a);

    m_color_popover.open(
        *dot, initial, /*with_alpha=*/true,
        [this, i, dot](const color::Color& c) {
          if (i >= (int)m_doc->layers.size()) return;
          auto *m = m_doc->layers[i].get();
          m->margin_color_r = c.r;
          m->margin_color_g = c.g;
          m->margin_color_b = c.b;
          m->margin_color_a = c.a;
          dot->queue_draw();
          m_sig_layer_changed.emit();
        });
  });
  dot->add_controller(dot_click);
  hdr->append(*dot);

  // Visibility
  auto *vis_btn = Gtk::make_managed<Gtk::Button>();
  vis_btn->set_has_frame(false);
  vis_btn->set_icon_name("curvz-eye-symbolic");
  vis_btn->set_tooltip_text(layer.visible ? "Hide margins" : "Show margins");
  if (!layer.visible)
    vis_btn->add_css_class("layer-hidden-btn");
  vis_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->visible;
    m_doc->layers[i]->visible = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/false, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*vis_btn);

  // Name
  auto *name_lbl = Gtk::make_managed<Gtk::Label>("Margins");
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  if (!layer.visible)
    name_lbl->add_css_class("layer-hidden-text");
  hdr->append(*name_lbl);

  // Lock
  auto *lock_btn = Gtk::make_managed<Gtk::Button>();
  lock_btn->set_has_frame(false);
  lock_btn->set_icon_name(layer.locked ? "curvz-locked-symbolic"
                                       : "curvz-unlocked-symbolic");
  lock_btn->set_tooltip_text(layer.locked ? "Unlock margins" : "Lock margins");
  if (layer.locked)
    lock_btn->add_css_class("layer-locked-btn");
  lock_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->locked;
    m_doc->layers[i]->locked = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/true, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*lock_btn);

  card->append(*hdr);
  if (i == m_active_layer)
    card->add_css_class("layer-row-active");

  setup_layer_drag(hdr, i);
  setup_layer_drop(hdr, i);

  auto click = Gtk::GestureClick::create();
  click->set_propagation_phase(Gtk::PropagationPhase::TARGET);
  click->signal_pressed().connect([this, i](int, double, double) {
    m_active_layer = i;
    m_doc->active_layer_index = i;
    m_sig_layer_selected.emit(i);
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->add_controller(click);
  parent->append(*card);
}
// ─────────────────────────────────────────────────────────────────── ──
// add_guide_layer_row ───────────────────────────────────────────────────────
// Renders the special Guides layer row: ruler icon, eye, lock, no delete,
// no rename, no color dot. DnD reordering still wired via
// setup_layer_drag/drop.
void LayersPanel::add_guide_layer_row(int i, Gtk::Box *parent) {
  SceneNode &layer = *m_doc->layers[i];

  std::string skey = layer_state_key(layer);
  bool expanded = lookup_expanded(skey, false);
  if (auto *e = find_collapse(i))
    expanded = e->expanded;

  auto *card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(4);
  hdr->set_margin_end(6);
  hdr->set_margin_top(2);
  hdr->set_margin_bottom(2);
  hdr->add_css_class("layer-header");
  hdr->set_focusable(true);

  m_row_entries.push_back({i, -1, nullptr, hdr});

  // ── Arrow
  auto *arrow_btn = Gtk::make_managed<Gtk::Button>();
  arrow_btn->set_has_frame(false);
  arrow_btn->set_label(expanded ? "▾" : "▸");
  arrow_btn->set_size_request(20, -1);
  arrow_btn->add_css_class("part-arrow");
  arrow_btn->signal_clicked().connect([this, i]() { toggle_layer(i); });
  hdr->append(*arrow_btn);

  // ── Guide color dot — clickable, opens color picker for all guides
  auto *color_dot = Gtk::make_managed<Gtk::DrawingArea>();
  color_dot->set_size_request(12, 12);
  color_dot->set_valign(Gtk::Align::CENTER);
  color_dot->set_can_target(true);
  color_dot->set_tooltip_text("Guide color");
  color_dot->set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        cr->arc(w / 2.0, h / 2.0, w / 2.0 - 0.5, 0, 2 * M_PI);
        cr->set_source_rgb(m_doc->guide_color_r, m_doc->guide_color_g,
                           m_doc->guide_color_b);
        cr->fill_preserve();
        cr->set_source_rgba(1, 1, 1, 0.15);
        cr->set_line_width(0.5);
        cr->stroke();
      });
  auto guide_dot_click = Gtk::GestureClick::create();
  guide_dot_click->set_button(1);
  guide_dot_click->signal_pressed().connect([this, color_dot](int, double, double) {
    color::Color initial(m_doc->guide_color_r, m_doc->guide_color_g,
                         m_doc->guide_color_b, 1.0);

    m_color_popover.open(
        *color_dot, initial, /*with_alpha=*/false,
        [this](const color::Color& c) {
          m_doc->guide_color_r = c.r;
          m_doc->guide_color_g = c.g;
          m_doc->guide_color_b = c.b;
          m_sig_layer_changed.emit();
          Glib::signal_idle().connect_once([this]() { rebuild(); });
        });
  });
  color_dot->add_controller(guide_dot_click);
  hdr->append(*color_dot);
  auto *vis_btn = Gtk::make_managed<Gtk::Button>();
  vis_btn->set_has_frame(false);
  vis_btn->set_icon_name("curvz-eye-symbolic");
  vis_btn->set_tooltip_text(layer.visible ? "Hide guides" : "Show guides");
  if (!layer.visible)
    vis_btn->add_css_class("layer-hidden-btn");
  vis_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->visible;
    m_doc->layers[i]->visible = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/false, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*vis_btn);

  // ── Name (fixed, not clickable for rename, but clickable to activate
  //    the guide layer — without this the active layer can never be the
  //    guide layer, and the canvas's guide hover hit-test
  //    (Canvas.cpp:on_motion, gated on `guide_idx <= active_layer_index`)
  //    only fires when guide layer is at-or-above the active drawing
  //    layer's z-order. Activating the guide layer satisfies that gate
  //    regardless of stack order.)
  auto *name_lbl = Gtk::make_managed<Gtk::Label>("Guides");
  name_lbl->set_xalign(0.0f);
  name_lbl->set_hexpand(true);
  name_lbl->add_css_class("prop-lbl");
  if (!layer.visible)
    name_lbl->add_css_class("layer-hidden-text");
  auto name_click = Gtk::GestureClick::create();
  name_click->set_button(1);
  name_click->signal_pressed().connect(
      [this, i](int, double, double) {
        if (m_rebuilding)
          return;
        m_active_layer = i;
        m_doc->active_layer_index = i;
        m_sig_layer_selected.emit(i);
        refresh_highlights();
      });
  name_lbl->add_controller(name_click);
  hdr->append(*name_lbl);

  // ── Lock
  auto *lock_btn = Gtk::make_managed<Gtk::Button>();
  lock_btn->set_has_frame(false);
  // lock_btn->set_icon_name(layer.locked ? "changes-prevent-symbolic"
  //                                      : "changes-allow-symbolic");
  lock_btn->set_icon_name(layer.locked ? "curvz-locked-symbolic"
                                       : "curvz-unlocked-symbolic");
  lock_btn->set_tooltip_text(layer.locked ? "Unlock guides" : "Lock guides");
  if (layer.locked)
    lock_btn->add_css_class("layer-locked-btn");
  lock_btn->signal_clicked().connect([this, i]() {
    if (i < 0 || i >= (int)m_doc->layers.size()) return;
    bool before = m_doc->layers[i]->locked;
    m_doc->layers[i]->locked = !before;
    push_edit_layer_bool_field(i, /*is_locked=*/true, before, !before);
    m_sig_layer_changed.emit();
    Glib::signal_idle().connect_once([this]() { rebuild(); });
  });
  hdr->append(*lock_btn);

  // ── Guide count
  char cnt[12];
  snprintf(cnt, sizeof(cnt), "%d", (int)layer.children.size());
  auto *count_lbl = Gtk::make_managed<Gtk::Label>(cnt);
  count_lbl->set_size_request(20, -1);
  count_lbl->set_xalign(1.0f);
  count_lbl->add_css_class("dim-label");
  count_lbl->set_tooltip_text("Guides");
  hdr->append(*count_lbl);

  card->append(*hdr);

  setup_layer_drag(hdr, i);
  setup_layer_drop(hdr, i);

  // ── Revealer — guide children listed as H/V position labels
  auto *revealer = Gtk::make_managed<Gtk::Revealer>();
  revealer->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);
  revealer->set_transition_duration(150);
  revealer->set_reveal_child(expanded);

  auto *inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  bool gl_locked = layer.locked;
  for (int j = (int)layer.children.size() - 1; j >= 0; --j) {
    SceneNode *g = layer.children[j].get();
    if (!g->is_guide())
      continue;

    bool is_sel = std::find(m_guide_selection.begin(), m_guide_selection.end(),
                            g) != m_guide_selection.end();

    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(4);
    row->set_margin_start(20);
    row->set_margin_end(6);
    row->set_margin_top(1);
    row->set_margin_bottom(1);
    row->set_focusable(!gl_locked);
    if (is_sel)
      row->add_css_class("guide-row-selected");

    const char *type_label = g->guide_is_horizontal() ? "H"
                           : g->guide_is_vertical()   ? "V"
                                                      : "A";
    auto *orient_lbl = Gtk::make_managed<Gtk::Label>(type_label);
    orient_lbl->add_css_class("dim-label");
    orient_lbl->set_size_request(14, -1);
    row->append(*orient_lbl);

    char pos[48];
    if (g->guide_is_horizontal()) {
      snprintf(pos, sizeof(pos), "%.6f", g->guide_y);
    } else if (g->guide_is_vertical()) {
      snprintf(pos, sizeof(pos), "%.6f", g->guide_x);
    } else {
      snprintf(pos, sizeof(pos), "%.2f,%.2f @%.1f°",
               g->guide_x, g->guide_y, g->guide_angle);
    }
    auto *pos_lbl = Gtk::make_managed<Gtk::Label>(pos);
    pos_lbl->set_xalign(0.0f);
    pos_lbl->set_hexpand(true);
    pos_lbl->add_css_class("prop-lbl");
    row->append(*pos_lbl);

    if (!gl_locked) {
      auto click = Gtk::GestureClick::create();
      click->set_button(1);
      // Capture RefPtr by value so the gesture stays alive for the lambda
      // lifetime
      click->signal_pressed().connect([this, g, click = click](int, double,
                                                               double) mutable {
        LOG_DEBUG("LayersPanel: guide row clicked g={} anchor=({:.4f},{:.4f}) "
                  "angle={:.2f}",
                  (void *)g, g->guide_x, g->guide_y, g->guide_angle);
        Gdk::ModifierType mod = click->get_current_event_state();
        bool ctrl =
            (mod & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
        LOG_DEBUG("LayersPanel: guide click ctrl={}", ctrl);
        if (ctrl) {
          auto it =
              std::find(m_guide_selection.begin(), m_guide_selection.end(), g);
          if (it != m_guide_selection.end())
            m_guide_selection.erase(it);
          else
            m_guide_selection.push_back(g);
        } else {
          m_guide_selection = {g};
        }
        LOG_DEBUG("LayersPanel: emitting guide_selection_changed size={}",
                  m_guide_selection.size());
        m_sig_guide_selection.emit(m_guide_selection);
        LOG_DEBUG("LayersPanel: guide_selection_changed emitted");
        // Schedule our own rebuild to show highlight — double-idle so
        // GTK fully unwinds the gesture before we tear down the widget tree.
        Glib::signal_idle().connect_once([this]() {
          Glib::signal_idle().connect_once([this]() { rebuild(); });
        });
      });
      row->add_controller(click);
    }

    inner->append(*row);
  }
  revealer->set_child(*inner);
  card->append(*revealer);
  parent->append(*card);

  m_collapse_entries.erase(
      std::remove_if(m_collapse_entries.begin(), m_collapse_entries.end(),
                     [i](const CollapseEntry &e) {
                       return e.group_node == nullptr && e.layer_idx == i;
                     }),
      m_collapse_entries.end());
  m_collapse_entries.push_back({i, nullptr, revealer, arrow_btn, expanded, skey});
}

// ── Macro quick-run section ───────────────────────────────────────────────
// Collapsible section at the top of the Layers panel. Lists macros whose
// `in_layer_panel` flag is set (★ toggle in MacroManagerWindow).
// Click the row or the ▶ button to run on the current canvas selection.
// When no macros are starred the section is hidden entirely.
void LayersPanel::add_macro_section(Gtk::Box *parent) {
  auto macros = MacroManager::instance().layer_panel_macros();
  if (macros.empty())
    return; // Nothing starred → no section at all

  // S89: route macros expansion state through m_collapse_state with a
  // singleton key. Same persistence semantics as every layer/group row —
  // toggle survives in-session rebuilds, default collapsed on first
  // appearance / restart. m_macros_expanded is kept in sync for the
  // toggle handler.
  static const std::string macros_skey = "MACROS:";
  m_macros_expanded = lookup_expanded(macros_skey, false);

  // ── Section header ────────────────────────────────────────────────────
  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->set_spacing(4);
  hdr->set_margin_start(4);
  hdr->set_margin_end(6);
  hdr->set_margin_top(4);
  hdr->set_margin_bottom(2);
  hdr->add_css_class("layer-header");

  auto *arrow_btn = Gtk::make_managed<Gtk::Button>();
  arrow_btn->set_has_frame(false);
  arrow_btn->set_label(m_macros_expanded ? "▾" : "▸");
  arrow_btn->set_size_request(20, -1);
  arrow_btn->add_css_class("part-arrow");
  hdr->append(*arrow_btn);

  auto *title_lbl = Gtk::make_managed<Gtk::Label>("Macros");
  title_lbl->set_xalign(0.0f);
  title_lbl->set_hexpand(true);
  title_lbl->add_css_class("prop-lbl");
  hdr->append(*title_lbl);

  auto *count_lbl = Gtk::make_managed<Gtk::Label>(
      std::to_string(macros.size()));
  count_lbl->set_size_request(20, -1);
  count_lbl->set_xalign(1.0f);
  count_lbl->add_css_class("dim-label");
  count_lbl->set_margin_end(4);
  hdr->append(*count_lbl);

  parent->append(*hdr);

  // ── Collapsible body ──────────────────────────────────────────────────
  auto *rev = Gtk::make_managed<Gtk::Revealer>();
  rev->set_reveal_child(m_macros_expanded);
  rev->set_transition_type(Gtk::RevealerTransitionType::SLIDE_DOWN);

  auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  body->set_spacing(0);

  arrow_btn->signal_clicked().connect([this, rev, arrow_btn]() {
    m_macros_expanded = !m_macros_expanded;
    rev->set_reveal_child(m_macros_expanded);
    arrow_btn->set_label(m_macros_expanded ? "▾" : "▸");
    // Persist in-session — survives panel rebuilds via m_collapse_state.
    write_expanded("MACROS:", m_macros_expanded);
  });

  // ── One row per starred macro ─────────────────────────────────────────
  for (Macro *mac : macros) {
    if (!mac)
      continue;
    auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row->set_spacing(4);
    row->set_margin_start(20);
    row->set_margin_end(6);
    row->set_margin_top(2);
    row->set_margin_bottom(2);
    row->add_css_class("path-row");

    // Star glyph — static indicator (toggle lives in the Manager).
    // Uses dim-label to sit lightly, matching the grip/type glyphs on
    // regular path rows.
    auto *star_lbl = Gtk::make_managed<Gtk::Label>("★");
    star_lbl->add_css_class("dim-label");
    row->append(*star_lbl);

    // Type label — matches the "Rect", "Text", etc. slot on path rows
    auto *type_lbl = Gtk::make_managed<Gtk::Label>("Macro");
    type_lbl->add_css_class("dim-label");
    type_lbl->set_size_request(36, -1);
    type_lbl->set_xalign(0.0f);
    row->append(*type_lbl);

    // Name — ellipsised, uses prop-lbl like path rows
    auto *nlbl = Gtk::make_managed<Gtk::Label>(mac->name);
    nlbl->set_xalign(0.0f);
    nlbl->set_hexpand(true);
    nlbl->add_css_class("prop-lbl");
    nlbl->set_ellipsize(Pango::EllipsizeMode::END);
    nlbl->set_tooltip_text(mac->name + "  (" +
                           std::to_string(mac->steps.size()) + " steps)");
    row->append(*nlbl);

    // Step-count badge
    auto *badge = Gtk::make_managed<Gtk::Label>(
        std::to_string(mac->steps.size()));
    badge->add_css_class("dim-label");
    badge->set_margin_end(4);
    row->append(*badge);

    // ▶ run button — CAPTURE-phase gesture so the row-level click
    // gesture never sees the press (mirrors the pattern used in
    // MacroManagerWindow for its star/run/edit buttons).
    auto *run_btn = Gtk::make_managed<Gtk::Button>("▶");
    run_btn->add_css_class("flat");
    run_btn->set_has_frame(false);
    run_btn->set_tooltip_text("Run macro on current selection");
    const std::string mid = mac->internal_id;
    {
      auto g = Gtk::GestureClick::create();
      g->set_button(1);
      g->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
      g->signal_pressed().connect(
          [this, mid, g](int, double, double) {
            g->set_state(Gtk::EventSequenceState::CLAIMED);
            m_sig_run_macro.emit(mid);
          });
      run_btn->add_controller(g);
    }
    row->append(*run_btn);

    // Row click (anywhere outside the ▶ button) also runs
    auto row_gesture = Gtk::GestureClick::create();
    row_gesture->set_button(1);
    row_gesture->signal_pressed().connect(
        [this, mid](int, double, double) {
          m_sig_run_macro.emit(mid);
        });
    row->add_controller(row_gesture);

    body->append(*row);
  }

  rev->set_child(*body);
  parent->append(*rev);
}

// ── Rebuild
// ────────────────────────────────────────────────────────────────
void LayersPanel::rebuild() {
  m_rebuilding = true;
  m_row_entries.clear();

  while (auto *child = m_content.get_first_child())
    m_content.remove(*child);

  if (!m_doc) {
    m_rebuilding = false;
    return;
  }

  // Macro quick-run section at the top
  add_macro_section(&m_content);

  int n = (int)m_doc->layers.size();
  for (int i = n - 1; i >= 0; --i) {
    if (m_doc->layers[i]->is_guide_layer())
      add_guide_layer_row(i, &m_content);
    else if (m_doc->layers[i]->is_ref_layer())
      add_ref_layer_row(i, &m_content);
    else if (m_doc->layers[i]->is_measure_layer())
      add_measure_layer_row(i, &m_content);
    else if (m_doc->layers[i]->is_grid_layer())
      add_grid_layer_row(i, &m_content);
    else if (m_doc->layers[i]->is_margin_layer())
      add_margin_layer_row(i, &m_content);
    else
      add_layer_row(i, &m_content);
  }

  m_rebuilding = false;
}

// ── Add / Delete
// ──────────────────────────────────────────────────────────────
void LayersPanel::on_add_layer() {
  if (!m_doc)
    return;
  auto layer = std::make_unique<SceneNode>();
  layer->type = SceneNode::Type::Layer;
  // S95 m1 — names are uniquified through the document funnel rather
  // than computed from layer count. Old approach: "Layer N+1" where
  // N = layer count; collided whenever the user had deleted-then-added
  // (count goes back down, name gets reused). New approach asks the
  // document for the lowest free "Layer N", guaranteed unique.
  // S96 m3 — also mint an internal_id so the SVG-id codec produces a
  // collision-free derived id for this layer at write time.
  layer->internal_id = generate_internal_id();
  layer->name = m_doc->next_default_name(CurvzDocument::NameKind::Layer);
  // Auto-assign a colour from the palette (skip index 0 = empty)
  int normal_count = 0;
  for (auto &l : m_doc->layers)
    if (l->is_layer())
      ++normal_count;
  layer->color = palette((normal_count % (PALETTE_SIZE - 1)) + 1);
  // Insert at front but after any guide layer at position 0
  int insert_pos = 0;
  // Find first non-special position from the front
  for (int i = 0; i < (int)m_doc->layers.size(); ++i) {
    if (!m_doc->layers[i]->is_guide_layer() &&
        !m_doc->layers[i]->is_ref_layer() &&
        !m_doc->layers[i]->is_measure_layer() &&
        !m_doc->layers[i]->is_grid_layer() &&
        !m_doc->layers[i]->is_margin_layer()) {
      insert_pos = i;
      break;
    }
  }

  // s171 m1 — push AddLayerCommand if history+project are wired.
  // Pattern (per StylesPanel/Canvas convention): mutate live state
  // first, THEN push the command (which records before+after for undo
  // and redo). push() does NOT call execute() — it only appends to
  // the undo stack. Initial state-application is the direct mutation
  // below.
  //
  // Falls through to plain direct mutation if either history or
  // project is missing (test harness / partial init), preserving old
  // behaviour as a safety net.
  int active_before = m_doc->active_layer_index;
  int active_after  = insert_pos;
  std::string new_iid = layer->internal_id;

  // Pick the anchor BEFORE the mutation, so it's an iid that already
  // existed in the doc and survives this op (and any future undo).
  std::string anchor_iid;
  if (m_history && m_project) {
    for (auto& l : m_doc->layers) {
      if (l && !l->internal_id.empty()) {
        anchor_iid = l->internal_id;
        break;
      }
    }
    if (anchor_iid.empty()) {
      // Degenerate doc with no anchorable layer — log and proceed
      // with direct mutation only (no command pushed).
      LOG_WARN("[IIDDIAG] AddLayer: no anchor layer found, "
               "skipping command push");
    }
  }

  // Take a snapshot of the layer (deep clone) BEFORE moving it into
  // the doc, so the command can replay-insert on redo without holding
  // a reference to the live layer (which would dangle if user later
  // deletes it).
  std::unique_ptr<SceneNode> snap_for_cmd;
  if (m_history && m_project && !anchor_iid.empty()) {
    snap_for_cmd = clone_node(*layer);
  }

  // Direct mutation — same as pre-s171 behaviour.
  m_doc->layers.insert(m_doc->layers.begin() + insert_pos, std::move(layer));
  m_doc->invalidate_iid_index();
  m_active_layer = insert_pos;
  m_doc->active_layer_index = insert_pos;

  // Now record the command for undo/redo.
  if (snap_for_cmd) {
    auto cmd = std::make_unique<AddLayerCommand>(
        m_project, anchor_iid, std::move(snap_for_cmd),
        insert_pos, active_before, active_after);
    m_history->push(std::move(cmd));
  }

  m_sig_layer_selected.emit(insert_pos);
  m_sig_layer_changed.emit();
  Glib::signal_idle().connect_once([this]() { rebuild(); });
}

void LayersPanel::on_delete_layer() {
  if (!m_doc)
    return;
  // Never delete special layers
  if (m_active_layer >= 0 && m_active_layer < (int)m_doc->layers.size() &&
      (m_doc->layers[m_active_layer]->is_guide_layer() ||
       m_doc->layers[m_active_layer]->is_ref_layer() ||
       m_doc->layers[m_active_layer]->is_measure_layer() ||
       m_doc->layers[m_active_layer]->is_grid_layer() ||
       m_doc->layers[m_active_layer]->is_margin_layer()))
    return;
  // Need at least one normal layer remaining
  int normal_count = 0;
  for (auto &l : m_doc->layers)
    if (l->is_layer())
      ++normal_count;
  if (normal_count <= 1)
    return;

  // s171 m1 — capture pre-op state for the command, then mutate the
  // doc directly (matching pre-s171 behaviour), then push the command.
  // push() does NOT call execute(); the direct mutation below IS the
  // initial application. The command records before+after for undo/redo.
  int del_idx = m_active_layer;
  int active_before = m_doc->active_layer_index;

  // Pick anchor BEFORE the erase. Must NOT be the layer about to be
  // deleted (that iid won't resolve post-op). The doc always has at
  // least the special layers, so an anchor always exists.
  std::string anchor_iid;
  if (m_history && m_project) {
    for (int i = 0; i < (int)m_doc->layers.size(); ++i) {
      if (i == del_idx) continue;
      auto& l = m_doc->layers[i];
      if (l && !l->internal_id.empty()) {
        anchor_iid = l->internal_id;
        break;
      }
    }
    if (anchor_iid.empty()) {
      LOG_WARN("[IIDDIAG] DeleteLayer: no anchor layer found, "
               "skipping command push");
    }
  }

  // Deep clone the layer (with all children) before erase so undo
  // can restore everything — every child iid preserved. This is what
  // makes the s170 crash go away by construction: child commands queued
  // before the delete will resolve their iids correctly when undo walks
  // back through the delete-undo first.
  std::unique_ptr<SceneNode> snap_for_cmd;
  if (m_history && m_project && !anchor_iid.empty()) {
    snap_for_cmd = clone_node(*m_doc->layers[del_idx]);
  }

  // s171 m2 — scrub raw-pointer captures from undo/redo stacks before
  // the live nodes are destroyed. The pre-s171 commands on Stage 1 (e.g.
  // AddNodeCommand) hold raw `SceneNode* parent`, with a `references()`
  // override that returns true when their parent matches `target`.
  // Without this scrub, Ctrl+Z after restoring this layer would walk
  // back to those commands and dereference freed memory (the layer
  // SceneNode at this address is destroyed by the erase below; the
  // restored layer is a clone at a different address). The scrub drops
  // those commands from the stacks, so undo walks past them cleanly.
  // Recurses into children so any descendant container that has raw-
  // pointer captures (groups holding their own contents) also scrubs.
  // No-op for iid-based commands (default `references() == false`).
  // STRIP: do not strip. This is structural belt-and-braces until every
  // pre-s170 raw-pointer command is migrated to iid capture.
  if (m_history && m_active_layer >= 0
      && m_active_layer < (int)m_doc->layers.size()) {
    SceneNode* layer_to_delete = m_doc->layers[m_active_layer].get();
    std::function<void(SceneNode*)> scrub_walk = [&](SceneNode* n) {
      if (!n) return;
      m_history->scrub_command_history(n);
      for (auto& c : n->children) scrub_walk(c.get());
      if (n->clip_shape)     scrub_walk(n->clip_shape.get());
      if (n->blend_source_a) scrub_walk(n->blend_source_a.get());
      if (n->blend_source_b) scrub_walk(n->blend_source_b.get());
      if (n->warp_source)    scrub_walk(n->warp_source.get());
    };
    scrub_walk(layer_to_delete);
  }

  // Direct mutation — same as pre-s171 behaviour.
  m_doc->layers.erase(m_doc->layers.begin() + m_active_layer);
  m_doc->invalidate_iid_index();
  m_active_layer = std::min(m_active_layer, (int)m_doc->layers.size() - 1);
  // Ensure active layer is a normal layer
  while (m_active_layer >= 0 &&
         (m_doc->layers[m_active_layer]->is_guide_layer() ||
          m_doc->layers[m_active_layer]->is_ref_layer() ||
          m_doc->layers[m_active_layer]->is_measure_layer() ||
          m_doc->layers[m_active_layer]->is_grid_layer() ||
          m_doc->layers[m_active_layer]->is_margin_layer()))
    --m_active_layer;
  if (m_active_layer < 0)
    m_active_layer = 0;
  m_doc->active_layer_index = m_active_layer;
  int active_after = m_active_layer;

  // Now record the command.
  if (snap_for_cmd) {
    auto cmd = std::make_unique<DeleteLayerCommand>(
        m_project, anchor_iid, std::move(snap_for_cmd),
        del_idx, active_before, active_after);
    m_history->push(std::move(cmd));
  }

  m_sig_layer_selected.emit(m_active_layer);
  m_sig_layer_changed.emit();
  Glib::signal_idle().connect_once([this]() { rebuild(); });
}

} // namespace Curvz
