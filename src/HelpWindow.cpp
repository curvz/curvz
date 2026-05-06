#include "HelpWindow.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2

#include <cmark.h>
#include <gio/gio.h>
#include <gdkmm/texture.h>
#include <gtkmm/button.h>          // s129 m7 — flat-button leaf rows
#include <gtkmm/eventcontrollerkey.h>  // s132 m1 — Ctrl+F focuses search
#include <gtkmm/frame.h>
#include <gtkmm/gestureclick.h>    // s129 m7 — click-toggle on header rows
#include <gtkmm/image.h>           // s131 v4 — fixed-size icon for headings
#include <gtkmm/label.h>
#include <gtkmm/picture.h>
#include <gtkmm/separator.h>

#include <cstring>
#include <sstream>

namespace Curvz {

// ─── Static helpers (markdown rendering) ──────────────────────────────────

namespace {

// Read a gresource file's contents into a std::string. Returns empty on
// failure (logs a warning). Uses the C gio API to match the pattern used
// in Application.cpp's icon extraction — keeps the codebase consistent.
std::string read_resource(const std::string &path) {
  GError *err = nullptr;
  GBytes *bytes = g_resources_lookup_data(
      path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE, &err);
  if (!bytes) {
    LOG_WARN("HelpWindow: failed to read resource '{}': {}", path,
             err ? err->message : "?");
    if (err) g_error_free(err);
    return {};
  }
  gsize sz = 0;
  const char *p = static_cast<const char *>(g_bytes_get_data(bytes, &sz));
  std::string out(p, sz);
  g_bytes_unref(bytes);
  return out;
}

// Pango markup escape — &, <, >, '. Used when feeding cmark's plain text
// content into a Gtk::Label that uses set_use_markup(true) for inline
// emphasis tags (<b>, <i>, <tt>).
std::string pango_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '\'': out += "&apos;"; break;
      case '"':  out += "&quot;"; break;
      default:   out += c;
    }
  }
  return out;
}

