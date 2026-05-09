#pragma once
#include "CurvzSpinButton.hpp"
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/widget.h>

namespace Curvz {

struct CanvasModel;

// ── StepRepeatPopover ─────────────────────────────────────────────────────────
// s154 m2: was StepRepeatDialog (Gtk::Window). Same form, same callbacks,
// same Result shape — relocated into a popover anchored to the SnR
// toolbar button so the toolbar's right-click can summon it without a
// separate top-level window.
//
// autohide is forced OFF so canvas clicks pass through without
// dismissing the popover. Cancel/Repeat buttons are the only dismissal
// surface; this preserves the dialog-era pivot-drag-on-canvas
// interaction unchanged (popover visible, user drags crosshair on
// canvas, spins update, popover stays open until explicit dismissal).
//
// Form: Copies / Offset X/Y / (optional) Rotate around pivot.
//
// Pass 1 interactivity (unchanged from dialog era):
//   - Popover owns the pivot. Default = selection bbox center.
//   - User edits pivot via numeric spins OR by dragging the dot in the mini
//     preview OR by dragging the crosshair on canvas. All stay in sync.
//   - While the popover is open, a crosshair is shown on canvas at the
//     pivot (via pivot_change_cb).
//
// Callbacks:
//   - pivot_change_cb: fires on every pivot change, live. (px, py, visible)
//     Fires with visible=false on cancel/close so MainWindow hides the
//     crosshair.
//   - apply_cb: fires when the user clicks Repeat (or — for left-click on
//     the toolbar button — via apply_with_last(), which never opens UI).
//
// dx/dy/pivot are in DOC UNITS (doc px, Y-down — matches node storage).
// Copies is plain integer. angle_deg is per-copy rotation in degrees.

class StepRepeatPopover : public Gtk::Popover {
public:
    enum class AngleMode { Auto, Fixed };

    struct Result {
        int    copies;
        double dx;
        double dy;
        bool   rotate_enabled;
        double angle_deg;   // per-copy (auto: 360/copies; fixed: user-entered)
        double pivot_x;     // doc coords, Y-down
        double pivot_y;
    };

    using ApplyCb       = std::function<void(Result)>;
    // px/py in doc coords (Y-down). visible: true while popover open,
    // false on close.
    using PivotChangeCb = std::function<void(double px, double py, bool visible)>;

    StepRepeatPopover();

    // bbox_cx / bbox_cy: selection bbox center in doc coords (Y-down); used
    // to seed the default pivot on first show AND as reference for the mini
    // preview so the pivot dot is positioned correctly relative to the
    // selection. bbox_w / bbox_h: selection bbox size (for preview scale).
    // If w/h ~0, preview shows a fallback square.
    //
    // s154 m2: anchor is the widget the popover attaches to (typically
    // the SnR toolbar button). If the popover already has a parent set
    // and it differs from `anchor`, set_parent is re-pointed.
    void show(Gtk::Widget&       anchor,
              const CanvasModel* model,
              double             bbox_cx,
              double             bbox_cy,
              double             bbox_w,
              double             bbox_h,
              PivotChangeCb      pivot_cb,
              ApplyCb            apply_cb);

    // s154 m2: Apply the persisted last values without showing UI. Used
    // by the toolbar's left-click path: the user has already configured
    // SnR via the popover at least once, and now wants to re-apply the
    // same operation to a new selection. No popover popup, no preview,
    // no callbacks fired except apply_cb. If the user has never opened
    // the popover, the hardcoded constructor defaults are used (3 copies,
    // 20px right, no rotate) — same shape as the original dialog's
    // first-show defaults.
    void apply_with_last(const CanvasModel* model,
                         double             bbox_cx,
                         double             bbox_cy,
                         ApplyCb            apply_cb);

    // Called by MainWindow when the user drags the crosshair on canvas.
    // Updates spins + preview dot without re-firing pivot_cb back to canvas
    // (canvas already has the value).  px/py in doc coords (Y-down).
    void set_pivot_from_canvas(double px, double py);

private:
    void on_ok();
    void on_cancel();

    // Build (or rebuild) per-model spins against the active CanvasModel.
    void build_model_spins(const CanvasModel* model);

    // Emit pivot_change_cb with current state.
    void emit_pivot_change(bool visible);

    // Mini preview helpers.
    void on_preview_draw(const Cairo::RefPtr<Cairo::Context>& cr,
                         int width, int height);
    void on_preview_press(int n_press, double x, double y);
    void on_preview_drag_update(double offset_x, double offset_y);

    // Convert pixel coords in preview <-> doc coords (via bbox frame).
    void preview_px_to_doc(double px, double py, double& dx, double& dy) const;
    void doc_to_preview_px(double dx, double dy, double& px, double& py) const;

    // Refresh preview from current pivot spin values.
    void refresh_preview();

    // Update angle spin sensitivity + value based on mode + copies.
    void refresh_angle_ui();

    // ── Layout ────────────────────────────────────────────────────────────
    Gtk::Box  m_outer{Gtk::Orientation::VERTICAL};
    Gtk::Grid m_grid;

    // Copies
    Gtk::SpinButton  m_copies;

    // Offset X / Y  (per-show csb)
    Gtk::Box         m_off_x_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Box         m_off_y_row{Gtk::Orientation::HORIZONTAL};
    CurvzSpinButton* m_offset_x = nullptr;
    CurvzSpinButton* m_offset_y = nullptr;

    // Rotate enable + angle mode
    Gtk::CheckButton m_rotate_enable;
    Gtk::DropDown    m_angle_mode;

    // Angle (csb Angle type)
    Gtk::Box         m_angle_row{Gtk::Orientation::HORIZONTAL};
    CurvzSpinButton* m_angle_spin = nullptr;

    // Pivot X / Y (per-show csb — PositionX / PositionY)
    Gtk::Box         m_pivot_x_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Box         m_pivot_y_row{Gtk::Orientation::HORIZONTAL};
    CurvzSpinButton* m_pivot_x = nullptr;
    CurvzSpinButton* m_pivot_y = nullptr;

    // Mini preview
    Gtk::DrawingArea m_preview;

    Gtk::Button      m_btn_cancel{"Cancel"};
    Gtk::Button      m_btn_ok    {"Repeat"};

    // ── State ─────────────────────────────────────────────────────────────

    // Last values (doc units) — preserved across shows.
    double  m_last_dx      = 20.0;
    double  m_last_dy      = 0.0;
    bool    m_last_rotate  = false;
    double  m_last_angle   = 15.0;  // fixed-mode angle remembered per-session
    int     m_last_mode    = 0;     // 0 = Auto, 1 = Fixed

    // Per-show state.
    double  m_bbox_cx = 0.0;    // selection bbox centre (doc, Y-down)
    double  m_bbox_cy = 0.0;
    double  m_bbox_w  = 100.0;
    double  m_bbox_h  = 100.0;

    // Current pivot (doc coords, Y-down).
    double  m_pivot_dx = 0.0;
    double  m_pivot_dy = 0.0;

    // Preview drag state
    bool    m_preview_dragging = false;
    double  m_drag_anchor_pdx  = 0.0;
    double  m_drag_anchor_pdy  = 0.0;
    double  m_drag_anchor_px   = 0.0;  // preview pixel at press
    double  m_drag_anchor_py   = 0.0;

    // Re-entrancy guard for spin callbacks.
    bool    m_loading = false;

    ApplyCb       m_apply_cb;
    PivotChangeCb m_pivot_cb;
};

} // namespace Curvz
