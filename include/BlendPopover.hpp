#pragma once
#include "CurvzSpinButton.hpp"
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/widget.h>

namespace Curvz {

struct CanvasModel;

// ── BlendPopover ─────────────────────────────────────────────────────────────
// s154 m3: was BlendDialog (Gtk::Window). Same form, same Result shape,
// same equalize callback contract — relocated into a popover anchored
// to the Blend toolbar button.
//
// Unlike StepRepeatPopover, Blend has no canvas-interactive component
// (no pivot drag, no on-canvas crosshair), so autohide is left ON and
// outside-click dismisses cleanly.
//
// Form: Steps (1..50), Reverse direction, optional stroke-width range
// override with start/end spins. Node-count mismatch warning banner +
// Equalize button surface inline when the two source paths have
// different node counts. Equalize mutates the canvas via the supplied
// callback; the popover stays open across the mutation (the click is
// inside the popover, not outside).
//
// The popover DOES NOT perform the Blend itself — it returns a Result
// via apply_cb and the caller orchestrates. Same passive-data-collector
// contract as StepRepeatPopover.

class BlendPopover : public Gtk::Popover {
public:
  struct Result {
    int steps;              // 1..50
    bool reverse;           // swap A and B at apply
    bool stroke_w_override; // checkbox state
    double stroke_w_start;  // doc units
    double stroke_w_end;    // doc units
  };

  using ApplyCb = std::function<void(Result)>;
  // Equalize callback — invoked when the user clicks the Equalize button
  // in the node-count-mismatch warning banner. Implementation lives
  // outside the popover (MainWindow orchestrates
  // Canvas::equalize_blend_sources). The callback returns the resulting node
  // count on both sides so the popover can refresh its banner state.
  using EqualizeCb = std::function<int()>;

  BlendPopover();

  // Show the popover anchored to `anchor` (typically the Blend toolbar
  // button) against the active CanvasModel (for unit labels).
  // a_count / b_count: node counts of the two selected paths. If they
  //   don't match, the warning banner becomes visible.
  // a_stroke_w / b_stroke_w: current stroke widths of A and B (doc
  //   units) — used to seed the stroke-width range when the override
  //   is first toggled on.
  //
  // s154 m3: anchor replaces the old Gtk::Window& parent. If the
  // popover already has a parent set and it differs from `anchor`,
  // set_parent is re-pointed.
  void show(Gtk::Widget &anchor, const CanvasModel *model, int a_count,
            int b_count, double a_stroke_w, double b_stroke_w, ApplyCb apply_cb,
            EqualizeCb equalize_cb = nullptr);

  // s154 m3: Apply the persisted last values without showing UI. Used
  // by the toolbar's left-click path. Two-way contract:
  //   - If a_count == b_count: build a Result from m_last_* and fire
  //     apply_cb. No popup, no callbacks beyond apply_cb. Done.
  //   - If a_count != b_count: validation requires UI (the warning
  //     banner + Equalize button), so fall back to show() with the
  //     same params. The user sees the popover and resolves the
  //     mismatch interactively.
  // This makes left-click "do it now if possible; otherwise let me
  // configure" — the same shape as StepRepeatPopover except SnR has
  // no failure mode (any selection is repeatable).
  void apply_with_last(Gtk::Widget &anchor, const CanvasModel *model,
                       int a_count, int b_count, double a_stroke_w,
                       double b_stroke_w, ApplyCb apply_cb,
                       EqualizeCb equalize_cb = nullptr);

private:
  void on_ok();
  void on_cancel();

  // Rebuild the model-dependent spins (stroke-width start/end) against
  // the current CanvasModel. Called on every show().
  void build_model_spins(const CanvasModel *model);

  void refresh_stroke_row_sensitive();

  // ── Layout ────────────────────────────────────────────────────────────
  Gtk::Box m_outer{Gtk::Orientation::VERTICAL};
  Gtk::Grid m_grid;

  // Warning banner — hidden when counts match.
  Gtk::Box m_warn_row{Gtk::Orientation::HORIZONTAL};
  Gtk::Label m_warn_lbl;
  Gtk::Button m_btn_equalize{"Equalize"};

  // Steps
  CurvzSpinButton m_steps;

  // Reverse
  Gtk::CheckButton m_reverse{"Reverse direction (swap A↔B)"};

  // Stroke width override
  Gtk::CheckButton m_stroke_override{"Override stroke width range"};
  Gtk::Box m_stroke_start_row{Gtk::Orientation::HORIZONTAL};
  Gtk::Box m_stroke_end_row{Gtk::Orientation::HORIZONTAL};
  Gtk::Label m_stroke_start_lbl;
  Gtk::Label m_stroke_end_lbl;
  CurvzSpinButton *m_stroke_start = nullptr;
  CurvzSpinButton *m_stroke_end = nullptr;

  // Buttons
  Gtk::Box m_btn_row{Gtk::Orientation::HORIZONTAL};
  Gtk::Button m_btn_cancel{"Cancel"};
  Gtk::Button m_btn_ok{"Blend"};

  // Sticky last values across invocations.
  int m_last_steps = 4;
  bool m_last_reverse = false;
  bool m_last_stroke_override = false;
  double m_last_stroke_start = 0.0;
  double m_last_stroke_end = 0.0;

  // Seed defaults (per-show).
  double m_seed_a_stroke_w = 0.0;
  double m_seed_b_stroke_w = 0.0;
  int m_seed_a_count = 0;
  int m_seed_b_count = 0;

  bool m_loading = false;
  ApplyCb m_apply_cb;
  EqualizeCb m_equalize_cb;
};

} // namespace Curvz
