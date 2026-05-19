#include "MainWindow.hpp"
#include "AppPreferences.hpp" // s139 m2 / s143 m1 — boolean-cleanup quality pref + sync
#include "ContextBar.hpp"
#include "CoordSpace.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "DocTabBar.hpp"
#include "RecentProjects.hpp" // s144 m3 — Open Recent submenu
#include "Ruler.hpp"
#include "SvgOptimiser.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include "TemplateLibrary.hpp"
#include <functional>
#include <giomm/simpleactiongroup.h> // s144 m3 — recents action group
#include <gtkmm/application.h>
#include <gtkmm/separator.h>
// s147 m3: ThemesDialog include removed — surface is now ThemesPanel
// (already pulled in via MainWindow.hpp). The dialog source files
// remain in the tree until Scott deletes them on his end; CMake no
// longer references them in this milestone.
#include "UnitSystem.hpp"
#include "curvz_utils.hpp" // s117 m18 v2: apply_motif_class_from_parent
#include "style/StyleInterop.hpp" // mutate_appearance — inspector-driven appearance edits
#include <algorithm>
// s202 m6 — quick-jump float construction
#include "widgets/Button.hpp"  // s214 m2 — substrate Button
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>
#include <cctype> // s136 m4: std::isspace for library item name trim
#include <filesystem>
#include <fstream>
#include <giomm/file.h> // s125 m1a: Gio::File::create_for_path (folder picker)
#include <giomm/liststore.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/image.h>      // s117 m19: custom About dialog hero logo
#include <gtkmm/linkbutton.h> // s136 m1: About dialog outbound links
#include <gtkmm/settings.h>
#include <gtkmm/stack.h> // s117 m19: custom About dialog flip animation
#include <gtkmm/stylecontext.h>
#include <nlohmann/json.hpp>

// GDK key definitions
#include <gdk/gdkkeysyms.h>