// Walk an inline cmark node tree and return Pango markup.
// Handles: text, emph (<i>), strong (<b>), code (<tt>), softbreak/linebreak.
// Links render as "text" + " (url)" plain — clickable links would require
// LinkButton or a TextView with Pango link tags, deferred to v2.
std::string render_inlines(cmark_node *parent) {
  std::string out;
  cmark_iter *iter = cmark_iter_new(parent);
  cmark_event_type ev;
  while ((ev = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
    cmark_node *n = cmark_iter_get_node(iter);
    cmark_node_type t = cmark_node_get_type(n);
    bool entering = (ev == CMARK_EVENT_ENTER);

    // Skip the parent itself (we only want descendants)
    if (n == parent) continue;

    switch (t) {
      case CMARK_NODE_TEXT:
        if (entering) {
          const char *lit = cmark_node_get_literal(n);
          if (lit) out += pango_escape(lit);
        }
        break;
      case CMARK_NODE_SOFTBREAK:
      case CMARK_NODE_LINEBREAK:
        if (entering) out += " ";
        break;
      case CMARK_NODE_CODE:
        if (entering) {
          const char *lit = cmark_node_get_literal(n);
          out += "<tt>";
          if (lit) out += pango_escape(lit);
          out += "</tt>";
        }
        break;
      case CMARK_NODE_EMPH:
        out += entering ? "<i>" : "</i>";
        break;
      case CMARK_NODE_STRONG:
        out += entering ? "<b>" : "</b>";
        break;
      case CMARK_NODE_LINK:
        // Render as "text (url)" plain — no clickable handling in v1
        if (!entering) {
          const char *url = cmark_node_get_url(n);
          if (url && *url) {
            out += " (";
            out += pango_escape(url);
            out += ")";
          }
        }
        break;
      default:
        // Unhandled inline types fall through silently
        break;
    }
  }
  cmark_iter_free(iter);
  return out;
}

// Build a Gtk::Label widget for a paragraph or heading. The text is Pango
// markup with inline emphasis applied. Heading level adjusts CSS classes
// for typography.
Gtk::Label *make_text_block(const std::string &markup, int heading_level) {
  auto *lbl = Gtk::make_managed<Gtk::Label>();
  lbl->set_markup(markup);
  lbl->set_wrap(true);
  lbl->set_xalign(0.0f);
  lbl->set_halign(Gtk::Align::FILL);
  lbl->set_hexpand(true);
  lbl->set_selectable(true);
  if (heading_level == 1) {
    lbl->add_css_class("title-1");
    lbl->set_margin_top(8);
    lbl->set_margin_bottom(12);
  } else if (heading_level == 2) {
    lbl->add_css_class("title-2");
    lbl->set_margin_top(16);
    lbl->set_margin_bottom(8);
  } else if (heading_level >= 3) {
    lbl->add_css_class("title-3");
    lbl->set_margin_top(12);
    lbl->set_margin_bottom(6);
  } else {
    lbl->set_margin_bottom(8);
  }
  return lbl;
}

// s131: heading with a leading icon — produces a horizontal Box laying
// out a 36px square Picture followed by the heading Label, both
// vertically centred. Used when a heading like
//
//     # ![Pen tool icon](img/icons/curvz-pen-symbolic.svg) Pen
//
// is detected. The Label is built the same way make_text_block builds
// its level-1/2/3 label (same css classes, same wrap settings), but
// without the title's own top/bottom margins — those go on the outer
// Box instead so the icon and the title share margin baselines and
// don't drift apart vertically.
//
// Sizing: the icon is fixed at 36px regardless of heading level. Per
// s131 design call, all 15 chapter-4 tool leaves use H1 with the same
// icon size, so per-level scaling adds nothing today and would just
// invite drift later.
//
// `icon_url` follows the same gresource-path resolution as
// make_image_block (relative paths are anchored at
// /com/curvz/app/help/, absolute paths are taken as-is). Texture
// failures fall through to a placeholder string in place of the icon
// so the heading still renders and the broken reference is visible.
Gtk::Widget *make_heading_with_icon(const std::string &icon_url,
                                    const std::string &alt,
                                    const std::string &text_markup,
                                    int heading_level) {
  static constexpr int kIconSizePx = 36;

  // s150: detect "img/icons/curvz-<name>-symbolic.svg" — these are GTK
  // symbolic icons registered via Application.cpp's icon-theme
  // add_resource_path. Rendering them via set_from_icon_name lets GTK
  // honour `-gtk-icon-style: symbolic` and recolour with the active
  // theme (the icons are black-on-black on dark mode otherwise — the
  // raster Gdk::Texture path bakes the SVG's native fill colour). For
  // non-symbolic / non-icon images (screenshots, diagrams), keep the
  // existing texture path.
  std::string icon_name;
  {
    constexpr const char* kPrefix = "img/icons/";
    constexpr const char* kSuffix = ".svg";
    if (icon_url.rfind(kPrefix, 0) == 0 &&
        icon_url.size() > std::strlen(kPrefix) + std::strlen(kSuffix) &&
        icon_url.compare(icon_url.size() - std::strlen(kSuffix),
                         std::strlen(kSuffix), kSuffix) == 0) {
      icon_name = icon_url.substr(
          std::strlen(kPrefix),
          icon_url.size() - std::strlen(kPrefix) - std::strlen(kSuffix));
    }
  }

  // Resolve URL the same way make_image_block does.
  std::string res_path;
  if (!icon_url.empty() && icon_url[0] == '/') {
    res_path = icon_url;
  } else {
    res_path = "/com/curvz/app/help/" + icon_url;
  }

  // Probe-then-load (same defensive pattern as make_image_block — see
  // its comment block on why g_resources_get_info has to gate the load
  // call). Skipped when icon_name is set (symbolic path below renders
  // via icon-theme instead of texture).
  Glib::RefPtr<Gdk::Texture> tex;
  if (icon_name.empty()) {
    GError *probe_err = nullptr;
    if (g_resources_get_info(res_path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE,
                             nullptr, nullptr, &probe_err)) {
      try {
        tex = Gdk::Texture::create_from_resource(res_path);
      } catch (const Glib::Error &e) {
        LOG_WARN("HelpWindow: heading icon '{}' load failed: {}",
                 res_path, e.what());
      } catch (const std::exception &e) {
        LOG_WARN("HelpWindow: heading icon '{}' load threw std::exception: {}",
                 res_path, e.what());
      } catch (...) {
        LOG_WARN("HelpWindow: heading icon '{}' load threw unknown exception",
                 res_path);
      }
    } else {
      LOG_WARN("HelpWindow: heading icon resource '{}' not found: {}",
               res_path, probe_err ? probe_err->message : "?");
      if (probe_err) g_error_free(probe_err);
    }
  }

  // Build the row: HBox [icon | title] vertically centred.
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
  row->set_halign(Gtk::Align::FILL);
  row->set_hexpand(true);

  // Outer margins mirror the per-level margins make_text_block applies
  // to its bare label, so a heading-with-icon and a heading-without-icon
  // sit at the same vertical position on the page.
  if (heading_level == 1) {
    row->set_margin_top(8);
    row->set_margin_bottom(12);
  } else if (heading_level == 2) {
    row->set_margin_top(16);
    row->set_margin_bottom(8);
  } else if (heading_level >= 3) {
    row->set_margin_top(12);
    row->set_margin_bottom(6);
  } else {
    row->set_margin_bottom(8);
  }

  // Icon — fixed 36px square, vertically centred against the title text.
  //
  // s131 v3→v4: switched from Gtk::Picture to Gtk::Image because the
  // 15 tool SVGs declare wildly inconsistent intrinsic sizes (some
  // viewBox 16×16 with no width/height, some viewBox 1000×1000 with
  // width="1000" height="1000", some viewBox 1000×1000 with width="48"
  // — see s131 m15 v3 build report). Gtk::Picture honours the SVG's
  // intrinsic size as its natural size, so set_size_request was a
  // *minimum* and parents granting more space let the 1000-declared
  // ones balloon. Gtk::Image with set_pixel_size FORCES the rendered
  // size regardless of intrinsic — same behaviour Toolbar uses for
  // its tool icons via set_from_icon_name.
  //
  // s150: symbolic icons render via set_from_icon_name so GTK applies
  // the symbolic-recolour pipeline (`-gtk-icon-style: symbolic` +
  // `color: currentColor`). Without it, dark mode shows black-on-black.
  if (!icon_name.empty()) {
    auto *img = Gtk::make_managed<Gtk::Image>();
    img->set_from_icon_name(icon_name);
    img->set_pixel_size(kIconSizePx);
    img->set_valign(Gtk::Align::CENTER);
    img->set_halign(Gtk::Align::START);
    img->set_tooltip_text(alt);
    img->add_css_class("help-symbolic-icon");
    row->append(*img);
  } else if (tex) {
    auto *img = Gtk::make_managed<Gtk::Image>();
    img->set(tex);
    img->set_pixel_size(kIconSizePx);
    img->set_valign(Gtk::Align::CENTER);
    img->set_halign(Gtk::Align::START);
    img->set_tooltip_text(alt);
    row->append(*img);
  } else {
    auto *placeholder = Gtk::make_managed<Gtk::Label>();
    placeholder->set_text("[icon missing: " + icon_url + "]");
    placeholder->add_css_class("dim-label");
    placeholder->set_valign(Gtk::Align::CENTER);
    placeholder->set_halign(Gtk::Align::START);
    row->append(*placeholder);
  }

  // Title — same css class as make_text_block applies for the level,
  // but without margins (the row owns those) so the label hugs the
  // icon's vertical centre.
  auto *lbl = Gtk::make_managed<Gtk::Label>();
  lbl->set_markup(text_markup);
  lbl->set_wrap(true);
  lbl->set_xalign(0.0f);
  lbl->set_halign(Gtk::Align::FILL);
  lbl->set_hexpand(true);
  lbl->set_valign(Gtk::Align::CENTER);
  lbl->set_selectable(true);
  if (heading_level == 1) {
    lbl->add_css_class("title-1");
  } else if (heading_level == 2) {
    lbl->add_css_class("title-2");
  } else if (heading_level >= 3) {
    lbl->add_css_class("title-3");
  }
  row->append(*lbl);

  return row;
}

// Build a code-block widget: a Frame wrapping a monospace Label.
Gtk::Widget *make_code_block(const std::string &text) {
  auto *frame = Gtk::make_managed<Gtk::Frame>();
  frame->add_css_class("card");
  frame->set_margin_top(4);
  frame->set_margin_bottom(8);
  auto *lbl = Gtk::make_managed<Gtk::Label>(text);
  lbl->set_xalign(0.0f);
  lbl->set_halign(Gtk::Align::FILL);
  lbl->set_hexpand(true);
  lbl->set_selectable(true);
  lbl->add_css_class("monospace");
  lbl->set_margin_start(8);
  lbl->set_margin_end(8);
  lbl->set_margin_top(6);
  lbl->set_margin_bottom(6);
  frame->set_child(*lbl);
  return frame;
}

// Build one bullet-list item — "•  <markup>" inside an indented Box. Tight
// vs loose lists are not distinguished in v1 (both render the same).
Gtk::Widget *make_list_item(const std::string &markup) {
  auto *row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
  row->set_margin_start(12);
  row->set_margin_top(2);
  row->set_margin_bottom(2);
  auto *bullet = Gtk::make_managed<Gtk::Label>("•");
  bullet->set_valign(Gtk::Align::START);
  auto *lbl = Gtk::make_managed<Gtk::Label>();
  lbl->set_markup(markup);
  lbl->set_wrap(true);
  lbl->set_xalign(0.0f);
  lbl->set_halign(Gtk::Align::FILL);
  lbl->set_hexpand(true);
  lbl->set_selectable(true);
  row->append(*bullet);
  row->append(*lbl);
  return row;
}

// S124 m10: Build an image block from a markdown `![alt](url)` reference.
// The url is the relative path written in the markdown (e.g. "img/foo.png").
// We resolve it against the help gresource base path (/com/curvz/app/help/)
// so the resulting full path is e.g. /com/curvz/app/help/img/foo.png — which
// matches the alias= entries in resources.xml under the help <gresource>
// block.
//
// Loading uses Gdk::Texture::create_from_resource (GTK4-native) into a
// Gtk::Picture. If the resource is missing or fails to load we fall back to
// a dim italic placeholder showing the path so the topic still renders and
// the missing reference is visible to the author.
//
// `alt` becomes the picture's alternative text (a11y / tooltip).
Gtk::Widget *make_image_block(const std::string &url, const std::string &alt) {
  // Resolve relative path to gresource path. URLs that already start with /
  // are taken as-is in case a topic ever uses an absolute resource path.
  std::string res_path;
  if (!url.empty() && url[0] == '/') {
    res_path = url;
  } else {
    res_path = "/com/curvz/app/help/" + url;
  }

  // Belt-and-braces guard against a missing or unreadable image resource.
  //
  // Gdk::Texture::create_from_resource calls g_error() — i.e. abort() — when
  // the resource is missing, BEFORE any C++ exception can be thrown. So the
  // try/catch alone is not enough; we must check the resource exists first.
  // g_resources_get_info() is the existence probe (it doesn't load the data,
  // so it's cheap). Only when the probe succeeds do we attempt the load,
  // which CAN still throw on corrupted bytes / decode failure / OOM — hence
  // the try/catch around it. Both paths fall through to the placeholder
  // below, so missing screenshots show the path string instead of crashing.
  Glib::RefPtr<Gdk::Texture> tex;
  GError *probe_err = nullptr;
  GResourceLookupFlags flags = G_RESOURCE_LOOKUP_FLAGS_NONE;
  if (g_resources_get_info(res_path.c_str(), flags, nullptr, nullptr,
                           &probe_err)) {
    try {
      tex = Gdk::Texture::create_from_resource(res_path);
    } catch (const Glib::Error &e) {
      LOG_WARN("HelpWindow: image '{}' load failed: {}", res_path, e.what());
    } catch (const std::exception &e) {
      LOG_WARN("HelpWindow: image '{}' load threw std::exception: {}",
               res_path, e.what());
    } catch (...) {
      LOG_WARN("HelpWindow: image '{}' load threw unknown exception",
               res_path);
    }
  } else {
    LOG_WARN("HelpWindow: image resource '{}' not found: {}", res_path,
             probe_err ? probe_err->message : "?");
    if (probe_err) g_error_free(probe_err);
  }

  if (!tex) {
    // Visible placeholder so a missing image is obvious during drafting.
    auto *placeholder = Gtk::make_managed<Gtk::Label>();
    std::string txt = "[image missing: " + url + "]";
    placeholder->set_text(txt);
    placeholder->add_css_class("dim-label");
    placeholder->set_xalign(0.0f);
    placeholder->set_margin_top(8);
    placeholder->set_margin_bottom(8);
    return placeholder;
  }

  // S124 m14: Gtk::Picture sizing. Configuration:
  //   • hexpand=true + halign=FILL   — claim the row's full width
  //   • can_shrink=true              — shrink if narrower than natural
  //   • content_fit=CONTAIN          — preserve aspect ratio
  //   • size_request(-1, h)          — minimum reserved height
  //
  // Why size_request is needed: GTK4 vboxes don't pre-allocate
  // vertical space for unknown-height children, so without a
  // reservation the picture collapses to 0px tall in narrow
  // contexts. m13 tried removing it and the picture vanished.
  //
  // Why h must scale with available width, not the texture's
  // intrinsic height: m10 v2 reserved h = min(intrinsic_h, 600).
  // For a 1908×1346 source PNG that reserves 600px even when
  // CONTAIN scales the actual drawn picture to ~250px tall in a
  // narrow window — the difference shows as visible blank bands
  // above and below.
  //
  // Compromise here: reserve a height computed from the help
  // window's typical content width (~660px after sidebar) and
  // the texture's aspect ratio. For a 16:9-ish screenshot this
  // is a reasonable centre value; in narrower windows the
  // picture will shrink and leave a small (rather than huge)
  // gap; in much wider windows the picture will grow taller
  // than reserved and push down. Tune the kAssumedContentWidth
  // constant if the help window's default width changes.
  constexpr int kAssumedContentWidth = 660;  // ~ 900 - 240 sidebar
  auto *pic = Gtk::make_managed<Gtk::Picture>();
  pic->set_paintable(tex);
  pic->set_can_shrink(true);
  pic->set_content_fit(Gtk::ContentFit::CONTAIN);
  pic->set_hexpand(true);
  pic->set_halign(Gtk::Align::FILL);
  pic->set_vexpand(false);
  pic->set_valign(Gtk::Align::START);
  if (tex->get_intrinsic_height() > 0 && tex->get_intrinsic_width() > 0) {
    int iw = tex->get_intrinsic_width();
    int ih = tex->get_intrinsic_height();
    // Reserved height matches what CONTAIN draws at the assumed
    // width. If the picture is smaller than the assumed width,
    // we use its natural height (no upscaling) so we don't reserve
    // more than the picture actually draws.
    int reserved_h;
    if (iw <= kAssumedContentWidth) {
      reserved_h = ih;
    } else {
      reserved_h = (ih * kAssumedContentWidth) / iw;
    }
    pic->set_size_request(-1, reserved_h);
  }
  if (!alt.empty()) {
    pic->set_alternative_text(alt);
    pic->set_tooltip_text(alt);
  }
  return pic;
}

} // namespace

