// ── DepotWindow.cpp ─────────────────────────────────────────────────────────
//
// See DepotWindow.hpp for the design rationale. Implementation notes:
//
// - Window-level chrome: set_decorated(false) gives us a chromeless float.
//   The default Window CSS theming still paints a background; custom
//   rounded-frame + arrow chrome lands in a follow-on milestone.
//
// - Drag-to-move via Gtk::WindowHandle wrapping the top_row. This is the
//   GTK4 idiomatic answer for chromeless-window drag handles — the WM
//   handles the move natively when the user presses inside the handle's
//   region. Same mechanism HeaderBar uses internally. Interactive
//   children (the link button) still receive their clicks because the
//   WindowHandle propagates events to hit-testable descendants.
//
// - Drag-to-resize via GestureDrag on the corner grip → set_default_size.
//   Window resize is native (no popover-style grab pathology). Floor
//   240x140. No CSS-resize-drag attribute exists in GTK4; the GestureDrag
//   approach is what every chromeless-window tutorial uses.
//
// - Edit routing: TextView signal_changed → Canvas (the splice landing
//   into the Mgr's text_content + queue_draw). Reuses Canvas's
//   m_depot_suppress_changed flag for programmatic-write re-entry guard.
//
// - Cross-boundary key controller on the TextView in CAPTURE phase so
//   Left-at-offset-0 / Up-on-first-line beat the TextView's stock arrow
//   handling.
//
// - Dismiss paths consolidated through Window's signal_hide. Escape is
//   wired explicitly via a key controller on the Window (close() →
//   signal_hide). Click-outside dismiss is the Canvas's responsibility
//   (it has the press handler that can compare against the depot's
//   allocation); we just expose a clean close() path.

#include "DepotWindow.hpp"

#include <gdkmm/general.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturedrag.h>

#include "Canvas.hpp"
#include "CurvzLog.hpp"