namespace Curvz {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────
// MainWindow_helpers.cpp — reusable pieces called from bindings, handlers, or other helpers. Display sync, action enable predicates, flow orchestrators, persistence, defensive utilities, the inspector make_section/make_group_section pumps, the SVG import workhorses (import_svg_impl, import_svg_as_doc), and the Library write workhorse (write_library_item).
// ─────────────────────────────────────────────────────────────────────

// ── Debounced save ───────────────────────────────────────────────────────────
// Coalesces rapid changes (spinbox scroll, pane drag) into one write
// by resetting a 400ms timer on every call.
void MainWindow::schedule_save() {
  if (!m_project || m_project->directory.empty())
    return;
  m_save_timer.disconnect();
  m_save_timer = Glib::signal_timeout().connect(
      [this]() -> bool {
        if (m_project && !m_project->directory.empty()) {
          // Defer if a drag is in-flight — retry in 100ms
          if (m_canvas.is_dragging()) {
            schedule_save();
            return false;
          }
          m_project->save();
        }
        return false; // one-shot
      },
      400);
}


// s113 m2: gated outline toggle.
//
// In preview/normal rendering mode, drop-shadow rendering allocates a
// Cairo buffer sized to the doc bbox in *device pixels* (doc_size ×
// zoom). At extreme zoom the allocation is unbounded and crashes the
// app. The hazard is concentrated on the outline→preview transition —
// a cliff, not a ramp — because zooming up *within* preview is gradual
// and self-warning (lag builds visibly), but flipping to preview while
// already deep-zoomed in outline mode is an instant cliff.
//
// This helper:
//   • Always allows preview→outline (the safe direction).
//   • Always allows outline→preview when zoom is below the threshold.
//   • Refuses outline→preview when zoom is above the threshold and shows
//     an AlertDialog telling the user to zoom out first.
//
// Returns true if the toggle happened, false if it was refused. Caller
// is responsible for syncing action state and statusbar after a true.
bool MainWindow::try_toggle_outline_safely() {
  if (!m_canvas.preview_safe_at_current_zoom()) {
    curvz::utils::show_alert(
        *this, "Zoom too high for preview mode",
        "Switching to preview mode at this zoom level may crash the app "
        "because of how preview rendering allocates memory at high zoom. "
        "\n\n"
        "Zoom out first, then switch to preview. Outline view stays safe "
        "at any zoom.");
    return false;
  }
  m_canvas.toggle_outline_mode();
  return true;
}


void MainWindow::toggle_rulers(bool visible) {
  m_hruler.set_visible(visible);
  m_vruler.set_visible(visible);
  m_corner.set_visible(visible);
  LOG_DEBUG("Rulers {}", visible ? "shown" : "hidden");
}


// s144 m3 — Open Recent submenu rebuild.
//
// Called from setup_menu (initial population) and from
// RecentProjects::signal_changed (every list churn — add, remove, clear).
// Same Gio::Menu instance is recycled — remove_all() then re-append.
//
// Display name policy: strip trailing ".curvz" from the folder basename
// because every project is a .curvz folder and the suffix is noise.
// /home/scott/icons/Folio.curvz → "Folio".
//
// Disambiguation: when two entries have identical display names but
// different parent paths (e.g. ~/work/Folio.curvz and ~/personal/Folio.curvz),
// append a parenthesised parent path to both so the user can tell them
// apart. Cheap O(N²) check; N ≤ 50 by max-count clamp.
//
// Clear Recent Projects sits in its own Gio::Menu section so the
// separator renders without manual styling. The action is greyed via
// recents.clear's enabled state — toggled here based on list emptiness.
void MainWindow::rebuild_recents_menu() {
  if (!m_recents_menu)
    return;

  m_recents_menu->remove_all();

  const auto &paths = RecentProjects::instance().paths();

  // Build display names with disambiguation.
  std::vector<std::string> names(paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    std::string stem = fs::path(paths[i]).filename().string();
    // Strip ".curvz" suffix if present.
    const std::string ext = ".curvz";
    if (stem.size() > ext.size() &&
        stem.compare(stem.size() - ext.size(), ext.size(), ext) == 0)
      stem.resize(stem.size() - ext.size());
    names[i] = stem;
  }
  // Disambiguate collisions by appending the parent dir.
  for (size_t i = 0; i < names.size(); ++i) {
    for (size_t j = i + 1; j < names.size(); ++j) {
      if (names[i] == names[j]) {
        // Only annotate the ones that collide; leave others bare.
        std::string p_i = fs::path(paths[i]).parent_path().filename().string();
        std::string p_j = fs::path(paths[j]).parent_path().filename().string();
        if (!p_i.empty())
          names[i] += "  (" + p_i + ")";
        if (!p_j.empty())
          names[j] += "  (" + p_j + ")";
      }
    }
  }

  // Append items. Each item carries the path as a string Variant target so
  // recents.open(<path>) receives it directly via the action handler.
  for (size_t i = 0; i < paths.size(); ++i) {
    auto item = Gio::MenuItem::create(names[i], "");
    item->set_action_and_target("recents.open",
                                Glib::Variant<Glib::ustring>::create(paths[i]));
    m_recents_menu->append_item(item);
  }

  // Clear Recent Projects — only meaningful when there's something to clear.
  // append_section with an empty heading renders the separator.
  auto clear_section = Gio::Menu::create();
  clear_section->append("Clear Recent Projects", "recents.clear");
  m_recents_menu->append_section("", clear_section);

  // Toggle the clear action's enabled state to grey out the row when empty.
  if (m_recents_clear_action)
    m_recents_clear_action->set_enabled(!paths.empty());
}


Gtk::Box *MainWindow::make_section(const char *title, Gtk::Widget &child,
                                   bool expanded, bool vexpand_child,
                                   std::shared_ptr<bool> *out_flag) {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->add_css_class("inspector-section");

  // ── Slim header row — a plain Box with GestureClick, not a Button ──────
  // This removes the full-width "pressable button" visual and replaces it
  // with a tight label row that has a small arrow on the left.
  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->add_css_class("inspector-section-header");
  hdr->set_spacing(5);

  auto *arrow = Gtk::make_managed<Gtk::Label>(expanded ? "▾" : "▸");
  arrow->add_css_class("inspector-arrow");

  auto *lbl = Gtk::make_managed<Gtk::Label>(title);
  lbl->set_xalign(0.0f);
  lbl->set_hexpand(true);
  lbl->add_css_class("inspector-section-title");

  hdr->append(*arrow);
  hdr->append(*lbl);
  outer->append(*hdr);

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  outer->append(*sep);

  auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  body->set_visible(expanded);
  if (vexpand_child)
    body->set_vexpand(true);
  body->append(child);
  outer->append(*body);

  // Track open state so we can save it
  // shared_ptr bool — owned by the closure, no raw new/delete needed
  auto open_flag = std::make_shared<bool>(expanded);

  // s141: factor out the "apply this on/off state to the widgets and to
  // the project field" logic into one closure. The click handler calls
  // it with schedule_save=true; load_project calls it via m_sec_apply
  // with schedule_save=false (we just loaded, no need to save right
  // back). Keeping this in one place fixes a stale-state bug where
  // load_project's sync_flag only flipped the in-memory bool, leaving
  // the widget visibility from whatever setup_layout built it as.
  std::string sec_title = title;
  auto apply_state = [this, arrow, body, sec_title, open_flag](bool on,
                                                               bool save) {
    *open_flag = on;
    body->set_visible(on);
    arrow->set_text(on ? "▾" : "▸");
    if (!m_project)
      return;
    if (sec_title == "Preview")
      m_project->sec_preview_open = on;
    else if (sec_title == "Layers")
      m_project->sec_layers_open = on;
    else if (sec_title == "Library")
      m_project->sec_library_open = on;
    else if (sec_title == "Swatches")
      m_project->sec_swatches_open = on;
    else if (sec_title == "Styles")
      m_project->sec_styles_open = on;
    else if (sec_title == "Themes")
      m_project->sec_themes_open = on;
    else if (sec_title == "Documents")
      m_project->sec_documents_open = on;
    if (save)
      schedule_save();
  };

  // Register a load-time setter so sync_flag in load_project can drive
  // the widgets to match the saved state. The setter writes back to the
  // project field but does NOT schedule_save — we just loaded, the
  // value is already on disk.
  m_sec_apply[sec_title] = [apply_state](bool on) { apply_state(on, false); };

  // Click anywhere on the header row to toggle
  auto gesture = Gtk::GestureClick::create();
  gesture->signal_pressed().connect(
      [apply_state, open_flag](int, double, double) {
        apply_state(!(*open_flag), /*save=*/true);
      });
  hdr->add_controller(gesture);

  // Return the open_flag so the caller can drive open/close state
  if (out_flag)
    *out_flag = open_flag;
  return outer;
}


// Build a collapsible group header (Document/Object/Content style).
// No inline child widget — instead returns a container box for the
// caller to populate with make_section(...) children.  The container
// carries the inspector-group-container CSS class so its descendants
// are indented to indicate hierarchy.
MainWindow::GroupSection
MainWindow::make_group_section(const char *title, bool expanded,
                               std::shared_ptr<bool> *out_flag) {
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->add_css_class("inspector-section");

  auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  hdr->add_css_class("inspector-section-header");
  hdr->set_spacing(5);

  auto *arrow = Gtk::make_managed<Gtk::Label>(expanded ? "▾" : "▸");
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

  // Container — children go here; carries group-container class for indent.
  auto *container = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  container->add_css_class("inspector-group-container");
  container->set_visible(expanded);
  outer->append(*container);

  auto open_flag = std::make_shared<bool>(expanded);

  // s141: same apply_state factoring as make_section. Group toggles only
  // know about Content today; if more groups land later, add their
  // sec_*_open writes here.
  std::string sec_title = title;
  auto apply_state = [this, arrow, container, sec_title, open_flag](bool on,
                                                                    bool save) {
    *open_flag = on;
    container->set_visible(on);
    arrow->set_text(on ? "▾" : "▸");
    if (!m_project)
      return;
    if (sec_title == "Content")
      m_project->sec_content_open = on;
    if (save)
      schedule_save();
  };

  m_sec_apply[sec_title] = [apply_state](bool on) { apply_state(on, false); };

  auto gesture = Gtk::GestureClick::create();
  gesture->signal_pressed().connect(
      [apply_state, open_flag](int, double, double) {
        apply_state(!(*open_flag), /*save=*/true);
      });
  hdr->add_controller(gesture);

  if (out_flag)
    *out_flag = open_flag;
  return {outer, container};
}


void MainWindow::update_rulers() {
  auto *doc = m_project ? m_project->active_doc() : nullptr;

  RulerState rs;
  rs.zoom = m_canvas.zoom();
  rs.pan_x = m_canvas.pan_x();
  rs.pan_y = m_canvas.pan_y();
  rs.widget_w = (double)m_canvas.get_width();
  rs.widget_h = (double)m_canvas.get_height();
  rs.cursor_doc_x = m_canvas.cursor_doc_x();
  rs.cursor_doc_y = m_canvas.cursor_doc_y();

  if (doc) {
    rs.canvas_w = (double)doc->canvas_width();
    rs.canvas_h = (double)doc->canvas_height();
    rs.quality = doc->canvas.quality;
    rs.display_mode = doc->canvas.display_mode;
    rs.ruler_origin_x = doc->ruler_origin_x;
    rs.ruler_origin_y = doc->ruler_origin_y;
    rs.phys_short = std::min(doc->canvas.phys_width, doc->canvas.phys_height);
    rs.phys_unit = doc->canvas.phys_unit;
    rs.display_unit = doc->canvas.display_unit;
    // s265 m2: forward intent so the rulers report in user-units.
    rs.intended_w = doc->canvas.intended_w;
    rs.intended_h = doc->canvas.intended_h;
    rs.intended_unit = doc->canvas.intended_unit;
  }

  m_hruler.set_state(rs);
  m_vruler.set_state(rs);
  m_corner.set_state(rs);
}


void MainWindow::update_clip_actions_sensitive() {
  // Clip: any non-empty canvas selection is a valid starting point; the
  //   stricter precondition (no ref, no guide, etc.) is inside
  //   Canvas::make_clip_group — if violated, the action no-ops and
  //   LOG_INFOs. Enabling the menu item just means "there is something
  //   to clip".
  // Release Clip: primary selection must be a ClipGroup.
  bool have_sel = !m_canvas.selection().empty();
  SceneNode *primary = m_canvas.selected_object();
  bool is_cg = (primary && primary->is_clip_group());
  if (m_act_clip_make)
    m_act_clip_make->set_enabled(have_sel);
  if (m_act_clip_release)
    m_act_clip_release->set_enabled(is_cg);
}


void MainWindow::update_blend_action_sensitive() {
  // Blend: exactly 2 selected, both Paths. Deeper preconditions (same
  // parent, equal node counts, equal closed flag) are validated inside
  // Canvas::make_blend with a user-visible error message — not surfaced
  // as greyed state here because the user needs feedback about what's
  // wrong, not just a silent disabled menu.
  // Release Blend: primary selection is a Blend.
  const auto &sel = m_canvas.selection();
  bool make_ok = (sel.size() == 2) && sel[0] &&
                 sel[0]->type == SceneNode::Type::Path && sel[1] &&
                 sel[1]->type == SceneNode::Type::Path;
  if (m_act_blend_make)
    m_act_blend_make->set_enabled(make_ok);

  SceneNode *primary = m_canvas.selected_object();
  bool release_ok = (primary && primary->is_blend());
  if (m_act_blend_release)
    m_act_blend_release->set_enabled(release_ok);
}


void MainWindow::update_warp_action_sensitive() {
  // Warp Make: exactly 1 selected, type ∈ {Path, Compound, Group}. No
  // "already inside a Warp" check here — the user can legitimately
  // warp a warped result after Flatten, or nest Warps. Canvas::make_warp
  // validates and reports user-visible errors for edge cases.
  // Release Warp / Flatten Warp: primary selection is a Warp.
  const auto &sel = m_canvas.selection();
  bool make_ok = (sel.size() == 1) && sel[0] &&
                 (sel[0]->type == SceneNode::Type::Path ||
                  sel[0]->type == SceneNode::Type::Compound ||
                  sel[0]->type == SceneNode::Type::Group);
  if (m_act_warp_make)
    m_act_warp_make->set_enabled(make_ok);

  SceneNode *primary = m_canvas.selected_object();
  bool warp_ok = (primary && primary->is_warp());
  if (m_act_warp_edit)
    m_act_warp_edit->set_enabled(warp_ok);
  if (m_act_warp_release)
    m_act_warp_release->set_enabled(warp_ok);
  if (m_act_warp_flatten)
    m_act_warp_flatten->set_enabled(warp_ok);
}


// s138: Group / Ungroup sensitivity. Group requires >=2 selected (the
// minimum that produces a meaningful container). Ungroup requires the
// primary selection to be a Group node. Canvas::group_selection and
// Canvas::ungroup_selection are defensive about preconditions, but
// gating at the action level keeps the menu honest about what's
// available before the user tries to invoke it.
void MainWindow::update_group_actions_sensitive() {
  const auto &sel = m_canvas.selection();
  bool make_ok = sel.size() >= 2;
  if (m_act_group_make)
    m_act_group_make->set_enabled(make_ok);

  SceneNode *primary = m_canvas.selected_object();
  bool release_ok = (primary && primary->type == SceneNode::Type::Group);
  if (m_act_group_release)
    m_act_group_release->set_enabled(release_ok);
}


void MainWindow::update_bool_actions_sensitive() {
  // s122 m3: Union / Subtract / Intersect — at least 2 selected, each
  // either a closed Path or a Compound with all-closed children. Same-
  // parent check stays inside Canvas::boolean_op (with user-visible
  // error) — the user benefits from a specific message when that fails,
  // not a silently-disabled menu.
  // (s122 m2 originally hard-gated at exactly-2 to keep the unstable
  // iterative-fold path unreachable; m3 replaces the fold with Clipper2's
  // native N-way Union and an associative Intersect iteration, so the
  // hazard is gone and N>=2 is fully supported.)
  const auto &sel = m_canvas.selection();
  bool ok = (sel.size() >= 2);
  if (ok) {
    for (SceneNode *n : sel) {
      if (!n) {
        ok = false;
        break;
      }
      const bool is_path = (n->path != nullptr);
      const bool is_compound =
          (n->type == SceneNode::Type::Compound && !n->children.empty());
      if (!is_path && !is_compound) {
        ok = false;
        break;
      }
      // Closedness check, same shape as Canvas::boolean_op
      if (is_path) {
        if (!n->path->closed) {
          ok = false;
          break;
        }
      } else {
        for (auto &ch : n->children) {
          if (!ch || !ch->path || !ch->path->closed) {
            ok = false;
            break;
          }
        }
        if (!ok)
          break;
      }
    }
  }
  if (m_act_bool_union)
    m_act_bool_union->set_enabled(ok);
  if (m_act_bool_subtract)
    m_act_bool_subtract->set_enabled(ok);
  if (m_act_bool_intersect)
    m_act_bool_intersect->set_enabled(ok);
}


void MainWindow::refresh_inspector() {
  if (m_closing)
    return;
  // Action enable/disable follows the selection — refresh these up-front
  // (cheap, no widget rebuild) before the deferred panel refresh.
  update_clip_actions_sensitive();
  update_blend_action_sensitive();
  update_warp_action_sensitive();
  update_group_actions_sensitive(); // s138
  update_bool_actions_sensitive();  // s122 m2
  // s154 m1: Mirror the action-enable bits onto the toolbar's
  // transforms section. Single source of truth: the same predicates
  // that gated the menu items now also gate the toolbar buttons.
  // SnR has no separate action-enable predicate (the action stays
  // permanently sensitive; on_step_repeat early-returns on empty
  // selection), so we re-derive it here as "selection non-empty".
  {
    bool snr_ok   = !m_canvas.selection().empty();
    bool blend_ok = m_act_blend_make
        && m_act_blend_make->property_enabled();
    bool bool_ok  = m_act_bool_union
        && m_act_bool_union->property_enabled();
    bool warp_ok  = m_act_warp_make
        && m_act_warp_make->property_enabled();
    m_toolbar.set_transforms_enabled(snr_ok, blend_ok, bool_ok, warp_ok);
  }
  Glib::signal_idle().connect_once([this]() {
    if (m_closing)
      return;
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    SceneNode *sel = m_canvas.selected_object();
    CanvasModel *cm = doc ? &doc->canvas : nullptr;

    if (doc)
      m_properties.set_ruler_origin(doc->ruler_origin_x, doc->ruler_origin_y);
    else
      m_properties.set_ruler_origin(0.0, 0.0);

    int tool = (int)m_active_tool;
    int node = m_canvas.selected_node();
    LOG_DEBUG("refresh_inspector: tool={} sel={} node={}", tool, (void *)sel,
              node);

    if (sel && m_active_tool == ActiveTool::Node) {
      LOG_DEBUG("refresh_inspector → refresh_node(node={})", node);
      m_properties.refresh_node(cm, sel, node);
    } else if (sel && (m_active_tool == ActiveTool::Selection ||
                       m_active_tool == ActiveTool::TextOnPath)) {
      LOG_DEBUG("refresh_inspector → refresh(sel)");
      m_properties.refresh(cm, sel);
    } else if (cm) {
      LOG_DEBUG("refresh_inspector → refresh(no sel)");
      m_properties.refresh(cm, nullptr);
    } else {
      m_properties.show_empty();
    }
    // s178 m1: layers panel is the inspector's matched pair — same event,
    // two consumers. Pre-fix, panel sync only happened from
    // signal_selection_changed's listener, which is one of many sites that
    // refresh the inspector. That asymmetry left the panel stale on
    // canvas-side selections that reached the inspector via other routes
    // (e.g. refpt picks). Sync here so any inspector refresh implies a
    // matched panel refresh.
    m_layers.set_canvas_selection(m_canvas.selection());
    m_inspector_tool = m_active_tool;
  });
}


// ── refresh_status_counts ─────────────────────────────────────────────────
//
// s132 m2: replaces five duplicated "iterate doc.layers and sum
// children.size()" loops that all hardcoded `nodes=0` for the
// StatusBar's node counter — that counter has read 0 since the bar
// shipped because nobody had a recursive walk handy. Pumps live in
// curvz::utils so the bug class stops at one place.
//
// Safe to call in any state. No active project → 0/0. The pumps
// internally skip overlay layers (guides, grid, margins, refs,
// measurements) so the user-visible counts mirror what the Layers
// panel shows.
void MainWindow::refresh_status_counts() {
  if (m_closing)
    return;
  int objects = 0;
  int nodes = 0;
  if (m_project) {
    if (auto *doc = m_project->active_doc()) {
      objects = curvz::utils::doc_object_count(*doc);
      nodes = curvz::utils::doc_anchor_count(*doc);
    }
  }
  m_statusbar.set_counts(objects, nodes);
}


// ── Project management
// ────────────────────────────────────────────────────────

void MainWindow::load_project(std::unique_ptr<CurvzProject> project) {
  m_project = std::move(project);
  m_history = CommandHistory{};

  // Sync inspector section open-state flags from the loaded project.
  // The sections were built once in setup_layout; we update the shared flags
  // so any subsequent toggle gesture sees the correct starting state.
  auto sync_flag = [](const std::shared_ptr<bool> &flag, bool on) {
    if (flag)
      *flag = on;
  };
  sync_flag(m_sec_open_preview, m_project->sec_preview_open);
  sync_flag(m_sec_open_layers, m_project->sec_layers_open);
  sync_flag(m_sec_open_library, m_project->sec_library_open);
  sync_flag(m_sec_open_documents, m_project->sec_documents_open);
  sync_flag(m_sec_open_swatches, m_project->sec_swatches_open);
  sync_flag(m_sec_open_styles, m_project->sec_styles_open);
  sync_flag(m_sec_open_themes, m_project->sec_themes_open);
  sync_flag(m_sec_open_content, m_project->sec_content_open);

  // s141: drive each section's widgets to match the loaded state. Without
  // this, the bool flags above are correct but the body visibility and
  // arrow glyphs stay in whatever state setup_layout built them — which
  // is collapsed for everything since the section ctors hardcoded
  // expanded=false. Apply via m_sec_apply (registered by make_section
  // and make_group_section) so the visual state matches the saved
  // state on every project load. The setters intentionally do not
  // schedule_save — we just loaded, the value is already on disk.
  auto apply_sec = [this](const std::string &title, bool on) {
    auto it = m_sec_apply.find(title);
    if (it != m_sec_apply.end())
      it->second(on);
  };
  apply_sec("Content", m_project->sec_content_open);
  apply_sec("Layers", m_project->sec_layers_open);
  apply_sec("Library", m_project->sec_library_open);
  apply_sec("Swatches", m_project->sec_swatches_open);
  apply_sec("Styles", m_project->sec_styles_open);
  apply_sec("Themes", m_project->sec_themes_open);
  apply_sec("Documents", m_project->sec_documents_open);
  apply_sec("Preview", m_project->sec_preview_open);

  // Restore pane position — defer until after allocation
  Glib::signal_idle().connect_once([this]() {
    m_pane_ready = false;
    int pos = m_project ? m_project->pane_position : -1;
    // S94 m1 — same clamp as on first map (see ctor for rationale).
    const int RIGHT_PANE_MIN = 280;
    int paned_w = m_h_paned.get_width();
    if (pos > 0 && paned_w > 0 && pos > paned_w - RIGHT_PANE_MIN) {
      int clamped = std::max(100, paned_w - RIGHT_PANE_MIN);
      LOG_INFO("Pane position {} exceeds available width {}; "
               "clamping to {} so right pane keeps {}px minimum",
               pos, paned_w, clamped, RIGHT_PANE_MIN);
      pos = clamped;
    }
    LOG_INFO("Pane restore on project load: setting position to {}", pos);
    if (m_project)
      m_h_paned.set_position(pos);
    LOG_INFO("Pane position after set: {}", m_h_paned.get_position());
    m_pane_ready = true;
    m_pane_settled_at = g_get_monotonic_time() + 500000; // 500ms settle window
  });

  m_properties.set_project(m_project.get());

  // s141: re-seed LibraryPanel from the new project's saved category
  // expansion list. set_project clears m_expanded and repopulates it
  // from m_project->library_expanded_categories. The next refresh
  // (triggered by update_all_panels below) renders with the new state.
  m_library.set_project(m_project.get());

  // Propagate project-wide snap to all docs on load
  for (auto &doc : m_project->documents)
    doc->snap = m_project->snap;

  update_all_panels();
  update_project_sensitive();
  update_title();
  save_config();
  // s144 m3 — track this project as a recent. add() is move-to-front +
  // dedupe; safe to call on every load including the startup auto-reopen
  // (which just re-promotes the entry it was already at index 0). Skips
  // empty (unsaved) projects naturally — directory is "" and add() bails.
  if (m_project && !m_project->directory.empty())
    RecentProjects::instance().add(m_project->directory);
  LOG_INFO("Project loaded: '{}'", m_project->directory);
}


// ── apply_motif_to_window ────────────────────────────────────────────────
// Read the project's motif and add/remove the `curvz-light` CSS class
// on the main window. The class is the only mechanical state — CSS tokens
// defined in css.hpp under `window.curvz-light {}` outrank the dark
// defaults under `window {}` and var() references throughout the
// stylesheet re-resolve automatically.
//
// Motif is project-scope (s116 m6). Switching tabs within the same
// project never changes the app theme — every doc shares the project's
// appearance. Switching projects *does* re-apply.
//
// Idempotent: GTK's CSS-class API is set-membership (add/remove are no-ops
// when state already matches), so callers don't need to track previous
// motif. Called from update_all_panels (project load / project switch),
// on_doc_activated (tab click — defensive; no-op within a project), and
// the prop_changed handler (Project panel toggle).
void MainWindow::sync_motif_class_to(Gtk::Window &w) {
  if (!m_project)
    return;
  if (m_project->motif == Motif::Light) {
    w.add_css_class("curvz-light");
  } else {
    w.remove_css_class("curvz-light");
  }
}


void MainWindow::apply_motif_to_window() {
  if (!m_project)
    return;
  // S117 m15 v2: tell GTK to load its dark/light theme variant to
  // match our motif. Adding `curvz-light` to a window tells our CSS
  // which token block to use, but GTK's *system* theme (Adwaita,
  // including the CSD chrome) loads its dark or light variant based
  // on the `gtk-application-prefer-dark-theme` setting. If the system
  // setting says "dark" but our motif is light, dialogs resolve their
  // base style against system-dark and our overrides only patch the
  // specific selectors we author — leaving dialog chrome dark.
  // Setting this property pulls the system-theme variant in line, so
  // dialogs paint light on first present.
  if (auto settings = Gtk::Settings::get_default()) {
    settings->property_gtk_application_prefer_dark_theme().set_value(
        m_project->motif == Motif::Dark);
  }
  // S117 m15 v3: walk every top-level GTK window — INCLUDING transient
  // dialogs created via make_managed<Gtk::Window> + set_transient_for
  // that never go through Gtk::Application::add_window(). Inspector
  // confirmed (m17 diag) that ThemesDialog's window node had only the
  // 'background csd' classes — no curvz-light — so our v1 get_windows()
  // walk was missing it entirely. gtk_window_get_toplevels() returns
  // every GtkWindow GTK knows about, regardless of app registration.
  sync_motif_class_to(*this);
  GListModel *tops = gtk_window_get_toplevels();
  guint n = tops ? g_list_model_get_n_items(tops) : 0;
  LOG_INFO("apply_motif: walking {} top-level windows", (int)n);
  for (guint i = 0; i < n; ++i) {
    GObject *obj = G_OBJECT(g_list_model_get_item(tops, i));
    if (!obj)
      continue;
    auto wrapped = Glib::wrap(GTK_WINDOW(obj));
    if (wrapped && wrapped != this) {
      LOG_INFO("apply_motif:   -> styling window {}", (void *)wrapped);
      sync_motif_class_to(*wrapped);
    }
    g_object_unref(obj); // get_item gives us a ref
  }
  // S117 m3: rulers + corner are Cairo-painted (not CSS), so they need
  // explicit motif notification. Each setter is a no-op if the motif
  // didn't change, so calling unconditionally here is cheap and means
  // every motif-application path (boot, project switch, motif toggle)
  // automatically refreshes the ruler chrome.
  m_hruler.set_motif(m_project->motif);
  m_vruler.set_motif(m_project->motif);
  m_corner.set_motif(m_project->motif);
  // S117 m5: same shape for the toolbar's Cairo-painted align icon.
  m_toolbar.set_motif(m_project->motif);
  // S117 m14: thumbnails are Cairo-painted into ImageSurfaces inside
  // DocumentGallery::render_thumb, so they need a refresh() to redraw
  // with the new motif's artboard colour and currentColor sample.
  // S117 m14 v2: library thumbnails (LibraryPanel::render_thumb) follow
  // the same pattern — refresh both on motif change.
  m_gallery.refresh();
  m_library.refresh();
  // s183 m5a — inspector Document ▸ Canvas chips read the matching
  // pair of the doc's dual-pair motif storage. App-mode flip swaps
  // which pair is active, so the chips need to repaint with the
  // newly-active pair's colours. refresh_inspector rebuilds the
  // panel against the current motif via m_project->motif reads in
  // the chip lambdas.
  refresh_inspector();
  // BACKLOG: GTK4 caches the rendered pixels of the CSD chrome wrapper
  // (`window.csd > box.vertical`) independently of style. Class change
  // invalidates the style — var() references re-resolve correctly —
  // but the painted surface of the CSD wrapper is a cached frame that
  // queue_draw / queue_resize / class re-add / inspector-toggle hack
  // (tested in v7) do NOT reach. Result: headerbar in light motif
  // paints the dark token's colour until the user manually opens GTK
  // Inspector. No clean fix found in s117. Tracked for future session.
}


void MainWindow::update_all_panels() {
  auto *doc = m_project->active_doc();
  // s249 m1 fix — pointer-refresh ordering matters here.
  //
  // m_canvas.set_document(doc) synchronously fires
  // notify_object_selection_changed (Canvas.cpp:1148, added in s160 m2).
  // Listeners of that signal — including StylesPanel::set_canvas's
  // lambda — call refresh() and dereference per-panel m_library /
  // m_swatch_library pointers. If those pointers still point at the
  // PREVIOUS m_project's embedded library (which was freed when the
  // unique_ptr-move replaced m_project in load_project), the signal
  // path reads freed memory and crashes inside
  // StyleLibrary::app_categories.
  //
  // Diagnosed by gdb backtrace under s249 m1 testing:
  //   StylesPanel::m_library held a pointer into the previous
  //   m_project->styles (now destroyed); m_canvas.set_document(doc)
  //   fired notify_object_selection_changed BEFORE
  //   m_styles.set_library(...) below could update the pointer.
  //
  // Fix: refresh every panel pointer that any signal listener might
  // touch BEFORE firing any setter that emits signals. Two groups:
  //
  //   GROUP A (refresh first, no signals emitted):
  //     Per-project library/project pointer assignments. These are
  //     plain setter calls that just store a pointer; they don't
  //     emit any signal that downstream panels listen to.
  //
  //   GROUP B (then the canvas + document setters):
  //     m_canvas.set_document, set_zoom, set_history etc. These
  //     fire notify_object_selection_changed and other signals
  //     that downstream panels respond to — and the responders
  //     need their library/project pointers already-refreshed by
  //     Group A.
  //
  // Pre-fix order (the bug):  set_document → set_library
  // Post-fix order (correct): set_library → set_document
  //
  // The post-fix order also matches the implicit contract every
  // project-switch path expects: "by the time anyone reads project
  // state, the pointer is current." The fix makes that contract
  // explicit at the seam where it can be violated.

  // ── GROUP A: refresh all per-project pointers (no signals fired) ─
  m_canvas.set_swatch_library(&m_project->swatches); // Phase 5 M3
  m_canvas.set_style_library(&m_project->styles);    // S78 m3d
  m_canvas.set_project(m_project.get()); // s116 m6 — workspace appearance reads
  m_swatches.set_library(&m_project->swatches);
  m_styles.set_library(&m_project->styles);           // S80 m4c
  m_styles.set_swatch_library(&m_project->swatches);  // S85 cont-3
  m_themes.set_project(m_project.get());              // s147 m3
  m_toolbar.set_swatch_library(&m_project->swatches); // S91
  // s171 m1 — re-bind project pointer; m_history is a stable member
  // address so that wiring (in MainWindow_zones) doesn't need refreshing.
  m_layers.set_project(m_project.get());

  // ── GROUP B: setters that fire signals (now safe — Group A done) ─
  m_canvas.set_document(doc);
  // s266 m1 followup: set_document now runs zoom_fit synchronously when
  // the widget is sized (running-app doc switch). That sets m_zoom to the
  // fit value for the new doc's canvas dims. Re-applying m_project->zoom
  // here would clobber that with the project-wide stored value (which
  // defaults to 16.0 and is essentially a dead field — there are no
  // writers to m_project->zoom in the codebase as of this session). Skip
  // when set_document already fitted, i.e. when the canvas widget is sized.
  // First-open path (widget not yet sized) still falls through to the
  // deferred first-draw fit; we don't push a stale value at all.
  if (m_canvas.get_width() <= 0 || m_canvas.get_height() <= 0) {
    m_canvas.set_zoom(m_project->zoom);
  }
  m_canvas.set_history(&m_history);
  m_preview.set_document(doc);
  m_layers.set_document(doc);
  m_library.set_document(doc);
  // s141: refresh library so the freshly-seeded m_expanded (set by
  // load_project's m_library.set_project call) takes visual effect.
  // Without this, the panel still shows the previous project's
  // expansion state — set_project only updates the in-memory map.
  m_library.refresh();
  // S87 — restore the dropdown selection on project switch.
  m_styles.set_active_category(m_project->style_active_category,
                               m_project->style_active_is_app_tier);
  m_gallery.set_project(m_project.get());
  Glib::signal_idle().connect_once([this]() {
    if (m_closing)
      return;
    m_gallery.refresh();
  });
  m_doc_tabs.set_project(m_project.get());
  m_doc_tabs.refresh();
  if (doc) {
    // Propagate project-wide snap before building inspector so it reads correct
    // values
    doc->snap = m_project->snap;
    m_toolbar.set_snap_enabled(doc->snap.enabled);
    m_toolbar.set_snap_settings(doc->snap);
    // Keep popover unit labels in sync with document display unit
    m_toolbar.set_document(doc);
    // s265 m2: corner shows the intent unit when set (the user's typed
    // Size unit), else falls back to display_unit / phys_unit.
    auto resolve_corner_unit = [&]() -> Unit {
      const auto &c = doc->canvas;
      if (c.intended_w > 0.0 && c.intended_h > 0.0
          && !c.intended_unit.empty()) {
        if (c.intended_unit == "in") return Unit::In;
        if (c.intended_unit == "mm") return Unit::Mm;
        if (c.intended_unit == "pt") return Unit::Pt;
        return Unit::Px;
      }
      if (c.display_mode == DisplayMode::Physical)
        return UnitSystem::parse_unit(c.phys_unit);
      return c.display_unit;
    };
    Unit corner_u = resolve_corner_unit();
    m_corner.set_unit(corner_u);
    // Corner panel unit label
    {
      std::string unit_str = UnitSystem::label(corner_u);
      m_corner_unit_label.set_text("Units: " + unit_str);
      // Set a sensible default radius in current units (0.1 of current unit)
      m_corner_radius_spin.set_value(0.1);
    }
  }
  if (doc) {
    CanvasModel *cm = &doc->canvas;
    Glib::signal_idle().connect_once([this, cm]() {
      if (m_closing)
        return;
      m_properties.show_document_props(cm);
    });
  } else {
    Glib::signal_idle().connect_once([this]() {
      if (m_closing)
        return;
      m_properties.show_empty();
    });
  }
  refresh_status_counts(); // s132 m2 — replaces hand-rolled loop
  // s266 m1 followup: read live canvas zoom rather than m_project->zoom.
  // m_project->zoom is essentially a dead field (no writers in the
  // codebase) and defaults to 16.0 which would lie to the user. The
  // canvas just had its zoom set by set_document's eager zoom_fit (or
  // will, on first-draw, for the not-yet-sized first-open path) — use
  // that.
  m_statusbar.set_zoom(m_canvas.zoom() * 100.0);
  update_rulers();
  apply_motif_to_window();
}


void MainWindow::update_title() {
  // No window title shown — project name lives in the status bar footer.
  if (!m_project || m_project->directory.empty()) {
    m_statusbar.set_project_name("unsaved");
  } else {
    std::string name = fs::path(m_project->directory).filename().string();
    m_statusbar.set_project_name(name + "  ✓");
  }
  // Active document name in status bar footer
  auto *doc = m_project ? m_project->active_doc() : nullptr;
  if (doc && !doc->filename.empty()) {
    std::string dname = fs::path(doc->filename).filename().string();
    m_statusbar.set_doc_name(dname);
  } else {
    m_statusbar.set_doc_name("untitled");
  }
  // s150: active display unit, upper-cased for emphasis (matches the
  // Inspector ▸ Dimensions header accessory). When no doc is active,
  // fall back to PX (the default unit).
  if (doc) {
    std::string u = UnitSystem::label(doc->canvas.display_unit);
    for (char &c : u)
      c = std::toupper(static_cast<unsigned char>(c));
    m_statusbar.set_units(u);
  } else {
    m_statusbar.set_units("PX");
  }
}


// ── File operations (GTK4 async FileDialog)

// Helper: enable/disable project-dependent actions based on whether a
// project is currently open.
void MainWindow::update_project_sensitive() {
  bool open = (m_project != nullptr);
  for (const char *name :
       {"new", "save", "save-as", "save-as-template", "close-project",
        "import-svg", "place-image", "export", "print", "step-repeat",
        "undo", "redo", "zoom-fit", "zoom-in", "zoom-out", "zoom-100",
        "zoom-200", "zoom-selection"}) {
    if (auto act = lookup_action(name)) {
      if (auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act))
        sa->set_enabled(open);
    }
  }
}


