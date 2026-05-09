#include "DocTabBar.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp" // s121 m8: curvz::utils::set_name
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <giomm/actiongroup.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <giomm/simpleactiongroup.h>
#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/separator.h>

namespace Curvz {
namespace fs = std::filesystem;

static bool tab_icontains(const std::string &haystack,
                          const std::string &needle) {
  if (needle.empty())
    return true;
  auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
  return it != haystack.end();
}

DocTabBar::DocTabBar() : Gtk::Box(Gtk::Orientation::HORIZONTAL) {
  curvz::utils::set_name(*this, "dt", "doc_tab_bar_root");
  add_css_class("doc-tab-bar");
  set_hexpand(true);
  // S60 M3: halign=FILL is necessary in addition to hexpand=true when
  // the DocTabBar is the HeaderBar title widget. Without FILL, GTK4
  // HeaderBar sizes the title to its natural width and centers it
  // between pack_start and pack_end — producing big empty gaps on
  // both sides. FILL tells the parent to stretch us horizontally.
  set_halign(Gtk::Align::FILL);
  // S60 M3: explicit minimum width. Prevents transient HeaderBar
  // CenterBox measure passes from allocating the center slot at
  // negative widths (which cascade into negative-width child
  // allocations on chevrons).
  set_size_request(1, -1);

  // S60 M3: "Documents" label and separator used to live here but
  // were moved to MainWindow's headerbar pack_start so they hug the
  // logo on the left. This class now contains only the scroll-related
  // widgets that belong in the title-widget slot.

  // ── S60 M3: Left chevron ──────────────────────────────────────────────
  // Appears only when the scroll can pan left (content off-viewport on
  // the left side). Replaces the native horizontal scrollbar for a
  // cleaner tab-bar look.
  m_scroll_left.set_icon_name("go-previous-symbolic");
  m_scroll_left.set_has_frame(false);
  m_scroll_left.set_tooltip_text("Scroll tabs left");
  m_scroll_left.add_css_class("doc-tab-chevron");
  curvz::utils::set_name(m_scroll_left, "dt_l", "doc_tab_bar_scroll_left_btn");
  // S60 M3: explicit minimum size prevents GTK from computing a
  // negative width during transient measure passes when the parent
  // DocTabBar is in a tight width-for-height phase.
  m_scroll_left.set_size_request(10, -1);
  // NOTE: Do NOT set_visible(false) at construction — it triggers
  // negative-size warnings during the first measure pass (Button
  // width=-1, Image width=-20) because the CSS min-width on the
  // chevron conflicts with the zero allocation a hidden widget
  // receives. update_chevron_visibility() runs on the first scroll
  // adjustment signal (post-allocate) and sets the correct state.
  m_scroll_left.signal_clicked().connect([this]() {
    // Scroll by ~half the visible width
    auto adj = m_scroll.get_hadjustment();
    if (adj)
      scroll_by(-adj->get_page_size() * 0.5);
  });
  append(m_scroll_left);

  // Scrollable tab row
  m_scroll.set_hexpand(true);
  // S60 M3: EXTERNAL policy — we provide our own scroll UI via chevrons,
  // so the native scrollbar stays hidden regardless of overflow.
  m_scroll.set_policy(Gtk::PolicyType::EXTERNAL, Gtk::PolicyType::NEVER);
  m_scroll.add_css_class("doc-tab-scroll");

  m_tab_row.set_spacing(1);
  m_tab_row.set_homogeneous(false);
  // S60 M3: align to START so tabs keep their natural width and
  // overflow scrolls, rather than being evenly distributed/squished
  // across the available width.
  m_tab_row.set_halign(Gtk::Align::START);
  m_tab_row.add_css_class("doc-tab-row");
  m_scroll.set_child(m_tab_row);
  append(m_scroll);

  // ── S60 M3: Right chevron ─────────────────────────────────────────────
  // See note above on left chevron re: set_visible(false) at
  // construction causing negative-size warnings.
  m_scroll_right.set_icon_name("go-next-symbolic");
  m_scroll_right.set_has_frame(false);
  m_scroll_right.set_tooltip_text("Scroll tabs right");
  m_scroll_right.add_css_class("doc-tab-chevron");
  curvz::utils::set_name(m_scroll_right, "dt_r",
                         "doc_tab_bar_scroll_right_btn");
  m_scroll_right.set_size_request(22, -1);
  m_scroll_right.signal_clicked().connect([this]() {
    auto adj = m_scroll.get_hadjustment();
    if (adj)
      scroll_by(adj->get_page_size() * 0.5);
  });
  append(m_scroll_right);

  // ── S60 M3: Chevron visibility driven by scroll position + content
  //    width. The horizontal adjustment fires property_value/_upper/
  //    _page_size signals on any of those changing, so we listen to all
  //    three. Initial state is set after the first allocate via idle.
  if (auto adj = m_scroll.get_hadjustment()) {
    adj->signal_changed().connect(
        sigc::mem_fun(*this, &DocTabBar::update_chevron_visibility));
    adj->signal_value_changed().connect(
        sigc::mem_fun(*this, &DocTabBar::update_chevron_visibility));
  }

  // Right-click on empty scroll area → "New Document"
  auto scroll_ctx = Gtk::GestureClick::create();
  scroll_ctx->set_button(3);
  scroll_ctx->signal_pressed().connect(
      [this](int, double x, double y) { show_bar_menu(x, y); });
  m_scroll.add_controller(scroll_ctx);

  LOG_DEBUG("DocTabBar created");
}

// S60 M3: Programmatic scroll by dx pixels, clamped to the adjustment's
// legal range. Called by the chevron click handlers.
void DocTabBar::scroll_by(double dx) {
  auto adj = m_scroll.get_hadjustment();
  if (!adj)
    return;
  double lo = adj->get_lower();
  double hi = adj->get_upper() - adj->get_page_size();
  double v = std::clamp(adj->get_value() + dx, lo, hi);
  adj->set_value(v);
}

// S60 M3: Chevron state driven by scroll position. Use set_sensitive
// (not set_visible) because visibility changes mid-layout cause
// BoxLayout to redistribute widths and can trigger negative-width
// allocations on sibling widgets. Insensitive chevrons are rendered
// greyed out but keep their allocation, which is layout-stable.
void DocTabBar::update_chevron_visibility() {
  auto adj = m_scroll.get_hadjustment();
  if (!adj)
    return;
  const double eps = 0.5;
  const double value = adj->get_value();
  const double upper = adj->get_upper();
  const double page = adj->get_page_size();
  const bool overflowing = (upper - page) > eps;
  m_scroll_left.set_sensitive(overflowing && value > eps);
  m_scroll_right.set_sensitive(overflowing && value < (upper - page - eps));
}

void DocTabBar::show_tab_menu(int idx, double x, double y) {
  m_ctx_idx = idx;

  auto menu = Gio::Menu::create();
  bool can_remove = m_project && !m_project->documents.empty();

  auto item_new = Gio::MenuItem::create("New Document", "tabctx.new");
  menu->append_item(item_new);
  if (can_remove) {
    auto item_rm =
        Gio::MenuItem::create("Remove This Document", "tabctx.remove");
    menu->append_item(item_rm);
  }

  // Build action group
  auto ag = Gio::SimpleActionGroup::create();
  auto act_new = Gio::SimpleAction::create("new");
  act_new->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_sig_add.emit(); });
  ag->add_action(act_new);

  if (can_remove) {
    auto act_rm = Gio::SimpleAction::create("remove");
    const int capture_idx = m_ctx_idx;
    act_rm->signal_activate().connect(
        [this, capture_idx](const Glib::VariantBase &) {
          m_sig_remove.emit(capture_idx);
        });
    ag->add_action(act_rm);
  }

  insert_action_group("tabctx", ag);

  auto *popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
  popover->set_parent(*this);
  popover->set_has_arrow(false);
  // Position near click point
  Gdk::Rectangle rect;
  rect.set_x((int)x);
  rect.set_y((int)y);
  rect.set_width(1);
  rect.set_height(1);
  popover->set_pointing_to(rect);
  popover->popup();
}

