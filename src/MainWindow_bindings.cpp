#include "MainWindow.hpp"
#include "AppPreferences.hpp" // s139 m2 / s143 m1 — boolean-cleanup quality pref + sync
#include "ContextBar.hpp"
#include "CoordSpace.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "DocTabBar.hpp"
#include "ExportDialog.hpp"
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
#include "ExportDocsDialog.hpp"
#include "UnitSystem.hpp"
#include "curvz_utils.hpp" // s117 m18 v2: apply_motif_class_from_parent
#include "style/StyleInterop.hpp" // mutate_appearance — inspector-driven appearance edits
#include <algorithm>
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
// MainWindow_bindings.cpp — signal connections, Gio action declarations + their inline slot bodies, key/click controllers, accelerator registrations. After category extraction this file is still the largest TU; many of its lines are inline lambda slot bodies that travel with the wiring.
// ─────────────────────────────────────────────────────────────────────

void MainWindow::connect_signals() {
  // Window close button (X) — GTK4 uses signal_close_request, not delete-event
  signal_close_request().connect(
      [this]() -> bool {
        if (!m_closing)
          on_quit();
        return false; // allow GTK to proceed with closing
      },
      false);

  m_toolbar.signal_tool_changed().connect(
      sigc::mem_fun(*this, &MainWindow::on_tool_changed));

  // DocTabBar signals — primary navigation
  m_doc_tabs.signal_doc_activated().connect(
      sigc::mem_fun(*this, &MainWindow::on_doc_activated));

  m_doc_tabs.signal_add_doc().connect([this]() {
    if (!m_project)
      return;
    // s148 m1: preview colours from active doc (re-promoted to per-doc).
    // Falls back to dark struct defaults when no active doc — the new-
    // doc dialog won't have a meaningful "match what user is looking
    // at" reference in that case anyway.
    auto *active = m_project->active_doc();
    double pv_ws_r = 0.09, pv_ws_g = 0.09, pv_ws_b = 0.09;
    double pv_ab_r = 0.157, pv_ab_g = 0.157, pv_ab_b = 0.157;
    if (active) {
      pv_ws_r = active->workspace_bg_r;
      pv_ws_g = active->workspace_bg_g;
      pv_ws_b = active->workspace_bg_b;
      pv_ab_r = active->artboard_bg_r;
      pv_ab_g = active->artboard_bg_g;
      pv_ab_b = active->artboard_bg_b;
    }
    m_new_doc_dialog.show(
        *this, ndd_available_themes(),
        m_project->motif == Motif::Light ? templates::MotifTag::Light
                                         : templates::MotifTag::Dark,
        pv_ws_r, pv_ws_g, pv_ws_b, pv_ab_r, pv_ab_g, pv_ab_b,
        [this](std::unique_ptr<CurvzDocument> seed, std::string name,
               std::optional<theme::ThemeId> theme_id) {
          if (!seed)
            return;
          // Apply chosen theme (if any) BEFORE assigning filename / pushing to
          // project. apply_theme_to_doc may add layers (grid, margin) that
          // weren't in the seed, and we want all that wiring done before the
          // doc is observable by the rest of the UI.
          ndd_apply_chosen_theme(*seed, theme_id);

          // Sanitise name → filename stem (see doc_stem_from_name comment).
          std::string base = curvz::utils::doc_stem_from_name(name);
          std::string fname = base + ".svg";
          // Ensure unique within project
          int suffix = 2;
          while (std::any_of(
              m_project->documents.begin(), m_project->documents.end(),
              [&](const auto &d) { return d->filename == fname; }))
            fname = base + std::to_string(suffix++) + ".svg";

          seed->filename = fname;

          m_project->documents.push_back(std::move(seed));
          m_project->active_doc_index = (int)m_project->documents.size() - 1;
          m_project->save();
          update_all_panels();
          LOG_INFO("Added document '{}' to project", fname);
        });
  });

  m_doc_tabs.signal_remove_doc().connect([this](int idx) {
    if (!m_project)
      return;
    if (idx < 0 || idx >= (int)m_project->documents.size())
      return;
    m_project->documents.erase(m_project->documents.begin() + idx);
    // Clamp active index, but allow empty gallery (-> -1 is fine; active_doc()
    // returns nullptr in that case, and update_all_panels handles it).
    if (m_project->documents.empty()) {
      m_project->active_doc_index = 0; // harmless; active_doc() guards empty
    } else {
      m_project->active_doc_index =
          std::min(idx, (int)m_project->documents.size() - 1);
    }
    m_project->save();
    // Defer panel rebuild — we're inside a click handler on the doc-tab's
    // close button. update_all_panels() tears down and rebuilds the tab bar
    // and gallery subtrees, which invalidates the button that just fired
    // this handler. Running it synchronously produces
    //   "Trying to snapshot gtkmm__GtkBox ... without a current allocation"
    // warnings on the next frame. Same S48 / S60 / S61 class of bug.
    Glib::signal_idle().connect_once([this]() {
      if (m_closing)
        return;
      update_all_panels();
    });
    LOG_INFO("Removed document at index {}", idx);
  });

  // Legacy gallery signals (gallery widget kept for thumbnail rendering only;
  // its add/remove/clear signals are no longer the primary path)
  m_gallery.signal_doc_activated().connect(
      sigc::mem_fun(*this, &MainWindow::on_doc_activated));

  m_gallery.signal_add_doc().connect([this]() {
    if (!m_project)
      return;
    // Show canvas settings dialog then add new doc to project.
    // s148 m1: preview colours from active doc (see twin block above
    // at signal_add_doc — same pattern).
    auto *active = m_project->active_doc();
    double pv_ws_r = 0.09, pv_ws_g = 0.09, pv_ws_b = 0.09;
    double pv_ab_r = 0.157, pv_ab_g = 0.157, pv_ab_b = 0.157;
    if (active) {
      pv_ws_r = active->workspace_bg_r;
      pv_ws_g = active->workspace_bg_g;
      pv_ws_b = active->workspace_bg_b;
      pv_ab_r = active->artboard_bg_r;
      pv_ab_g = active->artboard_bg_g;
      pv_ab_b = active->artboard_bg_b;
    }
    m_new_doc_dialog.show(
        *this, ndd_available_themes(),
        m_project->motif == Motif::Light ? templates::MotifTag::Light
                                         : templates::MotifTag::Dark,
        pv_ws_r, pv_ws_g, pv_ws_b, pv_ab_r, pv_ab_g, pv_ab_b,
        [this](std::unique_ptr<CurvzDocument> seed, std::string name,
               std::optional<theme::ThemeId> theme_id) {
          if (!seed)
            return;
          ndd_apply_chosen_theme(*seed, theme_id);

          // Sanitise name → filename stem.
          std::string base = curvz::utils::doc_stem_from_name(name);
          std::string fname = base + ".svg";
          int suffix = 2;
          while (std::any_of(
              m_project->documents.begin(), m_project->documents.end(),
              [&](const auto &d) { return d->filename == fname; }))
            fname = base + std::to_string(suffix++) + ".svg";

          seed->filename = fname;

          m_project->documents.push_back(std::move(seed));
          m_project->active_doc_index = (int)m_project->documents.size() - 1;
          m_project->save();
          update_all_panels();
          LOG_INFO("Added document '{}' to project", fname);
        });
  });

  // Duplicate the active doc. Uses SvgWriter→SvgParser round-trip as the
  // clone seam: every field the writer emits round-trips for free, so we
  // can't forget one (the same lesson as S98 m2 fix1, applied to docs).
  // After parse we mint fresh internal_ids on every node and remap
  // text_path_id references through the old→new map so text-on-path
  // survives. Non-SVG state (canvas model, ruler origin, bg/guide colours,
  // export metadata, measure flags) is copied directly from the source —
  // the SVG round-trip doesn't carry those.
  m_gallery.signal_dup_doc().connect([this](int idx) {
    if (!m_project)
      return;
    if (idx < 0 || idx >= (int)m_project->documents.size())
      return;

    const CurvzDocument &src = *m_project->documents[idx];

    // ── 1. Round-trip the scene tree ──────────────────────────────────
    std::string svg_str = write_svg(src);
    auto dup = parse_svg(svg_str);
    if (!dup) {
      LOG_ERROR("dup_doc: parse_svg failed on round-trip of '{}'",
                src.filename);
      return;
    }

    // ── 2. Regenerate iids; build old→new map for reference remap ─────
    std::unordered_map<std::string, std::string> iid_map;
    std::function<void(SceneNode *)> rewrite_iids = [&](SceneNode *n) {
      if (!n->internal_id.empty()) {
        std::string fresh = generate_internal_id();
        iid_map[n->internal_id] = fresh;
        n->internal_id = fresh;
      } else {
        n->internal_id = generate_internal_id();
      }
      for (auto &ch : n->children)
        rewrite_iids(ch.get());
    };
    for (auto &layer : dup->layers)
      rewrite_iids(layer.get());

    // ── 3. Remap iid-keyed cross-references inside the clone ──────────
    // text_path_id is the only cross-reference Curvz currently keeps as
    // an iid. If more iid-keyed refs land later, extend this walker.
    std::function<void(SceneNode *)> remap_refs = [&](SceneNode *n) {
      if (n->is_text() && !n->text_path_id.empty()) {
        auto it = iid_map.find(n->text_path_id);
        if (it != iid_map.end())
          n->text_path_id = it->second;
        // If not in map, the original ref was already broken — leave it
        // for the parser's stale-ref rescue logic to handle on next load.
      }
      for (auto &ch : n->children)
        remap_refs(ch.get());
    };
    for (auto &layer : dup->layers)
      remap_refs(layer.get());

    // ── 4. Copy non-SVG state from the source ─────────────────────────
    dup->canvas = src.canvas;
    dup->ruler_origin_x = src.ruler_origin_x;
    dup->ruler_origin_y = src.ruler_origin_y;
    dup->active_layer_index = src.active_layer_index;
    dup->snap = src.snap;
    dup->guide_color_r = src.guide_color_r;
    dup->guide_color_g = src.guide_color_g;
    dup->guide_color_b = src.guide_color_b;
    // s116 m6: artboard_bg / workspace_bg / motif are project-scope now;
    // the duplicated doc's legacy fields stay at struct defaults — they
    // are write-on-load-only and not consulted by any paint or theming
    // code post-m6.
    dup->measure_save_to_layer = src.measure_save_to_layer;
    dup->measure_destruct_after_copy = src.measure_destruct_after_copy;
    dup->export_name = src.export_name;
    dup->export_category = src.export_category;

    // ── 4b. Carry grid + margin layers from source ────────────────────
    // SvgWriter deliberately skips Grid/MarginLayer (they live in
    // project.json, not in the SVG), so the round-tripped dup only has
    // whatever defaults parse_svg / ensure_*_layer produce. Mirror them
    // by replacing each special layer in dup with a clone of the
    // source's. Uses clone_node so the field set is centralised — any
    // future grid_/margin_ field added to SceneNode flows through here
    // for free.
    auto replace_layer = [](CurvzDocument &doc, const SceneNode &src_layer) {
      // Find the existing layer of the same type in `doc` and replace
      // the unique_ptr slot. Keeps doc->layers ordering intact (matters
      // for active_layer_index and the layer panel). SceneNode is
      // move-only (it owns unique_ptrs), so we substitute the whole slot
      // rather than copy-assigning fields.
      for (auto &l : doc.layers) {
        if (l->type == src_layer.type) {
          l = clone_node(src_layer);
          return;
        }
      }
      // No existing layer of that type — append a clone.
      doc.layers.push_back(clone_node(src_layer));
    };
    if (const SceneNode *src_grid = src.grid_layer())
      replace_layer(*dup, *src_grid);
    if (const SceneNode *src_margin = src.margin_layer())
      replace_layer(*dup, *src_margin);
    // Regenerate iids on the freshly-cloned grid/margin layer nodes so
    // they don't collide with the source. They typically have no
    // children, so a single-level mint is enough; if that ever changes,
    // route through rewrite_iids() instead.
    for (auto &l : dup->layers) {
      if (l->is_grid_layer() || l->is_margin_layer())
        l->internal_id = generate_internal_id();
    }

    // ── 5. Pick a unique filename ─────────────────────────────────────
    // Source "foo.svg" → "foo copy.svg" → "foo copy 2.svg" → ...
    auto strip_ext = [](const std::string &s) -> std::string {
      if (s.size() > 4 && s.substr(s.size() - 4) == ".svg")
        return s.substr(0, s.size() - 4);
      return s;
    };
    std::string src_stem =
        strip_ext(src.filename.empty() ? "untitled" : src.filename);
    auto exists = [&](const std::string &fname) {
      return std::any_of(m_project->documents.begin(),
                         m_project->documents.end(),
                         [&](const auto &d) { return d->filename == fname; });
    };
    std::string fname = src_stem + " copy.svg";
    int suffix = 2;
    while (exists(fname))
      fname = src_stem + " copy " + std::to_string(suffix++) + ".svg";
    dup->filename = fname;

    // ── 6. Insert immediately after the source, set active, save ──────
    int insert_at = idx + 1;
    m_project->documents.insert(m_project->documents.begin() + insert_at,
                                std::move(dup));
    m_project->active_doc_index = insert_at;
    m_project->save();
    update_all_panels();
    LOG_INFO("Duplicated document '{}' → '{}'", src.filename, fname);
  });

  m_gallery.signal_remove_doc().connect([this](int idx) {
    if (!m_project)
      return;
    if (idx < 0 || idx >= (int)m_project->documents.size())
      return;
    // Delete the SVG file from disk before erasing from the document list.
    if (!m_project->directory.empty()) {
      std::string fname = m_project->documents[idx]->filename;
      if (!fname.empty()) {
        auto svg_path =
            fs::path(m_project->directory) / fs::path(fname).filename();
        std::error_code ec;
        fs::remove(svg_path, ec);
        if (ec)
          LOG_WARN("remove_doc: could not delete '{}': {}", svg_path.string(),
                   ec.message());
        else
          LOG_INFO("remove_doc: deleted '{}'", svg_path.string());
      }
    }
    m_project->documents.erase(m_project->documents.begin() + idx);
    if (m_project->documents.empty()) {
      m_project->active_doc_index = 0;
    } else {
      m_project->active_doc_index =
          std::min(idx, (int)m_project->documents.size() - 1);
    }
    m_project->save();
    // Same deferral as the doc-tab remove path above — we're inside a click
    // handler on a gallery thumbnail's delete button; rebuilding the gallery
    // synchronously yanks that button out from under the event.
    Glib::signal_idle().connect_once([this]() {
      if (m_closing)
        return;
      update_all_panels();
    });
    LOG_INFO("Removed document at index {}", idx);
  });

  m_gallery.signal_clear_all().connect([this]() {
    if (!m_project)
      return;
    // Delete all SVG files from disk before clearing the document list.
    if (!m_project->directory.empty()) {
      for (const auto &doc : m_project->documents) {
        if (!doc->filename.empty()) {
          auto svg_path = fs::path(m_project->directory) /
                          fs::path(doc->filename).filename();
          std::error_code ec;
          fs::remove(svg_path, ec);
          if (ec)
            LOG_WARN("clear_all: could not delete '{}': {}", svg_path.string(),
                     ec.message());
          else
            LOG_INFO("clear_all: deleted '{}'", svg_path.string());
        }
      }
    }
    m_project->documents.clear();
    m_project->active_doc_index = 0;
    m_project->save();
    update_all_panels();
    LOG_INFO("Cleared all documents from project");
  });

  m_gallery.signal_filter_changed().connect(
      [this](std::string query) { m_doc_tabs.set_filter(query); });

  m_gallery.signal_preview_icon().connect(
      [this](std::string path) { on_preview_icon(path); });

  m_gallery.signal_copy_icon().connect(
      [this](std::string path) { on_copy_icon(path); });

  m_properties.signal_prop_changed().connect([this]() {
    LOG_DEBUG("signal_prop_changed: fired — calling queue_draw");
    // Any inspector edit may affect a Blend's A or B — invalidate caches
    // so the next draw regenerates intermediates.
    m_canvas.mark_all_blends_dirty();
    m_canvas.queue_draw();
    m_layers.refresh();
    // s116 m5: motif may have been the edited prop (Motif panel switch);
    // re-apply so the CSS class on the window reflects current state.
    // Idempotent — if motif didn't change, this is a pair of no-op
    // class membership checks.
    apply_motif_to_window();
    // S80 m4c-2: keep the Styles panel's active-binding indicator dot
    // in sync after inspector edits that change bound_style — namely
    // the Style section's Unbind button. Selection is unchanged so the
    // panel's selection-changed hook won't fire. Cheap rebuild.
    m_styles.refresh();
    schedule_save();
    // Keep toolbar well in sync with the selection's appearance. After an
    // inspector edit on a multi-select, run the full uniformity check
    // instead of just syncing from primary — otherwise the toolbar would
    // not clear its mixed-stripe display when a commit unifies everyone.
    m_toolbar.sync_from_selection(m_canvas.selection(),
                                  m_canvas.selected_object());
    LOG_DEBUG("signal_prop_changed: queue_draw done — scheduling gallery idle");
    Glib::signal_idle().connect_once([this]() {
      LOG_DEBUG("signal_prop_changed: gallery idle firing");
      m_gallery.refresh();
      LOG_DEBUG("signal_prop_changed: gallery idle done");
    });
    LOG_DEBUG("signal_prop_changed: handler done");
  });

  // Blend section's Release button → Canvas::release_blend. Primary
  // selection is guaranteed to be a Blend at this point (the button only
  // exists when build_blend_section ran against a Blend).
  m_properties.signal_request_release_blend().connect(
      [this]() { m_canvas.release_blend(); });

  // s146 m2 — Warp section's Release / Flatten buttons. Same single-
  // source-of-truth approach: route to the existing on_warp_release /
  // on_warp_flatten handlers used by the Path menu entries. Selection
  // is guaranteed to be a Warp when these buttons exist.
  m_properties.signal_request_release_warp().connect(
      [this]() { on_warp_release(); });
  m_properties.signal_request_flatten_warp().connect(
      [this]() { on_warp_flatten(); });

  // S91 Inspector → Edit gradient → open the modal GradientDialog.
  // The inspector packages "what to do on Apply" as the apply_cb
  // closure (writes back via mutate_appearance + EditAppearanceCommand
  // + sibling broadcast); we just open the dialog and forward the
  // edited FillStyle to the callback. m_gradient_dialog is a single
  // long-lived window member; show() reseeds it each call.
  m_properties.signal_request_gradient_edit().connect(
      [this](FillStyle current, std::function<void(FillStyle)> apply_cb) {
        m_gradient_dialog.show(*this, current, [apply_cb](FillStyle edited) {
          if (apply_cb)
            apply_cb(std::move(edited));
        });
      });

  // Inspector node coordinate/handle edits — route through canvas so it owns
  // all writes to obj->path. Values arrive in display space; convert to doc
  // space.
  m_properties.signal_request_node_edit().connect(
      [this](int node_idx, double ax, double ay, double ix, double iy,
             double ox, double oy) {
        auto *doc = m_project ? m_project->active_doc() : nullptr;
        if (!doc)
          return;
        const CanvasModel &cm = doc->canvas;
        double rox = doc->ruler_origin_x;
        double roy = doc->ruler_origin_y;
        int q = std::max(1, cm.quality);
        double ch = (double)cm.canvas_height();

        // Display → doc space (inverse of node_doc_to_display_x/y)
        auto to_dx = [&](double disp) -> double {
          double user = disp;
          if (cm.display_mode == DisplayMode::RatioQuality)
            user = disp / 100.0 * q;
          else if (cm.display_mode == DisplayMode::Physical) {
            double sp = std::min(cm.phys_width, cm.phys_height);
            user = sp > 0 ? disp / sp * q : disp;
          }
          return user + rox;
        };
        auto to_dy = [&](double disp) -> double {
          double user = disp;
          if (cm.display_mode == DisplayMode::RatioQuality)
            user = disp / 100.0 * q;
          else if (cm.display_mode == DisplayMode::Physical) {
            double sp = std::min(cm.phys_width, cm.phys_height);
            user = sp > 0 ? disp / sp * q : disp;
          }
          // Y-up display → Y-down doc
          return ch - (user + roy);
        };

        m_canvas.apply_node_edit(node_idx, to_dx(ax), to_dy(ay), to_dx(ix),
                                 to_dy(iy), to_dx(ox), to_dy(oy));
      });

  // Canvas property changes (quality, ratio) from inspector
  m_properties.signal_request_reverse().connect([this]() {
    m_properties.reset_undo_coalesce();
    m_canvas.reverse_selected_path();
    // Inspector needs a refresh so the direction indicator flips
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_request_open_at_node().connect([this]() {
    m_properties.reset_undo_coalesce();
    m_canvas.open_selected_at_node();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_request_split().connect([this]() {
    m_properties.reset_undo_coalesce();
    m_canvas.split_selected_at_node();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_request_node_type_change().connect(
      [this](BezierNode::Type type) {
        m_canvas.set_selected_nodes_type(type);
        Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
      });

  m_properties.signal_request_flip().connect(
      [this](bool horizontal) { m_canvas.flip_selection(horizontal); });

  m_properties.signal_request_detach_text().connect([this](SceneNode *node) {
    if (!node)
      return;
    // Ensure the node is in the canvas selection so release_text_from_path
    // finds it
    m_canvas.set_selection_single(node);
    m_canvas.release_text_from_path();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  m_properties.signal_canvas_changed().connect([this](CanvasModel cm) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    doc->canvas = cm;
    m_canvas.zoom_fit();
    m_canvas.queue_draw();
    schedule_save();
    // s150: status bar carries the active unit; refresh on any
    // canvas change (the Units dropdown is the trigger that matters,
    // but other CanvasModel edits land here too — cheap to refresh).
    update_title();
    LOG_INFO("Canvas changed: {}×{} quality={} ratio={:.4g}:{:.4g}",
             cm.canvas_width(), cm.canvas_height(), cm.quality, cm.ratio_w,
             cm.ratio_h);
  });

  m_properties.signal_doc_renamed().connect([this](std::string new_name) {
    if (!m_project)
      return;
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    rename_doc(doc, std::move(new_name));
  });

  // Gallery: double-click a tile (or right-click → Rename) to rename. The
  // gallery passes the doc index explicitly so any doc can be renamed
  // without first activating it.
  m_gallery.signal_rename_doc().connect([this](int idx, std::string new_name) {
    if (!m_project)
      return;
    if (idx < 0 || idx >= (int)m_project->documents.size())
      return;
    rename_doc(m_project->documents[idx].get(), std::move(new_name));
  });

  m_properties.signal_request_canvas_focus().connect(
      [this]() { m_canvas.grab_focus(); });

  m_canvas.signal_zoom_changed().connect([this](double zoom) {
    m_statusbar.set_zoom(zoom * 100.0);
    m_toolbar.set_zoom(zoom);
    update_rulers();
  });

  // Zoom tool Alt feedback — swap toolbar icon and update context bar
  m_canvas.signal_zoom_alt_changed().connect([this](bool alt_down) {
    m_toolbar.set_active_tool_icon(ActiveTool::Zoom,
                                   alt_down ? "zoom-out-symbolic"
                                            : "system-search-symbolic");
    m_toolbar.set_zoom_alt(alt_down);
  });

  m_canvas.signal_cursor_moved().connect([this](double x, double y) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    CoordSpace cs{(double)doc->canvas_height()};
    // Apply ruler origin offset to status bar display
    double ux = cs.to_user_x(x) - doc->ruler_origin_x;
    double uy = cs.to_user_y(y) - doc->ruler_origin_y;
    m_statusbar.set_cursor_pos(ux, uy);
    // Update cursor marker in rulers (cheap — just update cursor fields)
    update_rulers();
    // Continuously sync selection panel position during drag
    if (m_canvas.is_dragging() && m_canvas.selected_object() &&
        (m_active_tool == ActiveTool::Selection ||
         m_active_tool == ActiveTool::Node)) {
      m_properties.sync_selection(m_canvas.selected_object());
    }
  });

  // Corner square — set new ruler origin
  m_corner.signal_origin_changed().connect([this](double ux, double uy) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;

    // Snap origin to the full snap stack (guides, refs, grid, margins).
    // The corner-square emits user-space coords; Canvas::snap_x/snap_y
    // operate in doc space, so flip Y, snap, flip back. snap_x/snap_y
    // honour the global Snap toggle (doc->snap.enabled) and per-class
    // flags — when snap is off they pass the input through unchanged.
    const double ch = (double)doc->canvas_height();
    double doc_x = ux;
    double doc_y = ch - uy;
    doc_x = m_canvas.snap_x(doc_x);
    doc_y = m_canvas.snap_y(doc_y);
    ux = doc_x;
    uy = ch - doc_y;

    doc->ruler_origin_x = ux;
    doc->ruler_origin_y = uy;
    update_rulers();
    schedule_save();
    m_canvas.grab_focus();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
    LOG_INFO("Ruler origin set to ({:.2f}, {:.2f}) user space", ux, uy);
  });

  // Live drag preview — show dashed lines on rulers
  m_corner.signal_origin_preview().connect([this](double ux, double uy) {
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    if (!doc)
      return;

    // Apply the same full snap as the commit handler so the preview
    // shows what will land. user-space → doc-space → snap → user-space.
    const double ch = (double)doc->canvas_height();
    {
      double doc_x = ux;
      double doc_y = ch - uy;
      doc_x = m_canvas.snap_x(doc_x);
      doc_y = m_canvas.snap_y(doc_y);
      ux = doc_x;
      uy = ch - doc_y;
    }

    RulerState rs;
    rs.zoom = m_canvas.zoom();
    rs.pan_x = m_canvas.pan_x();
    rs.pan_y = m_canvas.pan_y();
    rs.widget_w = (double)m_canvas.get_width();
    rs.widget_h = (double)m_canvas.get_height();
    rs.cursor_doc_x = m_canvas.cursor_doc_x();
    rs.cursor_doc_y = m_canvas.cursor_doc_y();
    rs.canvas_w = (double)doc->canvas_width();
    rs.canvas_h = (double)doc->canvas_height();
    rs.quality = doc->canvas.quality;
    rs.display_mode = doc->canvas.display_mode;
    rs.ruler_origin_x = doc->ruler_origin_x;
    rs.ruler_origin_y = doc->ruler_origin_y;
    rs.phys_short = std::min(doc->canvas.phys_width, doc->canvas.phys_height);
    rs.phys_unit = doc->canvas.phys_unit;
    rs.display_unit = doc->canvas.display_unit;
    rs.preview_active = true;
    rs.preview_origin_x = ux;
    rs.preview_origin_y = uy;
    m_hruler.set_state(rs);
    m_vruler.set_state(rs);
    m_canvas.set_origin_preview(ux, uy);
  });

  // Drag ended — clear preview
  m_corner.signal_preview_stop().connect([this]() {
    m_canvas.clear_origin_preview();
    update_rulers();
  });

  // Corner square double-click — reset origin to 0,0 (artboard bottom-left)
  m_corner.signal_origin_reset().connect([this]() {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    doc->ruler_origin_x = 0.0;
    doc->ruler_origin_y = 0.0;
    update_rulers();
    schedule_save();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
    LOG_INFO("Ruler origin reset to 0,0");
  });

  // HRuler — drag downward creates horizontal guide (constant Y).
  // doc_y comes from the ruler in raw doc space; snap_y routes through
  // the full snap stack (guides, refs, grid, margins) and respects the
  // global Snap toggle.
  m_hruler.signal_guide_dragging().connect([this](double doc_y) {
    doc_y = m_canvas.snap_y(doc_y);
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_y, true);
    else
      m_canvas.update_guide_drag(doc_y);
  });
  m_hruler.signal_guide_created().connect([this](double doc_y) {
    doc_y = m_canvas.snap_y(doc_y);
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_y, true);
    m_canvas.end_guide_drag(doc_y);
    schedule_save();
  });
  m_hruler.signal_guide_drag_cancel().connect(
      [this]() { m_canvas.cancel_guide_drag(); });

  // VRuler — drag rightward creates vertical guide (constant X). Same
  // snap pattern as HRuler but on the X axis.
  m_vruler.signal_guide_dragging().connect([this](double doc_x) {
    doc_x = m_canvas.snap_x(doc_x);
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_x, false);
    else
      m_canvas.update_guide_drag(doc_x);
  });
  m_vruler.signal_guide_created().connect([this](double doc_x) {
    doc_x = m_canvas.snap_x(doc_x);
    if (!m_canvas.guide_drag_active())
      m_canvas.begin_guide_drag(doc_x, false);
    m_canvas.end_guide_drag(doc_x);
    schedule_save();
  });
  m_vruler.signal_guide_drag_cancel().connect(
      [this]() { m_canvas.cancel_guide_drag(); });

  // Unit change from right-click on either ruler
  auto on_unit_changed = [this](Unit u) {
    auto *doc = m_project->active_doc();
    if (!doc)
      return;
    doc->canvas.display_unit = u;
    schedule_save();
    update_rulers();
    m_properties.refresh(&doc->canvas, nullptr);
    m_toolbar.set_document(doc);
    m_corner.set_unit(u);
    {
      Unit cu = (doc->canvas.display_mode == DisplayMode::Physical)
                    ? UnitSystem::parse_unit(doc->canvas.phys_unit)
                    : u;
      std::string unit_str = UnitSystem::label(cu);
      m_corner_unit_label.set_text("Units: " + unit_str);
    }
  };
  m_hruler.signal_unit_changed().connect(on_unit_changed);
  m_vruler.signal_unit_changed().connect(on_unit_changed);

  // Context bar actions
  m_toolbar.signal_fit_requested().connect([this]() { m_canvas.zoom_fit(); });

  // Zoom-to-exact-value from the right-click popover Apply button.
  // target is a raw zoom factor (1.0 = 100% = 1:1 pixels).
  m_toolbar.signal_zoom_to().connect([this](double target) {
    double cx = m_canvas.get_width() / 2.0;
    double cy = m_canvas.get_height() / 2.0;
    double current = m_canvas.zoom();
    if (current > 0.0)
      m_canvas.zoom_toward(cx, cy, target / current);
  });
  m_toolbar.signal_request_canvas_focus().connect(
      [this]() { m_canvas.grab_focus(); });
  m_toolbar.signal_snap_toggled().connect([this](bool enabled) {
    if (!m_project)
      return;
    m_project->snap.enabled = enabled;
    // Apply to all documents
    for (auto &doc : m_project->documents)
      doc->snap.enabled = enabled;
    schedule_save();
  });

  m_toolbar.signal_snap_pop_open().connect([this]() {
    if (m_project)
      m_toolbar.set_snap_settings(m_project->snap);
  });

  m_toolbar.signal_snap_settings_changed().connect([this](SnapSettings s) {
    if (!m_project)
      return;
    m_project->snap = s;
    // Apply to all documents so Canvas always reads correct values
    for (auto &doc : m_project->documents)
      doc->snap = s;
    m_canvas.queue_draw();
    schedule_save();
    Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
  });

  // s150 — Ruler / Measure settings popover signal. Toolbar wrote
  // directly to doc->measure_* already; we just need to schedule_save.
  // No project mirror, no cross-doc apply (measure prefs are per-doc).
  // No inspector refresh (the inspector Measure section was deleted in
  // s150; no remaining UI surface to refresh).
  m_toolbar.signal_measure_settings_changed().connect([this]() {
    if (!m_project)
      return;
    schedule_save();
  });

  // ── Align & Distribute ───────────────────────────────────────────────────
  m_toolbar.signal_align_requested().connect(
      [this](AlignOp op) { m_canvas.align_selection(op); });

  // s152 — Toolbar density picker. The user right-clicks on the toolbar
  // background, picks a density from the popover; signal_density_changed
  // fires and we persist the choice through AppPreferences. The
  // Toolbar's CSS class flip already applied the visual change before
  // the signal fires, so this connection is purely for persistence —
  // no need to call set_density() back on the toolbar.
  m_toolbar.signal_density_changed().connect([](Toolbar::Density d) {
    AppPreferences::instance().set_toolbar_density(static_cast<int>(d));
  });

  m_toolbar.signal_macro_manager().connect(
      sigc::mem_fun(*this, &MainWindow::on_macro_manager));

  // Toolbar macro button left-click: run the current macro. Mirrors
  // the Ctrl+M keyboard path — on_run_macro() opens the manager itself
  // if there's no current macro or the current macro is empty.
  m_toolbar.signal_macro_run().connect([this]() {
    Macro *cur = MacroManager::instance().current_macro();
    on_run_macro(cur ? cur->internal_id : std::string(""));
  });

  // ── s154 m1: Transforms section (SnR, Blend, Bool, Warp) ────────────
  // Toolbar buttons route through the same handlers the Path-menu items
  // do. The toolbar's faked-disabled (set_transforms_enabled, mirrored
  // from the m_act_*->property_enabled() bits in refresh_inspector)
  // already gates left-click — so by the time the signal fires, the
  // operation is known to be legal. Direct handler calls match the
  // codebase idiom (no GIO-action round-trip).
  //
  // s154 m2: SnR left-click no longer opens the popover. It calls
  // apply_step_repeat_with_last() — uses the persisted last values
  // and fires immediately. The popover itself is summoned via
  // signal_step_repeat_configure (right-click) or the Path menu item.
  // s154 m3: Blend follows the same shape — left-click goes through
  // apply_blend_with_last(), which falls back to opening the popover
  // when node counts mismatch (validation requires the warning banner).
  m_toolbar.signal_step_repeat().connect(
      [this]() { apply_step_repeat_with_last(); });
  m_toolbar.signal_step_repeat_configure().connect(
      [this]() { on_step_repeat(); });
  m_toolbar.signal_blend().connect(
      [this]() { apply_blend_with_last(); });
  m_toolbar.signal_blend_configure().connect(
      [this]() { on_blend(); });
  m_toolbar.signal_warp().connect(
      [this]() { on_warp_make(); });
  // s154 m4a: Warp right-click → open WarpPopover (defaults editor).
  // Coexists with the inspector's Application ▸ Warp subsection for
  // a comparison trial — both edit AppPreferences, last write wins.
  m_toolbar.signal_warp_configure().connect(
      [this]() { m_warp_popover.show(m_toolbar.warp_button()); });
  m_toolbar.signal_bool_op().connect([this](BoolOp op) {
    switch (op) {
      case BoolOp::Union:
        m_canvas.boolean_op(BooleanOpType::Union); break;
      case BoolOp::Subtract:
        m_canvas.boolean_op(BooleanOpType::Subtract); break;
      case BoolOp::Intersect:
        m_canvas.boolean_op(BooleanOpType::Intersect); break;
    }
  });

  // Enable the align button whenever the selection tool is active
  // and 2+ objects are selected.
  // s135 m1: also flip the eight Align/Distribute SimpleActions so the
  // menu items grey out in lockstep with the toolbar button. Single
  // predicate, two enable surfaces — same shape as how the boolean ops
  // sensitivity pump works.
  auto update_align_btn = [this]() {
    bool ok = (m_active_tool == ActiveTool::Selection) &&
              (m_canvas.selection().size() >= 2);
    m_toolbar.set_align_enabled(ok);
    if (m_act_align_left)
      m_act_align_left->set_enabled(ok);
    if (m_act_align_center_h)
      m_act_align_center_h->set_enabled(ok);
    if (m_act_align_right)
      m_act_align_right->set_enabled(ok);
    if (m_act_align_top)
      m_act_align_top->set_enabled(ok);
    if (m_act_align_center_v)
      m_act_align_center_v->set_enabled(ok);
    if (m_act_align_bottom)
      m_act_align_bottom->set_enabled(ok);
    if (m_act_distribute_h)
      m_act_distribute_h->set_enabled(ok);
    if (m_act_distribute_v)
      m_act_distribute_v->set_enabled(ok);
  };

  // s138 fix: canvas selection-change must also refresh the inspector so
  // action sensitivity (warp / blend / clip / bool) updates. Pre-fix this
  // lambda only updated the align toolbar button; the four sensitivity
  // helpers ran from refresh_inspector() at other call sites (tool change,
  // scene mutation, document switch) but NOT on plain canvas-click
  // selection changes. Net effect: clicking a path on the canvas would
  // leave Warp Make / Blend Make / Clip / Boolean ops disabled until some
  // unrelated event happened to refresh the inspector. refresh_inspector
  // already includes update_align_btn-equivalent work via its panel
  // refresh, so the explicit call below is belt-and-braces — keep both.
  m_canvas.signal_selection_changed().connect(
      [this, update_align_btn](SceneNode *) {
        update_align_btn();
        refresh_inspector();
      });

  // Also re-check on tool change (handled in on_tool_changed, but we store
  // the lambda so we can call it there too).
  // Store it in a shared_ptr so on_tool_changed can invoke it.
  m_update_align_btn = update_align_btn;

  m_toolbar.signal_zoom_step().connect([this](double dir) {
    // Zoom ladder expressed as fit-zoom multipliers.
    // 1.0 = artboard fills the viewport (Ctrl+0 / zoom_fit).
    // Values below 1.0 zoom out; above 1.0 zoom in.
    // Upper end extends to 128× for high-quality documents
    // (quality=40000 billboard → you still want to see individual units).
    static const double steps[] = {
        0.0125, 0.025, 0.05, 0.10, 0.15, 0.20, 0.25, 0.33,  0.50, 0.67, 0.75,
        1.0,    1.25,  1.5,  2.0,  3.0,  4.0,  5.0,  6.0,   8.0,  10.0, 12.0,
        16.0,   20.0,  24.0, 32.0, 48.0, 64.0, 96.0, 128.0, 192.0};
    static constexpr int N = sizeof(steps) / sizeof(steps[0]);

    // Current zoom expressed as fit-relative multiplier — same units as the
    // ladder.
    double fit = m_canvas.fit_zoom_value();
    double cur = (fit > 0.0) ? (m_canvas.zoom() / fit) : 1.0;

    int idx = 0;
    if (dir > 0) {
      for (int i = 0; i < N; ++i)
        if (steps[i] > cur * 1.02) {
          idx = i;
          goto found;
        }
      idx = N - 1;
    } else {
      for (int i = N - 1; i >= 0; --i)
        if (steps[i] < cur * 0.98) {
          idx = i;
          goto found;
        }
      idx = 0;
    }
  found:
    // Convert the target ladder step back to a raw zoom factor for zoom_toward.
    double target_raw = steps[idx] * fit;
    double cx = m_canvas.get_width() / 2.0;
    double cy = m_canvas.get_height() / 2.0;
    m_canvas.zoom_toward(cx, cy, target_raw / m_canvas.zoom());
  });

  // Helper: convert a popover value from the document's display unit to
  // doc-units (quality pixels). place_*_precise expects doc-units.
  // In physical mode, popover values are in phys_unit (e.g. inches at 300dpi),
  // and 1 doc-unit = phys_short / quality physical units.
  m_toolbar.signal_place_ref().connect(
      [this](double ux, double uy) { m_canvas.place_ref_at_display(ux, uy); });
  m_toolbar.signal_place_rect().connect(
      [this](double x, double y, double w, double h) {
        m_canvas.place_rect_precise(pop_to_px(x), pop_to_px(y), pop_to_px(w),
                                    pop_to_px(h));
      });
  m_toolbar.signal_place_ellipse().connect(
      [this](double cx, double cy, double rx, double ry) {
        m_canvas.place_ellipse_precise(pop_to_px(cx), pop_to_px(cy),
                                       pop_to_px(rx), pop_to_px(ry));
      });
  m_toolbar.signal_place_polygon().connect(
      [this](double cx, double cy, double radius, int sides, double inflection,
             double angle_rad) {
        m_canvas.set_polygon_settings(sides, inflection);
        m_canvas.place_polygon_precise(pop_to_px(cx), pop_to_px(cy),
                                       pop_to_px(radius), sides, inflection,
                                       angle_rad);
      });
  m_toolbar.signal_place_spiral().connect(
      [this](double cx, double cy, double outer_r, double inner_r, double turns,
             double angle_rad) {
        m_canvas.set_spiral_settings(
            turns, (outer_r > 0.0) ? (inner_r / outer_r * 100.0) : 0.0);
        m_canvas.place_spiral_precise(pop_to_px(cx), pop_to_px(cy),
                                      pop_to_px(outer_r), pop_to_px(inner_r),
                                      turns, angle_rad);
      });
  m_toolbar.signal_place_line().connect(
      [this](double x1, double y1, double x2, double y2) {
        m_canvas.place_line_precise(pop_to_px(x1), pop_to_px(y1), pop_to_px(x2),
                                    pop_to_px(y2));
      });
  m_toolbar.signal_place_text().connect(
      [this](double x, double y, std::string family, double size_pt, bool bold,
             bool italic, std::string anchor, std::string align) {
        // x, y: display units → doc px via pop_to_px
        // size: always in points → convert pt→doc px
        auto *doc = m_project ? m_project->active_doc() : nullptr;
        double size_doc = size_pt / 72.0 * 96.0; // fallback: screen dpi
        if (doc) {
          const CanvasModel &cm = doc->canvas;
          if (cm.display_mode == DisplayMode::Physical && cm.quality > 0) {
            double phys_short = std::min(cm.phys_width, cm.phys_height);
            if (phys_short > 0)
              size_doc = size_pt / 72.0 / phys_short * cm.quality;
          } else {
            size_doc = size_pt / 72.0 * 96.0; // pt → screen px (96dpi)
          }
        }
        m_canvas.place_text_precise(pop_to_px(x), pop_to_px(y), family,
                                    size_doc, bold, italic, anchor, align);
      });

  m_toolbar.signal_defaults_changed().connect([this](FillStyle fill,
                                                     StrokeStyle stroke) {
    LOG_DEBUG("[s153 diag] MainWindow defaults_changed: ENTRY  fill.type={} "
              "stroke.type={}",
              (int)fill.type, (int)stroke.paint.type);
    m_canvas.set_default_style(fill, stroke);

    // Apply to the full selection (multi-select aware).
    // For groups and composites, recurse into all path descendants.
    const auto &sel = m_canvas.selection();
    SceneNode *primary = m_canvas.selected_object();

    // Gather every leaf paint target that should receive the new appearance.
    // S58g: Compound is treated as a TERMINAL target (its own fill/stroke
    // are the canonical paint per the S58d rule, and the Canvas renderer
    // reads them directly). Group remains a pass-through container —
    // setting fill/stroke on a Group broadcasts to its descendants.
    std::vector<SceneNode *> targets;
    std::function<void(SceneNode *)> collect = [&](SceneNode *node) {
      if (!node)
        return;
      if (node->is_path() || node->is_compound() ||
          node->type == SceneNode::Type::Text) {
        targets.push_back(node);
      } else if (node->is_group()) {
        for (auto &child : node->children)
          collect(child.get());
      }
    };

    if (!sel.empty()) {
      for (SceneNode *obj : sel)
        collect(obj);
    } else if (primary) {
      collect(primary);
    }

    if (targets.empty()) {
      LOG_DEBUG(
          "[s153 diag] MainWindow defaults_changed: targets.empty, EXIT-early");
      return;
    }

    auto composite = std::make_unique<CompositeCommand>("Edit appearance");
    for (SceneNode *node : targets) {
      // S82 m4f: capture pre-edit swatch ids before the funnel clears
      // them; capture post-edit ids after the funnel runs. Reordered
      // from push-then-mutate to mutate-then-push so the after snapshot
      // reflects the funnel's break-on-override result. Same pattern as
      // PropertiesPanel broadcast sites.
      // S92 m3: same shape for bound_style — funnel clears it on every
      // direct mutate_appearance call.
      FillStyle fb = node->fill;
      StrokeStyle sb = node->stroke;
      std::string fsib = node->fill_swatch_id;
      std::string ssib = node->stroke_swatch_id;
      std::string bsb = node->bound_style;
      // Inspector-driven appearance edit is a user override — break any
      // Style binding on the target (S74 m2).
      Curvz::style::mutate_appearance(*node, [&](SceneNode &n) {
        n.fill = fill;
        n.stroke = stroke;
      });
      std::string fsia = node->fill_swatch_id;
      std::string ssia = node->stroke_swatch_id;
      std::string bsa = node->bound_style;
      // s168 m1 DIAG — STRIP after triage
      LOG_INFO("[IIDDIAG] EditAppearance::push (defaults broadcast) "
               "iid='{}' obj_name='{}' obj_type={}",
               node->internal_id, node->name, (int)node->type);
      composite->add(std::make_unique<EditAppearanceCommand>(
          m_project.get(), node->internal_id,
          std::move(fb), std::move(sb), fill, stroke, std::move(fsib),
          std::move(ssib), std::move(fsia), std::move(ssia), std::move(bsb),
          std::move(bsa)));
    }
    m_history.push(std::move(composite));
    m_canvas.queue_draw();
    LOG_DEBUG("[s153 diag] MainWindow defaults_changed: targets={} pushed "
              "history; scheduling idle refresh_inspector",
              targets.size());
    Glib::signal_idle().connect_once([this]() {
      LOG_DEBUG("[s153 diag] MainWindow defaults_changed idle: "
                "refresh_inspector starting");
      refresh_inspector();
      LOG_DEBUG("[s153 diag] MainWindow defaults_changed idle: "
                "refresh_inspector done");
    });
    LOG_DEBUG("[s153 diag] MainWindow defaults_changed: EXIT");
  });

  // S91 Toolbar → Edit gradient → open the modal GradientDialog (same
  // instance the inspector uses). The toolbar packages "what to do on
  // Apply" as the apply_cb closure (writes back to m_def_fill / m_def_
  // stroke.paint and re-emits defaults_changed); this side just opens
  // the dialog and forwards the result.
  m_toolbar.signal_gradient_edit_requested().connect(
      [this](FillStyle current, std::function<void(FillStyle)> apply_cb) {
        m_gradient_dialog.show(*this, current, [apply_cb](FillStyle edited) {
          if (apply_cb)
            apply_cb(std::move(edited));
        });
      });

  m_canvas.signal_selection_changed().connect([this](GlyphObject *obj) {
    // Deduplicate — skip if the inspector already shows this object for the
    // same tool.  A tool switch (e.g. Selection → TextOnPath) requires a
    // rebuild even when the selected object pointer hasn't changed.
    // TextOnPath is never deduplicated: phase transitions (0→1→2) keep the
    // same selected object pointer but require an inspector rebuild each time.
    //
    // S58m: the selection *size* is also part of dedup identity. Shift-click
    // add/remove keeps the primary pointer stable but changes the set —
    // without the size check, Appearance's mixed/uniform display stayed
    // stale until the user reselected everything.
    size_t sel_size_now = m_canvas.selection().size();
    bool same = (m_active_tool != ActiveTool::TextOnPath) &&
                (m_properties.current_object() == obj) &&
                (m_inspector_tool == m_active_tool) &&
                (sel_size_now == m_prev_selection_size);
    if (same) {
      LOG_DEBUG(
          "selection_changed: same object={} tool={} size={}, skip rebuild",
          (void *)obj, (int)m_active_tool, sel_size_now);
      return;
    }
    m_prev_selection_size = sel_size_now;
    LOG_DEBUG("selection_changed: new obj={} tool={} size={}, scheduling idle "
              "rebuild",
              (void *)obj, (int)m_active_tool, sel_size_now);
    Glib::signal_idle().connect_once([this, obj]() {
      if (m_closing)
        return;
      // s156 — pointer-validity guard. The captured `obj` may have been
      // freed by a destructive op (e.g. cross-path Join) that ran after
      // this idle was queued but before it dispatched. Reading state off
      // a freed SceneNode hits undefined behaviour and produced
      // std::bad_array_new_length crashes during the join arc.
      // is_node_alive walks slot pointers without dereffing `obj`.
      if (obj && !m_canvas.is_node_alive(obj)) {
        LOG_DEBUG("selection_changed idle: captured obj={} no longer in doc, "
                  "skip", (void *)obj);
        return;
      }
      size_t sel_size_now2 = m_canvas.selection().size();
      bool same2 = (m_active_tool != ActiveTool::TextOnPath) &&
                   (m_properties.current_object() == obj) &&
                   (m_inspector_tool == m_active_tool) &&
                   (sel_size_now2 == m_prev_selection_size);
      if (same2) {
        LOG_DEBUG("selection_changed idle: already showing obj={} tool={} "
                  "size={}, skip",
                  (void *)obj, (int)m_active_tool, sel_size_now2);
        return;
      }
      LOG_DEBUG("selection_changed idle: rebuilding for obj={} tool={} size={}",
                (void *)obj, (int)m_active_tool, sel_size_now2);
      refresh_inspector();
      // S58n: give the toolbar the whole selection so it can detect mixed
      // fill/stroke and render diagonal-stripe swatches. Previously passed
      // only the primary's paint, which left the toolbar unable to tell a
      // uniform selection from a mixed one.
      m_toolbar.sync_from_selection(m_canvas.selection(), obj);
      // Sync layers panel highlight to canvas selection
      m_layers.set_canvas_selection(m_canvas.selection());
    });
  });

  m_canvas.signal_node_changed().connect([this](SceneNode *obj, int node_idx) {
    if (m_closing)
      return;
    auto *doc = m_project ? m_project->active_doc() : nullptr;
    CanvasModel *cm = doc ? &doc->canvas : nullptr;
    // Fast-path is safe to call synchronously — it only calls set_value() on
    // existing adjustments, no widget tree changes.
    // Full rebuild (new obj, new node, OR type changed) MUST be deferred via
    // idle — GTK4 must not rebuild the widget tree inside a live signal.
    bool same_obj =
        (obj && node_idx >= 0 && obj == m_properties.current_object() &&
         node_idx == m_properties.current_node());
    bool type_same =
        same_obj && m_properties.current_node_type_matches(obj, node_idx);
    if (same_obj && type_same) {
      m_properties.refresh_node(cm, obj,
                                node_idx); // fast path — no idle needed
    } else {
      Glib::signal_idle().connect_once([this, cm, obj, node_idx]() {
        if (m_closing)
          return;
        // s156 — pointer-validity guard (see selection_changed idle above
        // for full rationale). Cross-path Join frees path B between the
        // shift+click that queued this idle and the J-press that ran the
        // destructive op; without this guard, refresh_node derefs a freed
        // SceneNode and the renderer / spinbutton refresh hits
        // std::bad_array_new_length.
        if (obj && !m_canvas.is_node_alive(obj)) {
          LOG_DEBUG("node_changed idle: captured obj={} no longer in doc, "
                    "skip", (void *)obj);
          return;
        }
        m_properties.refresh_node(cm, obj, node_idx);
      });
    }
  });

  m_canvas.signal_document_changed().connect([this]() {
    // Canvas-driven mutations (drag-move, node drags, transform handles,
    // structural edits) come through doc_changed — not prop_changed.
    // Invalidate blend caches here too so live regen works for A/B
    // edited directly on canvas, not just via the inspector.
    m_canvas.mark_all_blends_dirty();
    refresh_status_counts(); // s132 m2 — replaces hand-rolled loop
    m_gallery.refresh();
    m_doc_tabs.refresh();
    // Refresh layers panel so new object appears
    m_layers.refresh();
    // Persist structural changes (e.g. text-to-path link UUID) to disk
    schedule_save();
    // Sync inspector selection position/size when object moves
    if (m_canvas.selected_object() && (m_active_tool == ActiveTool::Selection ||
                                       m_active_tool == ActiveTool::Node)) {
      m_properties.sync_selection(m_canvas.selected_object());
    }
  });

  // ── Guide selection — three-way sync: canvas ↔ layers ↔ inspector ──────────
  // Canvas → layers + inspector
  m_canvas.signal_guide_selection_changed().connect(
      [this](std::vector<SceneNode *> sel) {
        m_layers.set_guide_selection(sel);
        m_properties.set_guide_selection(sel);
        Glib::signal_idle().connect_once([this]() {
          if (m_closing)
            return;
          refresh_inspector();
        });
      });

  // LayersPanel → canvas + inspector
  m_layers.signal_guide_selection_changed().connect(
      [this](std::vector<SceneNode *> sel) {
        LOG_DEBUG("MainWindow: layers signal_guide_selection_changed size={}",
                  sel.size());
        m_canvas.set_guide_selection(sel);
        m_properties.set_guide_selection(sel);
        Glib::signal_idle().connect_once([this]() {
          if (m_closing)
            return;
          LOG_DEBUG("MainWindow: guide selection idle refresh_inspector");
          refresh_inspector();
        });
      });

  // Inspector delete emits selection-changed with empty set — canvas + layers
  // update
  m_properties.signal_guide_selection_changed().connect(
      [this](std::vector<SceneNode *> sel) {
        m_canvas.set_guide_selection(sel);
        m_layers.set_guide_selection(sel);
        m_canvas.queue_draw();
      });

  // Guide added/deleted from inspector — refresh layers panel and canvas.
  // Also sync guide selection state: PropertiesPanel already cleared its own
  // m_guide_selection; tell canvas and layers to match.
  m_properties.signal_guide_layer_changed().connect([this]() {
    m_canvas.set_guide_selection(m_properties.guide_selection());
    m_layers.set_guide_selection(m_properties.guide_selection());
    m_layers.refresh();
    m_canvas.queue_draw();
  });

  // Guide construct from inspector "From 2 points…" button.  Begins the
  // canvas-side mode and arranges for the review dialog to open after the
  // user clicks the second point.
  m_properties.signal_guide_construct_requested().connect([this]() {
    m_canvas.set_guide_construct_review_callback(
        [this]() { open_guide_review_dialog(); });
    m_canvas.begin_guide_construct();
    m_canvas.grab_focus();
  });

  m_canvas.signal_request_tool().connect([this](ActiveTool tool) {
    m_toolbar.select_tool(tool);
    on_tool_changed(tool);
  });

  // s125 m1a: canvas right-click → "Save to Library…" emits this. We open
  // the same folder picker that LibraryPanel's + button uses, then route
  // through on_save_selection_to_library. Two callers (the panel's + and
  // this) currently duplicate the picker setup; if a third caller appears,
  // extract — for now duplication is cheaper than the abstraction.
  m_canvas.signal_request_save_to_library().connect(
      [this]() { on_request_save_selection_to_library(); });

  // Non-blocking info/warning messages emitted by Canvas (e.g. boolean op
  // skips)
  m_canvas.signal_show_message().connect(
      [this](std::string title, std::string body) {
        curvz::utils::show_alert(*this, title, body);
      });

  // s113 m3: sync UI state when outline mode flips for any reason —
  // manual toggle, keyboard E, settings restore, or the m3 zoom-safety
  // auto-flip. Single sync seam means future outline-mode change paths
  // pick up the sync automatically.
  m_canvas.signal_outline_mode_changed().connect([this]() {
    const bool om = m_canvas.is_outline_mode();
    m_statusbar.set_mode(om ? "Outline" : "Preview");
    if (auto act = lookup_action("toggle-outline")) {
      auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act);
      if (sa)
        sa->set_state(Glib::Variant<bool>::create(om));
    }
  });

  // After an eyedropper pick, sync the toolbar well so the sampled colour
  // becomes the new fill or stroke default shown in the well.
  m_canvas.signal_eyedropper_pick().connect(
      [this](FillStyle color, bool to_stroke) {
        FillStyle new_fill = to_stroke ? m_toolbar.default_fill() : color;
        StrokeStyle new_stroke = m_toolbar.default_stroke();
        if (to_stroke)
          new_stroke.paint = color;
        m_toolbar.sync_from_object(new_fill, new_stroke);
      });

  m_canvas.signal_corner_sel_changed().connect(
      [this](int count) { show_corner_panel(count > 0); });

  // Keyboard shortcuts — attached to MainWindow so they work regardless of
  // which child widget has focus.
  auto keys = Gtk::EventControllerKey::create();

  keys->signal_key_pressed().connect(
      [this](guint kv, guint, Gdk::ModifierType mod) -> bool {
        bool ctrl =
            (mod & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
        bool shift =
            (mod & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
        bool alt = (mod & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};

        // ── Alt press — forward to canvas for zoom cursor/icon ─────────────
        // The Canvas key controller only fires when the canvas has focus, which
        // is unreliable.  Handle Alt here at the window level instead.
        if (kv == GDK_KEY_Alt_L || kv == GDK_KEY_Alt_R) {
          m_canvas.notify_alt_pressed();
          return false; // never consume Alt — other handlers may need it
        }

        // ── Text-input focus guard ──────────────────────────────────────────
        // In CAPTURE phase we see all keypresses. Skip non-Ctrl shortcuts when
        // a text-entry widget (SpinButton, Entry, CurvzEntry, the inner GtkText
        // that GTK4 actually focuses inside an Entry, etc.) has keyboard focus
        // so normal typing/editing is not disrupted.
        //
        // s157: use GTK_IS_EDITABLE — the GTK4 interface every text-input
        // widget implements. This catches everything by interface rather
        // than by enumerating wrapper types via dynamic_cast (which can
        // fail when get_focus() returns the inner GtkText with no gtkmm
        // wrapper).
        //
        // NOTE: this guard only protects against shortcuts wired here in
        // the CAPTURE controller. set_accels_for_action() dispatch in
        // Gio::Application runs AFTER this controller returns false, so
        // any bare-key accelerator registered there will hijack text
        // input regardless of this guard. Bare-key accels for zoom etc.
        // were stripped in s157 from Application::on_startup for that
        // reason.
        auto *focused = get_focus();
        bool text_focused = focused && GTK_IS_EDITABLE(focused->gobj());

        // Ctrl shortcuts always fire regardless of focus
        if (!ctrl) {
          if (text_focused)
            return false;
        }

        // ── Space press — forward to canvas for Space+drag pan ──────────────
        // In Ruler tool, space clears the current measurement for a fresh pick.
        if (kv == GDK_KEY_space) {
          if (m_canvas.active_tool() == ActiveTool::Measure) {
            m_canvas.ruler_clear();
            return true;
          }
          m_canvas.notify_space_pressed();
          return true;
        }

        // ── Shortcuts dialog ─────────────────────────────────────────────
        if (!ctrl && !shift && !alt &&
            (kv == GDK_KEY_question || kv == GDK_KEY_slash)) {
          m_shortcuts_dialog.show(*this);
          return true;
        }

        // ── Help manual ──────────────────────────────────────────────────
        // F1 is the universal "help" key on every desktop platform. On
        // Apple Silicon Macs (e.g. Asahi Fedora on M-series) the function
        // row defaults to system controls and F1 is unreliable without
        // fn-modifier gymnastics, so Alt+? is wired as a parallel that
        // works on any keyboard. ? is also the Shortcuts dialog (handler
        // immediately above) without a modifier — Alt+? is distinct and
        // not in conflict.
        // win.show-help action exists (see act_help around line 928); the
        // bind() entry at MainWindow.cpp:1134 is cosmetic-only per s123 m4
        // findings, so the action is wired here in CAPTURE explicitly.
        if (!ctrl && !shift && !alt && kv == GDK_KEY_F1) {
          m_help_window.show(*this);
          return true;
        }
        if (alt && !ctrl && (kv == GDK_KEY_question || kv == GDK_KEY_slash)) {
          m_help_window.show(*this);
          return true;
        }

        // ── Document navigation (s108 m7) ────────────────────────────────
        // Window-level CAPTURE-phase dispatch. Action accels alone proved
        // unreliable for Ctrl+Tab because GTK4's built-in focus-traversal
        // default consumes the key for Tab-trapping widgets before the
        // accel system gets a turn. Catching it here (CAPTURE phase, ahead
        // of any focus dispatch) is the same pattern this file uses for
        // Ctrl+Z, Ctrl+Shift+M, etc.
        //
        // Two key pairs supported: Tab (browser/IDE muscle memory, works
        // on keyboards without dedicated PgUp/PgDn) and Page_Down/Up
        // (full-keyboard convention). Both funnel through the same
        // activate_action call so the menu items show one accel each.
        if (ctrl && !alt &&
            (kv == GDK_KEY_Tab || kv == GDK_KEY_ISO_Left_Tab ||
             kv == GDK_KEY_Page_Down || kv == GDK_KEY_Page_Up ||
             kv == GDK_KEY_KP_Page_Down || kv == GDK_KEY_KP_Page_Up)) {
          const bool prev = shift || kv == GDK_KEY_ISO_Left_Tab ||
                            kv == GDK_KEY_Page_Up || kv == GDK_KEY_KP_Page_Up;
          cycle_doc(prev ? -1 : +1);
          return true;
        }

        // S128 — S75 m3a Ctrl+Shift+M test harness removed. The harness
        // claimed Ctrl+Shift+M to exercise BindStyleCommand /
        // materialise_from_style during Phase 1 development, returning
        // true unconditionally and shadowing the Macro Manager handler
        // farther down in the Ctrl block (~line 3390). Phase 2 long
        // since shipped real binding affordances (Styles panel +
        // inspector "Style" row + Ctrl+Shift+B unbind), so the harness
        // was dead code — and its shadow of the macro hotkey was a
        // user-facing bug. Removed; Ctrl+Shift+M now reaches
        // on_macro_manager() as documented.

        // S91 — Ctrl+Alt+9 hotkey removed. Gradient editor is now reachable
        // from the inspector's Appearance section (Gradient toggle → Edit…)
        // and from the Toolbar fill / stroke popovers (Gradient toggle →
        // Edit…). MainWindow connects both signal_request_gradient_edit
        // (PropertiesPanel) and signal_gradient_edit_requested (Toolbar)
        // to m_gradient_dialog.show — the dialog itself is unchanged.

        // ── S80 m4c-2: Ctrl+Shift+B — Unbind selection from style ────────
        // Power-user accelerator. The canonical Unbind path is the
        // inspector's "Style" binding row at the top of Appearance
        // (S83 m4h); this hotkey provides a keyboard-only alternative
        // for users who keep both hands on the keyboard during work.
        //
        // Behaviour: walks the Path/Compound selection, builds an
        // UnbindStyleCommand for currently-bound nodes only (skips
        // already-unbound), pushes one atomic command, refreshes
        // canvas + inspector. Mirrors the inspector handler in
        // PropertiesPanel.cpp's add_fill_stroke_section binding-row
        // block (the "Style" row's Unbind button) exactly.
        //
        // Unlike the m3a/m3b/m3d test harnesses above and below,
        // this one is permanent — survives the m4c-3+ cleanup pass.
        if (ctrl && shift && !alt && (kv == GDK_KEY_B || kv == GDK_KEY_b)) {
          if (m_canvas.selection().empty()) {
            LOG_INFO("Ctrl+Shift+B: no selection");
            return true;
          }
          // Style-eligible selection (Path + Compound).
          std::vector<SceneNode *> targets;
          for (SceneNode *n : m_canvas.selection()) {
            if (!n)
              continue;
            if (n->type == SceneNode::Type::Path ||
                n->type == SceneNode::Type::Compound)
              targets.push_back(n);
          }
          if (targets.empty())
            return true;

          // Mutate-then-push pattern (CommandHistory::push doesn't
          // execute — see CommandHistory.cpp ~line 7). Skip already-
          // unbound nodes so undo only restores ones that actually
          // changed; matches the inspector handler's approach.
          std::vector<UnbindStyleCommand::TargetSnap> snaps;
          snaps.reserve(targets.size());
          for (SceneNode *n : targets) {
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
            // fill / stroke deliberately untouched (break-on-override
            // v1 — pre-unbind cache IS the post-unbind appearance).
          }
          if (snaps.empty()) {
            LOG_INFO("Ctrl+Shift+B: nothing in selection was bound");
            return true;
          }

          const bool plural = snaps.size() > 1;
          m_history.push(std::make_unique<UnbindStyleCommand>(
              std::move(snaps), plural ? std::string("Unbind style (multiple)")
                                       : std::string("Unbind style")));
          m_canvas.queue_draw();
          // Refresh the Styles panel so the active-binding indicator
          // dot clears off the row(s) that just unbound. Selection
          // didn't change, so signal_selection_changed won't fire on
          // its own.
          m_styles.refresh();
          Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
          return true;
        }

        // ── M4c-2e: Escape clears Warp envelope pick set ─────────────────
        // Must run BEFORE the pen-cancel Escape handler below so the key
        // isn't consumed. Only intercepts when primary is a Warp and
        // the pick set is non-empty; otherwise falls through to the
        // general Escape cascade.
        if (kv == GDK_KEY_Escape && !ctrl && !shift && !alt) {
          SceneNode *sel = m_canvas.primary_selection();
          if (sel && sel->is_warp() &&
              m_canvas.active_tool() == ActiveTool::Selection &&
              !m_canvas.warp_env_picks().empty()) {
            m_canvas.warp_env_picks_clear();
            return true;
          }
        }

        // ── Pen tool commit/cancel ──────────────────────────────────────────
        if (kv == GDK_KEY_Escape) {
          if (m_canvas.guide_construct_active()) {
            m_canvas.cancel_guide_construct();
            close_guide_review_dialog();
            return true;
          }
          m_canvas.cancel_pen_path();
          m_canvas.cancel_line_path();
          m_canvas.cancel_text_edit();
          return true;
        }
        if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) {
          if (text_focused)
            return false;
          m_canvas.commit_pen_path();
          m_canvas.commit_line_path();
          m_canvas.commit_text_edit();
          if (m_canvas.active_tool() == ActiveTool::Measure)
            m_canvas.ruler_place_measurement();
          return true;
        }

        // ── M4c-2e: Warp envelope pick-set keyboard shortcuts ─────────────
        // Only when Selection tool is active AND primary selection is a
        // Warp. Gates on no-Ctrl so Ctrl+T/L/R etc. still reach other
        // handlers. Shift-A is handled explicitly since it picks a
        // different set than A alone.
        {
          SceneNode *sel = m_canvas.primary_selection();
          if (!ctrl && m_canvas.active_tool() == ActiveTool::Selection && sel &&
              sel->is_warp()) {
            // Escape clears the pick set.
            if (!shift && !alt && kv == GDK_KEY_Escape) {
              m_canvas.warp_env_picks_clear();
              return true;
            }
            // T — all top anchors
            if (!shift && !alt && (kv == GDK_KEY_t || kv == GDK_KEY_T)) {
              m_canvas.warp_env_picks_select_all_top_anchors();
              return true;
            }
            // B — all bottom anchors
            if (!shift && !alt && (kv == GDK_KEY_b || kv == GDK_KEY_B)) {
              m_canvas.warp_env_picks_select_all_bottom_anchors();
              return true;
            }
            // L — leftmost of top + leftmost of bottom
            if (!shift && !alt && (kv == GDK_KEY_l || kv == GDK_KEY_L)) {
              m_canvas.warp_env_picks_select_leftmost_pair();
              return true;
            }
            // R — rightmost of top + rightmost of bottom
            if (!shift && !alt && (kv == GDK_KEY_r || kv == GDK_KEY_R)) {
              m_canvas.warp_env_picks_select_rightmost_pair();
              return true;
            }
            // C — interior anchors on both envelopes
            if (!shift && !alt && (kv == GDK_KEY_c || kv == GDK_KEY_C)) {
              m_canvas.warp_env_picks_select_interior_anchors();
              return true;
            }
            // A — select all: every anchor + every visible handle on
            // both envelopes.
            if (!shift && !alt && (kv == GDK_KEY_a || kv == GDK_KEY_A)) {
              m_canvas.warp_env_picks_select_all();
              return true;
            }
          }
        }

        // ── Delete selected object or guides ──────────────────────────────
        if ((kv == GDK_KEY_Delete || kv == GDK_KEY_BackSpace) && !ctrl) {
          // Guide selection takes priority — delete guides if any are selected
          if (!m_canvas.guide_selection().empty()) {
            m_canvas.delete_selected_guides();
            // Sync layers and inspector
            m_layers.set_guide_selection({});
            m_properties.set_guide_selection({});
            m_layers.refresh();
            Glib::signal_idle().connect_once([this]() { refresh_inspector(); });
            return true;
          }
          if (m_canvas.delete_selected())
            return true;
        }

        // ── Node tool keys ──────────────────────────────────────────────────
        if (m_canvas.node_tool_key(kv, shift, ctrl, alt))
          return true;

        // ── Selection tool keys ─────────────────────────────────────────────
        if (m_canvas.selection_tool_key(kv, shift, ctrl, alt))
          return true;

        // ── Shift+U: release text from path ──────────────────────────────
        if (!ctrl && shift && !alt && (kv == GDK_KEY_u || kv == GDK_KEY_U)) {
          m_canvas.release_text_from_path();
          return true;
        }

        // ── No-modifier tool hotkeys ────────────────────────────────────────
        if (!ctrl && !shift && !alt) {
          // Up/Down arrows cycle through toolbar tools — only when nothing
          // selected
          if (m_canvas.selection().empty()) {
            if (kv == GDK_KEY_Up) {
              m_toolbar.cycle_tool(-1);
              return true;
            }
            if (kv == GDK_KEY_Down) {
              m_toolbar.cycle_tool(+1);
              return true;
            }
          }
          auto switch_tool = [&](ActiveTool t) {
            m_toolbar.select_tool(t);
            // select_tool emits signal_tool_changed which calls on_tool_changed
          };
          switch (kv) {
          case GDK_KEY_s:
          case GDK_KEY_S:
            switch_tool(ActiveTool::Selection);
            return true;
          case GDK_KEY_n:
          case GDK_KEY_N:
            switch_tool(ActiveTool::Node);
            return true;
          case GDK_KEY_p:
          case GDK_KEY_P:
            switch_tool(ActiveTool::Pen);
            return true;
          case GDK_KEY_r:
          case GDK_KEY_R:
            // If Selection tool is active with objects selected, R activates
            // pivot placement mode rather than switching to the Rect tool.
            if (m_canvas.active_tool() == ActiveTool::Selection &&
                !m_canvas.selection().empty()) {
              m_canvas.notify_r_pressed();
              return true;
            }
            switch_tool(ActiveTool::Rect);
            return true;
          case GDK_KEY_e:
          case GDK_KEY_E:
            switch_tool(ActiveTool::Ellipse);
            return true;
          case GDK_KEY_l:
          case GDK_KEY_L:
            switch_tool(ActiveTool::Line);
            return true;
          case GDK_KEY_f:
          case GDK_KEY_F:
            switch_tool(ActiveTool::Ref);
            return true;
          case GDK_KEY_t:
          case GDK_KEY_T:
            switch_tool(ActiveTool::Text);
            return true;
          case GDK_KEY_i:
          case GDK_KEY_I:
            switch_tool(ActiveTool::Eyedropper);
            return true;
          case GDK_KEY_k:
          case GDK_KEY_K:
            switch_tool(ActiveTool::Corner);
            return true;
          case GDK_KEY_g:
          case GDK_KEY_G:
            switch_tool(ActiveTool::Polygon);
            return true;
          case GDK_KEY_w:
          case GDK_KEY_W:
            switch_tool(ActiveTool::Spiral);
            return true;
          case GDK_KEY_z:
          case GDK_KEY_Z:
            switch_tool(ActiveTool::Zoom);
            return true;
          case GDK_KEY_m:
          case GDK_KEY_M:
            switch_tool(ActiveTool::Measure);
            return true;
          case GDK_KEY_u:
          case GDK_KEY_U:
            switch_tool(ActiveTool::TextOnPath);
            return true;
          case GDK_KEY_q:
          case GDK_KEY_Q: {
            // Toggle snap
            auto *doc = m_project ? m_project->active_doc() : nullptr;
            if (doc) {
              doc->snap.enabled = !doc->snap.enabled;
              m_project->snap.enabled = doc->snap.enabled;
              m_toolbar.set_snap_enabled(doc->snap.enabled);
              LOG_DEBUG("Snap toggled: {}", doc->snap.enabled);
            }
            return true;
          }
          case GDK_KEY_question: {
            m_shortcuts_dialog.show(*this);
            return true;
          }
          default:
            break;
          }
          // +/- zoom
          if (kv == GDK_KEY_plus || kv == GDK_KEY_equal ||
              kv == GDK_KEY_KP_Add) {
            m_toolbar.signal_zoom_step().emit(+1.0);
            return true;
          }
          if (kv == GDK_KEY_minus || kv == GDK_KEY_KP_Subtract) {
            m_toolbar.signal_zoom_step().emit(-1.0);
            return true;
          }
          if (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0) {
            m_canvas.zoom_fit();
            return true;
          }
          if (kv == GDK_KEY_1 || kv == GDK_KEY_KP_1) {
            double cx = m_canvas.get_width() / 2.0;
            double cy = m_canvas.get_height() / 2.0;
            double target = m_canvas.fit_zoom_value();
            m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
            return true;
          }
          if (kv == GDK_KEY_2 || kv == GDK_KEY_KP_2) {
            double cx = m_canvas.get_width() / 2.0;
            double cy = m_canvas.get_height() / 2.0;
            double target = m_canvas.fit_zoom_value() * 2.0;
            m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
            return true;
          }
          if (kv == GDK_KEY_3 || kv == GDK_KEY_KP_3) {
            m_canvas.zoom_to_selection();
            return true;
          }
        }

        // ── Alt shortcuts ───────────────────────────────────────────────────
        if (alt && !ctrl && !shift) {
          if (kv == GDK_KEY_d || kv == GDK_KEY_D) {
            m_canvas.clone_selected();
            return true;
          }
        }
        // Ctrl+Alt+D → Step and Repeat (moved from Ctrl+Shift+D so the
        // latter can open the GTK Inspector).
        if (ctrl && alt && !shift && (kv == GDK_KEY_d || kv == GDK_KEY_D)) {
          on_step_repeat();
          return true;
        }

        // s135 m1: Align hotkeys. Shape mirrors the boolean ops block —
        // wired directly in CAPTURE because set_accels_for_action accel
        // dispatch is cosmetic in this codebase. Each branch consults the
        // SimpleAction's enabled state so the same gate that greys out
        // the menu item also gates the hotkey path. Distribute is
        // menu-only by design (no hotkey).
        //
        // fix3: top/bottom moved from Ctrl+Alt+Up/Down to Ctrl+Alt+P/B
        // because GNOME (Fedora aarch64 default) intercepts Ctrl+Alt+arrow
        // for workspace switching — the keys never reach the app. Letter
        // family is consistent across all six ops and avoids WM-intercept
        // class entirely. Mnemonic: P=toP, B=Bottom.
        if (ctrl && alt && !shift && (kv == GDK_KEY_l || kv == GDK_KEY_L)) {
          if (m_act_align_left && m_act_align_left->get_enabled())
            m_canvas.align_selection(AlignOp::AlignLeft);
          return true;
        }
        if (ctrl && alt && !shift && (kv == GDK_KEY_h || kv == GDK_KEY_H)) {
          if (m_act_align_center_h && m_act_align_center_h->get_enabled())
            m_canvas.align_selection(AlignOp::AlignCenterH);
          return true;
        }
        if (ctrl && alt && !shift && (kv == GDK_KEY_r || kv == GDK_KEY_R)) {
          if (m_act_align_right && m_act_align_right->get_enabled())
            m_canvas.align_selection(AlignOp::AlignRight);
          return true;
        }
        if (ctrl && alt && !shift && (kv == GDK_KEY_p || kv == GDK_KEY_P)) {
          if (m_act_align_top && m_act_align_top->get_enabled())
            m_canvas.align_selection(AlignOp::AlignTop);
          return true;
        }
        if (ctrl && alt && !shift && (kv == GDK_KEY_m || kv == GDK_KEY_M)) {
          if (m_act_align_center_v && m_act_align_center_v->get_enabled())
            m_canvas.align_selection(AlignOp::AlignCenterV);
          return true;
        }
        if (ctrl && alt && !shift && (kv == GDK_KEY_b || kv == GDK_KEY_B)) {
          if (m_act_align_bottom && m_act_align_bottom->get_enabled())
            m_canvas.align_selection(AlignOp::AlignBottom);
          return true;
        }

        // ── Ctrl shortcuts ──────────────────────────────────────────────────
        if (!ctrl)
          return false;

        if (!shift && (kv == GDK_KEY_z || kv == GDK_KEY_Z)) {
          on_undo();
          return true;
        }
        if (shift && (kv == GDK_KEY_z || kv == GDK_KEY_Z)) {
          on_redo();
          return true;
        }
        if (!shift && (kv == GDK_KEY_y || kv == GDK_KEY_Y)) {
          on_redo();
          return true;
        }
        // Clipboard
        if (!shift && (kv == GDK_KEY_a || kv == GDK_KEY_A)) {
          // M4c-2e: Suppress Ctrl+A when primary is a Warp — global
          // object-select-all would disrupt envelope editing by
          // changing primary selection away from the Warp.
          SceneNode *sel = m_canvas.primary_selection();
          if (sel && sel->is_warp() &&
              m_canvas.active_tool() == ActiveTool::Selection) {
            return true;
          }
          m_canvas.select_all();
          return true;
        }
        // s136 m5: Ctrl+Shift+A — deselect all. Counterpart to Ctrl+A,
        // matches Illustrator/Affinity convention. Esc is left alone (it
        // continues to mean "cancel in-progress operation only" — see the
        // Esc handlers above).
        if (shift && (kv == GDK_KEY_a || kv == GDK_KEY_A)) {
          m_canvas.clear_selection();
          return true;
        }
        if (!shift && (kv == GDK_KEY_c || kv == GDK_KEY_C)) {
          m_canvas.copy_selected();
          return true;
        }
        if (!shift && (kv == GDK_KEY_x || kv == GDK_KEY_X)) {
          m_canvas.cut_selected();
          return true;
        }
        if (!shift && (kv == GDK_KEY_v || kv == GDK_KEY_V)) {
          m_canvas.paste_clipboard();
          return true;
        }
        if (!shift && (kv == GDK_KEY_d || kv == GDK_KEY_D)) {
          m_canvas.duplicate_selected();
          return true;
        }
        if (!shift && (kv == GDK_KEY_m || kv == GDK_KEY_M)) {
          Macro *cur = MacroManager::instance().current_macro();
          LOG_INFO("Ctrl+M: current_macro={}", cur ? cur->name : "(none)");
          on_run_macro(cur ? cur->internal_id : std::string(""));
          return true;
        }
        if (shift && (kv == GDK_KEY_m || kv == GDK_KEY_M)) {
          on_macro_manager();
          return true;
        }
        if (!shift && (kv == GDK_KEY_g || kv == GDK_KEY_G)) {
          m_canvas.group_selection();
          return true;
        }
        if (shift && (kv == GDK_KEY_g || kv == GDK_KEY_G)) {
          m_canvas.ungroup_selection();
          return true;
        }
        // Clipping — Ctrl+7 makes a clip group (arms pick mode),
        // Ctrl+Alt+7 releases a selected ClipGroup. '7' has no Shift
        // sibling ('&') so we accept both keysyms — some keyboards
        // map Ctrl+Shift+7 as the '/' key in certain layouts but
        // distinguishing that isn't worth the complexity here; we
        // require !shift to keep Ctrl+Shift+7 free.
        if (!shift && !alt && kv == GDK_KEY_7) {
          m_canvas.make_clip_group();
          return true;
        }
        if (!shift && alt && kv == GDK_KEY_7) {
          m_canvas.release_clip_group();
          return true;
        }
        // Arrange z-order
        if (!shift && !alt && kv == GDK_KEY_Up) {
          m_canvas.arrange(Canvas::ArrangeOp::BringForward);
          return true;
        }
        if (!shift && !alt && kv == GDK_KEY_Down) {
          m_canvas.arrange(Canvas::ArrangeOp::SendBackward);
          return true;
        }
        if (shift && !alt && kv == GDK_KEY_Up) {
          m_canvas.arrange(Canvas::ArrangeOp::BringToFront);
          return true;
        }
        if (shift && !alt && kv == GDK_KEY_Down) {
          m_canvas.arrange(Canvas::ArrangeOp::SendToBack);
          return true;
        }
        // Boolean path ops: wired directly in this CAPTURE controller
        // because set_accels_for_action() accelerator dispatch loses to
        // the controller (CAPTURE handler claims the event before GTK's
        // accelerator machinery sees it). Every other "bound" shortcut
        // in this file follows the same explicit pattern. We still
        // consult the action's enabled state so the sensitivity gate
        // (set by update_bool_actions_sensitive) is respected — same
        // protection s122 m2 was reaching for.
        if (shift && (kv == GDK_KEY_u || kv == GDK_KEY_U)) {
          if (m_act_bool_union && m_act_bool_union->get_enabled())
            m_canvas.boolean_op(BooleanOpType::Union);
          return true;
        }
        if (shift && (kv == GDK_KEY_e || kv == GDK_KEY_E)) {
          if (m_act_bool_subtract && m_act_bool_subtract->get_enabled())
            m_canvas.boolean_op(BooleanOpType::Subtract);
          return true;
        }
        if (shift && (kv == GDK_KEY_i || kv == GDK_KEY_I)) {
          if (m_act_bool_intersect && m_act_bool_intersect->get_enabled())
            m_canvas.boolean_op(BooleanOpType::Intersect);
          return true;
        }
        if (shift && (kv == GDK_KEY_o || kv == GDK_KEY_O)) {
          auto *doc = m_project ? m_project->active_doc() : nullptr;
          const CanvasModel *cm = doc ? &doc->canvas : nullptr;
          m_offset_path_dialog.show(
              *this, cm, [this](OffsetPathDialog::Options opts) {
                m_canvas.offset_path_op(opts.distance, opts.side,
                                        opts.keep_original);
              });
          return true;
        }
        if (shift && (kv == GDK_KEY_x || kv == GDK_KEY_X)) {
          m_canvas.expand_stroke_op();
          return true;
        }
        if (!shift && kv == GDK_KEY_8) {
          m_canvas.make_compound_path();
          return true;
        }
        if (shift && (kv == GDK_KEY_8 || kv == GDK_KEY_asterisk)) {
          m_canvas.split_compound_path();
          return true;
        }
        if (!shift && (kv == GDK_KEY_n || kv == GDK_KEY_N)) {
          on_new();
          return true;
        }
        if (!shift && (kv == GDK_KEY_o || kv == GDK_KEY_O)) {
          on_open();
          return true;
        }
        if (!shift && (kv == GDK_KEY_s || kv == GDK_KEY_S)) {
          on_save();
          return true;
        }
        if (shift && (kv == GDK_KEY_s || kv == GDK_KEY_S)) {
          on_save_as();
          return true;
        }
        if (!shift && (kv == GDK_KEY_p || kv == GDK_KEY_P)) {
          if (m_project)
            m_print_manager.show(*this, *m_project);
          return true;
        }
        if (!shift && (kv == GDK_KEY_q || kv == GDK_KEY_Q)) {
          on_quit();
          return true;
        }
        if (!shift && (kv == GDK_KEY_w || kv == GDK_KEY_W)) {
          on_quit();
          return true;
        }
        if (!shift && (kv == GDK_KEY_question || kv == GDK_KEY_slash)) {
          m_shortcuts_dialog.show(*this);
          return true;
        }
        if (!shift && (kv == GDK_KEY_e || kv == GDK_KEY_E)) {
          // s113 m2: same gate as the action — outline→preview at extreme
          // zoom would crash the app via drop-shadow buffer allocation.
          // s113 m3: action/statusbar sync handled by
          // signal_outline_mode_changed connection.
          try_toggle_outline_safely();
          return true;
        }
        if (!shift && (kv == GDK_KEY_r || kv == GDK_KEY_R)) {
          m_rulers_visible = !m_rulers_visible;
          toggle_rulers(m_rulers_visible);
          // Sync menu checkmark via action state
          if (auto act = lookup_action("toggle-rulers")) {
            auto sa = std::dynamic_pointer_cast<Gio::SimpleAction>(act);
            if (sa)
              sa->set_state(Glib::Variant<bool>::create(m_rulers_visible));
          }
          return true;
        }

        // Ctrl+0/1/2 zoom presets
        // All expressed relative to fit-zoom (1× = fits screen, 2× = double,
        // etc.)
        if (!shift && (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0)) {
          m_canvas.zoom_fit(); // Ctrl+0 — fit artboard
          return true;
        } else if (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0) {
          m_canvas.zoom_to_all_objects(); // Ctrl+Shift+0 — fit all objects
                                          // incl. off-canvas
          return true;
        }

        if (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0) {
          if (shift)
            m_canvas.zoom_to_all_objects(); // Ctrl+Shift+0 — fit all objects
                                            // incl. off-canvas
          else
            m_canvas.zoom_fit(); // Ctrl+0 — fit artboard
          return true;
        }

        if (!shift && (kv == GDK_KEY_1 || kv == GDK_KEY_KP_1)) {
          // 1× = fit-zoom (artboard fills viewport with margin)
          double cx = m_canvas.get_width() / 2.0;
          double cy = m_canvas.get_height() / 2.0;
          double target = m_canvas.fit_zoom_value();
          m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
          return true;
        }

        if (!shift && (kv == GDK_KEY_2 || kv == GDK_KEY_KP_2)) {
          // 2× = double fit-zoom
          double cx = m_canvas.get_width() / 2.0;
          double cy = m_canvas.get_height() / 2.0;
          double target = m_canvas.fit_zoom_value() * 2.0;
          m_canvas.zoom_toward(cx, cy, target / m_canvas.zoom());
          return true;
        }

        if (!shift && (kv == GDK_KEY_3 || kv == GDK_KEY_KP_3)) {
          m_canvas.zoom_to_selection(); // Ctrl+3 — zoom to fit selection
          return true;
        }

        return false;
      },
      false);

  // Alt release — forward to canvas at window level for same reason
  keys->signal_key_released().connect(
      [this](guint kv, guint, Gdk::ModifierType) {
        if (kv == GDK_KEY_Alt_L || kv == GDK_KEY_Alt_R)
          m_canvas.notify_alt_released();
        if (kv == GDK_KEY_space)
          m_canvas.notify_space_released();
        if (kv == GDK_KEY_r || kv == GDK_KEY_R)
          m_canvas.notify_r_released();
      });

  keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  add_controller(keys);
}

} // namespace Curvz
