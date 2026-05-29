// ── OverlayDummy.cpp ────────────────────────────────────────────────────────
//
// See include/OverlayDummy.hpp.
//
// Multi-textbox model: ordered list of members (flow = z = storage
// order), overlay (ovw, ovh) owned by the TAIL. Paint walks members
// 0..N (tail last, on top); hit-test walks tail-first (newest on top
// wins overlapping clicks). Link button appends a tail; overflow
// re-anchors automatically because the grip is derived from tail.BR.

#include "Canvas.hpp"
#include "CurvzLog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace Curvz {

namespace {

constexpr double kHandlePx = 8.0;
constexpr double kHandleHitSlackPx = 4.0;
constexpr double kToggleBtnPx = 14.0;
constexpr double kLinkBtnPx = 16.0;
constexpr double kMinTextboxW = 40.0;
constexpr double kMinTextboxH = 30.0;
constexpr double kMinOverlayW = 20.0;
constexpr double kMinOverlayH = 20.0;
constexpr double kLinkOffset = 30.0;   // new member offset down-right

inline bool sizer_touches_left  (int i) { return i == 0 || i == 6 || i == 7; }
inline bool sizer_touches_right (int i) { return i == 2 || i == 3 || i == 4; }
inline bool sizer_touches_top   (int i) { return i == 0 || i == 1 || i == 2; }
inline bool sizer_touches_bottom(int i) { return i == 4 || i == 5 || i == 6; }

struct Rect { double x, y, w, h; };

using Sizers = OverlayDummy::Sizers;

inline Rect textbox_rect(const Sizers& s) {
  // Corners 0, 2, 4, 6 are authoritative.
  const double L = s[0].x;
  const double T = s[0].y;
  const double R = s[2].x;
  const double B = s[4].y;
  return { L, T, R - L, B - T };
}

// The tail member — owns the overlay. Always textboxes.back().
inline const Sizers& tail(const OverlayDummy& d) { return d.textboxes.back(); }

// Grip position — derived from tail.textbox.BR + (ovw, ovh). Never
// stored, always read fresh from the current tail.
inline OverlayDummy::Point grip_position(const OverlayDummy& d) {
  const Sizers& t = tail(d);
  const double tb_right  = t[2].x;
  const double tb_bottom = t[4].y;
  return { tb_right + d.ovw, tb_bottom + d.ovh };
}

inline Rect overlay_rect(const OverlayDummy& d) {
  const Sizers& t = tail(d);
  const double tb_right  = t[2].x;
  const double tb_bottom = t[4].y;
  return { tb_right, tb_bottom, d.ovw, d.ovh };
}

inline Rect container_bbox(const OverlayDummy& d) {
  // Union of every member rect (+ overlay when shown). Pure
  // visualization — never a hit target.
  double L = 1e18, T = 1e18, R = -1e18, B = -1e18;
  for (const auto& s : d.textboxes) {
    Rect r = textbox_rect(s);
    L = std::min(L, r.x);
    T = std::min(T, r.y);
    R = std::max(R, r.x + r.w);
    B = std::max(B, r.y + r.h);
  }
  if (d.overlay_shown && d.ovw > 0.0 && d.ovh > 0.0) {
    Rect ov = overlay_rect(d);
    L = std::min(L, ov.x);
    T = std::min(T, ov.y);
    R = std::max(R, ov.x + ov.w);
    B = std::max(B, ov.y + ov.h);
  }
  return { L, T, R - L, B - T };
}

void recompute_midpoints(Sizers& s) {
  const double L = s[0].x, T = s[0].y;
  const double R = s[2].x;
  const double B = s[4].y;
  s[1] = { (L + R) * 0.5, T };
  s[3] = { R, (T + B) * 0.5 };
  s[5] = { (L + R) * 0.5, B };
  s[7] = { L, (T + B) * 0.5 };
}

// Toggle button sits at the tail's textbox upper-right inset (it acts
// on the overlay, which belongs to the tail).
inline OverlayDummy::Point toggle_btn_center(const OverlayDummy& d, double zoom) {
  Rect tb = textbox_rect(tail(d));
  const double inset_doc = 12.0 / std::max(zoom, 0.001);
  return { tb.x + tb.w - inset_doc, tb.y + inset_doc };
}

// Link button sits centered in the overlay region (conceptually "the
// place text spills out of" is where you link the next box). Only
// meaningful when the overlay is shown.
inline OverlayDummy::Point link_btn_center(const OverlayDummy& d) {
  Rect ov = overlay_rect(d);
  return { ov.x + ov.w * 0.5, ov.y + ov.h * 0.5 };
}

} // namespace