// Helper: if there are unsaved changes, show a Save/Discard/Cancel dialog.
// Calls `then` only when the user chooses Save (after saving) or Discard.
void MainWindow::check_unsaved_then(std::function<void()> then) {
  if (!m_project || !m_history.can_undo()) {
    then();
    return;
  }
  curvz::utils::show_confirm(
      *this, "Save changes to this project?",
      "Your changes will be lost if you don't save them.",
      {"Cancel", "Discard", "Save"},
      /*default_button=*/2, /*cancel_button=*/0, [this, then](int btn) {
        if (btn == 0 || btn == -1)
          return; // Cancel / dismissed
        if (btn == 1) {
          then();
          return;
        } // Discard
        if (btn == 2) {
          on_save();
          then();
        } // Save then proceed
      });
}


// S104 m1 follow-on — NDD theme dropdown helpers ─────────────────────────
//
// Snapshot the user-tier of the project's theme library as a flat
// vector<Theme>, in the same display order ThemesDialog uses. Ordered
// pass: each user category in insertion order, then each theme within
// the category. Returns empty when there's no project.
std::vector<theme::Theme> MainWindow::ndd_available_themes() const {
  std::vector<theme::Theme> out;
  if (!m_project)
    return out;
  for (const std::string &cat : m_project->themes.user_categories()) {
    for (const theme::Theme *t :
         m_project->themes.user_themes_in_category(cat)) {
      if (t)
        out.push_back(*t); // copy by value — sub-100 themes, cheap
    }
  }
  return out;
}


