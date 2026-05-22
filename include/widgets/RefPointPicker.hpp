// widgets/RefPointPicker.hpp ──────────────────────────────────────────
//
// Compound first-class scriptable widget. Picks a point on a rectangular
// bbox via two paths:
//
//   • Preset mode (default) — 9 toggle points laid out on a 3x3 grid:
//     NW N NE / W C E / SW S SE. The 4 corners + 4 edge midpoints +
//     center. The X/Y inputs are editable and show the computed
//     (doc-x, doc-y) for the picked preset against the current bbox.
//     X/Y refresh live as set_bbox() changes the source rectangle.
//
//   • Arbitrary mode — entered implicitly when the user types into the
//     X or Y spinner. The preset grid greys out and deselects (no dot
//     highlighted); the user's typed values are reported verbatim,
//     independent of the bbox.
//
// s286 — there is no UI checkbox to toggle modes. Mode is inferred
// from the user's last action: click a preset → Preset; type a number
// → Arbitrary. The Mode enum, set_mode(), signal_mode_changed, and
// the `set_checkbox` script verb are all retained for the scripting /
// signal surface; the change is purely UI.
//
// Opaque inside, public action outside. Internally owns a DrawingArea
// (the 9-grid), two CurvzSpinButtons (unit-aware X/Y), and an internal
// Box hierarchy for layout. NONE of these children register with the
// scriptable registry — only the composite itself does. Scripts see
// one widget name with one verb table; the internal pieces are
// implementation detail. The composite is the script object.
//
// State remembered across mode flips:
//   - Last picked preset persists when the user switches to Arbitrary
//     and back, so the dot they previously selected is still highlighted.
//   - Last arbitrary x/y persists when the user switches to Preset and
//     back, so they don't lose typed values.
//
// First home: the refpt-pivot right-click "Set Position" popover wired
// from Canvas.cpp's right-click handler when the pivot crosshair is
// visible (m_r_held or m_has_custom_pivot). The widget is substrate-
// level — registered as a scriptable, reusable anywhere by inserting
// it into a container.

#pragma once
#include "CurvzSpinButton.hpp"
#include "scripting/ScriptableWidget.hpp"
#include "widgets/RefPointGrid.hpp"

#include <gtkmm/box.h>
#include <gtkmm/label.h>

#include <sigc++/signal.h>
#include <utility>

namespace curvz::widgets {

class RefPointPicker : public curvz::scripting::ScriptableWidget<Gtk::Box> {
public:
  enum class Mode { Preset, Arbitrary };
  // s290: Preset enum aliased to RefPointGrid::Preset so the two stay
  // in lockstep. Existing callers using RefPointPicker::Preset::C / NW /
  // etc keep working unchanged.
  using Preset = RefPointGrid::Preset;

  // The CanvasModel pointer is forwarded to the internal CurvzSpinButtons
  // so they display in the document's current unit and handle the Y-flip
  // for PositionY automatically. The picker itself stores points in
  // doc-space (Y-down, raw canvas units) — the spinners do the user-space
  // conversion at display time.
  explicit RefPointPicker(std::string_view name,
                          const Curvz::CanvasModel *canvas_model = nullptr,
                          double ruler_origin_x = 0.0,
                          double ruler_origin_y = 0.0);

  // ── Public C++ API (in addition to script verbs) ─────────────────────
  // Caller feeds the source rectangle the preset dots evaluate against.
  // In Preset mode this triggers a live X/Y label refresh and a
  // point_changed C++ signal emission. In Arbitrary mode the bbox is
  // stored but doesn't affect the user's chosen point.
  void set_bbox(double x, double y, double w, double h);

  // Programmatic mode switch. Equivalent to clicking the checkbox.
  void set_mode(Mode m);

  // Programmatic preset pick. Implicitly switches to Preset mode if
  // currently in Arbitrary.
  void set_preset(Preset p);

  // Programmatic arbitrary-coord set. Implicitly switches to Arbitrary
  // mode if currently in Preset.
  void set_arbitrary_xy(double doc_x, double doc_y);