void Canvas::toggle_overlay_dummy() {
  m_overlay_dummy.visible = !m_overlay_dummy.visible;
  if (m_overlay_dummy.visible) {
    // If a prior session deleted every member (empty-Mgr auto-delete),
    // re-seed a fresh dummy so F12 always brings one back. A default-
    // constructed OverlayDummy carries the two seed members + reset
    // selection/drag state; copy its data fields, preserving only the
    // freshly-flipped visibility.
    if (m_overlay_dummy.textboxes.empty()) {
      OverlayDummy fresh;          // seeded with the two starter members
      fresh.visible = true;        // we are turning it on
      m_overlay_dummy = fresh;
      LOG_INFO("Canvas: overlay dummy re-seeded (was empty)");
    }
    LOG_INFO("Canvas: overlay dummy ON — members={} overlay=({:.1f}x{:.1f}) "
             "shown={}",
             m_overlay_dummy.textboxes.size(),
             m_overlay_dummy.ovw, m_overlay_dummy.ovh,
             m_overlay_dummy.overlay_shown ? "true" : "false");
  } else {
    LOG_INFO("Canvas: overlay dummy OFF");
  }
  queue_draw();
}

// Append a new tail member. Inherits the current tail's size, offset
// down-right by kLinkOffset so it doesn't sit exactly on top. The new
// member becomes the tail; the overlay re-anchors to it automatically
// because grip_position derives from textboxes.back().
void Canvas::overlay_dummy_link() {
  OverlayDummy& d = m_overlay_dummy;
  Rect prev = textbox_rect(tail(d));
  const double nx = prev.x + kLinkOffset;
  const double ny = prev.y + kLinkOffset;

  Sizers s;
  const double L = nx, T = ny, R = nx + prev.w, B = ny + prev.h;
  s[0] = { L, T };
  s[2] = { R, T };
  s[4] = { R, B };
  s[6] = { L, B };
  recompute_midpoints(s);
  d.textboxes.push_back(s);
  d.selected.push_back(false);

  LOG_INFO("Canvas: overlay-dummy LINK — appended member, members={} "
           "new-tail=({:.1f},{:.1f},{:.1f}x{:.1f})",
           d.textboxes.size(), L, T, prev.w, prev.h);
  queue_draw();
}

// Plain click selects one member (active + sole selection, clearing
// others). Shift-click toggles the member into the selection set
// without disturbing the rest. A plain click also clears any edit
// state. Returns true if handled (member valid).
bool Canvas::overlay_dummy_select_click(int member, bool shift) {
  OverlayDummy& d = m_overlay_dummy;
  if (member < 0 || member >= static_cast<int>(d.textboxes.size()))
    return false;
  if (d.selected.size() != d.textboxes.size())
    d.selected.assign(d.textboxes.size(), false);

  if (shift) {
    d.selected[member] = !d.selected[member];
    if (d.selected[member]) d.active = member;
    LOG_INFO("Canvas: overlay-dummy SHIFT-select — member {} now {}, active={}",
             member, d.selected[member] ? "selected" : "deselected", d.active);
  } else {
    std::fill(d.selected.begin(), d.selected.end(), false);
    d.selected[member] = true;
    d.active = member;
    d.editing = -1;
    LOG_INFO("Canvas: overlay-dummy select — member {} (sole)", member);
  }
  queue_draw();
  return true;
}