namespace Curvz {

DepotWindow::DepotWindow(Canvas* canvas, SceneNode* mgr)
    : m_canvas(canvas),
      m_mgr(mgr),
      m_mgr_iid(mgr ? mgr->internal_id : std::string()) {
  build_chrome();
  build_layout();
  wire_edit_routing();
  wire_cross_boundary();
  wire_drag_to_move();
  wire_drag_to_resize();
  wire_dismiss();
  LOG_INFO("DepotWindow: built for Mgr iid='{}'", m_mgr_iid);
}

void DepotWindow::build_chrome() {
  // Chromeless float. The user gets no titlebar, no resize edges from
  // the WM — all chrome is ours to draw (m1 leans on default Window
  // background; custom rounded-frame + arrow chrome is a follow-on).
  set_decorated(false);

  // Hide-on-close so the per-Mgr cache keeps the widget alive across
  // dismiss-and-reopen cycles. Scroll position, edit state, drag-
  // resized size, screen position all survive because the widget
  // itself survives. Matches the s311/s312 long-lived popover lifetime.
  set_hide_on_close(true);

  // Not in the taskbar — this is a tool float, not a peer window. On
  // GTK4 there's no direct API for this; set_transient_for usually
  // takes care of it on most WMs because transient toplevels are by
  // convention not taskbar-listed.

  // Initial size is computed by the caller (Canvas) and pushed via
  // set_default_size before show(); we don't pick it here because the
  // caller knows the canvas-view interior width to match.
  set_default_size(280, 220);

  add_css_class("curvz-depot-window");
}

void DepotWindow::build_layout() {
  // ── Root vbox ──────────────────────────────────────────────────────────
  m_root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
  m_root->set_margin(8);

  // ── Top row: stats + link button (also the drag-to-move handle) ────────
  // Wrapped in a Gtk::WindowHandle — GTK4's API for marking a region of
  // an undecorated window as a drag-to-move handle. The WM does the
  // move natively when the user presses-and-drags inside the handle's
  // allocation; interactive children (the link button) still get their
  // clicks because the WindowHandle propagates events through to them
  // for hit-testable widgets.
  auto* handle = Gtk::make_managed<Gtk::WindowHandle>();
  m_top_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

  m_stats_label = Gtk::make_managed<Gtk::Label>("");
  m_stats_label->set_halign(Gtk::Align::START);
  m_stats_label->set_hexpand(true);
  m_stats_label->add_css_class("dim-label");
  m_top_row->append(*m_stats_label);

  m_link_btn = Gtk::make_managed<Gtk::Button>();
  m_link_btn->set_icon_name("curvz-link-to-symbolic");
  m_link_btn->set_has_frame(false);
  m_link_btn->set_tooltip_text(
      "Link overflow to another textbox (coming soon)");
  // s316: the link verb is moving to the canvas-drawn overflow area
  //   (the mockup proved the overflow is NOT a window). This button on
  //   the DepotWindow is inert again; the live link button lives inside
  //   the canvas overflow region. DepotWindow is being retired.
  m_link_btn->signal_clicked().connect([]() {
    LOG_INFO("DepotWindow: link-to button clicked (inert — verb moved to canvas)");
  });
  m_top_row->append(*m_link_btn);

  handle->set_child(*m_top_row);
  m_root->append(*handle);

  // ── Scrolled TextView ──────────────────────────────────────────────────
  m_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
  m_scroll->set_policy(Gtk::PolicyType::AUTOMATIC,
                       Gtk::PolicyType::AUTOMATIC);
  m_scroll->set_vexpand(true);
  m_scroll->set_hexpand(true);
  m_scroll->set_min_content_height(140);

  m_text_view = Gtk::make_managed<Gtk::TextView>();
  m_text_view->set_editable(true);
  m_text_view->set_cursor_visible(true);
  m_text_view->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
  // CSS class shared with the s312 popover textview rule — pins the
  // font to 12pt regardless of theme. Class name kept stable so the
  // CSS rule doesn't need touching during the migration.
  m_text_view->add_css_class("curvz-popover-textview");
  m_scroll->set_child(*m_text_view);
  m_root->append(*m_scroll);

  // ── Overlay-positioned bottom-right resize grip ────────────────────────
  // Wrapping the root in an Overlay lets the grip float at bottom-right
  // without taking flow space in the vbox. Same visual shape as the
  // s312 m2.1.fix2 attempt; works here because Gtk::Window doesn't have
  // the popover's autohide-on-content-resize pathology.
  m_overlay = Gtk::make_managed<Gtk::Overlay>();
  m_overlay->set_child(*m_root);

  auto* grip_img = Gtk::make_managed<Gtk::Image>();
  grip_img->set_from_icon_name("curvz-grip-symbolic");
  grip_img->set_pixel_size(16);

  m_grip = Gtk::make_managed<Gtk::Button>();
  m_grip->set_child(*grip_img);
  m_grip->set_has_frame(false);
  m_grip->set_tooltip_text("Drag to resize");
  m_grip->set_halign(Gtk::Align::END);
  m_grip->set_valign(Gtk::Align::END);
  m_grip->set_margin_end(4);
  m_grip->set_margin_bottom(4);
  m_grip->set_size_request(20, 20);
  m_overlay->add_overlay(*m_grip);

  set_child(*m_overlay);
}

void DepotWindow::wire_edit_routing() {
  // signal_changed on the TextView's buffer → splice into Mgr buffer +
  // Canvas redraw. Canvas owns the splice logic because it knows about
  // view_byte_start, the suppress flag, and the queue_draw target. We
  // just hand it the new text + the Mgr; it does the rest.
  //
  // The lambda captures `this` and `m_canvas` raw. DepotWindow's
  // lifetime is bounded by the per-Mgr cache in Canvas; Canvas
  // (m_canvas) outlives every DepotWindow it owns. m_mgr is captured
  // raw too; the GC pass in Canvas ensures Mgr-stale DepotWindows are
  // destroyed before any subsequent show could fire this handler.
  auto buffer = m_text_view->get_buffer();
  buffer->signal_changed().connect([this]() {
    if (!m_canvas || !m_mgr || !m_text_view) return;
    m_canvas->on_depot_text_changed(m_mgr, m_text_view);
  });
}

void DepotWindow::wire_cross_boundary() {
  // Left at TextView offset 0 / Up on first visual line → cross back to
  // canvas. CAPTURE phase so we intercept before TextView's stock arrow-
  // key handling. Modifier chords (shift, ctrl, alt) fall through.
  auto key_ctrl = Gtk::EventControllerKey::create();
  key_ctrl->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
  key_ctrl->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType mods) -> bool {
        if (!m_canvas || !m_mgr || !m_text_view) return false;
        const bool shift = (mods & Gdk::ModifierType::SHIFT_MASK)
                           != Gdk::ModifierType{};
        const bool ctrl  = (mods & Gdk::ModifierType::CONTROL_MASK)
                           != Gdk::ModifierType{};
        const bool alt   = (mods & Gdk::ModifierType::ALT_MASK)
                           != Gdk::ModifierType{};
        if (shift || ctrl || alt) return false;
        if (keyval != GDK_KEY_Left && keyval != GDK_KEY_Up) return false;
        auto buffer = m_text_view->get_buffer();
        if (!buffer) return false;
        auto iter = buffer->get_iter_at_mark(buffer->get_insert());
        const bool at_left_edge = (iter.get_offset() == 0);
        const bool on_first_line = (iter.get_line() == 0);
        const bool should_cross =
            (keyval == GDK_KEY_Left && at_left_edge) ||
            (keyval == GDK_KEY_Up   && on_first_line);
        if (!should_cross) return false;
        LOG_INFO("DepotWindow: cross-back gesture — Mgr '{}', "
                 "keyval={}, offset={}, line={}",
                 m_mgr->name, keyval, (int)iter.get_offset(),
                 (int)iter.get_line());
        m_canvas->cross_back_to_canvas(m_mgr);
        return true;
      }, false);
  m_text_view->add_controller(key_ctrl);
}