// Apply the user's theme dropdown choice (if any) to the freshly-built
// seed. Looks up the theme by id; if found, copies its settings into the
// seed via the same apply_theme_to_doc() pump used by ThemesDialog.
//
// Defensive: id may reference a theme that was deleted between NDD
// opening and Create being clicked — in that case find_theme returns
// null and we silently produce an un-themed doc. The user gets what
// they would have gotten with no theme; nothing breaks.
void MainWindow::ndd_apply_chosen_theme(
    CurvzDocument &seed, const std::optional<theme::ThemeId> &id) const {
  if (!id || id->empty())
    return;
  if (!m_project)
    return;
  const theme::Theme *t = m_project->themes.find_theme(*id);
  if (!t) {
    LOG_INFO("ndd_apply_chosen_theme: theme id '{}' no longer exists, "
             "creating doc without theme",
             *id);
    return;
  }
  // s149 m1: apply needs the current motif so the right colour pair from
  // the theme's MotifSettings sub-bundle lands on the seed. Sub-ship 2
  // swaps m_project->motif for AppPreferences::appearance_mode.
  theme::apply_theme_to_doc(*t, seed, m_project->motif);
  LOG_INFO("ndd_apply_chosen_theme: applied '{}' to new doc", t->header.name);
}


// Shared import body.
//   force_currentcolor: convert Solid fills/strokes to CurrentColor.
//   normalize_to_1000:  rescale coords so long axis = 1000 (icon workflow).
//                       When false, use SVG's own dimensions verbatim.
void MainWindow::import_svg_impl(const std::string &path,
                                 bool force_currentcolor,
                                 bool normalize_to_1000) {
  if (!m_project)
    return;

  try {
    auto imported = Curvz::parse_svg_file(path);
    if (!imported) {
      LOG_ERROR("import_svg_impl: failed to parse '{}'", path);
      return;
    }

    double src_w = (double)imported->canvas_width();
    double src_h = (double)imported->canvas_height();
    if (src_w <= 0)
      src_w = 1000.0;
    if (src_h <= 0)
      src_h = 1000.0;

    double scale;
    int dst_w, dst_h;
    if (normalize_to_1000) {
      // Icon workflow — rescale so the longest axis is 1000 units.
      constexpr double QUALITY = 1000.0;
      scale = QUALITY / std::max(src_w, src_h);
      dst_w = (int)std::round(src_w * scale);
      dst_h = (int)std::round(src_h * scale);
    } else {
      // Generic SVG import — preserve authored geometry verbatim.
      scale = 1.0;
      dst_w = (int)std::round(src_w);
      dst_h = (int)std::round(src_h);
    }

    LOG_INFO("import_svg_impl: src={}x{} → dst={}x{} scale={:.6f} "
             "(normalize={}, force_cc={})",
             (int)src_w, (int)src_h, dst_w, dst_h, scale, normalize_to_1000,
             force_currentcolor);

    // Scale every node in the imported tree (no-op when scale == 1.0)
    std::function<void(SceneNode &)> scale_node = [&](SceneNode &n) {
      if (scale != 1.0 && n.path) {
        for (auto &nd : n.path->nodes) {
          nd.x = nd.x * scale;
          nd.y = nd.y * scale;
          nd.cx1 = nd.cx1 * scale;
          nd.cy1 = nd.cy1 * scale;
          nd.cx2 = nd.cx2 * scale;
          nd.cy2 = nd.cy2 * scale;
        }
      }
      if (scale != 1.0 && n.type == SceneNode::Type::Text) {
        n.text_x *= scale;
        n.text_y *= scale;
        n.text_font_size *= scale;
      }
      for (auto &child : n.children)
        scale_node(*child);
    };

    // ── Build a new CurvzDocument for this import ───────────────────────
    auto new_doc = std::make_unique<CurvzDocument>();

    if (normalize_to_1000) {
      // Icon workflow — canonical pixel canvas at normalized dimensions.
      new_doc->canvas = CanvasModel::from_pixels(dst_w, dst_h);
    } else {
      // Generic import — inherit the parsed canvas model verbatim so any
      // physical-mode settings, DPI, or display unit in the source SVG
      // survive the import.
      new_doc->canvas = imported->canvas;
    }

    // Filename derived from the SVG filename stem
    std::string stem = fs::path(path).stem().string();
    std::string fname = stem + ".svg";
    // Ensure unique within project
    int suffix = 2;
    while (std::any_of(m_project->documents.begin(), m_project->documents.end(),
                       [&](const auto &d) { return d->filename == fname; }))
      fname = stem + std::to_string(suffix++) + ".svg";
    new_doc->filename = fname;

    // Ensure the new doc has a default layer
    new_doc->ensure_guide_layer();
    auto layer = std::make_unique<SceneNode>();
    layer->type = SceneNode::Type::Layer;
    layer->internal_id = generate_internal_id();
    layer->name = new_doc->next_default_name(CurvzDocument::NameKind::Layer);
    SceneNode *target_layer = layer.get();
    new_doc->layers.insert(new_doc->layers.begin(), std::move(layer));
    new_doc->active_layer_index = 0;

    // Move scaled objects into the new doc's layer
    int imported_count = 0;
    static int s_import_counter = 1;

    // Helper: fix fill/stroke — convert hardcoded colors to currentColor
    std::function<void(SceneNode &)> fix_style = [&](SceneNode &n) {
      if (n.fill.type == FillStyle::Type::Solid)
        n.fill.type = FillStyle::Type::CurrentColor;
      if (n.stroke.paint.type == FillStyle::Type::Solid)
        n.stroke.paint.type = FillStyle::Type::CurrentColor;
      for (auto &child : n.children)
        fix_style(*child);
    };

    for (auto &imp_layer : imported->layers) {
      LOG_INFO("import_svg_impl: layer '{}' type={} is_layer={} children={}",
               imp_layer->name, (int)imp_layer->type, imp_layer->is_layer(),
               imp_layer->children.size());
      if (!imp_layer->is_layer())
        continue;
      for (auto &child : imp_layer->children) {
        LOG_INFO("import_svg_impl: scaling child type={} has_path={}",
                 (int)child->type, child->path != nullptr);
        scale_node(*child);
        if (force_currentcolor)
          fix_style(*child);
        child->id = "imp" + std::to_string(s_import_counter++);
        target_layer->children.push_back(std::move(child));
        ++imported_count;
      }
    }

    if (imported_count == 0) {
      LOG_INFO("import_svg_impl: no objects found in '{}'", path);
      return;
    }

    // Add new doc to project and activate it
    m_project->documents.push_back(std::move(new_doc));
    m_project->active_doc_index = (int)m_project->documents.size() - 1;
    m_project->save();
    update_all_panels();

    LOG_INFO("import_svg_impl: imported {} objects from '{}' as '{}' "
             "(force_currentcolor={})",
             imported_count, path, fname, force_currentcolor);

  } catch (...) {
    LOG_ERROR("import_svg_impl: exception during import");
  }
}