// Double-click a member → it enters edit (red rect). Also makes it the
// active member and the sole selection (drilling in implies selecting).
void Canvas::overlay_dummy_edit_click(int member) {
  OverlayDummy& d = m_overlay_dummy;
  if (member < 0 || member >= static_cast<int>(d.textboxes.size())) return;
  if (d.selected.size() != d.textboxes.size())
    d.selected.assign(d.textboxes.size(), false);
  std::fill(d.selected.begin(), d.selected.end(), false);
  d.selected[member] = true;
  d.active = member;
  d.editing = member;
  LOG_INFO("Canvas: overlay-dummy EDIT — member {} editing", member);
  queue_draw();
}
// the tail becomes back() and the overlay re-anchors to it. Deleting
// the last remaining member empties the dummy → hide it (empty-Mgr
// auto-delete, simulated). Any in-flight drag is cancelled.
void Canvas::overlay_dummy_delete_member(int member) {
  OverlayDummy& d = m_overlay_dummy;
  if (member < 0 || member >= static_cast<int>(d.textboxes.size())) return;

  d.textboxes.erase(d.textboxes.begin() + member);
  if (member < static_cast<int>(d.selected.size()))
    d.selected.erase(d.selected.begin() + member);
  d.active_sizer = -1;
  d.active_member = -1;
  d.drag_all = false;
  // Active/editing indices shift when a lower-indexed member is removed.
  if (d.active == member) d.active = -1;
  else if (d.active > member) --d.active;
  if (d.editing == member) d.editing = -1;
  else if (d.editing > member) --d.editing;

  if (d.textboxes.empty()) {
    d.visible = false;
    LOG_INFO("Canvas: overlay-dummy DELETE — member {} removed, dummy now "
             "empty -> hidden (empty-Mgr auto-delete)", member);
  } else {
    LOG_INFO("Canvas: overlay-dummy DELETE — member {} removed, members={} "
             "survivors promoted, tail re-anchored", member,
             d.textboxes.size());
  }
  queue_draw();
}

