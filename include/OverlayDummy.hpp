// ── OverlayDummy ────────────────────────────────────────────────────────────
//
// s315 m1 — Multi-textbox model. The dummy graduates from N=1 to N>=1
// textbox members, proving the TextboxMgr-as-one-object model: one
// object owns an ORDERED LIST of textbox members; text flows 0->1->2
// ->N; the TAIL (last member) owns the single overlay (overflow) region.
//
// Primary data:
//   - textboxes: vector of members, each 8 sizer anchors (doc-space).
//     Order IS flow order IS z-order. Member 0 first, tail (back()) last
//     and on top. Text starts at 0, overflow bounces to the tail.
//   - 2 overlay dimensions (ovw, ovh) on the Mgr (NOT on a member) —
//     define the overlay's size. Rendered against the tail's BR.
//   - 1 visibility flag (overlay_shown).
//
// Derived (computed on read, per member or for the tail):
//   - Textbox rect: bounding rect of a member's 4 corner sizers.
//   - Grip position: tail.textbox.BR + (ovw, ovh). Always derived from
//     the TAIL — never stored. Re-link or delete changes who the tail
//     is, and the overlay re-anchors automatically.
//   - Overlay rect: from tail.textbox.BR (top-left) to the grip
//     (bottom-right). Visible only when overlay_shown.
//   - Container bbox: union of all member rects (+ overlay when shown).
//
// Sizer indices (per member):
//   0: TL  1: T  2: TR  3: R  4: BR  5: B  6: BL  7: L
//   (Corners 0/2/4/6 authoritative; midpoints 1/3/5/7 kept at edge
//   midpoints by recompute_midpoints after every drag.)
//
// Hit codes (active_sizer):
//   0..7  textbox sizer drag (paired with active_member)
//   8     overlay grip drag — mutates (ovw, ovh)
//   9     toggle button click — flips overlay_shown
//   10    body drag — translates one member's 8 sizers (active_member)
//   11    link button click — appends a new tail member; overflow
//         re-anchors to it
//   -1    idle
//
// "Activate one activates all": activation/selection is whole-Mgr (all
// members + overlay draw their handles together). Drag targets the one
// member under the cursor. Link is opt-in (a deliberate click), matching
// the real model where threading is a user act, not automatic.

#pragma once

#include <array>
#include <vector>

namespace Curvz {

struct OverlayDummy {
  bool visible = false;          // dummy drawn on canvas
  bool overlay_shown = true;     // overlay region visible

  struct Point { double x, y; };
  using Sizers = std::array<Point, 8>;

  // ── Primary state ────────────────────────────────────────────────
  // N textbox members. Order = flow order = z-order (tail on top).
  // Seeded with two members: member 0 wide (a headline-shaped block),
  // member 1 a narrower column beneath it. The tail (member 1) owns
  // the overlay.
  std::vector<Sizers> textboxes = {
    // member 0 — wide headline block
    {{
      { 200.0, 160.0 },  // 0 TL
      { 400.0, 160.0 },  // 1 T
      { 600.0, 160.0 },  // 2 TR
      { 600.0, 200.0 },  // 3 R
      { 600.0, 240.0 },  // 4 BR
      { 400.0, 240.0 },  // 5 B
      { 200.0, 240.0 },  // 6 BL
      { 200.0, 200.0 },  // 7 L
    }},
    // member 1 — column beneath
    {{
      { 200.0, 280.0 },  // 0 TL
      { 290.0, 280.0 },  // 1 T
      { 380.0, 280.0 },  // 2 TR
      { 380.0, 400.0 },  // 3 R
      { 380.0, 520.0 },  // 4 BR
      { 290.0, 520.0 },  // 5 B
      { 200.0, 520.0 },  // 6 BL
      { 200.0, 400.0 },  // 7 L
    }},
  };

  // Overlay dimensions. On the Mgr, attached to the tail. Persistent —
  // survives hide/show, survives textbox reshapes, survives link/delete.
  // Grip position is derived as tail.BR + (ovw, ovh).
  double ovw = 180.0;
  double ovh = 120.0;

  // ── Selection state (s315 m3 mock) ───────────────────────────────
  // Two distinct states, per the model:
  //   - active: the single focus member (-1 = none).
  //   - selected: the set built by shift-click (parallel to textboxes).
  //   - editing: the member showing the red edit rect (-1 = none),
  //     set by double-click.
  // These vectors are kept in lockstep with textboxes by link/delete.
  int active = -1;
  int editing = -1;
  std::vector<bool> selected = { false, false };

  // ── Drag state ───────────────────────────────────────────────────
  int active_sizer = -1;     // 0..7 within active_member, or 8/10 verb
  int active_member = -1;    // which member the sizer/body belongs to
  bool drag_all = false;     // ctrl-drag: move every member as a whole
  Sizers drag_start_sizers;  // snapshot of active member's sizers
  std::vector<Sizers> drag_start_all;  // snapshot of all members (drag_all)
  double drag_start_ovw = 0.0;
  double drag_start_ovh = 0.0;
  double drag_press_doc_x = 0.0;
  double drag_press_doc_y = 0.0;
};

} // namespace Curvz
