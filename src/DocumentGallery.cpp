#include "DocumentGallery.hpp"
#include "CurvzLog.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include "curvz_utils.hpp"   // s135 m2 — cairo_set_source_pixbuf pump
#include "widgets/DropDown.hpp"  // s208 m5 — substrate system-tab dropdowns
#include "widgets/Entry.hpp"     // s211 m1 — unregistered substrate Entry for per-tile rename
#include "math/BezierPath.hpp"
#include "TextCursor.hpp"      // s357 m3 — compute_text_layout (TBM thumbnail text)
#include "GlyphOutline.hpp"    // s357 m3 — pattern_glyph_walk (ToP thumbnail text)
#include <pango/pangocairo.h>  // s357 m3 — glyph inking in thumbnails
#include <algorithm>
#include <cairomm/cairomm.h>
#include <cctype>
#include <cmath>
#include <functional>
#include <gdk/gdk.h>
#include <gdkmm/pixbuf.h>
#include <glibmm/main.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/frame.h>
#include <gtkmm/label.h>
#include <gtkmm/picture.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>

namespace Curvz {

static constexpr int THUMB_SIZE =
    64; // fixed px — FlowBox reflows columns automatically

// Case-insensitive substring match
static bool str_icontains(const std::string &haystack,
                          const std::string &needle) {
  if (needle.empty())
    return true;
  auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  return it != haystack.end();
}

// Derive display name from doc filename
static std::string doc_display_name(const CurvzDocument *doc) {
  std::string name = doc->filename.empty() ? "untitled" : doc->filename;
  auto slash = name.rfind('/');
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  if (name.size() > 4 && name.substr(name.size() - 4) == ".svg")
    name = name.substr(0, name.size() - 4);
  return name;
}

DocumentGallery::DocumentGallery() : Gtk::Box(Gtk::Orientation::VERTICAL) {
  set_size_request(-1, 200);
  add_css_class("gallery-panel");

  // ── Header ────────────────────────────────────────────────────────────
  m_header.set_margin_start(4);
  m_header.set_margin_end(4);
  m_header.set_margin_top(4);
  m_header.set_margin_bottom(3);
  m_header.set_spacing(2);

  // Search entry — expands to fill available space
  m_search.set_hexpand(true);
  m_search.set_placeholder_text("Search…");
  m_search.set_max_width_chars(0); // allow shrinking
  m_search.signal_search_changed().connect([this]() {
    m_filter = m_search.get_text();
    if (m_notebook.get_current_page() == 1) {
      rebuild_system_tab();
    } else {
      apply_filter();
      m_signal_filter_changed.emit(m_filter);
    }
  });
  m_header.append(m_search);

  // View toggle — grid icon when in thumbnail mode, list icon when in list mode
  m_btn_view.set_icon_name("view-grid-symbolic");
  m_btn_view.set_has_frame(false);
  m_btn_view.set_tooltip_text("Switch to list view");
  m_btn_view.signal_toggled().connect([this]() {
    if (m_btn_view.get_active()) {
      m_view_mode = ViewMode::List;
      m_btn_view.set_icon_name("view-list-symbolic");
      m_btn_view.set_tooltip_text("Switch to thumbnail view");
    } else {
      m_view_mode = ViewMode::Thumbnail;
      m_btn_view.set_icon_name("view-grid-symbolic");
      m_btn_view.set_tooltip_text("Switch to list view");
    }
    // s360 — single source of truth for scroll visibility: show the scroll for
    // the current mode, unless a filter matched nothing (then keep the
    // empty-state message and both scrolls hidden).
    update_filter_empty_state();
  });
  m_header.append(m_btn_view);

  m_btn_add.set_icon_name("list-add-symbolic");
  m_btn_add.set_has_frame(false);
  m_btn_add.set_tooltip_text("Add new document to project");
  m_btn_add.signal_clicked().connect([this]() { m_signal_add_doc.emit(); });
  m_header.append(m_btn_add);

  m_btn_dup.set_icon_name("edit-copy-symbolic");
  m_btn_dup.set_has_frame(false);
  m_btn_dup.set_tooltip_text("Duplicate active document");
  m_btn_dup.signal_clicked().connect([this]() {
    if (m_project)
      m_signal_dup_doc.emit(m_project->active_doc_index);
  });
  m_header.append(m_btn_dup);

  m_btn_remove.set_icon_name("list-remove-symbolic");
  m_btn_remove.set_has_frame(false);
  m_btn_remove.set_tooltip_text("Remove active document from project");
  m_btn_remove.signal_clicked().connect([this]() {
    if (m_project)
      m_signal_remove_doc.emit(m_project->active_doc_index);
  });
  m_header.append(m_btn_remove);

  m_btn_clear.set_icon_name("edit-clear-all-symbolic");
  m_btn_clear.set_has_frame(false);
  m_btn_clear.set_tooltip_text("Remove all documents from project");
  m_btn_clear.signal_clicked().connect([this]() {
    Gtk::Window *win = nullptr;
    for (Gtk::Widget *w = get_parent(); w; w = w->get_parent()) {
      win = dynamic_cast<Gtk::Window *>(w);
      if (win)
        break;
    }
    if (win) {
      auto dialog = Gtk::AlertDialog::create();
      dialog->set_detail("Are you sure you want to clear all documents\n"
                         "from the project? This cannot be undone.");
      dialog->set_message("Delete all the documents!");
      dialog->set_buttons({"YES", "NO"});
      dialog->set_default_button(1);
      dialog->set_cancel_button(1);
      dialog->choose(
          *win, [this, dialog](const Glib::RefPtr<Gio::AsyncResult> &result) {
            auto response = dialog->choose_finish(result);
            if (response == 0)
              m_signal_clear_all.emit();
          });
    }
  });
  m_header.append(m_btn_clear);
  append(m_header);

  auto *sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  append(*sep);

  // ── Notebook ──────────────────────────────────────────────────────────
  m_notebook.set_tab_pos(Gtk::PositionType::TOP);
  m_notebook.set_expand(true);

  // Project tab: contains a stack of thumb scroll + list scroll
  auto *proj_stack = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  proj_stack->set_expand(true);

  // Thumbnail FlowBox — vertical scroll, 2-column symmetric grid
  m_project_flow.set_homogeneous(false);
  m_project_flow.set_row_spacing(4);
  m_project_flow.set_column_spacing(4);
  m_project_flow.set_margin(6);
  // Selection mode = NONE because we manage the active-tile indicator
  // ourselves via the .gallery-thumb-active CSS class on the frame. With
  // selection mode SINGLE, GTK's FlowBox draws its own theme-blue
  // selection ring on top, and our per-tile GestureClick consumes the
  // press before FlowBox sees it — so the FlowBox selection sticks on
  // whichever child got it first while our manual indicator moves
  // independently. Two visual indicators, fighting each other. Solve at
  // the seam: don't let FlowBox track selection at all.
  m_project_flow.set_selection_mode(Gtk::SelectionMode::NONE);
  m_project_flow.set_valign(Gtk::Align::START);

  m_thumb_scroll.set_child(m_project_flow);
  m_thumb_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  m_thumb_scroll.set_expand(true);
  proj_stack->append(m_thumb_scroll);

  // List ListBox — vertical scroll
  // Same rationale as m_project_flow above — manual active-row indicator
  // via .gallery-list-row-active class, no GTK selection on top.
  m_list_box.set_selection_mode(Gtk::SelectionMode::NONE);
  m_list_box.add_css_class("gallery-list");

  m_list_scroll.set_child(m_list_box);
  m_list_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  m_list_scroll.set_expand(true);
  m_list_scroll.set_visible(false); // starts hidden; thumbnail is default
  proj_stack->append(m_list_scroll);

  // s360 — empty-state for an active search filter that matches nothing.
  // Hidden by default; rebuild_project_tab toggles it (and the scrolls) so a
  // filtered-to-zero gallery shows a message instead of looking empty.
  m_project_empty.set_halign(Gtk::Align::CENTER);
  m_project_empty.set_valign(Gtk::Align::CENTER);
  m_project_empty.set_vexpand(true);
  m_project_empty.set_wrap(true);
  m_project_empty.set_justify(Gtk::Justification::CENTER);
  m_project_empty.add_css_class("dim-label");
  m_project_empty.set_visible(false);
  proj_stack->append(m_project_empty);

  auto *proj_label = Gtk::make_managed<Gtk::Label>("Project");
  m_notebook.append_page(*proj_stack, *proj_label);

  // System tab
  m_sys_box.set_expand(true);

  // Controls row: theme dropdown + category dropdown
  m_sys_controls.set_spacing(4);
  m_sys_controls.set_margin_start(6);
  m_sys_controls.set_margin_end(6);
  m_sys_controls.set_margin_top(4);
  m_sys_controls.set_margin_bottom(4);

  auto *theme_label = Gtk::make_managed<Gtk::Label>("Theme:");
  theme_label->add_css_class("statusbar-label");
  m_sys_controls.append(*theme_label);

  auto theme_list = Gtk::StringList::create({"(scanning…)"});
  // s208 m5: substrate. DocumentGallery is a MainWindow member (single
  // instance, ctor runs once), so the substrate registration is unproblematic.
  m_sys_theme_drop = Gtk::make_managed<curvz::widgets::DropDown>(
      "gal_thm", theme_list);
  curvz::utils::set_name(m_sys_theme_drop, "gal_thm",
                         "document_gallery_system_theme_dd");
  m_sys_theme_drop->set_hexpand(true);
  m_sys_theme_drop->property_selected().signal_changed().connect([this]() {
    auto *sl =
        dynamic_cast<Gtk::StringList *>(m_sys_theme_drop->get_model().get());
    if (!sl)
      return;
    guint idx = m_sys_theme_drop->get_selected();
    auto item = sl->get_string(idx);
    for (const auto &t : m_scanner.themes()) {
      if (t.display == std::string(item)) {
        m_sys_theme = t.dir;
        break;
      }
    }
    auto cats = m_scanner.categories(m_sys_theme);
    auto cat_list = Gtk::StringList::create({"All"});
    for (const auto &c : cats)
      cat_list->append(c);
    m_sys_cat_drop->set_model(cat_list);
    m_sys_cat_drop->set_selected(0);
    m_sys_category = "";
    rebuild_system_tab();
  });
  m_sys_controls.append(*m_sys_theme_drop);

  auto *cat_label = Gtk::make_managed<Gtk::Label>("Cat:");
  cat_label->add_css_class("statusbar-label");
  m_sys_controls.append(*cat_label);

  auto cat_list = Gtk::StringList::create({"All"});
  m_sys_cat_drop = Gtk::make_managed<curvz::widgets::DropDown>(
      "gal_cat", cat_list);
  curvz::utils::set_name(m_sys_cat_drop, "gal_cat",
                         "document_gallery_system_category_dd");
  m_sys_cat_drop->set_hexpand(true);
  m_sys_cat_drop->property_selected().signal_changed().connect([this]() {
    auto *sl =
        dynamic_cast<Gtk::StringList *>(m_sys_cat_drop->get_model().get());
    if (!sl)
      return;
    guint idx = m_sys_cat_drop->get_selected();
    auto item = sl->get_string(idx);
    m_sys_category = (std::string(item) == "All") ? "" : std::string(item);
    rebuild_system_tab();
  });
  m_sys_controls.append(*m_sys_cat_drop);

  m_sys_box.append(m_sys_controls);

  auto *sys_sep =
      Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
  m_sys_box.append(*sys_sep);

  // Placeholder shown until scan completes
  m_system_placeholder.set_text("Opening System tab scans icon themes…");
  m_system_placeholder.set_halign(Gtk::Align::CENTER);
  m_system_placeholder.set_valign(Gtk::Align::CENTER);
  m_system_placeholder.add_css_class("dim-label");
  m_system_placeholder.set_vexpand(true);
  m_sys_box.append(m_system_placeholder);

  // System icon FlowBox (hidden until scan done)
  m_sys_flow.set_homogeneous(false);
  m_sys_flow.set_row_spacing(4);
  m_sys_flow.set_column_spacing(4);
  m_sys_flow.set_margin(6);
  m_sys_flow.set_selection_mode(Gtk::SelectionMode::SINGLE);
  m_sys_flow.set_valign(Gtk::Align::START);

  m_sys_scroll.set_child(m_sys_flow);
  m_sys_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  m_sys_scroll.set_expand(true);
  m_sys_scroll.set_visible(false);
  m_sys_box.append(m_sys_scroll);

  auto *sys_label = Gtk::make_managed<Gtk::Label>("System");
  m_notebook.append_page(m_sys_box, *sys_label);

  // Trigger scan when System tab is first switched to
  m_notebook.signal_switch_page().connect([this](Gtk::Widget *, guint page) {
    if (page == 1 && !m_scanner.is_scanned()) {
      m_system_placeholder.set_text("Scanning…");
      // Run scan on next idle so "Scanning…" text renders first
      Glib::signal_idle().connect_once([this]() {
        m_scanner.scan();
        // Populate theme dropdown — look up model fresh inside lambda
        auto *sl = dynamic_cast<Gtk::StringList *>(
            m_sys_theme_drop->get_model().get());
        if (sl) {
          while (sl->get_n_items() > 0)
            sl->remove(0);
          for (const auto &th : m_scanner.themes())
            sl->append(th.display);
          if (!m_scanner.themes().empty()) {
            m_sys_theme = m_scanner.themes()[0].dir;
            m_sys_theme_drop->set_selected(0);
            auto cats = m_scanner.categories(m_sys_theme);
            auto cat_sl = Gtk::StringList::create({"All"});
            for (const auto &c : cats)
              cat_sl->append(c);
            m_sys_cat_drop->set_model(cat_sl);
            m_sys_cat_drop->set_selected(0);
          }
        }
        m_system_placeholder.set_visible(false);
        m_sys_scroll.set_visible(true);
        rebuild_system_tab();
      });
    }
    if (page == 0) {
      // s360 — leaving the System tab: tell MainWindow to exit icon-preview
      // mode (single-click on a system icon swaps the canvas to a throwaway
      // preview doc; without this, switching back left preview active and the
      // project docs + doc-tab bar unrefreshed — "docs gone but still there").
      // Idempotent: the handler no-ops when preview isn't active.
      m_signal_show_project_tab.emit();
      // s360 — re-sync the gallery's own tile visibility + empty-state to the
      // current filter. The filter may have changed (e.g. cleared) while the
      // System tab was up, where search edits only rebuild the System tab; if
      // we only emit filter_changed (which updates the doc-tab bar) the
      // gallery would keep its stale pre-switch visibility. apply_filter()
      // restores the documents when the search was cleared.
      apply_filter();
      m_signal_filter_changed.emit(m_filter);
    }
  });

  append(m_notebook);
  LOG_DEBUG("DocumentGallery created");
}

void DocumentGallery::set_project(CurvzProject *project) {
  m_project = project;
  rebuild_project_tab();
}

void DocumentGallery::refresh() { rebuild_project_tab(); }

// ── Thumbnail renderer
// ────────────────────────────────────────────────────────
Cairo::RefPtr<Cairo::ImageSurface>
DocumentGallery::render_thumb(CurvzDocument *doc, int size) {
  auto surf =
      Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, size, size);
  auto cr = Cairo::Context::create(surf);