double MainWindow::pop_to_px(double v) const {
  auto *doc = m_project ? m_project->active_doc() : nullptr;
  if (!doc)
    return v;
  const CanvasModel &cm = doc->canvas;
  if (cm.display_mode == DisplayMode::Physical && cm.quality > 0) {
    double phys_short = std::min(cm.phys_width, cm.phys_height);
    if (phys_short <= 0)
      return v;
    return v / phys_short * cm.quality;
  }
  return UnitSystem::to_px(v, cm.display_unit);
}


bool MainWindow::import_svg_as_doc(const std::string &path,
                                   bool force_currentcolor) {
  if (!m_project)
    return false;

  auto imported = Curvz::parse_svg_file(path);
  if (!imported) {
    LOG_ERROR("import_svg_as_doc: failed to parse '{}'", path);
    return false;
  }

  double src_w = (double)imported->canvas_width();
  double src_h = (double)imported->canvas_height();
  if (src_w <= 0)
    src_w = 1000.0;
  if (src_h <= 0)
    src_h = 1000.0;

  constexpr double QUALITY = 1000.0;
  double scale = QUALITY / std::max(src_w, src_h);
  int dst_w = (int)std::round(src_w * scale);
  int dst_h = (int)std::round(src_h * scale);

  LOG_INFO("import_svg_as_doc: src={}x{} -> dst={}x{} scale={:.6f}", (int)src_w,
           (int)src_h, dst_w, dst_h, scale);

  std::function<void(SceneNode &)> scale_node = [&](SceneNode &n) {
    if (n.path) {
      for (auto &nd : n.path->nodes) {
        nd.x *= scale;
        nd.y *= scale;
        nd.cx1 *= scale;
        nd.cy1 *= scale;
        nd.cx2 *= scale;
        nd.cy2 *= scale;
      }
    }
    if (n.type == SceneNode::Type::Text) {
      n.text_x *= scale;
      n.text_y *= scale;
      n.text_font_size *= scale;
    }
    for (auto &child : n.children)
      scale_node(*child);
  };

  std::function<void(SceneNode &)> fix_style = [&](SceneNode &n) {
    if (n.fill.type == FillStyle::Type::Solid)
      n.fill.type = FillStyle::Type::CurrentColor;
    if (n.stroke.paint.type == FillStyle::Type::Solid)
      n.stroke.paint.type = FillStyle::Type::CurrentColor;
    for (auto &child : n.children)
      fix_style(*child);
  };

  auto new_doc = std::make_unique<CurvzDocument>();
  new_doc->canvas = CanvasModel::from_pixels(dst_w, dst_h);

  std::string stem = fs::path(path).stem().string();
  std::string fname = stem + ".svg";
  int suffix = 2;
  while (std::any_of(m_project->documents.begin(), m_project->documents.end(),
                     [&](const auto &d) { return d->filename == fname; }))
    fname = stem + std::to_string(suffix++) + ".svg";
  new_doc->filename = fname;

  new_doc->ensure_guide_layer();
  auto layer = std::make_unique<SceneNode>();
  layer->type = SceneNode::Type::Layer;
  layer->internal_id = generate_internal_id();
  layer->name = new_doc->next_default_name(CurvzDocument::NameKind::Layer);
  SceneNode *target_layer = layer.get();
  new_doc->layers.insert(new_doc->layers.begin(), std::move(layer));
  new_doc->active_layer_index = 0;

  int imported_count = 0;
  static int s_import_counter = 1;
  for (auto &imp_layer : imported->layers) {
    if (!imp_layer->is_layer())
      continue;
    for (auto &child : imp_layer->children) {
      scale_node(*child);
      if (force_currentcolor)
        fix_style(*child);
      child->id = "imp" + std::to_string(s_import_counter++);
      target_layer->children.push_back(std::move(child));
      ++imported_count;
    }
  }

  if (imported_count == 0) {
    LOG_INFO("import_svg_as_doc: no objects found in '{}'", path);
    return false;
  }

  m_project->documents.push_back(std::move(new_doc));
  m_project->active_doc_index = (int)m_project->documents.size() - 1;
  m_project->save();
  update_all_panels();
  LOG_INFO("import_svg_as_doc: imported {} objects from '{}' as '{}'",
           imported_count, path, fname);
  return true;
}


void MainWindow::exit_preview_mode() {
  if (!m_preview_active)
    return;
  m_preview_active = false;
  m_preview_doc.reset();
  m_project->active_doc_index = m_preview_saved_index;
  update_all_panels();
  m_statusbar.set_mode(m_canvas.is_outline_mode() ? "Outline" : "Preview");
}