void Canvas::draw_overlay_dummy(const Cairo::RefPtr<Cairo::Context>& cr) {
  if (!m_overlay_dummy.visible) return;
  const OverlayDummy& d = m_overlay_dummy;
  if (d.textboxes.empty()) return;

  const double inv_zoom = 1.0 / std::max(m_zoom, 0.001);
  const double handle_doc = kHandlePx * inv_zoom;
  const double half_handle = handle_doc * 0.5;
  const double hairline = 1.0 * inv_zoom;

  Rect bb = container_bbox(d);
  const bool overlay_visible_with_area =
      d.overlay_shown && d.ovw > 0.0 && d.ovh > 0.0;
  Rect ov = overlay_visible_with_area ? overlay_rect(d) : Rect{0,0,0,0};
  OverlayDummy::Point grip = grip_position(d);

  cr->save();

  // Container bbox — thin blue frame, derived visualization only.
  cr->rectangle(bb.x, bb.y, bb.w, bb.h);
  cr->set_source_rgba(0.2, 0.4, 0.8, 0.7);
  cr->set_line_width(hairline);
  cr->set_dash(std::vector<double>{}, 0.0);
  cr->stroke();

  // Flow connectors — between each consecutive pair of members, a
  // dashed line from member i's bottom-right to member i+1's top-left
  // with a midpoint arrowhead pointing in flow direction (0 -> N).
  // Drawn before the member outlines and handles so it sits under
  // them. Only meaningful when there's more than one member.
  if (d.textboxes.size() > 1) {
    cr->set_source_rgba(0.55, 0.3, 0.7, 0.8);
    const double arrow_doc = 9.0 * inv_zoom;
    std::vector<double> fdash = { 5.0 * inv_zoom, 4.0 * inv_zoom };
    for (std::size_t i = 0; i + 1 < d.textboxes.size(); ++i) {
      Rect a = textbox_rect(d.textboxes[i]);
      Rect b = textbox_rect(d.textboxes[i + 1]);
      const double ax = a.x + a.w, ay = a.y + a.h;   // i's BR
      const double bx = b.x,        by = b.y;         // i+1's TL

      cr->set_line_width(hairline * 1.3);
      cr->set_dash(fdash, 0.0);
      cr->move_to(ax, ay);
      cr->line_to(bx, by);
      cr->stroke();
      cr->set_dash(std::vector<double>{}, 0.0);

      // Midpoint arrowhead, pointing in flow direction (BR -> TL, i.e.
      // toward member i+1). Tip sits at the midpoint; the two barbs
      // trail BEHIND the tip along the travel direction.
      const double mx = (ax + bx) * 0.5, my = (ay + by) * 0.5;
      const double ang = std::atan2(by - ay, bx - ax);
      const double spread = 0.5;  // radians half-spread of the head
      cr->move_to(mx, my);
      cr->line_to(mx - arrow_doc * std::cos(ang - spread),
                  my - arrow_doc * std::sin(ang - spread));
      cr->move_to(mx, my);
      cr->line_to(mx - arrow_doc * std::cos(ang + spread),
                  my - arrow_doc * std::sin(ang + spread));
      cr->stroke();
    }
  }

  // Members — painted in flow order (0..N), tail last so it draws on
  // top. Tail gets a brighter green; earlier members a dimmer green so
  // z-order is legible. Selected members get a highlight tint; the
  // editing member gets a red inner rect.
  const std::size_t n = d.textboxes.size();
  const bool sel_ok = (d.selected.size() == n);
  for (std::size_t i = 0; i < n; ++i) {
    Rect tb = textbox_rect(d.textboxes[i]);
    const bool is_tail = (i + 1 == n);
    const bool is_sel = sel_ok && d.selected[i];
    const bool is_active = (static_cast<int>(i) == d.active);

    // Selection highlight — translucent fill behind the outline.
    if (is_sel) {
      cr->rectangle(tb.x, tb.y, tb.w, tb.h);
      cr->set_source_rgba(0.95, 0.8, 0.2, 0.18);
      cr->fill();
    }

    cr->rectangle(tb.x, tb.y, tb.w, tb.h);
    if (is_sel)       cr->set_source_rgba(0.95, 0.7, 0.1, 0.95);  // amber
    else if (is_tail) cr->set_source_rgba(0.1, 0.7, 0.3, 0.95);
    else              cr->set_source_rgba(0.1, 0.55, 0.3, 0.55);
    cr->set_line_width(hairline * (is_active ? 2.5 : 1.5));
    cr->stroke();

    // Editing member — red inner rect, inset a little.
    if (static_cast<int>(i) == d.editing) {
      const double inset = 6.0 * inv_zoom;
      cr->rectangle(tb.x + inset, tb.y + inset,
                    tb.w - 2 * inset, tb.h - 2 * inset);
      cr->set_source_rgba(0.85, 0.15, 0.15, 0.95);
      cr->set_line_width(hairline * 1.8);
      cr->stroke();
    }

    // Flow-order badge at the member's top-left.
    cr->set_source_rgba(0.1, 0.4, 0.2, 0.9);
    cr->move_to(tb.x + 4.0 * inv_zoom, tb.y + 14.0 * inv_zoom);
    cr->set_font_size(12.0 * inv_zoom);
    cr->show_text(std::to_string(i));
  }

  // Overlay — dashed gray, attached to the tail, when visible.
  if (overlay_visible_with_area) {
    cr->rectangle(ov.x, ov.y, ov.w, ov.h);
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.15);
    cr->fill_preserve();
    cr->set_source_rgba(0.4, 0.4, 0.4, 0.85);
    cr->set_line_width(hairline);
    std::vector<double> dashes = { 4.0 * inv_zoom, 3.0 * inv_zoom };
    cr->set_dash(dashes, 0.0);
    cr->stroke();
    cr->set_dash(std::vector<double>{}, 0.0);
  }

  // 8 sizer handles per member (all members — activate-one-activates-
  // all means every member shows its handles together).
  cr->set_source_rgba(0.2, 0.4, 0.8, 0.95);
  for (const auto& s : d.textboxes) {
    for (const auto& pt : s) {
      cr->rectangle(pt.x - half_handle, pt.y - half_handle,
                    handle_doc, handle_doc);
      cr->fill();
    }
  }

  // Overlay grip (derived from tail.BR) — orange, when overlay shown.
  if (d.overlay_shown) {
    cr->set_source_rgba(0.85, 0.4, 0.1, 0.95);
    cr->rectangle(grip.x - half_handle, grip.y - half_handle,
                  handle_doc, handle_doc);
    cr->fill();
  }

  // Toggle button at tail textbox upper-right inset.
  {
    auto t = toggle_btn_center(d, m_zoom);
    const double r = (kToggleBtnPx * 0.5) * inv_zoom;
    cr->arc(t.x, t.y, r, 0, 2 * M_PI);
    cr->set_source_rgba(0.95, 0.95, 0.95, 0.95);
    cr->fill_preserve();
    cr->set_source_rgba(0.2, 0.2, 0.2, 0.9);
    cr->set_line_width(hairline);
    cr->stroke();
    cr->set_source_rgba(0.2, 0.2, 0.2, 0.9);
    cr->set_line_width(hairline * 1.2);
    const double half_glyph = r * 0.5;
    cr->move_to(t.x - half_glyph, t.y);
    cr->line_to(t.x + half_glyph, t.y);
    cr->stroke();
    if (!d.overlay_shown) {
      cr->move_to(t.x, t.y - half_glyph);
      cr->line_to(t.x, t.y + half_glyph);
      cr->stroke();
    }
  }

  // Link button — centered in the overlay region. Only drawn when the
  // overlay is shown (nothing spilling, nowhere to link otherwise).
  // White disc with a chain-link "+" mark (a small box + arrow feel:
  // here a plus inside a rounded square to read as "add a box").
  if (overlay_visible_with_area) {
    auto l = link_btn_center(d);
    const double r = (kLinkBtnPx * 0.5) * inv_zoom;
    cr->arc(l.x, l.y, r, 0, 2 * M_PI);
    cr->set_source_rgba(0.95, 0.97, 1.0, 0.97);
    cr->fill_preserve();
    cr->set_source_rgba(0.2, 0.4, 0.8, 0.95);
    cr->set_line_width(hairline * 1.2);
    cr->stroke();
    // "+" mark.
    cr->set_source_rgba(0.2, 0.4, 0.8, 0.95);
    cr->set_line_width(hairline * 1.4);
    const double hg = r * 0.5;
    cr->move_to(l.x - hg, l.y);
    cr->line_to(l.x + hg, l.y);
    cr->stroke();
    cr->move_to(l.x, l.y - hg);
    cr->line_to(l.x, l.y + hg);
    cr->stroke();
  }

  cr->restore();
}