  // s205 m2 — update the arbitrary X/Y display WITHOUT changing mode
  // and WITHOUT emitting signal_point_changed. Use case: external
  // sync (inspector → picker) when canvas pivot xy needs to refresh
  // but the picker's mode is the source of truth, not the caller's.
  // set_arbitrary_xy's mode-flip is correct for the popover seed case
  // ("type in spin → become arbitrary") but wrong for sync — sync
  // pushing arbitrary xy into a Preset-mode picker would silently
  // demote it to Arbitrary on every canvas pivot mutation, hijacking
  // the user's mode choice. This entry point lets the caller refresh
  // the underlying numbers; if the picker is currently in Preset
  // mode, the spinners are read-only and the new values just sit in
  // m_arb_x/y until/unless the user flips the checkbox.
  void update_arbitrary_xy_silent(double doc_x, double doc_y);

  // Current picked point in doc space. In Preset mode, computed from
  // bbox + preset. In Arbitrary mode, the user's typed values.
  std::pair<double, double> point() const;

  Mode mode() const { return m_mode; }
  Preset preset() const { return m_last_preset; }

  // C++ signals for in-process subscribers (separate from the script
  // emit channel which routes through ScriptableRegistry to script
  // subscribers). The popover in Canvas.cpp subscribes here to update
  // the live pivot position as the user picks.
  sigc::signal<void(double, double)> &signal_point_changed() {
    return m_sig_point_changed;
  }
  sigc::signal<void(Mode)> &signal_mode_changed() { return m_sig_mode_changed; }

  // ── Scriptable leaf-side dispatch (overrides ScriptableWidget) ───────
  curvz::scripting::ScriptValue
  invoke_leaf(std::string_view verb,
              const curvz::scripting::ScriptArgs &args) override;
  curvz::scripting::ScriptValue
  query_leaf(std::string_view property) const override;
  std::vector<std::string> leaf_verbs() const override;
  std::vector<std::string> leaf_properties() const override;

protected:
  void bind_canonical() override;

private:
  // ── Internal layout ──────────────────────────────────────────────────
  // s290: the 9-point visual grid is the extracted RefPointGrid widget.
  // RefPointPicker is now grid + spinners + bbox logic — RefPointGrid
  // alone is the reusable atom for creator popovers etc.
  RefPointGrid m_grid;
  // s286: Arbitrary checkbox removed from the UI. Mode is now inferred
  // implicitly: clicking a preset → Preset; typing into X/Y → Arbitrary.
  // The Mode enum, set_mode(), signal_mode_changed, and the
  // `set_checkbox` script verb all remain — the scripting / signal
  // surface is preserved.
  Gtk::Label m_x_lbl;
  Gtk::Label m_y_lbl;
  Curvz::CurvzSpinButton *m_sp_x = nullptr;
  Curvz::CurvzSpinButton *m_sp_y = nullptr;
  Gtk::Label *m_x_unit_lbl = nullptr; // companion from CurvzSpinButton
  Gtk::Label *m_y_unit_lbl = nullptr;

  // ── State ────────────────────────────────────────────────────────────
  Mode m_mode = Mode::Preset;
  Preset m_last_preset = Preset::C;
  double m_bbox_x = 0.0, m_bbox_y = 0.0;
  double m_bbox_w = 0.0, m_bbox_h = 0.0;
  double m_arb_x = 0.0, m_arb_y = 0.0;

  // Guard so programmatic spinner updates (e.g. when bbox or preset
  // changes in Preset mode) don't reflect back as user edits. Same
  // m_loading idiom the inspector uses for its live-tracking spinners
  // (PropertiesPanel.cpp).
  bool m_loading = false;

  // ── Output signals ───────────────────────────────────────────────────
  sigc::signal<void(double, double)> m_sig_point_changed;
  sigc::signal<void(Mode)> m_sig_mode_changed;

  // ── Internal helpers ─────────────────────────────────────────────────
  double preset_doc_x(Preset p) const;
  double preset_doc_y(Preset p) const;
  void refresh_xy_display();
};

} // namespace curvz::widgets