// ─── Construction ─────────────────────────────────────────────────────────

HelpWindow::HelpWindow() {
  set_title("Curvz Help");
  set_modal(false);
  set_resizable(true);
  set_default_size(900, 700);
  set_hide_on_close(true);

  // ── Sidebar ────────────────────────────────────────────────────────────
  // s129 m7: the sidebar is a plain VBox holding chapter outers, mirroring
  // PropertiesPanel::add_group_collapsible / add_collapsible. Chapter and
  // Group rows are click-to-expand headers; Leaf rows are flat buttons
  // wired in build_sidebar(). No ListBox row-selected signal here — each
  // leaf carries its own click handler.
  //
  // s132 m1: a column wraps a SearchEntry on top of the scroll. The entry
  // filters the outline as you type — across leaf titles and bundled
  // markdown body text — and gets its accelerator hooked up so Ctrl+F
  // focuses it. Empty query restores the saved disclosure state.
  m_sidebar_box.add_css_class("navigation-sidebar");
  m_sidebar_box.set_hexpand(true);
  m_sidebar_scroll.set_child(m_sidebar_box);
  m_sidebar_scroll.set_policy(Gtk::PolicyType::NEVER,
                              Gtk::PolicyType::AUTOMATIC);
  m_sidebar_scroll.set_vexpand(true);

  curvz::utils::set_name(m_search_entry, "help_search",
                          "help_window_search_entry");
  m_search_entry.set_placeholder_text("Search the manual");
  m_search_entry.set_margin_start(8);
  m_search_entry.set_margin_end(8);
  m_search_entry.set_margin_top(6);
  m_search_entry.set_margin_bottom(4);
  m_search_entry.signal_search_changed().connect(
      sigc::mem_fun(*this, &HelpWindow::on_search_changed));

  m_sidebar_col.append(m_search_entry);
  m_sidebar_col.append(m_sidebar_scroll);
  m_sidebar_col.set_size_request(240, -1);

  // s132 m1: Ctrl+F focuses the search entry. CAPTURE-phase controller
  // on the window itself catches the keystroke before it reaches any
  // child that might consume it. Idiomatic wiring — same shape as
  // MainWindow's CAPTURE-phase shortcut dispatch.
  auto key_ctrl = Gtk::EventControllerKey::create();
  key_ctrl->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  key_ctrl->signal_key_pressed().connect(
      [this](guint keyval, guint /*kc*/, Gdk::ModifierType state) -> bool {
        const bool ctrl = (state & Gdk::ModifierType::CONTROL_MASK)
                          != Gdk::ModifierType{};
        if (ctrl && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
          m_search_entry.grab_focus();
          return true;  // stop propagation
        }
        return false;
      },
      false);  // after: run before default handler so we can claim the event
  add_controller(key_ctrl);

  // ── Content area ───────────────────────────────────────────────────────
  m_content_holder.add_css_class("help-content");
  m_content_holder.set_margin_start(20);
  m_content_holder.set_margin_end(20);
  m_content_holder.set_margin_top(16);
  m_content_holder.set_margin_bottom(16);
  m_content_holder.set_hexpand(true);
  m_content_holder.set_vexpand(true);
  m_content_scroll.set_child(m_content_holder);
  m_content_scroll.set_policy(Gtk::PolicyType::AUTOMATIC,
                              Gtk::PolicyType::AUTOMATIC);
  m_content_scroll.set_hexpand(true);
  m_content_scroll.set_vexpand(true);

  // ── Paned layout ───────────────────────────────────────────────────────
  m_paned.set_start_child(m_sidebar_col);  // s132 m1: column with search
  m_paned.set_end_child(m_content_scroll);
  m_paned.set_resize_start_child(false);
  m_paned.set_shrink_start_child(false);
  m_paned.set_position(240);
  m_paned.set_expand(true);

  m_root.append(m_paned);
  set_child(m_root);

  // ── Populate topics ────────────────────────────────────────────────────
  // s129 m7: build_topic_list() runs eagerly because it only populates the
  // m_topics vector — no widgets, no signals, cheap. build_sidebar() is
  // deferred to the first show() call: by that point MainWindow's
  // load_last_project_path has restored m_section_open from app config,
  // so the chapter/group disclosure defaults read the user's saved state
  // instead of the as-built defaults. Same idea as PropertiesPanel
  // building its sections lazily in refresh() rather than its ctor.
  build_topic_list();
}