int Canvas::hit_test_overlay_dummy(double doc_x, double doc_y,
                                   int* out_member) const {
  if (out_member) *out_member = -1;
  if (!m_overlay_dummy.visible) return -1;
  const OverlayDummy& d = m_overlay_dummy;
  if (d.textboxes.empty()) return -1;

  const double inv_zoom = 1.0 / std::max(m_zoom, 0.001);
  const double hit_radius = (kHandlePx * 0.5 + kHandleHitSlackPx) * inv_zoom;
  const std::size_t n = d.textboxes.size();
  const int tail_idx = static_cast<int>(n) - 1;

  // Link button (centered in overlay) — highest priority, only when
  // overlay shown. Belongs to the tail.
  if (d.overlay_shown && d.ovw > 0.0 && d.ovh > 0.0) {
    auto l = link_btn_center(d);
    const double r = (kLinkBtnPx * 0.5 + kHandleHitSlackPx) * inv_zoom;
    const double dx = doc_x - l.x;
    const double dy = doc_y - l.y;
    if (dx * dx + dy * dy <= r * r) {
      if (out_member) *out_member = tail_idx;
      return 11;
    }
  }

  // Toggle button (shadows tail's TR area).
  {
    auto t = toggle_btn_center(d, m_zoom);
    const double r = (kToggleBtnPx * 0.5 + kHandleHitSlackPx) * inv_zoom;
    const double dx = doc_x - t.x;
    const double dy = doc_y - t.y;
    if (dx * dx + dy * dy <= r * r) {
      if (out_member) *out_member = tail_idx;
      return 9;
    }
  }

  // Overlay grip (derived from tail.BR).
  if (d.overlay_shown) {
    OverlayDummy::Point grip = grip_position(d);
    if (std::abs(doc_x - grip.x) <= hit_radius &&
        std::abs(doc_y - grip.y) <= hit_radius) {
      if (out_member) *out_member = tail_idx;
      return 8;
    }
  }

  // Sizers + bodies — walk tail-first (newest on top wins overlapping
  // clicks). Within a member, sizers take priority over the body.
  for (int i = tail_idx; i >= 0; --i) {
    const Sizers& s = d.textboxes[i];
    for (int k = 0; k < 8; ++k) {
      if (std::abs(doc_x - s[k].x) <= hit_radius &&
          std::abs(doc_y - s[k].y) <= hit_radius) {
        if (out_member) *out_member = i;
        return k;
      }
    }
  }

  // Body hits — live regions only: any member rect, or the tail's
  // overlay. Everything else (masked corners, bbox interior gaps) is
  // dead space and falls through. Tail-first again.
  for (int i = tail_idx; i >= 0; --i) {
    Rect tb = textbox_rect(d.textboxes[i]);
    if (doc_x >= tb.x && doc_x <= tb.x + tb.w &&
        doc_y >= tb.y && doc_y <= tb.y + tb.h) {
      if (out_member) *out_member = i;
      return 10;
    }
  }
  if (d.overlay_shown && d.ovw > 0.0 && d.ovh > 0.0) {
    Rect ov = overlay_rect(d);
    if (doc_x >= ov.x && doc_x <= ov.x + ov.w &&
        doc_y >= ov.y && doc_y <= ov.y + ov.h) {
      // Overlay body — treat as tail body drag (move the tail member).
      if (out_member) *out_member = tail_idx;
      return 10;
    }
  }

  return -1;
}