  // s148 m1: thumb bg reads the doc's OWN artboard colour (re-promoted
  // to per-doc). Each doc's thumb shows that doc's actual tone, which
  // is more honest than the s116 m6 project-wide rendering.
  // currentColor luminance derives from the doc's own bg luminance:
  // if the doc's artboard is bright (max > 0.6), paint a dark icon
  // body; else paint a light icon body. Same threshold Canvas uses
  // for grid-line tinting, so library / gallery / canvas all flip
  // currentColor at the same point.
  double bg_r = 0.157, bg_g = 0.157, bg_b = 0.157;  // #282828 fallback
  if (doc) {
    // s183 m5a — doc now carries dual pairs; pick the one matching
    // the project's current motif.
    const Motif mo = m_project ? m_project->motif : Motif::Dark;
    bg_r = doc->artboard_bg_r(mo);
    bg_g = doc->artboard_bg_g(mo);
    bg_b = doc->artboard_bg_b(mo);
  }
  bool light_bg = std::max({bg_r, bg_g, bg_b}) > 0.60;
  double cc = light_bg ? 0.10 : 0.88;

  // Background
  cr->set_source_rgb(bg_r, bg_g, bg_b);
  cr->paint();

  if (!doc || doc->layers.empty())
    return surf;

  int cw = doc->canvas_width();
  int ch = doc->canvas_height();
  if (cw <= 0 || ch <= 0)
    return surf;

