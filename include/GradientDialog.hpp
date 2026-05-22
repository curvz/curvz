#pragma once
//
// GradientDialog — modal editor for FillStyle gradient values.
//
// S90 Stage 3a — first cut. Body widget is a vertical Gtk::Box with:
//   • Type selector (Linear / Radial radio toggles).
//   • Stop list (Gtk::ListBox; one row per stop showing offset + colour).
//   • Stop properties area: position spin, opacity spin, embedded
//     CurvzColorPicker bound to the selected stop's colour.
//   • Add Stop / Remove Stop buttons.
//   • Geometry section: x1/y1/x2/y2 spins (linear) or cx/cy/r/fx/fy
//     spins (radial). All in objectBoundingBox-fraction space (0..1).
//   • Action bar: Cancel / Apply.
//
// Modal-on-parent. Apply emits the edited FillStyle via the callback;
// Cancel does nothing. The caller is responsible for assigning the
// returned FillStyle into the appropriate selection's fill slot and
// queuing redraw.
//
// Stage 3a is functionally complete (you can edit a gradient end-to-end)
// but visually plain. The decorated stop track and preview ramp from the
// mockup land in 3b.
//
// Reusability note: the body widget machinery is nested inside this
// dialog class for now. Once Stage 4 (swatches integration) needs the
// editor embedded in a non-dialog host, the body widget will be lifted
// out into a separate GradientEditor class. That refactor is intentionally
// deferred — premature factoring on a single call site is overhead.
//

#include "CurvzColorPicker.hpp"
#include "SceneNode.hpp" // FillStyle, GradientStop

#include "CurvzSpinButton.hpp"
#include <giomm/simpleactiongroup.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>

#include <functional>

namespace Curvz {

class GradientDialog : public Gtk::Window {
public:
  using Callback = std::function<void(FillStyle)>;

  GradientDialog();

  // Open the dialog, seeded with `initial` (which should be a gradient
  // FillStyle). On Apply, `cb` is invoked with the edited FillStyle.
  // On Cancel, `cb` is not invoked.
  void show(Gtk::Window &parent, FillStyle initial, Callback cb);

private:
  // ── State ────────────────────────────────────────────────────────────
  FillStyle m_working; // mutable copy edited by the UI
  int m_selected_stop = 0;
  bool m_loading = false; // re-entrancy guard for sync-from-state
  Callback m_callback;

  // ── Layout containers ────────────────────────────────────────────────
  Gtk::Box m_root{Gtk::Orientation::VERTICAL};

  // Type selector row
  Gtk::Box m_type_row{Gtk::Orientation::HORIZONTAL};
  Gtk::ToggleButton m_btn_linear{"Linear"};
  Gtk::ToggleButton m_btn_radial{"Radial"};

  // Visual stop track (3b). Replaces the text ListBox from 3a. A
  // Cairo-drawn DrawingArea renders the gradient ramp on top and the
  // coloured-square stop handles on a horizontal track below. Click a
  // square to select; drag a square to retune offset live; double-click
  // on empty track adds a stop at that offset.
  Gtk::DrawingArea m_track_area;
  int m_track_drag_idx = -1;    // -1 = idle
  bool m_track_dragged = false; // distinguishes click from drag
  double m_track_press_x = 0.0; // press x in widget coords

  // Angle knob drag state. Press is anchored at the centre of the knob
  // (geometry-driven, not user-press-driven), and drag updates compute
  // angle = atan2(cur_y - centre, cur_x - centre). m_knob_press_x/y is
  // where the press landed so we can reconstruct cur_x/y from drag dx/dy.
  double m_knob_press_x = 0.0;
  double m_knob_press_y = 0.0;

  // Stop list action buttons
  Gtk::Box m_stop_btn_row{Gtk::Orientation::HORIZONTAL};
  Gtk::Button m_btn_add_stop{"+ Add stop"};
  Gtk::Button m_btn_remove_stop{"− Remove"};
  // S92 m1 — Stage 5 polish. Reverse flips stop ordering (offset →
  // 1-offset and array reverse so the canonical ascending invariant
  // holds). Distribute spreads stops evenly along the track in their
  // current order (k-th of n becomes k/(n-1)). Both no-op for n<2.
  Gtk::Button m_btn_reverse{"↻ Reverse"};
  Gtk::Button m_btn_distribute{"⇆ Distribute"};