void Canvas::overlay_dummy_drag_begin(int sizer, int member,
                                      double doc_x, double doc_y) {
  OverlayDummy& d = m_overlay_dummy;
  d.active_sizer = sizer;
  d.active_member = member;
  // Ctrl + body drag → move every member as a whole.
  d.drag_all = (m_mod_ctrl && sizer == 10);
  if (d.drag_all) d.drag_start_all = d.textboxes;
  if (member >= 0 && member < static_cast<int>(d.textboxes.size()))
    d.drag_start_sizers = d.textboxes[member];
  d.drag_start_ovw = d.ovw;
  d.drag_start_ovh = d.ovh;
  d.drag_press_doc_x = doc_x;
  d.drag_press_doc_y = doc_y;
  LOG_INFO("Canvas: overlay-dummy drag-begin — sizer={} member={} drag_all={} "
           "press=({:.1f},{:.1f})", sizer, member,
           d.drag_all ? "true" : "false", doc_x, doc_y);
}

void Canvas::overlay_dummy_drag_update(double doc_x, double doc_y) {
  OverlayDummy& d = m_overlay_dummy;
  if (d.active_sizer < 0) return;

  const double dx = doc_x - d.drag_press_doc_x;
  const double dy = doc_y - d.drag_press_doc_y;

  if (d.active_sizer >= 0 && d.active_sizer <= 7) {
    // ── Textbox sizer drag (one member: active_member) ─────────────
    const int mi = d.active_member;
    if (mi < 0 || mi >= static_cast<int>(d.textboxes.size())) return;
    Sizers& s = d.textboxes[mi];

    const int i = d.active_sizer;
    OverlayDummy::Point start = d.drag_start_sizers[i];
    double nx = start.x + dx;
    double ny = start.y + dy;

    if (i == 1 || i == 5) nx = start.x;  // top/bottom mid: lock x
    if (i == 3 || i == 7) ny = start.y;  // left/right mid: lock y

    const OverlayDummy::Point start_TL = d.drag_start_sizers[0];
    const OverlayDummy::Point start_TR = d.drag_start_sizers[2];
    const OverlayDummy::Point start_BR = d.drag_start_sizers[4];
    double L = start_TL.x;
    double T = start_TL.y;
    double R = start_TR.x;
    double B = start_BR.y;

    if (sizer_touches_left(i))   L = nx;
    if (sizer_touches_right(i))  R = nx;
    if (sizer_touches_top(i))    T = ny;
    if (sizer_touches_bottom(i)) B = ny;

    if (R - L < kMinTextboxW) {
      if (sizer_touches_left(i))  L = R - kMinTextboxW;
      if (sizer_touches_right(i)) R = L + kMinTextboxW;
    }
    if (B - T < kMinTextboxH) {
      if (sizer_touches_top(i))    T = B - kMinTextboxH;
      if (sizer_touches_bottom(i)) B = T + kMinTextboxH;
    }

    s[0] = { L, T };
    s[2] = { R, T };
    s[4] = { R, B };
    s[6] = { L, B };
    recompute_midpoints(s);
    // If this member is the tail, the overlay (ovw, ovh) is untouched
    // and the grip reprojects from the new tail.BR automatically.
  } else if (d.active_sizer == 8) {
    // ── Overlay grip drag — mutates (ovw, ovh) on the Mgr. ─────────
    double new_ovw = d.drag_start_ovw + dx;
    double new_ovh = d.drag_start_ovh + dy;
    if (new_ovw < kMinOverlayW) new_ovw = kMinOverlayW;
    if (new_ovh < kMinOverlayH) new_ovh = kMinOverlayH;
    d.ovw = new_ovw;
    d.ovh = new_ovh;
  } else if (d.active_sizer == 10) {
    // ── Body drag. Ctrl-drag (drag_all) translates every member as a
    // whole; otherwise translate only the grabbed member (independent
    // positioning). If the moved member is the tail, the overlay rides
    // along (grip derived from tail.BR).
    if (d.drag_all) {
      const std::size_t n = std::min(d.drag_start_all.size(),
                                     d.textboxes.size());
      for (std::size_t m = 0; m < n; ++m)
        for (int i = 0; i < 8; ++i) {
          d.textboxes[m][i].x = d.drag_start_all[m][i].x + dx;
          d.textboxes[m][i].y = d.drag_start_all[m][i].y + dy;
        }
    } else {
      const int mi = d.active_member;
      if (mi < 0 || mi >= static_cast<int>(d.textboxes.size())) return;
      Sizers& s = d.textboxes[mi];
      for (int i = 0; i < 8; ++i) {
        s[i].x = d.drag_start_sizers[i].x + dx;
        s[i].y = d.drag_start_sizers[i].y + dy;
      }
    }
  }
  queue_draw();
}

void Canvas::overlay_dummy_drag_end() {
  if (m_overlay_dummy.active_sizer < 0) return;
  LOG_INFO("Canvas: overlay-dummy drag-end — members={} overlay=({:.1f}x{:.1f})",
           m_overlay_dummy.textboxes.size(),
           m_overlay_dummy.ovw, m_overlay_dummy.ovh);
  m_overlay_dummy.active_sizer = -1;
  m_overlay_dummy.active_member = -1;
  m_overlay_dummy.drag_all = false;
}

} // namespace Curvz