  // Fit canvas into thumb with 2px margin
  double margin = 2.0;
  double scale =
      std::min((size - margin * 2.0) / cw, (size - margin * 2.0) / ch);
  double ox = (size - cw * scale) * 0.5;
  double oy = (size - ch * scale) * 0.5;

  cr->save();
  cr->translate(ox, oy);
  cr->scale(scale, scale);
  cr->rectangle(0, 0, cw, ch);
  cr->clip();

  // Recursive node draw
  std::function<void(const SceneNode &)> draw_node = [&](const SceneNode &obj) {
    if (!obj.visible)
      return;

    if (obj.type == SceneNode::Type::Path && obj.path) {
      BezierPath bp = BezierPath::from_path_data(*obj.path);
      bp.apply_to_cairo(cr);

      // s229 m2: capture the path's tight bbox via Cairo before the
      // fill consumes the path. Required by build_gradient_pattern
      // to lerp objectBoundingBox-fraction endpoints into doc-space.
      // Pre-s229 m2 the thumbnail renderer only handled Solid +
      // CurrentColor; gradient-filled / -stroked shapes left the
      // cairo source at whatever was set previously (the artboard
      // background paint from `cr->paint()` on the first object),
      // so a gradient-filled shape on a same-coloured artboard
      // rendered invisible — the reported bug.
      double bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
      cr->get_fill_extents(bx1, by1, bx2, by2);
      const double bw = bx2 - bx1;
      const double bh = by2 - by1;

      // ── Fill ─────────────────────────────────────────────────────
      if (obj.fill.type == FillStyle::Type::CurrentColor) {
        cr->set_source_rgb(cc, cc, cc);
      } else if (obj.fill.type == FillStyle::Type::Solid) {
        cr->set_source_rgb(obj.fill.r, obj.fill.g, obj.fill.b);
      } else if (obj.fill.is_gradient()) {
        auto pat = curvz::utils::build_gradient_pattern(
            obj.fill, bx1, by1, bw, bh);
        if (pat) cr->set_source(pat);
        else     cr->set_source_rgba(0, 0, 0, 0);
      }
      if (obj.fill.type != FillStyle::Type::None)
        cr->fill_preserve();

      // ── Stroke ───────────────────────────────────────────────────
      if (obj.stroke.paint.type == FillStyle::Type::CurrentColor) {
        cr->set_source_rgb(cc, cc, cc);
      } else if (obj.stroke.paint.type == FillStyle::Type::Solid) {
        cr->set_source_rgb(obj.stroke.paint.r, obj.stroke.paint.g,
                           obj.stroke.paint.b);
      } else if (obj.stroke.paint.is_gradient()) {
        auto pat = curvz::utils::build_gradient_pattern(
            obj.stroke.paint, bx1, by1, bw, bh);
        if (pat) cr->set_source(pat);
        else     cr->set_source_rgba(0, 0, 0, 0);
      }
      if (obj.stroke.paint.type != FillStyle::Type::None) {
        cr->set_line_width(obj.stroke.width);
        cr->stroke();
      } else {
        cr->begin_new_path();
      }

    } else if (obj.type == SceneNode::Type::Compound) {
      if (obj.children.empty())
        return;
      // S58p: Compound owns its paint (S58d rule). Read fill/stroke from
      // the Compound itself, not from the first child — children are
      // inert for rendering.
      FillStyle fill = obj.fill;
      StrokeStyle stroke = obj.stroke;

      // s127: descending iteration matches Canvas::draw_object's Compound
      // branch. Pixel output for the single fill_preserve+stroke pass is
      // order-independent under EVEN_ODD, but consistency keeps the
      // convention legible across all renderers.
      for (int i = (int)obj.children.size() - 1; i >= 0; --i) {
        const auto &child = obj.children[i];
        if (child->type == SceneNode::Type::Path && child->path) {
          BezierPath bp = BezierPath::from_path_data(*child->path);
          bp.apply_to_cairo(cr);
        }
      }

      // s229 m2: Compound gradient support — same bbox-from-Cairo
      // pattern as the Path branch above. The combined-path bbox
      // covers all subpaths, which is exactly what objectBoundingBox
      // semantics want for a Compound's gradient (mirrors PrintManager
      // line 1209-1214).
      double cbx1 = 0, cby1 = 0, cbx2 = 0, cby2 = 0;
      cr->get_fill_extents(cbx1, cby1, cbx2, cby2);
      const double cbw = cbx2 - cbx1;
      const double cbh = cby2 - cby1;

      // ── Fill ─────────────────────────────────────────────────────
      if (fill.type == FillStyle::Type::CurrentColor) {
        cr->set_source_rgb(cc, cc, cc);
      } else if (fill.type == FillStyle::Type::Solid) {
        cr->set_source_rgb(fill.r, fill.g, fill.b);
      } else if (fill.is_gradient()) {
        auto pat = curvz::utils::build_gradient_pattern(
            fill, cbx1, cby1, cbw, cbh);
        if (pat) cr->set_source(pat);
        else     cr->set_source_rgba(0, 0, 0, 0);
      }
      if (fill.type != FillStyle::Type::None) {
        cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
        cr->fill_preserve();
        cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
      }

      // ── Stroke ───────────────────────────────────────────────────
      if (stroke.paint.type == FillStyle::Type::CurrentColor) {
        cr->set_source_rgb(cc, cc, cc);
      } else if (stroke.paint.type == FillStyle::Type::Solid) {
        cr->set_source_rgb(stroke.paint.r, stroke.paint.g, stroke.paint.b);
      } else if (stroke.paint.is_gradient()) {
        auto pat = curvz::utils::build_gradient_pattern(
            stroke.paint, cbx1, cby1, cbw, cbh);
        if (pat) cr->set_source(pat);
        else     cr->set_source_rgba(0, 0, 0, 0);
      }
      if (stroke.paint.type != FillStyle::Type::None) {
        cr->set_line_width(stroke.width);
        cr->stroke();
      } else {
        cr->begin_new_path();
      }

    } else if (obj.type == SceneNode::Type::Group) {
      // Match Canvas convention: children[0] = top, so paint
      // in reverse order (bottom-up).
      for (int i = (int)obj.children.size() - 1; i >= 0; --i)
        draw_node(*obj.children[i]);

    } else if (obj.type == SceneNode::Type::Image) {
      if (obj.image_w < 0.01 || obj.image_h < 0.01)
        return;
      Cairo::RefPtr<Cairo::ImageSurface> img_surf;
      try {
        std::string ext = obj.image_path.substr(obj.image_path.rfind('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "png") {
          img_surf = Cairo::ImageSurface::create_from_png(obj.image_path);
        } else {
          auto pb = Gdk::Pixbuf::create_from_file(obj.image_path);
          if (pb) {
            int pw = pb->get_width(), ph = pb->get_height();
            auto surf2 = Cairo::ImageSurface::create(
                Cairo::Surface::Format::ARGB32, pw, ph);
            auto cr2 = Cairo::Context::create(surf2);
            // s135 m2: pumped — replaces deprecated gdk_cairo_set_source_pixbuf.
            curvz::utils::cairo_set_source_pixbuf(cr2, pb, 0, 0);
            cr2->paint();
            img_surf = surf2;
          }
        }
      } catch (...) {
      }

      if (img_surf) {
        int iw = img_surf->get_width();
        int ih = img_surf->get_height();
        if (iw > 0 && ih > 0) {
          cr->save();
          cr->translate(obj.image_x, obj.image_y);
          cr->scale(obj.image_w / iw, obj.image_h / ih);
          cr->set_source(img_surf, 0, 0);
          cr->paint_with_alpha(obj.opacity);
          cr->restore();
        }
      } else {
        // Placeholder — grey rect with X
        cr->save();
        cr->set_source_rgba(0.4, 0.4, 0.4, 0.5);
        cr->rectangle(obj.image_x, obj.image_y, obj.image_w, obj.image_h);
        cr->fill();
        cr->set_source_rgba(0.6, 0.6, 0.6, 0.8);
        cr->set_line_width(1.0);
        cr->move_to(obj.image_x, obj.image_y);
        cr->line_to(obj.image_x + obj.image_w, obj.image_y + obj.image_h);
        cr->move_to(obj.image_x + obj.image_w, obj.image_y);
        cr->line_to(obj.image_x, obj.image_y + obj.image_h);
        cr->stroke();
        cr->restore();
      }

    } else if (obj.type == SceneNode::Type::Text) {
      // s357 m3 — real glyph rendering, matching the canvas. Replaces the old
      // grey placeholder bar, which both failed to show glyphs AND used a
      // wrong `canvas_height - text_y` flip (the canvas and PngExporter treat
      // text_y as doc Y-down, no flip — the flip is why bound/anchored text
      // landed off the clip and read as "not drawn"). Three variants,
      // dispatched exactly like Canvas::draw_text_node: bound text (TBM) ->
      // compute_text_layout, guide-attached text (ToP v2) ->
      // pattern_glyph_walk, else free text. Both layout pumps are the same
      // free functions SvgWriter already reuses, so no Canvas instance is
      // needed; the doc resolves its own boundary/guide nodes by iid.
      const style::TextStyleLibrary *lib =
          m_project ? &m_project->text_styles : nullptr;

      // Fill source for text: currentColor -> doc luminance, Solid -> rgb.
      // Gradient (and None) fall back to the luminance ink — a per-glyph
      // gradient bbox isn't worth it at thumbnail size; visibility wins.
      auto set_text_src = [&](const FillStyle &f) {
        if (f.type == FillStyle::Type::Solid)
          cr->set_source_rgb(f.r, f.g, f.b);
        else
          cr->set_source_rgb(cc, cc, cc);
      };

      // ── Bound text (TBM) ────────────────────────────────────────────────
      if (!obj.text_boundary_ids.empty()) {
        SceneNode *boundary = doc->find_by_iid(obj.text_boundary_ids.front());
        if (boundary && boundary->path) {
          TextLayout tl = compute_text_layout(boundary, &obj, 0, lib);
          cr->save();
          if (tl.frame_angle != 0.0) {  // rotated box -> lay into its frame
            cr->translate(tl.frame_cx, tl.frame_cy);
            cr->rotate(tl.frame_angle);
            cr->translate(-tl.frame_cx, -tl.frame_cy);
          }
          set_text_src(obj.fill);
          for (const auto &bl : tl.baselines) {
            if (!bl.pango)
              continue;
            cr->save();
            double base_px =
                pango_layout_get_baseline(bl.pango.get()) / (double)PANGO_SCALE;
            cr->move_to(bl.x_start, bl.y - base_px);
            pango_cairo_show_layout(cr->cobj(), bl.pango.get());
            // s358 — trailing hyphen dash, mirroring Canvas::draw_text_node.
            // Draw-time overlay keyed on ended_by_hyphen; never baked into
            // bl.pango, so justify and the caret byte map stay clean.
            if (bl.ended_by_hyphen) {
              curvz::utils::draw_hyphen_dash(cr, bl.pango.get(), bl.x_start,
                                             bl.y);
              LOG_DEBUG("DocumentGallery: bound-text thumbnail hyphen dash "
                        "x_start={:.2f} base_y={:.2f}",
                        bl.x_start, bl.y);
            }
            cr->restore();
          }
          cr->restore();
          return;
        }
        // Dangling boundary -> fall through to free render (degraded but
        // visible), matching draw_text_node's own fallback.
      }

      // ── Guide-attached text (ToP v2) ────────────────────────────────────
      if (!obj.text_guide_id.empty()) {
        SceneNode *guide = doc->find_by_iid(obj.text_guide_id);
        if (guide && guide->path && guide->path->nodes.size() >= 2) {
          cr->save();
          bool src_set = false, src_fg = false;
          double sr = 0, sg = 0, sb = 0, sa = 0;
          pattern_glyph_walk(
              obj, *guide, lib, [&](const PatternGlyph &g) {
                if (!src_set || src_fg != g.has_fg ||
                    (g.has_fg && (sr != g.fg_r || sg != g.fg_g ||
                                  sb != g.fg_b || sa != g.fg_a))) {
                  if (g.has_fg)
                    cr->set_source_rgba(g.fg_r, g.fg_g, g.fg_b, g.fg_a);
                  else
                    set_text_src(obj.fill);
                  src_set = true; src_fg = g.has_fg;
                  sr = g.fg_r; sg = g.fg_g; sb = g.fg_b; sa = g.fg_a;
                }
                cr->save();
                cr->translate(g.pos.x, g.pos.y);
                cr->rotate(g.angle);
                PangoGlyphString single;
                int log_cluster = 0;
                single.num_glyphs = 1;
                single.glyphs = g.info;
                single.log_clusters = &log_cluster;
                cr->move_to(-g.adv_px * 0.5, g.pen_y);
                pango_cairo_show_glyph_string(cr->cobj(), g.font, &single);
                cr->restore();
              });
          cr->restore();
          return;
        }
        // Dangling guide -> fall through to free render.
      }

      // ── Free text ───────────────────────────────────────────────────────
      if (obj.text_content.empty())
        return;
      cr->save();
      // text_y is the baseline anchor in doc (Y-down) space — translate
      // straight to it, no flip (canvas convention).
      cr->translate(obj.text_x, obj.text_y);
      if (obj.text_mirror_h) cr->scale(-1.0, 1.0);
      if (obj.text_mirror_v) cr->scale(1.0, -1.0);
      set_text_src(obj.fill);

      PangoLayout *layout = pango_cairo_create_layout(cr->cobj());
      PangoFontDescription *desc = pango_font_description_new();
      pango_font_description_set_family(desc, obj.text_font_family.c_str());
      pango_font_description_set_absolute_size(desc,
                                               obj.text_font_size * PANGO_SCALE);
      if (obj.text_bold)
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
      if (obj.text_italic)
        pango_font_description_set_style(desc, PANGO_STYLE_ITALIC);
      pango_layout_set_font_description(layout, desc);
      pango_font_description_free(desc);
      pango_layout_set_text(layout, obj.text_content.c_str(), -1);
      if (obj.text_letter_spacing != 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(
            (int)(obj.text_letter_spacing * PANGO_SCALE)));
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
      }
      if (obj.text_align == "center")
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
      else if (obj.text_align == "right")
        pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
      else
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
      pango_layout_set_justify(layout, obj.text_align == "justify");

      PangoRectangle logical;
      pango_layout_get_pixel_extents(layout, nullptr, &logical);
      double off_x = 0.0;
      if (obj.text_anchor == "middle") off_x = -logical.width * 0.5;
      if (obj.text_anchor == "end")    off_x = -logical.width;

      double base_px =
          pango_layout_get_baseline(layout) / (double)PANGO_SCALE;
      cr->move_to(off_x, -base_px - obj.text_baseline_shift);
      pango_cairo_show_layout(cr->cobj(), layout);
      g_object_unref(layout);
      cr->restore();

    } else if (obj.is_text_box_mgr()) {
      // s357 m3-fix — TextBoxMgr render. Live boxed text is NOT a Type::Text
      // node (the earlier m3 only matched legacy Type::Text + boundary_ids,
      // so current Mgr nodes drew nothing). The Mgr is the text-bearing node
      // (text_content / font / fill live on it); its boundary sits one level
      // deeper: Mgr -> CanvasView -> children[0] = boundary Path. Overflow
      // chains across views — each view renders the slice starting where the
      // previous left off (flow chained by bytes_consumed). Mirrors the
      // TextBoxMgr branch in Canvas::draw_object.
      const style::TextStyleLibrary *mlib =
          m_project ? &m_project->text_styles : nullptr;
      auto set_mgr_src = [&](const FillStyle &f) {
        if (f.type == FillStyle::Type::Solid)
          cr->set_source_rgb(f.r, f.g, f.b);
        else
          cr->set_source_rgb(cc, cc, cc);
      };
      size_t flow = 0;
      for (const auto &view_ptr : obj.children) {
        if (!view_ptr) continue;
        const SceneNode &view = *view_ptr;
        if (!view.is_canvas_view() || !view.visible) continue;
        if (view.children.empty() || !view.children[0]) continue;
        const SceneNode &boundary = *view.children[0];
        if (boundary.type != SceneNode::Type::Path || !boundary.path) continue;
        // Box shape underneath — reuse the Path branch so its own
        // fill/stroke/gradient (a visible box background/border) render.
        draw_node(boundary);
        // Text slice for this member, starting where the prior view ended.
        TextLayout tl = compute_text_layout(&boundary, &obj, flow, mlib);
        cr->save();
        if (tl.frame_angle != 0.0) {  // rotated box -> lay into its frame
          cr->translate(tl.frame_cx, tl.frame_cy);
          cr->rotate(tl.frame_angle);
          cr->translate(-tl.frame_cx, -tl.frame_cy);
        }
        set_mgr_src(obj.fill);
        for (const auto &bl : tl.baselines) {
          if (!bl.pango)
            continue;
          cr->save();
          double base_px =
              pango_layout_get_baseline(bl.pango.get()) / (double)PANGO_SCALE;
          cr->move_to(bl.x_start, bl.y - base_px);
          pango_cairo_show_layout(cr->cobj(), bl.pango.get());
          // s358 — trailing hyphen dash, same draw-time overlay as the canvas.
          if (bl.ended_by_hyphen) {
            curvz::utils::draw_hyphen_dash(cr, bl.pango.get(), bl.x_start,
                                           bl.y);
            LOG_DEBUG("DocumentGallery: TBM thumbnail hyphen dash "
                      "x_start={:.2f} base_y={:.2f}",
                      bl.x_start, bl.y);
          }
          cr->restore();
        }
        cr->restore();
        flow = tl.bytes_consumed;  // chain overflow into the next view
      }
    }
  };

