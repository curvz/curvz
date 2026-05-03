#include "NewDocumentDialog.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <algorithm>
#include <cairomm/cairomm.h>
#include <cmath>
#include <gdkmm/pixbuf.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/stylecontext.h>
#include <string>

namespace Curvz {

// ── Quality slider stops ──────────────────────────────────────────────────────
struct QualityStop {
  const char *label;
  int value;
};
static const QualityStop QUALITY_STOPS[] = {
    {"Icon", 1000},
    {"Print", 4000},
    {"Poster", 10000},
    {"Billboard", 40000},
};
static constexpr int N_STOPS = 4;

// ── Pixel presets ─────────────────────────────────────────────────────────────
static const int PX_PRESETS[] = {16, 24, 32, 48, 64, 128, 256};

// ── Ratio presets ─────────────────────────────────────────────────────────────
struct RatioPreset {
  const char *label;
  double w;
  double h;
};
static const RatioPreset RATIO_PRESETS[] = {
    {"1:1", 1.0, 1.0},        {"4:3", 4.0, 3.0},          {"16:9", 16.0, 9.0},
    {"1:√2", 1.0, 1.4142135}, {"Golden", 1.0, 1.6180339}, {"2.35:1", 2.35, 1.0},
};

// ── DPI presets ───────────────────────────────────────────────────────────────
static const int DPI_PRESETS[] = {72, 96, 150, 300, 600};

// ── Thumbnail tile sizing ─────────────────────────────────────────────────────
static constexpr int THUMB_IMG_PX   = 128;   // image area
static constexpr int THUMB_LABEL_PX = 28;    // label height
static constexpr int TEMPLATE_COLS  = 3;     // grid columns

// ── Helper: make a labelled row for a grid ────────────────────────────────────
static void grid_row(Gtk::Grid &g, int row, const char *lbl, Gtk::Widget &w) {
  auto *label = Gtk::make_managed<Gtk::Label>(lbl);
  label->set_xalign(1.0f);
  label->set_margin_end(8);
  label->add_css_class("section-label");
  g.attach(*label, 0, row);
  g.attach(w, 1, row);
}

// ── Constructor ───────────────────────────────────────────────────────────────
NewDocumentDialog::NewDocumentDialog()
    : m_px_w(Gtk::Adjustment::create(24, 1, 65536, 1, 8), 0.0, 0),
      m_px_h(Gtk::Adjustment::create(24, 1, 65536, 1, 8), 0.0, 0),
      m_phys_w(Gtk::Adjustment::create(4.0, 0.001, 10000.0, 0.1, 1.0), 0.0, 3),
      m_phys_h(Gtk::Adjustment::create(4.0, 0.001, 10000.0, 0.1, 1.0), 0.0, 3),
      m_ratio_w(Gtk::Adjustment::create(1.0, 0.001, 100.0, 0.001, 0.1), 0.0, 4),
      m_ratio_h(Gtk::Adjustment::create(1.0, 0.001, 100.0, 0.001, 0.1), 0.0, 4),
      m_quality_slider(Gtk::Adjustment::create(0, 0, N_STOPS - 1, 1, 1),
                       Gtk::Orientation::HORIZONTAL),
      m_quality_spin(Gtk::Adjustment::create(1000, 1, 100000, 100, 1000), 0.0,
                     0) {
  curvz::utils::set_name(*this, "dlg_nd", "new_document_dialog_root");
  set_title("New Document");
  // Widened from the pre-template 520px so the 3×128 thumbnail grid fits
  // comfortably inside the Template tab.
  set_default_size(640, -1);
  set_resizable(false);
  set_hide_on_close(true);
  add_css_class("new-doc-dialog");

  // ── Root layout ───────────────────────────────────────────────────────
  auto *root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
  root->set_margin(16);
  root->set_spacing(12);
  set_child(*root);

  // ── Name row ──────────────────────────────────────────────────────────
  auto *name_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  name_row->set_spacing(8);
  auto *name_lbl = Gtk::make_managed<Gtk::Label>("Name");
  name_lbl->set_xalign(1.0f);
  name_lbl->set_width_chars(9);
  name_lbl->add_css_class("section-label");
  m_name_entry.set_hexpand(true);
  m_name_entry.set_placeholder_text("icon");
  curvz::utils::set_name(m_name_entry, "dlg_nd_nm", "new_document_dialog_name_entry");
  // NOTE: intentionally no on_commit(on_create). CurvzEntry's on_commit
  // fires on both Enter AND focus-leave; the latter means any click on a
  // template tile (which shifts focus off the entry) would submit the
  // dialog before the tile's clicked handler ran. User must click the
  // Create button to submit. Matches SaveAsTemplateDialog's behavior.
  // Track manual edits: once the user changes the text to something that
  // isn't our last autofill, treat the name as user-owned and stop
  // overwriting it on template selection.
  m_name_entry.signal_changed().connect([this]() {
    if (m_updating) return;
    std::string cur = m_name_entry.get_text();
    if (cur == m_last_autofill) {
      // Still the autofill — either it just ran, or the user cleared and
      // re-typed our exact string. In either case, we retain ownership.
      m_name_user_typed = false;
    } else {
      m_name_user_typed = true;
    }
  });
  name_row->append(*name_lbl);
  name_row->append(m_name_entry);
  root->append(*name_row);

  // ── Theme row (S104 m1 follow-on) ─────────────────────────────────────
  //
  // Sits directly under Name. Hidden when the project has no saved
  // themes (set_visible toggled in show()). Layout mirrors the Name row:
  // right-aligned label of matching width + hexpanding control.
  //
  // The dropdown's StringList and m_theme_drop_ids are populated fresh
  // on each show() — themes can be added/renamed/removed between opens
  // and we want the freshest list. Index 0 of the StringList is always
  // the "No theme" sentinel; m_theme_drop_ids[0] is the empty ThemeId.
  m_theme_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  m_theme_row->set_spacing(8);
  auto *theme_lbl = Gtk::make_managed<Gtk::Label>("Theme");
  theme_lbl->set_xalign(1.0f);
  theme_lbl->set_width_chars(9);
  theme_lbl->add_css_class("section-label");
  m_theme_drop = Gtk::make_managed<Gtk::DropDown>();
  curvz::utils::set_name(m_theme_drop, "dlg_nd_thm", "new_document_dialog_theme_dd");
  m_theme_drop->set_hexpand(true);
  // Remember selection on user change. Guard against the populate path
  // firing the signal during rebuild.
  m_theme_drop->property_selected().signal_changed().connect([this]() {
    if (m_updating) return;
    auto sel = static_cast<std::size_t>(m_theme_drop->get_selected());
    if (sel < m_theme_drop_ids.size()) {
      m_remembered_theme_id = m_theme_drop_ids[sel];
    }
  });
  m_theme_row->append(*theme_lbl);
  m_theme_row->append(*m_theme_drop);
  m_theme_row->set_visible(false);  // populated/shown by show() when applicable
  root->append(*m_theme_row);

  // ── Notebook ──────────────────────────────────────────────────────────
  curvz::utils::set_name(m_notebook, "dlg_nd_nb", "new_document_dialog_notebook");
  root->append(m_notebook);
  m_notebook.append_page(build_template_tab(), "Template");
  m_notebook.append_page(build_pixel_tab(),    "Pixel");
  m_notebook.append_page(build_physical_tab(), "Physical");
  m_notebook.append_page(build_ratio_tab(),    "Ratio / Quality");

  // ── Preview section ───────────────────────────────────────────────────
  auto *sep = Gtk::make_managed<Gtk::Separator>();
  root->append(*sep);

  auto *prev_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  prev_row->set_spacing(16);
  root->append(*prev_row);

  // Thumbnail
  m_thumb.set_size_request(80, 80);
  curvz::utils::set_name(m_thumb, "dlg_nd_tb", "new_document_dialog_thumb_da");
  m_thumb.set_draw_func(
      [this](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
        int cw = m_current.canvas_width();
        int ch = m_current.canvas_height();
        if (cw <= 0 || ch <= 0)
          return;
        double scale = std::min((double)(w - 4) / cw, (double)(h - 4) / ch);
        double dw = cw * scale;
        double dh = ch * scale;
        double ox = (w - dw) * 0.5;
        double oy = (h - dh) * 0.5;
        // shadow
        cr->set_source_rgba(0, 0, 0, 0.3);
        cr->rectangle(ox + 2, oy + 2, dw, dh);
        cr->fill();
        // canvas
        cr->set_source_rgb(0.15, 0.15, 0.15);
        cr->rectangle(ox, oy, dw, dh);
        cr->fill();
        // border
        cr->set_source_rgba(0.5, 0.5, 0.5, 0.8);
        cr->set_line_width(0.5);
        cr->rectangle(ox + 0.5, oy + 0.5, dw - 1, dh - 1);
        cr->stroke();
      });
  prev_row->append(m_thumb);

  // Preview label
  m_preview_label.set_xalign(0.0f);
  m_preview_label.add_css_class("dim-label");
  curvz::utils::set_name(m_preview_label, "dlg_nd_pvl", "new_document_dialog_preview_lbl");
  prev_row->append(m_preview_label);

  // ── Buttons ───────────────────────────────────────────────────────────
  auto *btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  btn_row->set_spacing(8);
  btn_row->set_halign(Gtk::Align::END);
  root->append(*btn_row);

  m_btn_cancel.set_margin_top(4);
  m_btn_create.set_margin_top(4);
  m_btn_create.add_css_class("suggested-action");
  curvz::utils::set_name(m_btn_cancel, "dlg_nd_cnc", "new_document_dialog_cancel_btn");
  curvz::utils::set_name(m_btn_create, "dlg_nd_cre", "new_document_dialog_create_btn");
  btn_row->append(m_btn_cancel);
  btn_row->append(m_btn_create);

  m_btn_cancel.signal_clicked().connect(
      sigc::mem_fun(*this, &NewDocumentDialog::on_cancel));
  m_btn_create.signal_clicked().connect(
      sigc::mem_fun(*this, &NewDocumentDialog::on_create));

  // Tab switch → refresh preview and, if leaving the template tab to a
  // dimension-input tab, clear any template selection so the Create button
  // reflects what the user is actually configuring.
  m_notebook.signal_switch_page().connect(
      [this](Gtk::Widget *, guint page) {
        if (page != 0) {
          // Non-template tab — clear any template pick so the canvas comes
          // from the tab inputs.
          if (m_selected_builtin != BuiltIn::None || m_selected_disk >= 0)
            clear_template_selection();
        }
        refresh_preview();
      });

  // Initial state
  m_current = CanvasModel::from_pixels(24, 24);
  refresh_preview();
}

// ── Tab: Template ─────────────────────────────────────────────────────────────
Gtk::Widget &NewDocumentDialog::build_template_tab() {
  m_tpl_box.set_margin(12);
  m_tpl_box.set_spacing(12);

  // The flow grid is rebuilt from scratch in populate_template_grid() on
  // every show(). Here we just wire the scroll container.
  m_tpl_scroll.set_child(m_tpl_box);
  m_tpl_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
  m_tpl_scroll.set_hexpand(true);
  m_tpl_scroll.set_vexpand(true);
  m_tpl_scroll.set_min_content_height(360);
  m_tpl_scroll.set_margin(12);
  return m_tpl_scroll;
}

// ── populate_template_grid ────────────────────────────────────────────────────
// Rebuild m_tpl_box contents from scratch. Structure:
//   ┌ "Built-in" heading
//   │   grid: [Blank][Default]
//   ├ "<category>" heading
//   │   grid: [tpl1][tpl2][tpl3]
//   └ ...
// Each tile is a Gtk::Frame with the .template-tile CSS class containing
// a DrawingArea for the thumbnail + Label for the name. A single GestureClick
// controller on the frame routes press 1 → select_builtin() / select_template()
// (updating m_selected_* and calling refresh_tile_selection() to flip the
// `.tile-selected` class), and press 2 → on_create() to commit-and-close.
// Frame (not Button) because Button's internal click handling consumes events
// in a way that prevents a sibling controller from seeing the n_press == 2
// streak — see make_tile().
void NewDocumentDialog::populate_template_grid() {
  // Clear existing children
  while (auto *child = m_tpl_box.get_first_child())
    m_tpl_box.remove(*child);

  // Scan disk templates
  m_disk_templates = templates::scan();

  // The scan now synthesizes Blank + Default as pseudo-entries in the
  // "builtin" category. The picker already surfaces those as fixed tiles
  // in the Built-in row below, so strip them here to avoid rendering the
  // pair twice (once in the fixed row, once under a "builtin" heading).
  m_disk_templates.erase(
      std::remove_if(m_disk_templates.begin(), m_disk_templates.end(),
                     [](const templates::TemplateEntry& e) {
                         return templates::is_builtin(e);
                     }),
      m_disk_templates.end());

  // Reset tile pointer registry — the widgets from the previous populate()
  // have been destroyed with their parent box, so these pointers are stale.
  // Disk-tile vector is sized to match m_disk_templates so index-based
  // lookup works after the fill loop below.
  m_tile_blank   = nullptr;
  m_tile_default = nullptr;
  m_tile_disk.assign(m_disk_templates.size(), nullptr);

  // Helpers --------------------------------------------------------------

  auto make_tile = [this](const std::string &name,
                          Gtk::DrawingArea *thumb_area,
                          std::function<void()> on_click) -> Gtk::Widget * {
    // Tile widget is a Gtk::Frame, not a Gtk::Button. A single
    // GestureClick controller on a Frame sees both press 1 and press 2
    // cleanly — Button's internal click controller would consume press 1
    // through its own machinery and break the double-click streak. Loses
    // keyboard activation on the tile (which the picker doesn't use); gains
    // press 1 = select, press 2 = commit-and-close in the natural way.
    auto *frame = Gtk::make_managed<Gtk::Frame>();
    frame->add_css_class("template-tile");

    auto *vbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    vbox->set_spacing(4);
    vbox->set_margin(4);

    thumb_area->set_size_request(THUMB_IMG_PX, THUMB_IMG_PX);
    vbox->append(*thumb_area);

    auto *lbl = Gtk::make_managed<Gtk::Label>(name);
    lbl->set_xalign(0.5f);
    lbl->set_ellipsize(Pango::EllipsizeMode::END);
    lbl->set_max_width_chars(16);
    lbl->set_size_request(-1, THUMB_LABEL_PX);
    vbox->append(*lbl);

    frame->set_child(*vbox);

    auto gesture = Gtk::GestureClick::create();
    gesture->signal_pressed().connect(
        [this, on_click](int n_press, double, double) {
          if (n_press == 1) on_click();
          else if (n_press == 2) on_create();
        });
    frame->add_controller(gesture);

    return frame;
  };

  auto add_heading = [this](const std::string &text) {
    auto *lbl = Gtk::make_managed<Gtk::Label>(text);
    lbl->set_xalign(0.0f);
    lbl->add_css_class("section-label");
    lbl->set_margin_top(4);
    m_tpl_box.append(*lbl);
  };

  auto make_grid = [this]() -> Gtk::Grid * {
    auto *g = Gtk::make_managed<Gtk::Grid>();
    g->set_row_spacing(12);
    g->set_column_spacing(12);
    m_tpl_box.append(*g);
    return g;
  };

  // ── Built-in section ──────────────────────────────────────────────────
  add_heading("Built-in");
  auto *builtin_grid = make_grid();

  // Blank tile
  {
    auto *area = Gtk::make_managed<Gtk::DrawingArea>();
    area->set_draw_func(
        [](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
          draw_builtin_thumb(cr, w, h, BuiltIn::Blank);
        });
    auto *tile = make_tile("Blank", area, [this]() {
      select_builtin(BuiltIn::Blank);
    });
    builtin_grid->attach(*tile, 0, 0);
    m_tile_blank = dynamic_cast<Gtk::Frame*>(tile);
  }
  // Default tile — normally renders the synthetic built-in Default seed and
  // calls select_builtin(Default). When the user has set a DISK template as
  // their default via Manage Templates (blue dot), the tile is replaced in-
  // place with that template: its thumbnail, its name, and clicking it
  // routes to select_template(disk_idx). This is the "Add to Project
  // defers to the user's chosen default" affordance — the user sees their
  // pick pre-offered at the top of the picker next to Blank.
  //
  // If the user starred a builtin (Blank or Default itself) we do NOT
  // substitute: starring Blank is expressed by pre-selection alone, and
  // starring Default is a no-op visually (the tile is already Default).
  {
    DefaultResolution r = resolve_effective_default();
    int user_disk_idx = (r.kind == ResolvedDefault::Disk) ? r.disk_idx : -1;

    auto *area = Gtk::make_managed<Gtk::DrawingArea>();
    std::string tile_label = "Default";

    if (user_disk_idx >= 0) {
      const auto &e = m_disk_templates[user_disk_idx];
      tile_label = e.meta.name;
      if (!e.thumb_path.empty()) {
        Glib::RefPtr<Gdk::Pixbuf> pb;
        try { pb = Gdk::Pixbuf::create_from_file(e.thumb_path); }
        catch (const Glib::Error &) {}
        area->set_draw_func(
            [pb](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
              if (!pb) {
                draw_builtin_thumb(cr, w, h, BuiltIn::Default);
                return;
              }
              int pw = pb->get_width();
              int ph = pb->get_height();
              if (pw <= 0 || ph <= 0) return;
              double scale = std::min((double)w / pw, (double)h / ph);
              double dw = pw * scale, dh = ph * scale;
              double ox = (w - dw) * 0.5, oy = (h - dh) * 0.5;
              cr->save();
              cr->translate(ox, oy);
              cr->scale(scale, scale);
              gdk_cairo_set_source_pixbuf(cr->cobj(), pb->gobj(), 0, 0);
              cr->paint();
              cr->restore();
            });
      } else {
        area->set_draw_func(
            [](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
              draw_builtin_thumb(cr, w, h, BuiltIn::Default);
            });
      }
    } else {
      area->set_draw_func(
          [](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
            draw_builtin_thumb(cr, w, h, BuiltIn::Default);
          });
    }

    auto *tile = make_tile(tile_label, area,
        [this, user_disk_idx]() {
          if (user_disk_idx >= 0) select_template(user_disk_idx);
          else                    select_builtin(BuiltIn::Default);
        });
    builtin_grid->attach(*tile, 1, 0);
    m_tile_default = dynamic_cast<Gtk::Frame*>(tile);
  }

  // ── Disk templates, grouped by category ───────────────────────────────
  // m_disk_templates arrives sorted: system-first, user second, alphabetical
  // by category then name within each group. We walk and open a new grid
  // whenever the category changes.
  std::string cur_category;
  Gtk::Grid *cat_grid = nullptr;
  int cat_col = 0;
  int cat_row = 0;

  for (size_t i = 0; i < m_disk_templates.size(); ++i) {
    const auto &e = m_disk_templates[i];
    if (e.meta.category != cur_category) {
      cur_category = e.meta.category;
      add_heading(cur_category);
      cat_grid = make_grid();
      cat_col  = 0;
      cat_row  = 0;
    }

    auto *area = Gtk::make_managed<Gtk::DrawingArea>();
    if (!e.thumb_path.empty()) {
      // Load the PNG once here; capture the pixbuf into the draw func so
      // redraws (hover, resize, theme change) don't re-hit the filesystem.
      // If the load fails the capture is null and the draw func paints a
      // placeholder.
      Glib::RefPtr<Gdk::Pixbuf> pb;
      try {
        pb = Gdk::Pixbuf::create_from_file(e.thumb_path);
      } catch (const Glib::Error &) {
        // Leave pb null — placeholder will be drawn
      }
      area->set_draw_func(
          [pb](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
            if (!pb) {
              cr->set_source_rgba(0.5, 0.5, 0.5, 0.3);
              cr->rectangle(2, 2, w - 4, h - 4);
              cr->fill();
              return;
            }
            int pw = pb->get_width();
            int ph = pb->get_height();
            if (pw <= 0 || ph <= 0) return;
            double scale = std::min((double)w / pw, (double)h / ph);
            double dw = pw * scale;
            double dh = ph * scale;
            double ox = (w - dw) * 0.5;
            double oy = (h - dh) * 0.5;
            cr->save();
            cr->translate(ox, oy);
            cr->scale(scale, scale);
            gdk_cairo_set_source_pixbuf(cr->cobj(), pb->gobj(), 0, 0);
            cr->paint();
            cr->restore();
          });
    } else {
      // No thumbnail file — paint a placeholder
      area->set_draw_func(
          [](const Cairo::RefPtr<Cairo::Context> &cr, int w, int h) {
            cr->set_source_rgba(0.5, 0.5, 0.5, 0.3);
            cr->rectangle(2, 2, w - 4, h - 4);
            cr->fill();
          });
    }

    int my_index = (int)i;
    auto *tile = make_tile(e.meta.name, area, [this, my_index]() {
      select_template(my_index);
    });
    cat_grid->attach(*tile, cat_col, cat_row);
    m_tile_disk[my_index] = dynamic_cast<Gtk::Frame*>(tile);
    if (++cat_col >= TEMPLATE_COLS) {
      cat_col = 0;
      ++cat_row;
    }
  }
}

// ── Selection handling ───────────────────────────────────────────────────────
NewDocumentDialog::DefaultResolution
NewDocumentDialog::resolve_effective_default() const {
  DefaultResolution r;
  auto ud = templates::user_default();
  if (!ud) return r;  // AppDefault, unset

  // Builtin/blank starred → treat Default tile as Blank.
  if (templates::is_builtin(*ud)) {
    switch (templates::builtin_kind(*ud)) {
      case templates::BuiltinKind::Blank:   r.kind = ResolvedDefault::Blank;      return r;
      case templates::BuiltinKind::Default: r.kind = ResolvedDefault::AppDefault; return r;
      case templates::BuiltinKind::None:    break;
    }
    return r;
  }

  // Disk template — find its index in m_disk_templates. Builtin entries
  // have been filtered out of the list, so indices here refer to disk
  // templates only.
  for (size_t i = 0; i < m_disk_templates.size(); ++i) {
    const auto& e = m_disk_templates[i];
    if (e.meta.category == ud->meta.category && e.slug == ud->slug) {
      r.kind = ResolvedDefault::Disk;
      r.disk_idx = (int)i;
      return r;
    }
  }

  // Default pointer references a template that doesn't resolve (stale
  // pointer, removed mid-session, etc.). Fall back to the app default.
  LOG_INFO("NewDocumentDialog: user default '{}/{}' did not resolve — using app default",
           ud->meta.category, ud->slug);
  return r;
}

void NewDocumentDialog::select_builtin(BuiltIn kind) {
  // The Default tile is special: if the user has set a default template,
  // clicking Default should act as if they'd clicked that template. This
  // keeps the preview, name autofill, and final seed consistent with what
  // Add to Project will produce when the user accepts without picking
  // anything else.
  if (kind == BuiltIn::Default) {
    auto r = resolve_effective_default();
    if (r.kind == ResolvedDefault::Disk) {
      select_template(r.disk_idx);
      return;
    }
    if (r.kind == ResolvedDefault::Blank) {
      // Recurse with Blank — the Blank branch below does the normal thing.
      select_builtin(BuiltIn::Blank);
      return;
    }
    // AppDefault: fall through to the synthetic Default seed.
  }

  m_selected_builtin = kind;
  m_selected_disk    = -1;
  const char *label = (kind == BuiltIn::Default) ? "Default" : "Blank";
  maybe_autofill_name(label);
  // Update preview: build the seed doc and read its canvas for the label
  auto seed = (kind == BuiltIn::Default) ? build_default_seed()
                                         : build_blank_seed();
  if (seed) m_current = seed->canvas;
  refresh_preview();
  refresh_tile_selection();
}

void NewDocumentDialog::select_template(int disk_index) {
  if (disk_index < 0 || disk_index >= (int)m_disk_templates.size()) return;
  m_selected_builtin = BuiltIn::None;
  m_selected_disk    = disk_index;
  const auto &e = m_disk_templates[disk_index];
  maybe_autofill_name(e.meta.name);
  // Parse the template SVG for an accurate preview. This is a bit
  // heavyweight — we're parsing on every click — but template SVGs are
  // small and this keeps the preview honest without a separate cache.
  auto seed = templates::load_document(e);
  if (seed) m_current = seed->canvas;
  refresh_preview();
  refresh_tile_selection();
}

void NewDocumentDialog::clear_template_selection() {
  m_selected_builtin = BuiltIn::None;
  m_selected_disk    = -1;
  refresh_tile_selection();
}

// ── refresh_tile_selection ───────────────────────────────────────────────────
// Walk the tile registry and apply `.selected` to exactly the tile matching
// the current m_selected_* state; strip it from the rest. Safe to call on a
// pre-populate state (null pointers) and on a stale state after rebuild —
// null checks everywhere.
//
// Special case: when the Default tile is "hijacked" into showing a user-
// chosen disk template (m_tile_default maps to that template), selecting
// that disk template via select_template() should highlight m_tile_default
// rather than the duplicate tile buried in a category section below. We
// detect this by matching m_selected_disk against the same resolution the
// grid builder used.
void NewDocumentDialog::refresh_tile_selection() {
  auto clear = [](Gtk::Frame* b) {
    if (b) b->remove_css_class("tile-selected");
  };
  auto set_on = [](Gtk::Frame* b) {
    if (b) b->add_css_class("tile-selected");
  };

  // Strip from every known tile first so we never end up with two tiles
  // lit simultaneously.
  clear(m_tile_blank);
  clear(m_tile_default);
  for (auto* b : m_tile_disk) clear(b);

  // Apply to the one matching current selection.
  if (m_selected_builtin == BuiltIn::Blank) {
    set_on(m_tile_blank);
    return;
  }
  if (m_selected_builtin == BuiltIn::Default) {
    set_on(m_tile_default);
    return;
  }
  if (m_selected_disk >= 0 && m_selected_disk < (int)m_tile_disk.size()) {
    // If this disk template is the one currently being shown in the top
    // Default slot (hijack case), highlight THAT tile — it's the visual
    // Anchor the user clicked. Also light up the category-section copy so
    // the duplication you noted is at least consistent.
    DefaultResolution r = resolve_effective_default();
    if (r.kind == ResolvedDefault::Disk && r.disk_idx == m_selected_disk) {
      set_on(m_tile_default);
    }
    set_on(m_tile_disk[m_selected_disk]);
  }
}

// ── Synthetic-thumbnail renderer ─────────────────────────────────────────────
void NewDocumentDialog::draw_builtin_thumb(
    const Cairo::RefPtr<Cairo::Context> &cr, int w, int h, BuiltIn kind) {
  // Background
  cr->set_source_rgb(0.12, 0.12, 0.12);
  cr->rectangle(0, 0, w, h);
  cr->fill();

  // Canvas area (inset)
  double inset = 10.0;
  double cx = inset;
  double cy = inset;
  double cw = w - inset * 2;
  double ch = h - inset * 2;

  // Canvas fill (lighter than background)
  cr->set_source_rgb(0.18, 0.18, 0.18);
  cr->rectangle(cx, cy, cw, ch);
  cr->fill();

  if (kind == BuiltIn::Default) {
    // Grid lines — 10×10 subdivisions to evoke 100×100 grid on a 1000-unit doc
    cr->set_source_rgba(0.45, 0.45, 0.7, 0.35);
    cr->set_line_width(0.5);
    for (int i = 1; i < 10; ++i) {
      double x = cx + cw * i / 10.0;
      cr->move_to(x, cy);
      cr->line_to(x, cy + ch);
      double y = cy + ch * i / 10.0;
      cr->move_to(cx, y);
      cr->line_to(cx + cw, y);
    }
    cr->stroke();
    // Margin rectangle (5% inset → corresponds to 50/1000 units)
    double m = 0.05;
    cr->set_source_rgba(0.85, 0.35, 0.35, 0.6);
    cr->set_line_width(1.0);
    cr->rectangle(cx + cw * m, cy + ch * m, cw * (1 - 2 * m),
                  ch * (1 - 2 * m));
    cr->stroke();
  }

  // Canvas border (always drawn)
  cr->set_source_rgba(0.55, 0.55, 0.55, 0.9);
  cr->set_line_width(1.0);
  cr->rectangle(cx + 0.5, cy + 0.5, cw - 1, ch - 1);
  cr->stroke();
}

// ── maybe_autofill_name ──────────────────────────────────────────────────────
// Writes "Untitled - <label>" to the name entry unless the user has typed
// their own value. The m_updating guard prevents the signal_changed handler
// from treating our own edit as user input.
void NewDocumentDialog::maybe_autofill_name(const std::string &label) {
  if (m_name_user_typed) return;
  std::string filled = "Untitled - " + label;
  m_updating    = true;
  m_last_autofill = filled;
  m_name_entry.set_text(filled);
  m_updating    = false;
}

// ── Built-in seed builders ───────────────────────────────────────────────────
std::unique_ptr<CurvzDocument> NewDocumentDialog::build_blank_seed() {
  auto doc = std::make_unique<CurvzDocument>();
  doc->canvas = CanvasModel::from_ratio(1.0, 1.0, 100);
  return doc;
}

std::unique_ptr<CurvzDocument> NewDocumentDialog::build_default_seed() {
  auto doc = std::make_unique<CurvzDocument>();
  doc->canvas = CanvasModel::from_ratio(1.0, 1.0, 1000);
  // Grid: 100×100 (the SceneNode default for grid_spacing_x/y)
  auto *gl = doc->ensure_grid_layer();
  gl->grid_spacing_x = 100.0;
  gl->grid_spacing_y = 100.0;
  gl->visible        = true;
  // Margins: 50 on all four sides
  auto *ml = doc->ensure_margin_layer();
  ml->margin_top    = 50.0;
  ml->margin_bottom = 50.0;
  ml->margin_left   = 50.0;
  ml->margin_right  = 50.0;
  ml->visible       = true;
  return doc;
}

// ── Tab: Pixel ────────────────────────────────────────────────────────────────
Gtk::Widget &NewDocumentDialog::build_pixel_tab() {
  m_pixel_box.set_margin(12);
  m_pixel_box.set_spacing(10);

  // Preset buttons
  auto *preset_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  preset_row->set_spacing(4);
  auto *preset_lbl = Gtk::make_managed<Gtk::Label>("Presets");
  preset_lbl->add_css_class("section-label");
  preset_lbl->set_margin_end(8);
  preset_row->append(*preset_lbl);

  for (int px : PX_PRESETS) {
    auto *btn = Gtk::make_managed<Gtk::Button>(std::to_string(px));
    btn->add_css_class("flat");
    btn->signal_clicked().connect([this, px]() {
      m_updating = true;
      m_px_w.set_value(px);
      m_px_h.set_value(px);
      m_updating = false;
      update_from_pixel();
    });
    preset_row->append(*btn);
  }
  m_pixel_box.append(*preset_row);

  // W / H inputs
  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(8);
  grid->set_column_spacing(8);
  m_pixel_box.append(*grid);

  m_px_w.set_digits(0);
  m_px_w.set_numeric(true);
  m_px_w.set_hexpand(true);
  curvz::utils::set_name(m_px_w, "dlg_nd_pxw", "new_document_dialog_pixel_w_spn");
  m_px_h.set_digits(0);
  m_px_h.set_numeric(true);
  m_px_h.set_hexpand(true);
  curvz::utils::set_name(m_px_h, "dlg_nd_pxh", "new_document_dialog_pixel_h_spn");

  grid_row(*grid, 0, "Width (px)", m_px_w);
  grid_row(*grid, 1, "Height (px)", m_px_h);

  m_px_w.signal_value_changed().connect([this]() {
    if (!m_updating) update_from_pixel();
  });
  m_px_h.signal_value_changed().connect([this]() {
    if (!m_updating) update_from_pixel();
  });

  return m_pixel_box;
}

// ── Tab: Physical ─────────────────────────────────────────────────────────────
Gtk::Widget &NewDocumentDialog::build_physical_tab() {
  m_phys_box.set_margin(12);
  m_phys_box.set_spacing(10);

  auto *grid = Gtk::make_managed<Gtk::Grid>();
  grid->set_row_spacing(8);
  grid->set_column_spacing(8);
  m_phys_box.append(*grid);

  m_phys_w.set_digits(3);
  m_phys_w.set_numeric(true);
  m_phys_w.set_hexpand(true);
  curvz::utils::set_name(m_phys_w, "dlg_nd_phw", "new_document_dialog_phys_w_spn");
  m_phys_h.set_digits(3);
  m_phys_h.set_numeric(true);
  m_phys_h.set_hexpand(true);
  curvz::utils::set_name(m_phys_h, "dlg_nd_phh", "new_document_dialog_phys_h_spn");

  // Unit dropdown
  auto unit_list = Gtk::StringList::create({"inches", "mm", "cm"});
  m_phys_unit.set_model(unit_list);
  m_phys_unit.set_selected(0);
  m_phys_unit.set_hexpand(true);
  curvz::utils::set_name(m_phys_unit, "dlg_nd_phu", "new_document_dialog_phys_unit_dd");

  // DPI dropdown
  auto dpi_list = Gtk::StringList::create({"72", "96", "150", "300", "600"});
  m_phys_dpi.set_model(dpi_list);
  m_phys_dpi.set_selected(3); // 300 dpi default
  m_phys_dpi.set_hexpand(true);
  curvz::utils::set_name(m_phys_dpi, "dlg_nd_phd", "new_document_dialog_phys_dpi_dd");

  grid_row(*grid, 0, "Width", m_phys_w);
  grid_row(*grid, 1, "Height", m_phys_h);
  grid_row(*grid, 2, "Units", m_phys_unit);
  grid_row(*grid, 3, "DPI", m_phys_dpi);

  auto update = [this]() {
    if (!m_updating) update_from_physical();
  };
  m_phys_w.signal_value_changed().connect(update);
  m_phys_h.signal_value_changed().connect(update);
  m_phys_unit.property_selected().signal_changed().connect(update);
  m_phys_dpi.property_selected().signal_changed().connect(update);

  return m_phys_box;
}

// ── Tab: Ratio / Quality ──────────────────────────────────────────────────────
Gtk::Widget &NewDocumentDialog::build_ratio_tab() {
  m_ratio_box.set_margin(12);
  m_ratio_box.set_spacing(10);

  // Ratio preset buttons
  auto *preset_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  preset_row->set_spacing(4);
  auto *preset_lbl = Gtk::make_managed<Gtk::Label>("Presets");
  preset_lbl->add_css_class("section-label");
  preset_lbl->set_margin_end(8);
  preset_row->append(*preset_lbl);

  for (const auto &p : RATIO_PRESETS) {
    auto *btn = Gtk::make_managed<Gtk::Button>(p.label);
    btn->add_css_class("flat");
    btn->signal_clicked().connect([this, p]() {
      m_updating = true;
      m_ratio_w.set_value(p.w);
      m_ratio_h.set_value(p.h);
      m_updating = false;
      update_from_ratio();
    });
    preset_row->append(*btn);
  }
  m_ratio_box.append(*preset_row);

  // Ratio inputs
  auto *ratio_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  ratio_row->set_spacing(6);
  auto *rl = Gtk::make_managed<Gtk::Label>("Ratio");
  rl->add_css_class("section-label");
  rl->set_margin_end(8);
  ratio_row->append(*rl);
  m_ratio_w.set_digits(4);
  m_ratio_w.set_numeric(true);
  m_ratio_w.set_hexpand(true);
  curvz::utils::set_name(m_ratio_w, "dlg_nd_rtw", "new_document_dialog_ratio_w_spn");
  ratio_row->append(m_ratio_w);
  auto *colon = Gtk::make_managed<Gtk::Label>(":");
  colon->set_margin_start(4);
  colon->set_margin_end(4);
  ratio_row->append(*colon);
  m_ratio_h.set_digits(4);
  m_ratio_h.set_numeric(true);
  m_ratio_h.set_hexpand(true);
  curvz::utils::set_name(m_ratio_h, "dlg_nd_rth", "new_document_dialog_ratio_h_spn");
  ratio_row->append(m_ratio_h);
  m_ratio_box.append(*ratio_row);

  // Quality slider
  auto *q_lbl = Gtk::make_managed<Gtk::Label>("Quality");
  q_lbl->add_css_class("section-label");
  q_lbl->set_xalign(0.0f);
  m_ratio_box.append(*q_lbl);

  // Slider stop labels
  auto *stop_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  stop_row->set_hexpand(true);
  for (const auto &s : QUALITY_STOPS) {
    auto *lbl = Gtk::make_managed<Gtk::Label>(s.label);
    lbl->add_css_class("caption-label");
    lbl->set_hexpand(true);
    lbl->set_xalign(0.5f);
    stop_row->append(*lbl);
  }
  m_ratio_box.append(*stop_row);

  m_quality_slider.set_hexpand(true);
  m_quality_slider.set_digits(0);
  m_quality_slider.set_draw_value(false);
  curvz::utils::set_name(m_quality_slider, "dlg_nd_qsl", "new_document_dialog_quality_slider");
  for (int i = 0; i < N_STOPS; ++i)
    m_quality_slider.add_mark(i, Gtk::PositionType::BOTTOM, "");
  m_ratio_box.append(m_quality_slider);

  // Quality spin (exact value)
  auto *spin_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
  spin_row->set_spacing(6);
  auto *sl = Gtk::make_managed<Gtk::Label>("Exact");
  sl->add_css_class("section-label");
  sl->set_margin_end(8);
  spin_row->append(*sl);
  m_quality_spin.set_digits(0);
  m_quality_spin.set_numeric(true);
  m_quality_spin.set_hexpand(true);
  curvz::utils::set_name(m_quality_spin, "dlg_nd_qsp", "new_document_dialog_quality_spn");
  spin_row->append(m_quality_spin);
  m_ratio_box.append(*spin_row);

  // Wire signals
  m_ratio_w.signal_value_changed().connect([this]() {
    if (!m_updating) update_from_ratio();
  });
  m_ratio_h.signal_value_changed().connect([this]() {
    if (!m_updating) update_from_ratio();
  });

  m_quality_slider.signal_value_changed().connect([this]() {
    if (m_updating) return;
    int idx = (int)std::round(m_quality_slider.get_value());
    idx = std::clamp(idx, 0, N_STOPS - 1);
    m_updating = true;
    m_quality_spin.set_value(QUALITY_STOPS[idx].value);
    m_updating = false;
    update_from_ratio();
  });
  m_quality_spin.signal_value_changed().connect([this]() {
    if (m_updating) return;
    int qval = (int)m_quality_spin.get_value();
    int best = 0;
    int best_d = std::abs(qval - QUALITY_STOPS[0].value);
    for (int i = 1; i < N_STOPS; ++i) {
      int d = std::abs(qval - QUALITY_STOPS[i].value);
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    m_updating = true;
    m_quality_slider.set_value(best);
    m_updating = false;
    update_from_ratio();
  });

  return m_ratio_box;
}

// ── Update from tab ───────────────────────────────────────────────────────────
void NewDocumentDialog::update_from_pixel() {
  m_current = compute_pixel();
  refresh_preview();
}
void NewDocumentDialog::update_from_physical() {
  m_current = compute_physical();
  refresh_preview();
}
void NewDocumentDialog::update_from_ratio() {
  m_current = compute_ratio();
  refresh_preview();
}

// ── Compute helpers ───────────────────────────────────────────────────────────
CanvasModel NewDocumentDialog::compute_pixel() const {
  int pw = std::max(1, (int)m_px_w.get_value());
  int ph = std::max(1, (int)m_px_h.get_value());
  return CanvasModel::from_pixels(pw, ph);
}
CanvasModel NewDocumentDialog::compute_physical() const {
  double w = m_phys_w.get_value();
  double h = m_phys_h.get_value();
  static const char *units[] = {"in", "mm", "cm"};
  int unit_idx = (int)m_phys_unit.get_selected();
  std::string unit = units[std::clamp(unit_idx, 0, 2)];
  int dpi = DPI_PRESETS[std::clamp((int)m_phys_dpi.get_selected(), 0, 4)];
  return CanvasModel::from_physical(w, h, unit, dpi);
}
CanvasModel NewDocumentDialog::compute_ratio() const {
  double rw = m_ratio_w.get_value();
  double rh = m_ratio_h.get_value();
  int quality = std::max(1, (int)m_quality_spin.get_value());
  return CanvasModel::from_ratio(rw, rh, quality);
}

// ── Refresh preview readout + thumbnail ──────────────────────────────────────
void NewDocumentDialog::refresh_preview() {
  int tab = m_notebook.get_current_page();
  // Template tab (0): m_current is already set by select_*; don't overwrite.
  // Other tabs: re-derive from inputs.
  if (tab == 1)
    m_current = compute_pixel();
  else if (tab == 2)
    m_current = compute_physical();
  else if (tab == 3)
    m_current = compute_ratio();

  int cw = m_current.canvas_width();
  int ch = m_current.canvas_height();

  // Build preview string
  std::string info;
  info += "Canvas:  " + std::to_string(cw) + " × " + std::to_string(ch) +
          " units\n";

  char ratio_buf[64];
  snprintf(ratio_buf, sizeof(ratio_buf), "Ratio:    %.4g : %.4g",
           m_current.ratio_w, m_current.ratio_h);
  info += ratio_buf;
  info += "\n";
  info +=
      "Quality: " + std::to_string(m_current.quality) + " units (short axis)";

  if (m_current.display_mode == DisplayMode::Physical) {
    char buf[128];
    snprintf(buf, sizeof(buf), "\n%.3g × %.3g %s @ %d dpi",
             m_current.phys_width, m_current.phys_height,
             m_current.phys_unit.c_str(), m_current.dpi);
    info += buf;
  }

  m_preview_label.set_text(info);
  m_thumb.queue_draw();
}

// ── show ──────────────────────────────────────────────────────────────────────

void NewDocumentDialog::show(Gtk::Window &parent,
                             const std::vector<theme::Theme>& available_themes,
                             Callback cb) {
  m_callback = std::move(cb);
  set_transient_for(parent);
  curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
  set_modal(true);

  // Reset name state — each show() starts fresh.
  m_updating       = true;
  m_name_user_typed = false;
  m_last_autofill.clear();
  m_name_entry.set_text("");
  m_updating       = false;

  // ── Theme dropdown population (S104 m1 follow-on) ──────────────────────
  //
  // Always rebuild from the caller-supplied list — themes may have been
  // added/renamed/removed since the last show(). Persistent selection:
  // if m_remembered_theme_id is non-empty AND still present in the new
  // list, pre-select it; otherwise fall back to "No theme" (index 0)
  // and clear the remembered id (the theme it referenced is gone).
  //
  // m_updating guards the dropdown's signal_changed so this rebuild
  // doesn't accidentally clobber m_remembered_theme_id.
  if (m_theme_row && m_theme_drop) {
    m_updating = true;
    m_theme_drop_ids.clear();

    auto list = Gtk::StringList::create({});
    list->append("No theme");
    m_theme_drop_ids.emplace_back();  // empty id = "No theme" sentinel

    int restored_index = 0;  // default: No theme
    for (const auto& t : available_themes) {
      list->append(t.header.name);
      m_theme_drop_ids.push_back(t.header.id);
      if (!m_remembered_theme_id.empty() &&
          t.header.id == m_remembered_theme_id) {
        restored_index = static_cast<int>(m_theme_drop_ids.size()) - 1;
      }
    }
    m_theme_drop->set_model(list);
    m_theme_drop->set_selected(static_cast<guint>(restored_index));

    // If the remembered id wasn't found, clear it — the theme was deleted.
    if (restored_index == 0) {
      m_remembered_theme_id.clear();
    }

    m_theme_row->set_visible(!available_themes.empty());
    m_updating = false;
  }

  // Clear any lingering selection from a previous show
  m_selected_builtin = BuiltIn::None;
  m_selected_disk    = -1;

  // Rebuild the template grid with the latest disk scan.
  // Deferred via signal_idle to avoid "snapshot without a current allocation"
  // warnings on GtkBox — same S48/S60 M1 pattern. Synchronous widget-tree
  // mutation during present() races the frame-clock paint idle.
  //
  // After population, pre-select the effective default so opening the
  // dialog and pressing Create without picking anything gives the user
  // their chosen template (or the built-in Default if none is set). This
  // is what makes "Add to Project defers to the user's chosen default"
  // visible — the right tile is highlighted and the preview reflects it
  // before any click.
  Glib::signal_idle().connect_once([this]() {
    populate_template_grid();
    DefaultResolution r = resolve_effective_default();
    switch (r.kind) {
      case ResolvedDefault::Disk:
        select_template(r.disk_idx);
        break;
      case ResolvedDefault::Blank:
        select_builtin(BuiltIn::Blank);
        break;
      case ResolvedDefault::AppDefault:
        select_builtin(BuiltIn::Default);
        break;
    }
  });

  // Default tab: Template (page 0)
  m_notebook.set_current_page(0);

  // Seed the preview with a sensible default while no template is picked.
  // Mirrors the pre-M3 default of a 1000-quality 1:1 canvas.
  m_current = CanvasModel::from_ratio(1.0, 1.0, 1000);
  m_updating = true;
  m_ratio_w.set_value(1.0);
  m_ratio_h.set_value(1.0);
  m_quality_spin.set_value(1000);
  m_quality_slider.set_value(0);
  m_px_w.set_value(1000);
  m_px_h.set_value(1000);
  m_updating = false;

  refresh_preview();
  present();
}

// ── Actions ───────────────────────────────────────────────────────────────────
void NewDocumentDialog::on_create() {
  std::unique_ptr<CurvzDocument> seed;

  // Build the seed according to the active mode:
  //   - Template tab + built-in picked      → synthetic seed
  //   - Template tab + disk template picked → load_document() clone
  //   - Template tab + nothing picked       → treat as Blank (forgiving UX)
  //   - Pixel/Physical/Ratio tab            → empty doc + tab's CanvasModel
  int tab = m_notebook.get_current_page();
  if (tab == 0) {
    if (m_selected_disk >= 0 &&
        m_selected_disk < (int)m_disk_templates.size()) {
      seed = templates::load_document(m_disk_templates[m_selected_disk]);
      if (!seed) {
        LOG_WARN("NewDocumentDialog: template load failed, falling back to blank");
        seed = build_blank_seed();
      }
    } else if (m_selected_builtin == BuiltIn::Default) {
      seed = build_default_seed();
    } else {
      // Blank or nothing picked
      seed = build_blank_seed();
    }
  } else {
    seed = std::make_unique<CurvzDocument>();
    CanvasModel cm;
    if (tab == 1) cm = compute_pixel();
    else if (tab == 2) cm = compute_physical();
    else cm = compute_ratio();
    seed->canvas = cm;
  }

  if (!seed) {
    LOG_ERROR("NewDocumentDialog::on_create: no seed produced");
    return;
  }

  // Filename is caller-assigned; clear any stray value the template may
  // have held (load_document already does this, but belt-and-braces).
  seed->filename.clear();

  std::string name = m_name_entry.get_text();
  // We let autofill names ("Untitled - Default", template name, etc.) pass
  // through to the caller unchanged. Previously this path cleared the name
  // when the user didn't edit the autofill, forcing the caller to fall
  // back to "icon" — that was wrong for the template flow (user picks a
  // template, sees the name autofill, expects that to be the doc's name).
  // An empty entry still produces an empty string here, which the caller
  // turns into "icon" via its own fallback.

  LOG_INFO("NewDocumentDialog: created canvas {}×{} ratio {:.4g}:{:.4g} quality {}",
           seed->canvas.canvas_width(), seed->canvas.canvas_height(),
           seed->canvas.ratio_w, seed->canvas.ratio_h, seed->canvas.quality);

  // Resolve the theme dropdown selection to an optional ThemeId. The
  // remembered id is kept on the dialog instance across opens — see
  // m_remembered_theme_id docs in the header.
  std::optional<theme::ThemeId> chosen_theme;
  if (!m_remembered_theme_id.empty()) {
    chosen_theme = m_remembered_theme_id;
  }

  hide();
  if (m_callback) m_callback(std::move(seed), std::move(name),
                             std::move(chosen_theme));
}

void NewDocumentDialog::on_cancel() { hide(); }

} // namespace Curvz