// s247 m1 — return type bool (was void). The new script-side caller
// (script_save_project_as) needs to observe the success/failure
// outcome of CurvzProject::save() to surface IoFailed to the DSL.
// Existing GUI call sites in on_save_as ignore the return; their
// behaviour is unchanged. On failure, m_project->directory has
// already been assigned the new path (this is the pre-existing
// posture — directory stamp happens before save attempt), and a
// LOG_ERROR line has been emitted as before.
bool MainWindow::do_save_as(const std::string &dir) {
  m_project->directory = dir;
  if (!m_project->save()) {
    LOG_ERROR("do_save_as: failed");
    return false;
  }
  save_config();
  update_title();
  return true;
}


// ── App config (last opened project)

std::string MainWindow::config_path() const {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  std::string base =
      xdg ? xdg
          : (std::string(getenv("HOME") ? getenv("HOME") : ".") + "/.config");
  return base + "/curvz/settings.json";
}


// s125 m1e: per-purpose last-folder accessors. See MainWindow.hpp banner.
std::string MainWindow::get_last_folder(const std::string &purpose) const {
  auto it = m_last_folders.find(purpose);
  return (it == m_last_folders.end()) ? std::string{} : it->second;
}


void MainWindow::set_last_folder(const std::string &purpose,
                                 const std::string &path) {
  if (path.empty())
    return;
  m_last_folders[purpose] = path;
  // Don't write settings.json synchronously here — that would mean a disk
  // hit per file picker close. Existing save_config call sites (project
  // load, save-as, quit) flush to disk; that's enough for "remembered
  // across runs" semantics. Worst case after a crash: forget the most
  // recent folder choice. Acceptable.
}


void MainWindow::save_config() const {
  // s125 m1e: relaxed the "no project → no save" early-return that used to
  // sit here. App-level state (window_maximized, outline_mode, inspector
  // sections, last_folders) shouldn't be lost when there's no project
  // open. last_project is written as "" in that case.
  std::string path = config_path();
  fs::create_directories(fs::path(path).parent_path());
  bool outline = m_canvas.is_outline_mode();

  // Serialize inspector section open/close state
  nlohmann::json sections = nlohmann::json::object();
  for (auto &[k, v] : m_properties.section_open_state())
    sections[k] = v;

  // s129 m7: help-window sidebar open/close state. Same shape as
  // inspector_sections — a flat object keyed by row title. Drives the
  // collapsibles in HelpWindow's outline tree.
  nlohmann::json help_sections = nlohmann::json::object();
  for (auto &[k, v] : m_help_window.sidebar_open_state())
    help_sections[k] = v;

  // s125 m1e: serialise last-folders.
  nlohmann::json folders = nlohmann::json::object();
  for (auto &[purpose, dir] : m_last_folders)
    folders[purpose] = dir;

  std::string last_project =
      (m_project && !m_project->directory.empty()) ? m_project->directory : "";

  nlohmann::json j = {{"last_project", last_project},
                      {"outline_mode", outline},
                      {"window_maximized", is_maximized()},
                      {"inspector_sections", sections},
                      {"help_sidebar_sections", help_sections},
                      {"last_folders", folders}};
  std::ofstream f(path);
  if (f) {
    f << j.dump(2) << "\n";
  }
}


std::string MainWindow::load_last_project_path() {
  std::ifstream f(config_path());
  if (!f)
    return {};
  try {
    nlohmann::json j;
    f >> j;

    // Restore outline mode. NOTE: this runs from setup_project() during
    // construction, before connect_signals() wires up
    // signal_outline_mode_changed — so the inline sync below is required
    // here. Other call sites rely on the signal handler.
    bool outline = j.value("outline_mode", false);
    if (outline != m_canvas.is_outline_mode()) {
      m_canvas.toggle_outline_mode();
      m_statusbar.set_mode(outline ? "Outline" : "Preview");
      if (auto act = lookup_action("toggle-outline")) {
        auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act);
        if (sa)
          sa->set_state(Glib::Variant<bool>::create(outline));
      }
    }

    // Restore window maximized state
    if (j.value("window_maximized", false))
      maximize();

    // Restore inspector section open/close state
    if (j.contains("inspector_sections") &&
        j["inspector_sections"].is_object()) {
      std::map<std::string, bool> state;
      for (auto &[k, v] : j["inspector_sections"].items())
        if (v.is_boolean())
          state[k] = v.get<bool>();
      m_properties.set_section_open_state(state);
    }

    // s129 m7: restore help-window sidebar open/close state.
    if (j.contains("help_sidebar_sections") &&
        j["help_sidebar_sections"].is_object()) {
      std::map<std::string, bool> state;
      for (auto &[k, v] : j["help_sidebar_sections"].items())
        if (v.is_boolean())
          state[k] = v.get<bool>();
      m_help_window.set_sidebar_open_state(state);
    }

    // s125 m1e: restore per-purpose last-folder map.
    if (j.contains("last_folders") && j["last_folders"].is_object()) {
      m_last_folders.clear();
      for (auto &[k, v] : j["last_folders"].items())
        if (v.is_string())
          m_last_folders[k] = v.get<std::string>();
    }

    return j.value("last_project", std::string{});
  } catch (...) {
    return {};
  }
}


// ── write_library_item ───────────────────────────────────────────────────────
//
// s136 m4: the actual file-writing helper. Pure function over (dest_dir,
// base_name) — assumes base_name is final, performs the geometry-collection
// and serialisation it always did, writes <dest_dir>/<base_name>.svg.
//
// Refresh of the LibraryPanel happens here on success so callers don't all
// have to remember it.

