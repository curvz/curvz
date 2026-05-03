#include "DocumentGallery.hpp"
#include "CurvzLog.hpp"
#include "SvgParser.hpp"
#include "SvgWriter.hpp"
#include "math/BezierPath.hpp"
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
      m_thumb_scroll.set_visible(false);
      m_list_scroll.set_visible(true);
    } else {
      m_view_mode = ViewMode::Thumbnail;
      m_btn_view.set_icon_name("view-grid-symbolic");
      m_btn_view.set_tooltip_text("Switch to list view");
      m_list_scroll.set_visible(false);
      m_thumb_scroll.set_visible(true);
    }
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
  m_sys_theme_drop = Gtk::make_managed<Gtk::DropDown>(theme_list);
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
  m_sys_cat_drop = Gtk::make_managed<Gtk::DropDown>(cat_list);
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
    if (page == 0)
      m_signal_filter_changed.emit(m_filter);
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

  // S117 m14: thumbnail bg reads project artboard colour (per-motif),
  // not the old hardcoded #282828. Falls back to the dark default if
  // we don't have a project yet (early gallery rebuilds during boot).
  // Same applies to the currentColor sites below — light icon on a dark
  // artboard, dark icon on a light artboard, picked from project motif.
  double bg_r = 0.157, bg_g = 0.157, bg_b = 0.157;  // #282828 fallback
  bool light_motif = false;
  if (m_project) {
    bg_r = m_project->artboard_r();
    bg_g = m_project->artboard_g();
    bg_b = m_project->artboard_b();
    light_motif = (m_project->motif == Motif::Light);
  }
  // currentColor sample: dark on light motif, light on dark motif.
  double cc = light_motif ? 0.10 : 0.88;

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

      if (obj.fill.type == FillStyle::Type::CurrentColor)
        cr->set_source_rgb(cc, cc, cc);
      else if (obj.fill.type == FillStyle::Type::Solid)
        cr->set_source_rgb(obj.fill.r, obj.fill.g, obj.fill.b);
      if (obj.fill.type != FillStyle::Type::None)
        cr->fill_preserve();

      if (obj.stroke.paint.type == FillStyle::Type::CurrentColor)
        cr->set_source_rgb(cc, cc, cc);
      else if (obj.stroke.paint.type == FillStyle::Type::Solid)
        cr->set_source_rgb(obj.stroke.paint.r, obj.stroke.paint.g,
                           obj.stroke.paint.b);
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

      if (fill.type == FillStyle::Type::CurrentColor)
        cr->set_source_rgb(cc, cc, cc);
      else if (fill.type == FillStyle::Type::Solid)
        cr->set_source_rgb(fill.r, fill.g, fill.b);
      if (fill.type != FillStyle::Type::None) {
        cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
        cr->fill_preserve();
        cr->set_fill_rule(Cairo::Context::FillRule::WINDING);
      }

      if (stroke.paint.type == FillStyle::Type::CurrentColor)
        cr->set_source_rgb(cc, cc, cc);
      else if (stroke.paint.type == FillStyle::Type::Solid)
        cr->set_source_rgb(stroke.paint.r, stroke.paint.g, stroke.paint.b);
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
            gdk_cairo_set_source_pixbuf(cr2->cobj(), pb->gobj(), 0, 0);
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
      // Draw a simple grey placeholder bar representing the text baseline
      double fs = obj.text_font_size;
      double tx = obj.text_x;
      // text_y is Y-up; convert to Y-down doc space for Cairo
      double ty = (double)doc->canvas_height() - obj.text_y - fs;
      cr->save();
      cr->set_source_rgba(0.7, 0.7, 0.7, 0.6);
      double bar_w = std::min(fs * (double)obj.text_content.size() * 0.55,
                              (double)cw - tx);
      double bar_h = fs * 0.75;
      if (bar_w > 1.0 && bar_h > 1.0)
        cr->rectangle(tx, ty, bar_w, bar_h);
      cr->fill();
      cr->restore();
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
      auto *entry = Gtk::make_managed<Gtk::Entry>();
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
      auto *entry = Gtk::make_managed<Gtk::Entry>();
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