void HelpWindow::show(Gtk::Window &parent) {
  // First show: lazy-build the sidebar (after MainWindow has had a chance
  // to restore the saved open/closed state into m_section_open) and pick
  // the opening leaf.
  if (m_sidebar_box.get_first_child() == nullptr) {
    build_sidebar();
    for (std::size_t i = 0; i < m_topics.size(); ++i) {
      const Topic &t = m_topics[i];
      if (t.kind == RowKind::Leaf && t.available) {
        select_leaf(i);
        break;
      }
    }
  }
  set_transient_for(parent);
  curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
  present();
}

// ─── Topic list ──────────────────────────────────────────────────────────

// s129 m6: the sidebar is the full outline, not just the drafted pages.
// Three row kinds — Chapter, Group, Leaf — interleaved in declaration
// order. Chapter and Group rows are inert headers; Leaf rows are
// clickable when `available` is true and dimmed-inert otherwise.
//
// To draft a new page: write the .md, alias it in resources.xml, then
// flip its `available` field below from false to true. No other change
// needed — the row already exists, the title is already set, the
// resource path is already pointing at the file once it's bundled.
//
// To add a row that doesn't exist yet (i.e. expanding the outline
// itself): insert the row in the right place below AND update
// docs/manual-outline-v2.md. Order here is the one source of truth for
// sidebar order; keep it parallel with the outline doc.
//
// Indent: 0 = chapter, 1 = section / group, 2 = sub-section under a
// group. The build_sidebar() margin formula multiplies indent by a
// per-level pixel step.
void HelpWindow::build_topic_list() {
  m_topics = {
    { RowKind::Chapter, 0, false, "", "1 Getting started" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/1-1-welcome.md",                "1.1 Welcome" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/1-2-about-curvz.md",            "1.2 About Curvz" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/1-3-workspace-tour.md",         "1.3 Workspace tour" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/1-4-how-curvz-thinks-about-size.md", "1.4 How Curvz thinks about size" },

    { RowKind::Chapter, 0, false, "", "2 Documents and files" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/2-1-projects-and-documents.md", "2.1 Projects & documents" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/2-2-templates.md",              "2.2 Templates" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/2-3-import-export.md",          "2.3 Import & export" },

    { RowKind::Chapter, 0, false, "", "3 The interface" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/3-1-header-and-tabs.md",        "3.1 Header & Document tabs" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/3-2-menus.md",                  "3.2 Menus" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/3-3-toolbar.md",                "3.3 Toolbar" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/3-4-context-bar.md",            "3.4 Context bar" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/3-5-status-bar.md",             "3.5 Status bar" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/3-6-canvas-rulers-corner.md",   "3.6 Canvas, rulers & corner" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/3-7-color-picker.md",           "3.7 Color picker & paint editor" },

    { RowKind::Chapter, 0, false, "", "4 Tools" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/4-1-tools-overview.md",         "4.1 Tools overview" },
    { RowKind::Group, 1, false, "", "4.2 Selection & Node" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-2-1-selection-tool.md",       "4.2.1 Selection tool" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-2-2-node-tool.md",            "4.2.2 Node tool" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/4-3-pen.md",                    "4.3 Pen" },
    { RowKind::Group, 1, false, "", "4.4 Shape tools" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-4-1-rectangle.md",            "4.4.1 Rectangle" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-4-2-ellipse.md",              "4.4.2 Ellipse" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-4-3-line.md",                 "4.4.3 Line" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-4-4-polygon.md",              "4.4.4 Polygon" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-4-5-spiral.md",               "4.4.5 Spiral" },
    { RowKind::Group, 1, false, "", "4.5 Text family" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-5-1-text.md",                 "4.5.1 Text" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-5-2-text-on-path.md",         "4.5.2 Text on Path" },
    { RowKind::Group, 1, false, "", "4.6 Utility tools" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-6-1-eyedropper.md",           "4.6.1 Eyedropper" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-6-2-zoom.md",                 "4.6.2 Zoom" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/4-6-3-measure.md",              "4.6.3 Measure" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/4-7-reference-points.md",       "4.7 Reference points" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/4-8-corner.md",                 "4.8 Corner" },

    { RowKind::Chapter, 0, false, "", "5 Inspector" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/5-1-inspector-overview.md",     "5.1 Inspector overview" },
    { RowKind::Group, 1, false, "", "5.2 Project group" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-2-1-motif.md",                "5.2.1 Motif" },
    { RowKind::Group, 1, false, "", "5.3 Document group" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-3-1-metadata.md",             "5.3.1 Metadata" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-3-2-canvas.md",               "5.3.2 Canvas" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-3-3-guides.md",               "5.3.3 Guides" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-3-4-grid.md",                 "5.3.4 Grid" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-3-5-margins.md",              "5.3.5 Margins" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-3-6-snap.md",                 "5.3.6 Snap" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-3-7-measure.md",              "5.3.7 Measure" },
    { RowKind::Group, 1, false, "", "5.4 Object group" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-4-1-selection.md",            "5.4.1 Selection" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-4-2-text.md",                 "5.4.2 Text" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-4-3-blend.md",                "5.4.3 Blend" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-4-4-node.md",                 "5.4.4 Node" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-4-5-appearance.md",           "5.4.5 Appearance" },
    { RowKind::Leaf, 2, true,  "/com/curvz/app/help/5-4-6-shadow.md",               "5.4.6 Shadow" },

    { RowKind::Chapter, 0, false, "", "6 Content" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/6-1-content-overview.md",       "6.1 Content overview" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/6-2-layers.md",                 "6.2 Layers" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/6-3-library.md",                "6.3 Library" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/6-4-swatches.md",               "6.4 Swatches" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/6-5-styles.md",                 "6.5 Styles" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/6-6-documents.md",              "6.6 Documents" },

    { RowKind::Chapter, 0, false, "", "7 Working with objects" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/7-1-step-and-repeat.md",        "7.1 Step and Repeat" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/7-2-clip-masks.md",             "7.2 Clip masks" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/7-3-blends.md",                 "7.3 Blends" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/7-4-align-distribute.md",       "7.4 Align & Distribute" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/7-5-group-ungroup.md",          "7.5 Group and Ungroup" },

    { RowKind::Chapter, 0, false, "", "8 Path operations" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/8-1-editing-paths.md",          "8.1 Editing paths" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/8-2-boolean-operations.md",     "8.2 Boolean operations" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/8-3-compound-and-derived.md",   "8.3 Compound & derived" },

    { RowKind::Chapter, 0, false, "", "9 Style and appearance" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/9-1-themes.md",                 "9.1 Themes" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/9-2-currentcolor.md",           "9.2 currentColor & symbolic icons" },

    { RowKind::Chapter, 0, false, "", "10 Layout and precision" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/10-1-view-options.md",          "10.1 View options" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/10-2-units.md",                 "10.2 Units" },

    { RowKind::Chapter, 0, false, "", "11 Reference" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/11-1-macros.md",                "11.1 Macros" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/11-2-keyboard-shortcuts.md",    "11.2 Keyboard shortcuts" },
    { RowKind::Leaf, 1, true,  "/com/curvz/app/help/11-3-troubleshooting.md",       "11.3 Troubleshooting" },
  };
}

// Build the nested-disclosure outline. Walks m_topics linearly: a Chapter
// row opens a new chapter outer with a header + body; subsequent Group or
// Leaf rows are appended to that body until the next Chapter row appears.
// Group rows nest the same way one level deeper. Leaves are flat buttons.
//
// The inspector's add_group_collapsible / add_collapsible is the canonical
// idiom this mirrors (see PropertiesPanel.cpp). We don't reuse those
// methods directly — they emit into the inspector's m_inner — but the
// header-row-and-body-with-set_visible shape is identical.
//
// Open/closed state lives in m_section_open keyed by row title. First
// appearance of a key uses a per-row default (only chapter 1 starts open
// in m_section_open's absence; everything else starts collapsed). After
// that the user's last toggle wins.
//
// As a side-effect we populate m_leaf_widgets in lockstep with m_topics,
// nullptr for Chapter and Group rows. select_leaf reads this index when
// applying the highlight class.
void HelpWindow::build_sidebar() {
  m_leaf_widgets.assign(m_topics.size(), nullptr);
  // s132 m1: parallel arrays for search-driven hide/show. Filled at
  // Chapter/Group iteration; Leaf rows leave these slots nullptr.
  m_section_outer.assign(m_topics.size(), nullptr);
  m_section_body.assign(m_topics.size(),  nullptr);
  m_section_arrow.assign(m_topics.size(), nullptr);

  Gtk::Box *chapter_body = nullptr;       // current chapter body box
  Gtk::Box *group_body   = nullptr;       // current nested group body box

  auto open_now_for = [this](const std::string &key, bool dflt) {
    auto it = m_section_open.find(key);
    bool v = (it != m_section_open.end()) ? it->second : dflt;
    m_section_open[key] = v;
    return v;
  };

  for (std::size_t i = 0; i < m_topics.size(); ++i) {
    const Topic &t = m_topics[i];

    if (t.kind == RowKind::Chapter) {
      // Default-open chapter 1 only; all other chapters start collapsed.
      // The "1 " prefix test is brittle by design — explicitly limited to
      // the first chapter so the manual lands on something readable but
      // doesn't dump 81 rows on the user's first F1.
      const bool dflt_open = (t.title.rfind("1 ", 0) == 0);
      const bool is_open = open_now_for(t.title, dflt_open);

      auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      outer->add_css_class("help-chapter");

      auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      hdr->add_css_class("help-chapter-header");
      hdr->set_spacing(5);
      auto *arrow = Gtk::make_managed<Gtk::Label>(is_open ? "▾" : "▸");
      arrow->add_css_class("help-arrow");
      auto *lbl = Gtk::make_managed<Gtk::Label>();
      lbl->set_markup("<b>" + pango_escape(t.title) + "</b>");
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      lbl->add_css_class("help-chapter-title");
      hdr->append(*arrow);
      hdr->append(*lbl);
      hdr->set_margin_start(8);
      hdr->set_margin_end(8);
      hdr->set_margin_top(6);
      hdr->set_margin_bottom(2);
      outer->append(*hdr);

      auto *sep = Gtk::make_managed<Gtk::Separator>(
          Gtk::Orientation::HORIZONTAL);
      outer->append(*sep);

      auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      body->set_visible(is_open);
      outer->append(*body);

      const std::string key = t.title;
      auto gesture = Gtk::GestureClick::create();
      gesture->signal_pressed().connect(
          [this, body, arrow, key](int, double, double) {
            bool on = !m_section_open[key];
            m_section_open[key] = on;
            body->set_visible(on);
            arrow->set_text(on ? "▾" : "▸");
          });
      hdr->add_controller(gesture);

      m_sidebar_box.append(*outer);
      // s132 m1: track for search filtering.
      m_section_outer[i] = outer;
      m_section_body[i]  = body;
      m_section_arrow[i] = arrow;
      chapter_body = body;
      group_body   = nullptr;        // a new chapter resets group context
      continue;
    }

    if (t.kind == RowKind::Group) {
      // Group rows always live inside the current chapter. If we somehow
      // see a group with no chapter open (declaration-order bug), skip it
      // safely rather than crash.
      if (!chapter_body) continue;

      // Groups default closed. The chapter that owns them starts closed
      // too (except chapter 1, which has none anyway), so opening a
      // chapter shouldn't immediately blast the user with sub-trees.
      const bool is_open = open_now_for(t.title, false);

      auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      outer->add_css_class("help-group");

      auto *hdr = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      hdr->add_css_class("help-group-header");
      hdr->set_spacing(5);
      auto *arrow = Gtk::make_managed<Gtk::Label>(is_open ? "▾" : "▸");
      arrow->add_css_class("help-arrow");
      auto *lbl = Gtk::make_managed<Gtk::Label>();
      // s131: section headers (4.2 Selection & Node, 4.4 Shape tools etc.)
      // previously rendered italic + dim-label, which read as "less
      // important than leaves" even though they're fully descriptive.
      // Dropping the italic markup and the dim-label class lifts them to
      // the same look as leaf rows; the css.hpp rule on .help-group-title
      // pegs the font-size to match the leaf button so they don't fall
      // back to GTK's default smaller label sizing.
      lbl->set_text(t.title);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      lbl->add_css_class("help-group-title");
      hdr->append(*arrow);
      hdr->append(*lbl);
      hdr->set_margin_start(20);
      hdr->set_margin_end(8);
      hdr->set_margin_top(2);
      hdr->set_margin_bottom(2);
      outer->append(*hdr);

      auto *body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      body->set_visible(is_open);
      outer->append(*body);

      const std::string key = t.title;
      auto gesture = Gtk::GestureClick::create();
      gesture->signal_pressed().connect(
          [this, body, arrow, key](int, double, double) {
            bool on = !m_section_open[key];
            m_section_open[key] = on;
            body->set_visible(on);
            arrow->set_text(on ? "▾" : "▸");
          });
      hdr->add_controller(gesture);

      chapter_body->append(*outer);
      // s132 m1: track for search filtering.
      m_section_outer[i] = outer;
      m_section_body[i]  = body;
      m_section_arrow[i] = arrow;
      group_body = body;
      continue;
    }

    // Leaf — a flat-styled button when available, a dim label otherwise.
    // Container choice: Button gives us hover/focus/keyboard activation
    // for free, plus a clear "this is interactive" affordance. Inert
    // placeholders use a Label inside a Box so they don't pretend to be
    // clickable but still occupy a row in the outline.
    Gtk::Box *parent_body = group_body ? group_body : chapter_body;
    if (!parent_body) continue;            // declaration-order guard

    const int leaf_indent_px = group_body ? 36 : 24;

    Gtk::Widget *leaf_widget = nullptr;
    if (t.available) {
      auto *btn = Gtk::make_managed<Gtk::Button>();
      btn->add_css_class("flat");
      btn->add_css_class("help-leaf");
      auto *lbl = Gtk::make_managed<Gtk::Label>(t.title);
      lbl->set_xalign(0.0f);
      lbl->set_hexpand(true);
      btn->set_child(*lbl);
      btn->set_margin_start(leaf_indent_px);
      btn->set_margin_end(8);
      btn->set_margin_top(1);
      btn->set_margin_bottom(1);
      const std::size_t leaf_idx = i;
      btn->signal_clicked().connect(
          [this, leaf_idx]() { select_leaf(leaf_idx); });
      parent_body->append(*btn);
      leaf_widget = btn;
    } else {
      auto *lbl = Gtk::make_managed<Gtk::Label>(t.title);
      lbl->set_xalign(0.0f);
      lbl->add_css_class("dim-label");
      lbl->add_css_class("help-leaf-placeholder");
      lbl->set_margin_start(leaf_indent_px);
      lbl->set_margin_end(8);
      lbl->set_margin_top(2);
      lbl->set_margin_bottom(2);
      parent_body->append(*lbl);
      leaf_widget = lbl;
    }

    m_leaf_widgets[i] = leaf_widget;
  }
}

// Apply the visual highlight to one leaf (clearing the previous one) and
// invoke show_topic. The "selected" CSS class rides whatever the active
// motif's selection colour is — same idiom Curvz uses elsewhere for
// row-selected feedback.
void HelpWindow::select_leaf(std::size_t i) {
  if (i >= m_topics.size()) return;
  const Topic &t = m_topics[i];
  if (t.kind != RowKind::Leaf || !t.available) return;

  // Clear the old highlight first so we don't have two selected rows if
  // anything goes sideways below.
  if (m_selected_leaf < m_leaf_widgets.size() &&
      m_leaf_widgets[m_selected_leaf]) {
    m_leaf_widgets[m_selected_leaf]->remove_css_class("help-leaf-selected");
  }

  if (m_leaf_widgets[i]) {
    m_leaf_widgets[i]->add_css_class("help-leaf-selected");
  }
  m_selected_leaf = i;

  show_topic(i);
}

void HelpWindow::show_topic(std::size_t i) {
  if (i >= m_topics.size()) return;
  if (m_current_content) {
    m_content_holder.remove(*m_current_content);
    m_current_content = nullptr;
  }

  // render_markdown walks the cmark AST and constructs widgets for every
  // block — including images that resolve to gresource paths. If anything
  // in that pipeline throws (missing resource, malformed Pango markup,
  // pixbuf decode failure, OOM), we don't want it to unwind through the
  // sidebar's row-selected signal handler and into GTK's dispatch loop —
  // that's the path that turns "image not found" into an app crash. Catch
  // here and replace the content area with a visible error banner so the
  // user can keep using the app and switch to another topic.
  Gtk::Widget *w = nullptr;
  try {
    w = render_markdown(m_topics[i].resource_path);
  } catch (const Glib::Error &e) {
    LOG_WARN("HelpWindow: topic '{}' render failed (Glib::Error): {}",
             m_topics[i].resource_path, e.what());
  } catch (const std::exception &e) {
    LOG_WARN("HelpWindow: topic '{}' render failed: {}",
             m_topics[i].resource_path, e.what());
  } catch (...) {
    LOG_WARN("HelpWindow: topic '{}' render failed (unknown exception)",
             m_topics[i].resource_path);
  }

  if (!w) {
    auto *banner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    banner->set_hexpand(true);
    auto *err = Gtk::make_managed<Gtk::Label>(
        "(This topic could not be displayed. See log for details.)");
    err->add_css_class("dim-label");
    err->set_xalign(0.0f);
    err->set_wrap(true);
    banner->append(*err);
    w = banner;
  }

  m_content_holder.append(*w);
  m_current_content = w;
}

// ─── Targeted-open and search (s132 m1) ──────────────────────────────────

void HelpWindow::open_at(Gtk::Window &parent,
                          const std::string &resource_path) {
  // Lazy-build same as show(): first call constructs the sidebar so that
  // m_topics's parallel-arrays exist. Don't lean on show() to do this for
  // us — show() also picks an opening leaf, and we want our own pick.
  if (m_sidebar_box.get_first_child() == nullptr) {
    build_sidebar();
  }

  // Walk topics for the requested path. If we miss, just fall back to
  // showing the window with its saved/default leaf — better than refusing.
  std::size_t target = static_cast<std::size_t>(-1);
  for (std::size_t i = 0; i < m_topics.size(); ++i) {
    if (m_topics[i].kind == RowKind::Leaf &&
        m_topics[i].available &&
        m_topics[i].resource_path == resource_path) {
      target = i;
      break;
    }
  }

  if (target == static_cast<std::size_t>(-1)) {
    LOG_WARN("HelpWindow::open_at: no leaf for path '{}'", resource_path);
    show(parent);
    return;
  }

  // Force-open the parent chapter and (if any) the parent group so the
  // selected row is visible in the outline. We track these by walking
  // backward from the target: the closest preceding Group is the parent
  // group, and the closest preceding Chapter is the parent chapter. This
  // mirrors the build_sidebar's declaration-order assumption.
  std::size_t parent_chapter = static_cast<std::size_t>(-1);
  std::size_t parent_group   = static_cast<std::size_t>(-1);
  for (std::size_t j = target; j-- > 0;) {
    if (parent_group == static_cast<std::size_t>(-1) &&
        m_topics[j].kind == RowKind::Group) {
      parent_group = j;
    }
    if (m_topics[j].kind == RowKind::Chapter) {
      parent_chapter = j;
      break;
    }
  }

  auto force_open = [this](std::size_t idx) {
    if (idx == static_cast<std::size_t>(-1)) return;
    if (!m_section_body[idx]) return;
    m_section_body[idx]->set_visible(true);
    if (m_section_arrow[idx]) m_section_arrow[idx]->set_text("▾");
    m_section_open[m_topics[idx].title] = true;
  };
  force_open(parent_chapter);
  force_open(parent_group);

  // If a search filter happens to be active when the user right-clicks the
  // bar, clearing the entry first guarantees the row is reachable. Doing
  // it before select_leaf means we don't have to fight the filter.
  if (m_search_entry.get_text().size() > 0) {
    m_search_entry.set_text("");  // emits signal_search_changed → restores
  }

  set_transient_for(parent);
  curvz::utils::apply_motif_class_from_parent(*this, parent);
  select_leaf(target);
  present();
}

// ── Search ──────────────────────────────────────────────────────────────

std::string HelpWindow::lower_ascii(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out += (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c;
  }
  return out;
}

// Strip enough markdown noise that body matches reflect prose, not file
// paths. We keep:
//   • plain text and inline emphasis content
// We drop:
//   • image tags  ![alt](url)        → keep alt text only
//   • link targets [txt](url)        → keep txt only
//   • leading '#' heading marks
//   • backtick fences and inline code is left in (it's prose-relevant)
// Not a full sanitiser — just enough to keep "pen" from matching every
// "img/icons/curvz-pen-symbolic.svg" reference but still match prose
// like "the Pen tool drops anchors".
std::string HelpWindow::sanitise_body_for_search(const std::string &md) {
  std::string out;
  out.reserve(md.size());
  const std::size_t n = md.size();
  for (std::size_t i = 0; i < n; ) {
    // Image: ![alt](url) — keep alt, drop url.
    if (i + 1 < n && md[i] == '!' && md[i+1] == '[') {
      std::size_t close = md.find("](", i + 2);
      if (close != std::string::npos) {
        std::size_t end = md.find(')', close + 2);
        if (end != std::string::npos) {
          out.append(md, i + 2, close - (i + 2));   // alt
          out += ' ';
          i = end + 1;
          continue;
        }
      }
    }
    // Link: [txt](url) — keep txt, drop url.
    if (md[i] == '[') {
      std::size_t close = md.find("](", i + 1);
      if (close != std::string::npos) {
        std::size_t end = md.find(')', close + 2);
        if (end != std::string::npos) {
          out.append(md, i + 1, close - (i + 1));
          out += ' ';
          i = end + 1;
          continue;
        }
      }
    }
    // Skip leading '#' heading marks at start of line.
    if (md[i] == '#' && (i == 0 || md[i-1] == '\n')) {
      while (i < n && md[i] == '#') ++i;
      continue;
    }
    out += md[i];
    ++i;
  }
  return out;
}

void HelpWindow::load_search_index_if_needed() {
  if (m_search_index_loaded) return;
  m_title_search_text.assign(m_topics.size(), {});
  for (std::size_t i = 0; i < m_topics.size(); ++i) {
    m_title_search_text[i] = lower_ascii(m_topics[i].title);
    if (m_topics[i].kind != RowKind::Leaf) continue;
    if (!m_topics[i].available) continue;
    if (m_topics[i].resource_path.empty()) continue;
    std::string raw = read_resource(m_topics[i].resource_path);
    if (raw.empty()) continue;
    m_body_search_text[i] = lower_ascii(sanitise_body_for_search(raw));
  }
  m_search_index_loaded = true;
  LOG_DEBUG("HelpWindow: search index built ({} body entries)",
            m_body_search_text.size());
}

void HelpWindow::on_search_changed() {
  std::string q = lower_ascii(m_search_entry.get_text());
  // Trim leading/trailing whitespace; users typing "  pen" should match.
  while (!q.empty() && (q.front() == ' ' || q.front() == '\t')) q.erase(q.begin());
  while (!q.empty() && (q.back()  == ' ' || q.back()  == '\t')) q.pop_back();

  if (q.empty()) {
    // Restore everything: every section visible, body visibility from the
    // saved m_section_open map, every leaf visible. Don't touch
    // m_section_open itself — that's the user's saved state.
    for (std::size_t i = 0; i < m_topics.size(); ++i) {
      if (m_section_outer[i]) m_section_outer[i]->set_visible(true);
      if (m_section_body[i]) {
        const auto it = m_section_open.find(m_topics[i].title);
        const bool open = (it != m_section_open.end()) ? it->second : false;
        m_section_body[i]->set_visible(open);
        if (m_section_arrow[i]) m_section_arrow[i]->set_text(open ? "▾" : "▸");
      }
      if (m_leaf_widgets[i]) m_leaf_widgets[i]->set_visible(true);
    }
    return;
  }

  load_search_index_if_needed();
  apply_search_filter(q);
}

// Filter walks topics in declaration order, deciding visibility per row.
// A leaf is visible if its title-or-body contains the query. A section
// (chapter or group) is visible if it has at least one visible descendant
// leaf, and is force-opened so matches are reachable.
void HelpWindow::apply_search_filter(const std::string &q) {
  // First pass: per-leaf match decision, also collecting which sections
  // contain matches. We track current chapter/group indices as we walk.
  std::vector<bool> leaf_match(m_topics.size(), false);
  std::vector<bool> section_has_match(m_topics.size(), false);

  std::size_t cur_chapter = static_cast<std::size_t>(-1);
  std::size_t cur_group   = static_cast<std::size_t>(-1);

  for (std::size_t i = 0; i < m_topics.size(); ++i) {
    const Topic &t = m_topics[i];
    if (t.kind == RowKind::Chapter) {
      cur_chapter = i;
      cur_group = static_cast<std::size_t>(-1);
      continue;
    }
    if (t.kind == RowKind::Group) {
      cur_group = i;
      continue;
    }
    // Leaf
    if (!t.available) continue;
    bool m = false;
    if (m_title_search_text[i].find(q) != std::string::npos) m = true;
    if (!m) {
      auto it = m_body_search_text.find(i);
      if (it != m_body_search_text.end() &&
          it->second.find(q) != std::string::npos) {
        m = true;
      }
    }
    leaf_match[i] = m;
    if (m) {
      if (cur_group   != static_cast<std::size_t>(-1))
        section_has_match[cur_group] = true;
      if (cur_chapter != static_cast<std::size_t>(-1))
        section_has_match[cur_chapter] = true;
    }
  }

  // Second pass: apply visibility. Sections with no match hide entirely;
  // sections with a match force-open and stay visible. Leaves: visible
  // iff matched (or always visible inside an unmatched-section outer that
  // we hid anyway, so the per-leaf flag drives the user-visible state).
  for (std::size_t i = 0; i < m_topics.size(); ++i) {
    const Topic &t = m_topics[i];
    if (t.kind == RowKind::Chapter || t.kind == RowKind::Group) {
      const bool show_section = section_has_match[i];
      if (m_section_outer[i]) m_section_outer[i]->set_visible(show_section);
      if (m_section_body[i]) {
        m_section_body[i]->set_visible(show_section);
        if (m_section_arrow[i]) m_section_arrow[i]->set_text(show_section ? "▾" : "▸");
      }
      continue;
    }
    if (m_leaf_widgets[i]) {
      m_leaf_widgets[i]->set_visible(t.available && leaf_match[i]);
    }
  }
}



// ─── Markdown rendering ──────────────────────────────────────────────────

// Walk the cmark document tree and build a vertical Gtk::Box of widgets,
// one per top-level block. Inline emphasis is converted to Pango markup
// inside the block's Label.
Gtk::Widget *HelpWindow::render_markdown(const std::string &resource_path) {
  std::string source = read_resource(resource_path);
  auto *outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
  outer->set_hexpand(true);

  if (source.empty()) {
    auto *err = Gtk::make_managed<Gtk::Label>(
        "(Topic content failed to load.)");
    err->add_css_class("dim-label");
    err->set_xalign(0.0f);
    outer->append(*err);
    return outer;
  }

  cmark_node *doc = cmark_parse_document(source.c_str(), source.size(),
                                         CMARK_OPT_DEFAULT);
  if (!doc) {
    auto *err = Gtk::make_managed<Gtk::Label>("(Markdown parse failed.)");
    err->add_css_class("dim-label");
    err->set_xalign(0.0f);
    outer->append(*err);
    return outer;
  }

  // Walk top-level children of the document.
  for (cmark_node *child = cmark_node_first_child(doc); child;
       child = cmark_node_next(child)) {
    cmark_node_type t = cmark_node_get_type(child);
    switch (t) {
      case CMARK_NODE_HEADING: {
        const int lvl = cmark_node_get_heading_level(child);

        // s131: detect a leading image inside the heading, e.g.
        //
        //     # ![alt](img/icons/foo.svg) Pen
        //
        // cmark parses that as a heading whose first child is an IMAGE
        // and whose subsequent children are the text inlines. When we
        // see that shape, build a side-by-side icon + title row via
        // make_heading_with_icon. Anything else (no image, image not
        // first, image alone with no following text) falls through to
        // the existing label-only path.
        cmark_node *first = cmark_node_first_child(child);
        if (first && cmark_node_get_type(first) == CMARK_NODE_IMAGE) {
          const char *url_c = cmark_node_get_url(first);
          std::string url = url_c ? url_c : "";

          // Alt text — TEXT children of the IMAGE node, same shape as
          // the existing image-only-paragraph branch below.
          std::string alt;
          for (cmark_node *t = cmark_node_first_child(first); t;
               t = cmark_node_next(t)) {
            if (cmark_node_get_type(t) == CMARK_NODE_TEXT) {
              const char *lit = cmark_node_get_literal(t);
              if (lit) alt += lit;
            }
          }

          // Build the title text from the heading's remaining children
          // (everything *after* the leading image). render_inlines walks
          // the parent and emits markup for each child; we want it to
          // skip the IMAGE node, so move it out of the way temporarily,
          // run render_inlines, and put it back. cmark_node_unlink
          // detaches; cmark_node_prepend_child re-attaches at the
          // original position.
          cmark_node_unlink(first);
          std::string text_markup = render_inlines(child);
          // Trim a single leading space (the markdown space between the
          // image and the title text becomes a SOFTBREAK or a leading
          // space in render_inlines output).
          if (!text_markup.empty() && text_markup.front() == ' ') {
            text_markup.erase(0, 1);
          }
          cmark_node_prepend_child(child, first);

          outer->append(*make_heading_with_icon(url, alt, text_markup, lvl));
          break;
        }

        std::string markup = render_inlines(child);
        outer->append(*make_text_block(markup, lvl));
        break;
      }
      case CMARK_NODE_PARAGRAPH: {
        // S124 m10: detect "image-only" paragraphs (markdown lines that
        // contain only `![alt](url)`). cmark wraps images as INLINE nodes
        // inside a paragraph; when the paragraph's sole child is an
        // IMAGE, we render the picture as a block-level widget instead
        // of a label that would otherwise show "" (because render_inlines
        // currently doesn't handle CMARK_NODE_IMAGE).
        cmark_node *first = cmark_node_first_child(child);
        cmark_node *second = first ? cmark_node_next(first) : nullptr;
        if (first && !second &&
            cmark_node_get_type(first) == CMARK_NODE_IMAGE) {
          const char *url_c = cmark_node_get_url(first);
          std::string url = url_c ? url_c : "";
          // Alt text: cmark stores it as TEXT children of the IMAGE node.
          std::string alt;
          for (cmark_node *t = cmark_node_first_child(first); t;
               t = cmark_node_next(t)) {
            if (cmark_node_get_type(t) == CMARK_NODE_TEXT) {
              const char *lit = cmark_node_get_literal(t);
              if (lit) alt += lit;
            }
          }
          outer->append(*make_image_block(url, alt));
          break;
        }
        std::string markup = render_inlines(child);
        outer->append(*make_text_block(markup, 0));
        break;
      }
      case CMARK_NODE_CODE_BLOCK: {
        const char *lit = cmark_node_get_literal(child);
        std::string text = lit ? lit : "";
        // Strip a single trailing newline from cmark's literal output.
        if (!text.empty() && text.back() == '\n') text.pop_back();
        outer->append(*make_code_block(text));
        break;
      }
      case CMARK_NODE_LIST: {
        // Iterate list items; render each as bullet + inlines from its
        // first paragraph child. Nested blocks inside list items are not
        // handled in v1.
        for (cmark_node *li = cmark_node_first_child(child); li;
             li = cmark_node_next(li)) {
          if (cmark_node_get_type(li) != CMARK_NODE_ITEM) continue;
          cmark_node *para = cmark_node_first_child(li);
          if (para && cmark_node_get_type(para) == CMARK_NODE_PARAGRAPH) {
            std::string markup = render_inlines(para);
            outer->append(*make_list_item(markup));
          }
        }
        // Bottom margin after a list
        auto *spacer = Gtk::make_managed<Gtk::Label>("");
        spacer->set_margin_top(4);
        outer->append(*spacer);
        break;
      }
      case CMARK_NODE_BLOCK_QUOTE: {
        // s131 m18: render markdown blockquotes (`> text`) as a styled
        // callout box. The manual uses these for "Note:" and
        // "Appears when:" advisory blocks; before s131 m18 they were
        // silently dropped, which made every callout invisible to
        // readers.
        //
        // Layout: a vertical Gtk::Box marked with .help-callout — CSS
        // gives it a left border, indent, and a slight tint so the
        // block reads as advisory rather than ordinary prose. Inside
        // the box we re-walk the blockquote's children, handling
        // PARAGRAPH and LIST. Other block types nested in quotes
        // (further blockquotes, code blocks, headings) are uncommon
        // in the manual; they're silently dropped for now and can
        // grow if a leaf needs them.
        auto *callout = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
        callout->add_css_class("help-callout");
        callout->set_margin_top(8);
        callout->set_margin_bottom(8);

        for (cmark_node *qc = cmark_node_first_child(child); qc;
             qc = cmark_node_next(qc)) {
          cmark_node_type qt = cmark_node_get_type(qc);
          if (qt == CMARK_NODE_PARAGRAPH) {
            std::string markup = render_inlines(qc);
            // heading_level=0 → no per-level styling. Reusing
            // make_text_block keeps wrap/selectable/xalign consistent
            // with body prose; the .help-callout CSS class on the
            // wrapper handles the visual differentiation.
            callout->append(*make_text_block(markup, 0));
          } else if (qt == CMARK_NODE_LIST) {
            for (cmark_node *li = cmark_node_first_child(qc); li;
                 li = cmark_node_next(li)) {
              if (cmark_node_get_type(li) != CMARK_NODE_ITEM) continue;
              cmark_node *para = cmark_node_first_child(li);
              if (para && cmark_node_get_type(para) == CMARK_NODE_PARAGRAPH) {
                std::string markup = render_inlines(para);
                callout->append(*make_list_item(markup));
              }
            }
          }
          // Other types silently skipped — see comment above.
        }

        outer->append(*callout);
        break;
      }
      case CMARK_NODE_THEMATIC_BREAK: {
        auto *sep =
            Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
        sep->set_margin_top(8);
        sep->set_margin_bottom(8);
        outer->append(*sep);
        break;
      }
      default:
        // Block types not yet rendered: HTML.
        // Inline images are handled in the PARAGRAPH case above (s124 m10).
        // BLOCK_QUOTE is handled in its own case above (s131 m18).
        break;
    }
  }

  cmark_node_free(doc);
  return outer;
}

} // namespace Curvz