  for (const auto &layer_uptr : doc->layers) {
    if (!layer_uptr->visible)
      continue;
    if (layer_uptr->is_guide_layer() || layer_uptr->is_ref_layer())
      continue;
    // Match Canvas convention: children[0] = top, paint in reverse.
    for (int i = (int)layer_uptr->children.size() - 1; i >= 0; --i)
      draw_node(*layer_uptr->children[i]);
  }
  cr->restore();

  // Border
  cr->set_source_rgba(0.5, 0.5, 0.5, 0.6);
  cr->set_line_width(0.5);
  cr->rectangle(ox + 0.25, oy + 0.25, cw * scale - 0.5, ch * scale - 0.5);
  cr->stroke();

  return surf;
}

// ── apply_filter — show/hide children in both views based on m_filter
// ─────────
void DocumentGallery::apply_filter() {
  if (!m_project)
    return;

  // Thumbnail view — iterate FlowBox children
  int fi = 0;
  for (Gtk::Widget *w = m_project_flow.get_first_child(); w;
       w = w->get_next_sibling(), ++fi) {
    if (fi >= (int)m_project->documents.size())
      break;
    std::string name = doc_display_name(m_project->documents[fi].get());
    w->set_visible(str_icontains(name, m_filter));
  }

  // List view — iterate ListBox rows
  int li = 0;
  for (Gtk::Widget *w = m_list_box.get_first_child(); w;
       w = w->get_next_sibling(), ++li) {
    if (li >= (int)m_project->documents.size())
      break;
    std::string name = doc_display_name(m_project->documents[li].get());
    w->set_visible(str_icontains(name, m_filter));
  }

  // s360 — keep the no-matches empty-state in sync during live typing.
  update_filter_empty_state();
}