bool MainWindow::write_library_item(const std::string &dest_dir,
                                    const std::string &base_name) {
  if (!m_project)
    return false;
  auto *doc = m_project->active_doc();
  if (!doc)
    return false;

  const auto &sel = m_canvas.selection();
  if (sel.empty())
    return false;

  // ── Build scratch document ────────────────────────────────────────────
  CurvzDocument scratch;
  scratch.canvas = doc->canvas; // preserve viewBox dimensions

  auto layer = std::make_unique<SceneNode>();
  layer->type = SceneNode::Type::Layer;
  layer->internal_id = generate_internal_id();
  layer->name = scratch.next_default_name(CurvzDocument::NameKind::Layer);
  layer->visible = true;
  SceneNode *tgt = layer.get();
  scratch.layers.push_back(std::move(layer));
  scratch.active_layer_index = 0;

  // Compute bounding box of selection to re-centre in a tight canvas
  double bx1 = 1e9, by1 = 1e9, bx2 = -1e9, by2 = -1e9;
  for (SceneNode *obj : sel) {
    // Walk the node to find path bboxes
    std::function<void(const SceneNode &)> collect_bb =
        [&](const SceneNode &n) {
          if (n.path) {
            for (const auto &nd : n.path->nodes) {
              bx1 = std::min(bx1, nd.x);
              bx2 = std::max(bx2, nd.x);
              by1 = std::min(by1, nd.y);
              by2 = std::max(by2, nd.y);
            }
          }
          for (const auto &c : n.children)
            collect_bb(*c);
        };
    collect_bb(*obj);
  }
  // Clamp if no geometry found
  if (bx1 > bx2) {
    bx1 = 0;
    by1 = 0;
    bx2 = 48;
    by2 = 48;
  }

  // Pad slightly and set canvas to tight bbox
  double pad = 2.0;
  double cw = (bx2 - bx1) + pad * 2.0;
  double ch = (by2 - by1) + pad * 2.0;
  scratch.canvas = CanvasModel::from_pixels(std::max(1, (int)std::round(cw)),
                                            std::max(1, (int)std::round(ch)));

  // Clone selected objects, translate so bbox is at (pad, pad) in doc space
  double tx = pad - bx1;
  double ty = pad - by1;
  for (SceneNode *obj : sel) {
    auto node = clone_node(*obj);
    // Translate all path nodes
    std::function<void(SceneNode &)> translate = [&](SceneNode &n) {
      if (n.path) {
        for (auto &nd : n.path->nodes) {
          nd.x += tx;
          nd.y += ty;
          nd.cx1 += tx;
          nd.cy1 += ty;
          nd.cx2 += tx;
          nd.cy2 += ty;
        }
      }
      if (n.type == SceneNode::Type::Text) {
        n.text_x += tx;
        n.text_y += ty;
      }
      for (auto &c : n.children)
        translate(*c);
    };
    translate(*node);
    tgt->children.push_back(std::move(node));
  }

  // ── Serialise via optimise_svg ────────────────────────────────────────
  std::string svg = optimise_svg(scratch);

  // Ensure dest_dir exists
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(dest_dir, ec);

  // s136 m4: filename comes from caller. Collision was handled upstream by
  // the orchestrator (Replace / Cancel) — here we just write.
  const std::string dest = dest_dir + "/" + base_name + ".svg";

  // Write
  std::ofstream f(dest);
  if (!f) {
    LOG_ERROR("write_library_item: cannot write '{}'", dest);
    curvz::utils::show_alert(*this, "Save failed",
                             "Could not write to:\n" + dest);
    return false;
  }
  f << svg;
  f.close();

  LOG_INFO("write_library_item: saved {} object(s) to '{}'", sel.size(), dest);

  // Refresh panel so new item appears immediately
  m_library.refresh();
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ━━━ s202 m6 ━━━ Inspector focus + quick-jump.
// ─────────────────────────────────────────────────────────────────────────────
//
// Two affordances: Alt+Space collapses every inspector section
// (collapse_all_inspector_sections); Ctrl+Space pops a small floating
// chooser (show_quick_jump_popover) listing the currently relevant
// sections. Picking a section invokes focus_inspector_on which
// collapses everything and then opens just that section's chain.
//
// The state machinery spans two collapse stores: m_sec_apply (Content
// group and its inner make_section children, registered at
// setup_layout) AND PropertiesPanel::m_section_open (Document and
// Object groups plus every section inside them, rebuilt per
// PropertiesPanel::refresh). Both stores are written through; the
// PropertiesPanel side then re-renders via refresh_inspector so the
// arrow glyphs and body visibilities catch up.

// Walk PropertiesPanel's m_section_open map setting every entry to
// `on`, then call refresh_inspector so the panel rebuilds with the
// new flags in place. This is the cheapest path: PropertiesPanel
// reads m_section_open[title] at build time to decide each section's
// initial-visible state (see add_collapsible's `open_now = it != end
// ? it->second : expanded` line), so writing the map then refreshing
// is equivalent to clicking every header.
static void set_all_properties_sections(PropertiesPanel& panel, bool on) {
  auto state = panel.section_open_state();  // copy
  for (auto& kv : state) kv.second = on;
  panel.set_section_open_state(state);
}

// s222 m2 fix-2 — parent-resolution helpers, lifted from the local
// lambdas that focus_inspector_on used to declare inline. The lift
// gives us a single source of truth for "what is X's parent?" — used
// by focus_inspector_on (collapse-all + open-with-ancestors) and
// apply_section_open (open-with-ancestors only, no collapse), and
// the next consumer (whoever wants the same n-order ancestor walk)
// picks them up too.
//
// Two helpers because the inspector mixes two parent-resolution
// mechanisms: PropertiesPanel-internal sections use a static
// title→group table (parent_group_for) plus an intermediate-parent
// table for the Theme-disclosure children (intermediate_parent_for).
// The right-panel sections use a uniform rule: every key in
// m_sec_apply except "Content" itself is a child of "Content."
// parent_of_section composes those rules into a single
// "give me the immediate parent of this title" answer that the
// while-loop ancestor walk in apply_section_open can stop on.
//
// Return convention: empty string means "no parent" (the title is a
// root, or it's unknown — both cases stop the ancestor walk). When
// the parent IS a PropertiesPanel GROUP (Document / Object /
// Application), the returned string is the "__group__"-prefixed
// key, ready to look up directly in PropertiesPanel::section_open_state
// (the prefix is how PropertiesPanel disambiguates group flags from
// section flags in its flat map).
static const char* parent_group_for(const std::string& title) {
  // Document group — Metadata + Dimensions directly; Theme is its
  // own collapsible that contains Canvas / Margins / Grid as nested
  // children (handled separately via intermediate_parent_for).
  if (title == "Metadata" || title == "Theme" || title == "Canvas" ||
      title == "Dimensions" || title == "Margins" || title == "Grid")
    return "Document";
  // Object group — selection-dependent sections, Guides under Object
  // (s179 m3v2), Styling under Object as the apply-style surface.
  if (title == "Selection" || title == "Text" || title == "Blend" ||
      title == "Warp" || title == "Node" || title == "Appearance" ||
      title == "Shadow" || title == "Styling" || title == "Guides")
    return "Object";
  // Application group — boot-time + editing prefs.
  if (title == "Startup" || title == "Editing" || title == "Paths" ||
      title == "Boolean cleanup")
    return "Application";
  return nullptr;
}

// s203 m5 fix1 — Theme-nested children. Canvas / Margins / Grid live
// inside the Theme disclosure (build_theme_disclosure), not directly
// under Document. The m_section_open keys are flat — "Canvas",
// "Margins", "Grid" all sit at the same level in the map as "Theme"
// itself — but the visual disclosure tree is Document → Theme →
// {Canvas, Margins, Grid}. The intermediate parent is Theme; the
// outer group is still Document, walked via parent_group_for.
//
// Returns the intermediate section title to also flip open, or empty
// string if the title is not nested inside an intermediate.
static std::string intermediate_parent_for(const std::string& title) {
  if (title == "Canvas" || title == "Margins" || title == "Grid")
    return "Theme";
  return {};
}

// Resolve the immediate parent of any inspector section. Returns
// empty string for roots (Content, or unknown titles). The result is
// either:
//   * A bare title that appears as a key in m_sec_apply
//     (e.g. "Content" — currently the only such non-leaf in the
//     right-panel registry).
//   * A bare title that appears as a key in PropertiesPanel's
//     section_open_state map (e.g. "Theme" — intermediate parent of
//     Canvas / Margins / Grid).
//   * A "__group__"-prefixed key for PropertiesPanel group headers
//     (e.g. "__group__Document" — outermost parent of Metadata /
//     Dimensions / etc.).
//
// The caller is responsible for routing the returned key to the
// right registry. apply_section_open's ancestor walk currently
// handles only the m_sec_apply branch (right-panel sections); a
// future variant can extend the routing to PropertiesPanel sections
// as well, using the prefix to discriminate.
static std::string parent_of_section(const std::string& title) {
  // PropertiesPanel intermediate parent wins first — Canvas/Margins/
  // Grid have BOTH an intermediate (Theme) AND a group parent
  // (Document). The walk visits the intermediate first; the next
  // step of the walk will resolve the intermediate's own parent
  // (Theme → __group__Document).
  std::string intermediate = intermediate_parent_for(title);
  if (!intermediate.empty()) return intermediate;
  // PropertiesPanel group parent — for titles like Metadata,
  // Dimensions, Theme, Selection, etc. that sit directly inside a
  // Document/Object/Application group.
  if (const char* group = parent_group_for(title)) {
    return std::string("__group__") + group;
  }
  // Right-panel rule: every right-panel section except "Content"
  // itself is a child of Content. We don't enumerate the children
  // explicitly — the rule is "if it's not Content, it's under
  // Content." Group headers ("__group__Document" etc.) are roots
  // (they have no parent above them).
  if (title == "Content") return {};
  if (title.rfind("__group__", 0) == 0) return {};
  // The remaining cases — Layers, Library, Swatches, Styles, Themes,
  // Documents, Preview — all sit under Content. We don't check
  // m_sec_apply membership here because the helper is static (no
  // access to the map); the apply walk's lookup-or-skip at each step
  // handles the "unknown title" case naturally.
  return "Content";
}

void MainWindow::collapse_all_inspector_sections() {
  // Phase 1: collapse every section in MainWindow's m_sec_apply registry
  // (Content group, Layers, Library, Swatches, Styles, Themes,
  // Documents). Each closure handles the cascade — body visibility,
  // arrow glyph, project field, schedule_save.
  for (auto& kv : m_sec_apply) {
    kv.second(false);
  }

  // Phase 2: collapse every section PropertiesPanel knows about
  // (Document + Object groups and their inner sections, rebuilt per
  // selection). Write the open-state map, then refresh so the panel
  // re-renders with the new flags in place.
  set_all_properties_sections(m_properties, false);
  refresh_inspector();
}

// s222 m2 — single-section open (no collapse of siblings). See the
// header comment for the limitation: only m_sec_apply-registered
// sections are reached; PropertiesPanel-internal sections need the
// keyboard quick-jump's focus path (or a future cascade-aware variant
// of this method).
//
// s222 m2 fix-2 — ancestor walk on open. The first cut missed that
// every right-panel section except "Content" is a CHILD of the
// Content group. Opening a leaf without first opening its parent
// sets the leaf's body visible-flag to true, but the Content
// container that holds it is still hidden, so the leaf renders
// inside a hidden parent and the user sees nothing. The fix is the
// "while parent closed, open parent" routine — generalised so any
// future deeper nesting (PropertiesPanel sections inside a group
// inside an intermediate) just works without per-case code here.
//
// The walk uses parent_of_section (file-local static) to resolve
// each ancestor; the loop stops when the resolver returns empty.
// Each step looks the ancestor up in m_sec_apply and opens it if
// present; ancestors that resolve to PropertiesPanel-side keys
// (the "__group__..." form, or intermediate names like "Theme")
// won't be found in m_sec_apply and the step silently skips them,
// which is correct for m2's scope — opening a PropertiesPanel
// section through this verb isn't supported yet (see header note),
// so we don't need to drive PropertiesPanel's map. When that scope
// extends, the same ancestor walk runs unchanged; only the per-
// step apply gains a PropertiesPanel branch.
//
// Cascade only fires on open (on=true). On close (on=false) we
// don't walk — close is monotonic (an already-hidden body stays
// hidden regardless of its parent's state) and closing ancestors
// would close siblings the caller didn't ask to close. Bare lookup
// is correct for the close direction.
//
// A title that isn't in m_sec_apply is a silent no-op (the leaf
// step's lookup misses); this mirrors focus_inspector_on's behaviour
// on stale/unknown titles, and means the script-side `inspector
// open "Bogus"` doesn't crash or emit errors — the ancestor walk
// runs (parent_of_section returns empty for unknown titles, so the
// walk stops immediately), then the leaf lookup misses too.
void MainWindow::apply_section_open(const std::string& section_title, bool on) {
  // Verify the leaf is something we can act on before doing any work.
  // An unknown title should be a strict no-op — not "no-op on the
  // leaf but open Content as a side-effect because the resolver
  // guessed Content as its parent." For m2 we only address
  // m_sec_apply leaves; PropertiesPanel sections aren't yet supported
  // through this entry point (see header note), so an unknown title
  // either way produces no mutation.
  auto leaf = m_sec_apply.find(section_title);
  if (leaf == m_sec_apply.end()) return;

  if (on) {
    // Ancestor walk — collect chain bottom-up, then apply top-down so
    // a future PropertiesPanel-aware variant (which DOES need
    // outer-first ordering because GTK visibility cascades through
    // the widget tree at refresh time) reads the same shape. Today's
    // m_sec_apply-only path could apply in either order — body
    // visibility is a flat boolean — but uniform ordering is cheaper
    // to reason about than two shapes.
    std::vector<std::string> chain;
    std::string cur = parent_of_section(section_title);
    while (!cur.empty()) {
      chain.push_back(cur);
      cur = parent_of_section(cur);
    }
    // Apply outermost-first. The chain was built innermost-first
    // (immediate parent → grandparent → root), so iterate reverse.
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      auto found = m_sec_apply.find(*it);
      if (found != m_sec_apply.end()) {
        found->second(true);
      }
      // Else: ancestor lives in PropertiesPanel's map, not m_sec_apply
      // — out of scope for m2's verb (see header). Silently skip.
    }
  }
  // Apply the leaf itself, whether on or off.
  leaf->second(on);
}

void MainWindow::focus_inspector_on(const std::string& section_title) {
  // Compose: collapse everything, then open just what's needed to
  // view the picked section. "Just what's needed" means the section
  // itself and its single parent group — no siblings, no other
  // groups, no extras.
  //
  // Parent group is determined by which registry the section lives
  // in and (for PropertiesPanel sections) by a static title→group
  // mapping. PropertiesPanel stores section flags keyed by title
  // alone — no parent context in the key — so the mapping table
  // is the single source of truth for "which group owns which title."

  // First: collapse everything.
  collapse_all_inspector_sections();

  // Case 1: m_sec_apply — Content group + its leaves.
  auto open_main_section = [this](const std::string& title) {
    auto it = m_sec_apply.find(title);
    if (it != m_sec_apply.end()) it->second(true);
  };
  if (section_title == "Content") {
    open_main_section("Content");
    refresh_inspector();
    return;
  }
  if (m_sec_apply.find(section_title) != m_sec_apply.end()) {
    open_main_section("Content");
    open_main_section(section_title);
    refresh_inspector();
    return;
  }

  // Case 2: PropertiesPanel sections — Document / Object / Application.
  // Parent-resolution helpers (parent_group_for, intermediate_parent_for)
  // are file-local statics — see their definitions near the top of this
  // TU. Lifted from inline lambdas during s222 m2 fix-2 so apply_section_open
  // can share them; the table is the single source of truth for "which
  // group owns which title" across both consumers.

  auto state = m_properties.section_open_state();
  if (state.find(section_title) != state.end()) {
    state[section_title] = true;
    if (auto* group = parent_group_for(section_title)) {
      state[std::string("__group__") + group] = true;
    }
    // s203 m5 fix1 — also flip the intermediate parent (e.g. Theme) so
    // the leaf is actually visible inside an opened disclosure.
    auto intermediate = intermediate_parent_for(section_title);
    if (!intermediate.empty() && state.find(intermediate) != state.end()) {
      state[intermediate] = true;
    }
    m_properties.set_section_open_state(state);
    refresh_inspector();
    return;
  }

  // Group-header pick: "Document", "Object", "Application" themselves.
  std::string group_key = "__group__" + section_title;
  if (state.find(group_key) != state.end()) {
    state[group_key] = true;
    m_properties.set_section_open_state(state);
    refresh_inspector();
    return;
  }

  // No match — pick was stale. Refresh anyway so the collapse-all
  // from phase 1 is visible.
  refresh_inspector();
}