  // S92 m1 — Right-click context menu on stop squares (Delete /
  // Duplicate / Set offset…). The PopoverMenu dispatch on bare
  // set_parent is broken in GTK4; the canonical fix from S72 is to
  // route a fresh per-click PopoverMenu through a hidden MenuButton
  // that lives in the widget tree (so size_allocate runs and the
  // action muxer wires up correctly). m_ctx_button is sized 0x0 +
  // opacity 0; m_ctx_stop_idx remembers which stop the menu was
  // opened on so the action handlers know their target.
  Gtk::MenuButton m_ctx_button;
  Glib::RefPtr<Gio::SimpleActionGroup> m_actions;
  int m_ctx_stop_idx = -1;

  // Selected-stop properties
  Gtk::Grid m_stop_props;
  CurvzSpinButton m_pos_spin;
  CurvzSpinButton m_opacity_spin;
  CurvzColorPicker m_color_picker;

  // Geometry section
  Gtk::Separator m_sep_geom;
  Gtk::Label m_geom_header{"Geometry"};
  Gtk::Grid m_geom_grid;
  // Angle (linear only) — editing recomputes x1/y1/x2/y2 around bbox
  // centre. Editing endpoints back-computes angle. Two-way binding
  // gated by m_loading to avoid feedback loops. The knob is a small
  // circular DrawingArea that drives the same value as the spin —
  // drag it to rotate; the spin is for keyboard precision.
  Gtk::DrawingArea m_angle_knob;
  CurvzSpinButton m_angle_spin;
  Gtk::Label *m_lbl_angle = nullptr;
  // Linear endpoints
  CurvzSpinButton m_x1_spin;
  CurvzSpinButton m_y1_spin;
  CurvzSpinButton m_x2_spin;
  CurvzSpinButton m_y2_spin;
  // Radial extras
  CurvzSpinButton m_r_spin;
  // (cx/cy reuse x2/y2; fx/fy reuse x1/y1 — same fields, different labels)
  Gtk::Label *m_lbl_r = nullptr;

  // Action bar
  Gtk::Separator m_sep_actions;
  Gtk::Box m_action_row{Gtk::Orientation::HORIZONTAL};
  Gtk::Button m_btn_cancel{"Cancel"};
  Gtk::Button m_btn_apply{"Apply"};

  // ── Build helpers (called once from ctor) ────────────────────────────
  void build_type_row();
  void build_stop_track();
  void build_stop_props();
  void build_geom_section();
  void build_action_row();
  void wire_signals();

  // ── State sync ───────────────────────────────────────────────────────
  // Push m_working into all widgets. Sets m_loading while running.
  void sync_from_state();
  // Repaint the stop track. Wraps queue_draw on m_track_area.
  void redraw_track();
  // Select stop by index and update the properties area.
  void select_stop(int idx);
  // Toggle radial-only widgets (r spin + label) by current type.
  void apply_type_visibility();

  // ── Stop track callbacks (Cairo + gestures) ──────────────────────────
  void draw_track(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h);
  // Returns the index of the stop whose square contains (x, y), or -1.
  int hit_stop_square(double x, double y, int w, int h) const;
  // Convert widget x to gradient offset (0..1) accounting for gutters.
  double x_to_offset(double x, int w) const;
  void on_track_press(int n_press, double x, double y);
  void on_track_drag_update(double dx, double dy);
  void on_track_drag_end();

  // ── Angle knob callbacks ─────────────────────────────────────────────
  void draw_angle_knob(const Cairo::RefPtr<Cairo::Context> &cr, int w, int h);
  void on_knob_press(int n_press, double x, double y);
  void on_knob_drag_update(double dx, double dy);

  // ── Edit handlers ────────────────────────────────────────────────────
  void on_type_changed(); // fired by either toggle button
  void on_pos_changed();
  void on_opacity_changed();
  void on_color_changed(color::Color c);
  void on_add_stop();
  void on_remove_stop();
  // S92 m1 — Stage 5 polish + right-click context menu.
  void on_distribute_stops();
  void on_reverse_stops();
  // Right-click on a stop square. Stashes the hit index in
  // m_ctx_stop_idx, builds + presents the context menu via the
  // hidden MenuButton.
  void on_track_secondary_press(int n_press, double x, double y);
  void on_ctx_delete_stop();
  void on_ctx_duplicate_stop();
  void on_ctx_set_offset();
  void on_geom_changed();  // any of x1/y1/x2/y2/r
  void on_angle_changed(); // angle spin → recompute endpoints

  // ── Dialog actions ───────────────────────────────────────────────────
  void on_apply();
  void on_cancel();
};

} // namespace Curvz