// s360 — show "No documents match …" when a filter is active and nothing
// matches, hiding the (now-empty) scrolls; otherwise hide the message and
// restore the scroll for the current view mode. Shared by apply_filter and
// rebuild_project_tab. Documents lists are small, so the extra match pass is
// cheap.
void DocumentGallery::update_filter_empty_state() {
  if (!m_project)
    return;
  int matches = 0;
  for (const auto &d : m_project->documents)
    if (str_icontains(doc_display_name(d.get()), m_filter))
      ++matches;
  bool no_match =
      !m_filter.empty() && matches == 0 && !m_project->documents.empty();
  m_project_empty.set_visible(no_match);
  if (no_match) {
    m_project_empty.set_text("No documents match \"" + m_filter + "\"");
    m_thumb_scroll.set_visible(false);
    m_list_scroll.set_visible(false);
  } else {
    // Mirror the view toggle: show the scroll for the current view mode.
    m_thumb_scroll.set_visible(m_view_mode == ViewMode::Thumbnail);
    m_list_scroll.set_visible(m_view_mode == ViewMode::List);
  }
}

// ── Gallery rebuild
// ───────────────────────────────────────────────────────────
void DocumentGallery::rebuild_project_tab() {
  // Clear thumbnail FlowBox. Pre-S100 m4 there was a set_visible(false)/
  // (true) dance around this, but it caused "Trying to snapshot Box
  // without current allocation" warnings on first paint of each tile —
  // the visibility flip put the layout into a partially-realised state
  // that the subsequent appends hit before allocation settled. GTK4
  // handles plain remove-all-then-append-all cleanly without it.
  while (auto *child = m_project_flow.get_first_child())
    m_project_flow.remove(*child);

  // Clear list ListBox
  while (auto *child = m_list_box.get_first_child())
    m_list_box.remove(*child);

  if (!m_project)
    return;

  m_btn_remove.set_sensitive(!m_project->documents.empty());
  m_btn_dup.set_sensitive(!m_project->documents.empty());

  for (int i = 0; i < (int)m_project->documents.size(); ++i) {
    auto *doc = m_project->documents[i].get();
    std::string name = doc_display_name(doc);
    bool visible = str_icontains(name, m_filter);

    // ── Thumbnail entry ───────────────────────────────────────────────
    auto *frame = Gtk::make_managed<Gtk::Frame>();
    frame->set_size_request(THUMB_SIZE + 4, THUMB_SIZE + 20);
    frame->set_vexpand(false);
    frame->set_valign(Gtk::Align::START);
    frame->add_css_class("gallery-thumb");
    if (i == m_project->active_doc_index)
      frame->add_css_class("gallery-thumb-active");
    frame->set_visible(visible);

    auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_spacing(1);
    vbox->set_margin(2);
    vbox->set_vexpand(false);

    auto *da = Gtk::make_managed<Gtk::DrawingArea>();
    da->set_size_request(THUMB_SIZE, THUMB_SIZE);
    da->set_vexpand(false);
    da->set_halign(Gtk::Align::CENTER);
    CurvzDocument *doc_ptr = doc;
    da->set_draw_func(
        [this, doc_ptr](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
          if (w < 4 || h < 4)
            return;
          int sz = std::min(w, h);
          auto surf = render_thumb(doc_ptr, sz);
          if (!surf)
            return;
          double ox = (w - sz) * 0.5;
          double oy = (h - sz) * 0.5;
          cr->set_source(surf, ox, oy);
          cr->paint();
        });
    vbox->append(*da);

    auto *name_label = Gtk::make_managed<Gtk::Label>(name);
    name_label->set_ellipsize(Pango::EllipsizeMode::END);
    name_label->set_max_width_chars(6);
    name_label->add_css_class("caption-label");
    vbox->append(*name_label);

    frame->set_child(*vbox);

    // ── Inline rename helper ──────────────────────────────────────────
    // Swaps the caption label for an Entry, focuses it, and on Enter
    // emits signal_rename_doc(idx, new_name); on Esc or empty-commit
    // reverts. Used by both double-click on the tile and the right-click
    // context menu's "Rename" item below — same code path keeps both
    // affordances equivalent.
    //
    // Capturing `name` (the original caption) lets Esc restore exactly
    // what was there. Capturing `vbox` and `name_label` lets us swap
    // back to the label widget after the edit settles.
    const int dc_idx = i;
    const std::string orig_name = name;
    auto begin_rename = [this, vbox, name_label, dc_idx, orig_name]() {
      // Replace label with Entry
      // s211 m1 — unregistered substrate Entry. Per-tile transient
      // built inside a gesture-triggered lambda; the gallery has N
      // tiles so any shared abbrev (e.g. "gal_rename") would collide
      // across simultaneous renames. The rename UX is fully local to
      // the tile (Enter commits, Esc reverts, focus-loss commits via
      // the explicit `commit` slot below) — no script-addressability
      // requirement. Same pattern as ContextBar's add_btn (s209 m1).
      auto *entry = Gtk::make_managed<curvz::widgets::Entry>(
                        curvz::scripting::unregistered);
      entry->set_text(orig_name);
      entry->set_max_width_chars(8);
      entry->set_width_chars(8);
      entry->add_css_class("caption-label");
      // Replace the label's slot in the vbox: remove label, append entry,
      // then reorder isn't required because label is the last child.
      vbox->remove(*name_label);
      vbox->append(*entry);

      // Track commit vs revert exactly once. Without this guard, both
      // signal_activate (Enter) and a focus-leave handler can fire and
      // we'd emit twice or revert after committing.
      auto done_flag = std::make_shared<bool>(false);
      auto commit = [this, entry, dc_idx, done_flag]() {
        if (*done_flag) return;
        *done_flag = true;
        std::string new_name = entry->get_text();
        // Empty rename = no-op revert; let refresh() restore the label.
        if (!new_name.empty())
          m_signal_rename_doc.emit(dc_idx, new_name);
        // Either way refresh repaints the gallery with the live name.
        Glib::signal_idle().connect_once([this]() { refresh(); });
      };
      auto revert = [this, done_flag]() {
        if (*done_flag) return;
        *done_flag = true;
        Glib::signal_idle().connect_once([this]() { refresh(); });
      };

      entry->signal_activate().connect(commit);

      // Esc to revert. CAPTURE phase so the Entry's own key handling
      // doesn't swallow it first.
      auto kc = Gtk::EventControllerKey::create();
      kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
      kc->signal_key_pressed().connect(
          [revert](guint kv, guint, Gdk::ModifierType) {
            if (kv == GDK_KEY_Escape) {
              revert();
              return true;
            }
            return false;
          },
          false);
      entry->add_controller(kc);

      entry->grab_focus();
      entry->select_region(0, -1);
    };

    auto gesture = Gtk::GestureClick::create();
    gesture->signal_pressed().connect(
        [this, dc_idx, begin_rename](int n_press, double, double) {
          if (n_press == 2) {
            begin_rename();
            return;
          }
          m_signal_doc_activated.emit(dc_idx);
        });
    frame->add_controller(gesture);

    // ── Right-click context menu ──────────────────────────────────────
    // Build once per tile, reuse on every right-click. This avoids two
    // problems the per-click + unparent-on-close pattern caused:
    //   1. Mutating the vbox (label → entry swap inside begin_rename)
    //      while the popover is still dismissing produces a "Trying to
    //      snapshot Box without current allocation" warning, because
    //      the layout is in flux.
    //   2. Focus handoff to the freshly-created Entry races with the
    //      popover's dismiss-and-unparent — the Entry would appear but
    //      could not be edited.
    // Building once at tile creation means the popover lives exactly as
    // long as the frame; refresh() rebuilds tiles wholesale, and the
    // popover is destroyed cleanly along with its parent. No leaks, no
    // races.
    {
      auto group = Gio::SimpleActionGroup::create();
      group->add_action("rename", [begin_rename]() { begin_rename(); });
      group->add_action("dup",
                        [this, dc_idx]() { m_signal_dup_doc.emit(dc_idx); });
      group->add_action("del",
                        [this, dc_idx]() { m_signal_remove_doc.emit(dc_idx); });
      frame->insert_action_group("tile", group);

      auto menu = Gio::Menu::create();
      menu->append("Rename",    "tile.rename");
      menu->append("Duplicate", "tile.dup");
      menu->append("Delete",    "tile.del");

      auto *popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
      popover->set_parent(*frame);
      popover->set_has_arrow(false);
      // GTK4 lifetime: a popover attached via set_parent() is NOT a normal
      // child of its parent — managed-widget cleanup doesn't reach it.
      // When refresh() destroys the frame, the popover is "still a child"
      // and GTK warns. Hook signal_destroy on the parent and unparent
      // there so the teardown happens in the right order.
      frame->signal_destroy().connect([popover]() { popover->unparent(); });

      auto rclick = Gtk::GestureClick::create();
      rclick->set_button(GDK_BUTTON_SECONDARY);
      rclick->signal_pressed().connect(
          [popover](int, double x, double y) {
            Gdk::Rectangle rect{(int)x, (int)y, 1, 1};
            popover->set_pointing_to(rect);
            popover->popup();
          });
      frame->add_controller(rclick);
    }

    m_project_flow.append(*frame);

    // ── List entry ────────────────────────────────────────────────────
    auto *row = Gtk::make_managed<Gtk::ListBoxRow>();
    row->set_visible(visible);
    row->add_css_class("gallery-list-row");
    if (i == m_project->active_doc_index)
      row->add_css_class("gallery-list-row-active");

    auto *row_label = Gtk::make_managed<Gtk::Label>(name);
    row_label->set_xalign(0.0f);
    row_label->set_ellipsize(Pango::EllipsizeMode::END);
    row_label->set_margin_start(8);
    row_label->set_margin_end(8);
    row_label->set_margin_top(5);
    row_label->set_margin_bottom(5);
    auto *row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    row_box->append(*row_label);
    row->set_child(*row_box);

    // List-mode rename uses the same begin_rename pattern, scoped to row_box.
    const int ridx = i;
    const std::string row_orig_name = name;
    auto begin_rename_row = [this, row_box, row_label, ridx, row_orig_name]() {
      // s211 m1 — unregistered substrate Entry. Same rationale as the
      // icon-mode `begin_rename` lambda above: per-tile transient
      // built inside a gesture-triggered lambda, no shared abbrev to
      // collide on, no script-addressability needed.
      auto *entry = Gtk::make_managed<curvz::widgets::Entry>(
                        curvz::scripting::unregistered);
      entry->set_text(row_orig_name);
      entry->set_hexpand(true);
      entry->set_margin_start(4);
      entry->set_margin_end(8);
      entry->set_margin_top(2);
      entry->set_margin_bottom(2);
      row_box->remove(*row_label);
      row_box->append(*entry);

      auto done_flag = std::make_shared<bool>(false);
      auto commit = [this, entry, ridx, done_flag]() {
        if (*done_flag) return;
        *done_flag = true;
        std::string new_name = entry->get_text();
        if (!new_name.empty())
          m_signal_rename_doc.emit(ridx, new_name);
        Glib::signal_idle().connect_once([this]() { refresh(); });
      };
      auto revert = [this, done_flag]() {
        if (*done_flag) return;
        *done_flag = true;
        Glib::signal_idle().connect_once([this]() { refresh(); });
      };

      entry->signal_activate().connect(commit);

      auto kc = Gtk::EventControllerKey::create();
      kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
      kc->signal_key_pressed().connect(
          [revert](guint kv, guint, Gdk::ModifierType) {
            if (kv == GDK_KEY_Escape) {
              revert();
              return true;
            }
            return false;
          },
          false);
      entry->add_controller(kc);

      entry->grab_focus();
      entry->select_region(0, -1);
    };

    auto rgesture = Gtk::GestureClick::create();
    rgesture->signal_pressed().connect(
        [this, ridx, begin_rename_row](int n_press, double, double) {
          if (n_press == 2) {
            begin_rename_row();
            return;
          }
          m_signal_doc_activated.emit(ridx);
        });
    row->add_controller(rgesture);

    // Right-click rename on list row too. Same build-once-reuse pattern
    // as the thumbnail tile above — see comment there.
    {
      auto group = Gio::SimpleActionGroup::create();
      group->add_action("rename",
                        [begin_rename_row]() { begin_rename_row(); });
      group->add_action("dup",
                        [this, ridx]() { m_signal_dup_doc.emit(ridx); });
      group->add_action("del",
                        [this, ridx]() { m_signal_remove_doc.emit(ridx); });
      row->insert_action_group("tile", group);

      auto menu = Gio::Menu::create();
      menu->append("Rename",    "tile.rename");
      menu->append("Duplicate", "tile.dup");
      menu->append("Delete",    "tile.del");

      auto *popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
      popover->set_parent(*row);
      popover->set_has_arrow(false);
      row->signal_destroy().connect([popover]() { popover->unparent(); });

      auto rrclick = Gtk::GestureClick::create();
      rrclick->set_button(GDK_BUTTON_SECONDARY);
      rrclick->signal_pressed().connect(
          [popover](int, double x, double y) {
            Gdk::Rectangle rect{(int)x, (int)y, 1, 1};
            popover->set_pointing_to(rect);
            popover->popup();
          });
      row->add_controller(rrclick);
    }

    m_list_box.append(*row);
  }

  // s360 — refresh the no-matches empty-state after rebuilding tiles.
  update_filter_empty_state();
}

} // namespace Curvz