// s202 m6_v2 — phase detection. Three buckets, computed from app
// state at quick-jump invocation time. The phase keys the counter
// store so different workflow modes learn independently — what you
// jump to during Execution is tracked separately from what you jump
// to during Setup.
//
//   Setup     — no active doc, OR active doc has no objects. The
//               canvas hasn't been populated yet; the user is most
//               likely configuring the document itself.
//   Execution — selection is non-empty. The user is actively
//               modifying something.
//   Polish    — content exists but no selection. Between-tasks
//               state — looking around, adjusting global settings,
//               browsing the library.
//
// Phase detection is intentionally coarse for m6_v2. A more
// sophisticated reading might consider active tool, doc maturity,
// etc., but the three-bucket split is enough granularity for the
// counter system to learn useful weights without diluting them
// across a long tail of micro-phases.
//
// Implemented inline as a lambda since it needs private access to
// m_canvas + m_project.

void MainWindow::show_quick_jump_popover() {
  // ── Phase detection (see comment block above) ──────────────────────
  auto detect_phase = [this]() -> int {
    if (!m_canvas.selection().empty()) return 1;  // Execution
    if (!m_project) return 0;                     // Setup — no project
    auto* doc = m_project->active_doc();
    if (!doc) return 0;                           // Setup — no doc
    for (const auto& layer : doc->layers) {
      if (layer && !layer->children.empty()) return 2;  // Polish
    }
    return 0;                                     // Setup — empty doc
  };

  // ── Phase 1: gather candidates from both registries ────────────────
  //
  // PropertiesPanel only builds sections that apply to the current
  // selection / inspector tool state — so keys in section_open_state
  // ARE the context-filter for Document-group and Object-group
  // sections. Content panels (Layers, Library, Swatches, Styles,
  // Themes, Documents) are always present in m_sec_apply.
  std::vector<std::string> candidates;
  candidates.reserve(16);
  for (const auto& kv : m_properties.section_open_state()) {
    if (kv.first.rfind("__group__", 0) == 0) continue;
    candidates.push_back(kv.first);
  }
  for (const auto& kv : m_sec_apply) {
    if (kv.first == "Content") continue;
    candidates.push_back(kv.first);
  }

  // ── Phase 2: detect phase and sort by counter ──────────────────────
  //
  // detect_quick_jump_phase reads canvas selection + active doc.
  // Counter store lives on AppPreferences — per-user, persists across
  // runs. Counts grow on each pick (see click handler below); the
  // sort is descending so most-picked rises to the top.
  //
  // Tiebreaker: insertion order from the gather above (PropertiesPanel
  // sections first, then m_sec_apply leaves). Stable enough — when
  // counts are equal (notably zero on cold-start), the order matches
  // the inspector's own top-down layout, which is a reasonable
  // default scan order.
  const int phase = detect_phase();
  auto& prefs = AppPreferences::instance();
  std::stable_sort(candidates.begin(), candidates.end(),
      [&prefs, phase](const std::string& a, const std::string& b) {
        return prefs.quick_jump_count(phase, a) >
               prefs.quick_jump_count(phase, b);
      });

  // ── Phase 3: lazy window construction ──────────────────────────────
  //
  // s203 m5 fix2 — Was modal + decorated(false) — that meant the user
  // could only dismiss via Escape; no titlebar drag, no native × close,
  // and modal blocked click-outside-to-dismiss too. Now decorated +
  // non-modal: native titlebar (drag handle + ×), Escape still works,
  // and clicking outside the window naturally dismisses since nothing
  // is grabbing focus. Matches the chrome decision for s203 m1's
  // ClipboardViewWindow — working tool surfaces earn their titlebar.
  if (!m_quick_jump_win) {
    m_quick_jump_win = std::make_unique<Gtk::Window>();
    m_quick_jump_win->set_title("Jump to…");
    m_quick_jump_win->set_transient_for(*this);
    m_quick_jump_win->set_hide_on_close(true);
    m_quick_jump_win->set_resizable(false);

    auto kc = Gtk::EventControllerKey::create();
    kc->signal_key_pressed().connect(
        [this](guint kv, guint, Gdk::ModifierType) -> bool {
          if (kv == GDK_KEY_Escape) {
            m_quick_jump_win->set_visible(false);
            return true;
          }
          return false;
        },
        /*after=*/false);
    m_quick_jump_win->add_controller(kc);
  }

  // s203 m5 fix3 — Reset size on each invocation. set_resizable(false)
  // sizes the window to its content's natural request, but once More…
  // has been clicked and the window has grown to fit the tail, GTK
  // (or the compositor) caches that larger size and reuses it on the
  // next present even when the new content is smaller. Calling
  // set_default_size(-1, -1) before populating tells GTK to drop the
  // cached size and recompute from natural request on next layout.
  // Cheap, no architectural change, fixes the "permanent long empty
  // window after More…" bug.
  m_quick_jump_win->set_default_size(-1, -1);

  // ── Phase 4: mini UI — compact list, top-5 visible + More expander ─
  //
  // Visual targets:
  //   - 1px row spacing
  //   - no button frame (set_has_frame(false))
  //   - tight 2px vertical padding via CSS class .qj-row
  //   - smaller font via .dim-label-strength is NOT used because
  //     dim-label fades to near-invisible; instead we let the row
  //     button's natural font stand and rely on the compact padding
  //     to communicate "mini"
  //
  // "More…" affordance: top 5 visible by default; clicking More
  // reveals the rest in the same list. The reveal happens in-place
  // (no second window, no scroll change) — we just remove the More
  // button and append the tail. State doesn't persist across pops;
  // each Ctrl+Space starts collapsed.

  auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  outer->set_margin_top(3);
  outer->set_margin_bottom(3);
  outer->set_margin_start(3);
  outer->set_margin_end(3);
  outer->set_spacing(1);

  if (candidates.empty()) {
    auto* none = Gtk::make_managed<Gtk::Label>("Nothing relevant right now");
    none->add_css_class("dim-label");
    none->set_margin_top(4);
    none->set_margin_bottom(4);
    none->set_margin_start(8);
    none->set_margin_end(8);
    outer->append(*none);
    m_quick_jump_win->set_child(*outer);
    m_quick_jump_win->present();
    return;
  }

  // Helper that builds one row button. The signature returns the
  // raw pointer so the caller can append it in either the always-
  // visible block or the More-revealed block.
  auto make_row = [this, phase, &prefs](const std::string& title) -> Gtk::Button* {
    // s214 m2: unregistered substrate Button — N instances per popover
    // show, sharing the role of "quick-jump row." Per-row addressability
    // would be a model-Scriptables question (inspector sections by name)
    // and that's already addressed at the section level, not the row.
    auto* btn = Gtk::make_managed<curvz::widgets::Button>(
        curvz::scripting::unregistered, title);
    btn->set_has_frame(false);
    btn->add_css_class("qj-row");
    btn->set_margin_top(0);
    btn->set_margin_bottom(0);
    // Left-align the label inside the button. Gtk::Button wraps the
    // text in a Label child; grab it and pin xalign to 0.
    if (auto* lbl = dynamic_cast<Gtk::Label*>(btn->get_child())) {
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
    }
    btn->signal_clicked().connect([this, title, phase, &prefs]() {
      // Bump BEFORE invoking focus, so the counter update is part of
      // the user's gesture commitment (not contingent on the focus
      // call succeeding).
      prefs.bump_quick_jump_count(phase, title);
      m_quick_jump_win->set_visible(false);
      focus_inspector_on(title);
    });
    return btn;
  };

  // Always show top 5.
  const size_t TOP_N = 5;
  size_t visible = std::min(TOP_N, candidates.size());
  for (size_t i = 0; i < visible; ++i) {
    outer->append(*make_row(candidates[i]));
  }

  // If there's a tail, append a "More…" row that swaps itself out
  // for the tail on click. Self-removing pattern: capture outer +
  // tail vector; on click, remove self and append each tail row.
  if (candidates.size() > TOP_N) {
    // s214 m2: unregistered substrate Button — one per popover show,
    // but the popover itself rebuilds on every quick-jump invocation.
    auto* more = Gtk::make_managed<curvz::widgets::Button>(
        curvz::scripting::unregistered, "More…");
    more->set_has_frame(false);
    more->add_css_class("qj-row");
    more->add_css_class("dim-label");
    if (auto* lbl = dynamic_cast<Gtk::Label*>(more->get_child())) {
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
    }
    // Capture tail by copy; the candidates vector is local and will
    // go out of scope as soon as show_quick_jump_popover returns.
    std::vector<std::string> tail(candidates.begin() + TOP_N,
                                  candidates.end());
    more->signal_clicked().connect(
        [this, outer, more, tail, make_row]() {
          outer->remove(*more);
          for (const auto& title : tail) {
            outer->append(*make_row(title));
          }
        });
    outer->append(*more);
  }

  m_quick_jump_win->set_child(*outer);
  m_quick_jump_win->present();
}

// ── show_clipboard_view ─────────────────────────────────────────────────────
//
// s203 m1 — Edit ▸ View Clipboard… handler. Lazy-build the mini float on
// first call, then refresh and present on every call. The window owns its
// own lifecycle (hide_on_close, Escape closes, titlebar × closes);
// MainWindow just holds the unique_ptr and triggers a refresh on each
// invocation so the user always sees the current clipboard, not whatever
// was there last time they opened it.
void MainWindow::show_clipboard_view() {
  if (!m_clipboard_view_win) {
    m_clipboard_view_win = std::make_unique<ClipboardViewWindow>();
    m_clipboard_view_win->set_transient_for(*this);
  }
  m_clipboard_view_win->refresh();
  m_clipboard_view_win->present();
}

} // namespace Curvz