void DepotWindow::wire_drag_to_move() {
  // No code here — drag-to-move is handled structurally by the
  // Gtk::WindowHandle that wraps the top_row in build_layout. When the
  // user presses inside the handle's allocation, the WindowHandle
  // initiates a WM-native window move drag. Interactive children
  // (the link button) still receive their clicks because GTK4's
  // WindowHandle is event-aware about interactive descendants.
  //
  // Kept as a method (rather than removed) so the s313 milestone
  // story stays "five wire_* helpers, one per concern" and the
  // narrative in DepotWindow's constructor reads cleanly. If
  // WindowHandle turns out not to work on this build, the fallback
  // is a GestureDrag on top_row calling begin_move_drag in
  // signal_drag_begin; banked but not wired in m1.
}

void DepotWindow::wire_drag_to_resize() {
  // GestureDrag on the grip → set_default_size on the window. Floors
  // at 240x140. Window resize is native (no popover-grab pathology),
  // so set_default_size mid-drag works cleanly.
  struct ResizeState {
    int start_w = 0;
    int start_h = 0;
  };
  auto state = std::make_shared<ResizeState>();
  auto drag = Gtk::GestureDrag::create();
  drag->set_button(1);
  // BUBBLE phase — the button's own click handler is empty so claim
  // priority doesn't matter; this matches how Curvz wires other
  // grip-style controllers.
  drag->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
  drag->signal_drag_begin().connect(
      [this, state](double, double) {
        int w = 0, h = 0;
        get_default_size(w, h);
        state->start_w = w;
        state->start_h = h;
        LOG_INFO("DepotWindow: resize BEGIN — Mgr iid='{}', "
                 "start size=({}, {})", m_mgr_iid, w, h);
      });
  drag->signal_drag_update().connect(
      [this, state](double dx, double dy) {
        int new_w = state->start_w + (int)std::round(dx);
        int new_h = state->start_h + (int)std::round(dy);
        if (new_w < 240) new_w = 240;
        if (new_h < 140) new_h = 140;
        set_default_size(new_w, new_h);
      });
  drag->signal_drag_end().connect(
      [this, state](double dx, double dy) {
        int w = 0, h = 0;
        get_default_size(w, h);
        LOG_INFO("DepotWindow: resize END — Mgr iid='{}', "
                 "delta=({}, {}), size=({}, {})",
                 m_mgr_iid, (int)std::round(dx), (int)std::round(dy),
                 w, h);
      });
  m_grip->add_controller(drag);
}

void DepotWindow::wire_dismiss() {
  // Escape on the Window → close. close() with set_hide_on_close(true)
  // hides without destroying; the cache keeps the widget alive.
  auto key_ctrl = Gtk::EventControllerKey::create();
  key_ctrl->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
  key_ctrl->signal_key_pressed().connect(
      [this](guint keyval, guint, Gdk::ModifierType) -> bool {
        if (keyval == GDK_KEY_Escape) {
          LOG_INFO("DepotWindow: Escape → close (Mgr iid='{}')",
                   m_mgr_iid);
          close();
          return true;
        }
        return false;
      }, false);
  add_controller(key_ctrl);

  // signal_hide fires on every dismiss path (close from Escape, close
  // from Canvas's cross_back, close from Canvas's click-outside handler,
  // close from auto-close-on-empty-depot). Single hookup covers them
  // all; Canvas resumes the canvas TextCursor blink.
  signal_hide().connect([this]() {
    LOG_INFO("DepotWindow: signal_hide — Mgr iid='{}'", m_mgr_iid);
    if (m_canvas) m_canvas->resume_text_cursor_blink();
  });
}

void DepotWindow::set_stats(const std::string& text) {
  if (m_stats_label) m_stats_label->set_text(text);
}

void DepotWindow::seed_text(const std::string& overflow_text) {
  if (!m_text_view) return;
  // Caller is responsible for setting Canvas's m_depot_suppress_changed
  // around this call. We don't gate internally because the flag lives
  // on Canvas and only Canvas knows the right scope.
  m_text_view->get_buffer()->set_text(overflow_text);
}

void DepotWindow::show() {
  // present() brings the window up and asks for focus. On first show
  // the WM places it (we have set_transient_for from the caller before
  // show()); on subsequent shows the window remembers its position
  // because we set_hide_on_close instead of destroying.
  present();
  // Focus the TextView so the user's keystrokes go straight to the
  // depot surface without an extra click. The TextView's native blinking
  // caret appears at whatever position GTK chose.
  if (m_text_view) m_text_view->grab_focus();
}

} // namespace Curvz