namespace Curvz {

// ── SVG thumbnail renderer (system icons) ────────────────────────────────────
Cairo::RefPtr<Cairo::ImageSurface>
DocumentGallery::render_svg_thumb(const std::string &path, int size) {
  auto surf =
      Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, size, size);
  auto cr = Cairo::Context::create(surf);
  cr->set_source_rgb(0.13, 0.13, 0.13);
  cr->paint();

  auto doc = Curvz::parse_svg_file(path);
  if (!doc)
    return surf;

  // Normalize fill/stroke: convert any solid color to currentColor so
  // symbolic icons (which often hardcode black) render visibly on dark bg
  std::function<void(SceneNode &)> fix_style = [&](SceneNode &n) {
    if (n.fill.type == FillStyle::Type::Solid)
      n.fill.type = FillStyle::Type::CurrentColor;
    if (n.stroke.paint.type == FillStyle::Type::Solid)
      n.stroke.paint.type = FillStyle::Type::CurrentColor;
    for (auto &child : n.children)
      fix_style(*child);
  };
  for (auto &layer : doc->layers)
    fix_style(*layer);

  return render_thumb(doc.get(), size);
}

// ── System tab rebuild
// ────────────────────────────────────────────────────────
void DocumentGallery::rebuild_system_tab() {
  // Clear existing flow children
  while (auto *child = m_sys_flow.get_first_child())
    m_sys_flow.remove(*child);

  if (!m_scanner.is_scanned())
    return;

  auto icons = m_scanner.query(m_sys_theme, m_sys_category, m_filter);
  LOG_DEBUG("SystemTab: rendering {} icons (theme='{}' cat='{}' filter='{}')",
            icons.size(), m_sys_theme, m_sys_category, m_filter);

  for (const auto *ic : icons) {
    auto *frame = Gtk::make_managed<Gtk::Frame>();
    frame->set_size_request(THUMB_SIZE + 4, THUMB_SIZE + 20);
    frame->set_vexpand(false);
    frame->set_valign(Gtk::Align::START);
    frame->add_css_class("gallery-thumb");

    auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_spacing(1);
    vbox->set_margin(2);
    vbox->set_vexpand(false);

    auto *da = Gtk::make_managed<Gtk::DrawingArea>();
    da->set_size_request(THUMB_SIZE, THUMB_SIZE);
    da->set_vexpand(false);
    da->set_halign(Gtk::Align::CENTER);
    std::string icon_path = ic->path;
    da->set_draw_func([this, icon_path](const Cairo::RefPtr<Cairo::Context> &cr,
                                        int w, int h) {
      if (w < 4 || h < 4)
        return;
      int sz = std::min(w, h);
      auto surf = render_svg_thumb(icon_path, sz);
      if (!surf)
        return;
      double ox = (w - sz) * 0.5;
      double oy = (h - sz) * 0.5;
      cr->set_source(surf, ox, oy);
      cr->paint();
    });
    vbox->append(*da);

    auto *name_label = Gtk::make_managed<Gtk::Label>(ic->name);
    name_label->set_ellipsize(Pango::EllipsizeMode::END);
    name_label->set_max_width_chars(8);
    name_label->add_css_class("caption-label");
    vbox->append(*name_label);

    frame->set_child(*vbox);

    // Single click → preview (stage 3)
    // Double click → copy (stage 3)
    // For now just add gesture stubs
    auto gesture = Gtk::GestureClick::create();
    std::string cap_path = ic->path;
    gesture->signal_pressed().connect(
        [this, cap_path](int n_press, double, double) {
          if (n_press == 1)
            m_signal_preview_icon.emit(cap_path);
          else if (n_press == 2)
            m_signal_copy_icon.emit(cap_path);
        });
    frame->add_controller(gesture);

    m_sys_flow.append(*frame);
  }
}

} // namespace Curvz