void DocTabBar::show_bar_menu(double x, double y) {
  auto menu = Gio::Menu::create();
  menu->append("New Document", "tabctx.new");

  auto ag = Gio::SimpleActionGroup::create();
  auto act_new = Gio::SimpleAction::create("new");
  act_new->signal_activate().connect(
      [this](const Glib::VariantBase &) { m_sig_add.emit(); });
  ag->add_action(act_new);
  insert_action_group("tabctx", ag);

  auto *popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
  popover->set_parent(*this);
  popover->set_has_arrow(false);
  Gdk::Rectangle rect;
  rect.set_x((int)x);
  rect.set_y((int)y);
  rect.set_width(1);
  rect.set_height(1);
  popover->set_pointing_to(rect);
  popover->popup();
}

void DocTabBar::set_project(CurvzProject *project) {
  m_project = project;
  rebuild();
}

void DocTabBar::refresh() { rebuild(); }

void DocTabBar::set_filter(const std::string &query) {
  m_filter = query;
  rebuild();
}

void DocTabBar::rebuild() {
  while (auto *child = m_tab_row.get_first_child())
    m_tab_row.remove(*child);

  if (!m_project)
    return;

  const int active = m_project->active_doc_index;

  for (int i = 0; i < (int)m_project->documents.size(); ++i) {
    auto *doc = m_project->documents[i].get();

    // Derive display name
    std::string name = doc->filename.empty() ? "untitled" : doc->filename;
    auto slash = name.rfind('/');
    if (slash != std::string::npos)
      name = name.substr(slash + 1);
    for (auto ext : {".svg", ".curvz"}) {
      std::string e{ext};
      if (name.size() > e.size() && name.substr(name.size() - e.size()) == e)
        name = name.substr(0, name.size() - e.size());
    }

    auto *tab = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    tab->set_spacing(4);
    tab->add_css_class("doc-tab");
    if (i == active)
      tab->add_css_class("doc-tab-active");
    tab->set_visible(tab_icontains(name, m_filter));

    auto *lbl = Gtk::make_managed<Gtk::Label>(name);
    lbl->set_ellipsize(Pango::EllipsizeMode::END);
    // S60 M3: set_width_chars (not set_max_width_chars) forces each
    // tab to a proper natural width. Without this, labels can shrink
    // to just the ellipsis width, which lets the Box squish all tabs
    // evenly into the viewport rather than overflowing and letting
    // the scroll engage.
    lbl->set_width_chars(14);
    lbl->add_css_class("doc-tab-label");
    lbl->set_margin_start(10);
    tab->append(*lbl);

    // Unsaved dot
    bool is_new = doc->filename.empty() || !fs::exists(doc->filename);
    if (is_new) {
      auto *dot = Gtk::make_managed<Gtk::Label>("●");
      dot->add_css_class("doc-tab-dot");
      tab->append(*dot);
    }

    tab->set_margin_end(1);

    // Left-click → activate
    auto gesture = Gtk::GestureClick::create();
    gesture->set_button(1);
    const int idx = i;
    gesture->signal_pressed().connect(
        [this, idx](int, double, double) { m_sig_activated.emit(idx); });
    tab->add_controller(gesture);

    // Middle-click → remove
    auto gesture_mid = Gtk::GestureClick::create();
    gesture_mid->set_button(2);
    gesture_mid->signal_pressed().connect([this, idx](int, double, double) {
      if (m_project && !m_project->documents.empty())
        m_sig_remove.emit(idx);
    });
    tab->add_controller(gesture_mid);

    // Right-click → context menu
    auto gesture_ctx = Gtk::GestureClick::create();
    gesture_ctx->set_button(3);
    gesture_ctx->signal_pressed().connect(
        [this, idx, tab](int, double x, double y) {
          // Convert tab-local coords to DocTabBar coords
          double tx, ty;
          tab->translate_coordinates(*this, x, y, tx, ty);
          show_tab_menu(idx, tx, ty);
        });
    tab->add_controller(gesture_ctx);

    m_tab_row.append(*tab);
  }

  LOG_DEBUG("DocTabBar rebuilt: {} tabs, active={}",
            m_project->documents.size(), active);

  // S60 M3: Content width just changed — re-evaluate whether the
  // scroll-left/right chevrons should be visible. Deferred via idle so
  // it runs after GTK has re-allocated m_tab_row and the scroll
  // adjustment reflects the new upper bound.
  Glib::signal_idle().connect_once([this]() { update_chevron_visibility(); });
}

} // namespace Curvz
