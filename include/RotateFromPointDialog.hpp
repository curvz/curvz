#pragma once
//
// RotateFromPointDialog — modal dialog for the R-key rotate-from-point
// flow. Hosts the pivot X/Y spinners (already populated from the right-
// click pivot placement), an angle spinner (always 0° on open), and a
// Cancel/Apply button row.
//
// s210 m2 — lifted out of `Canvas_input.cpp`'s `on_pivot_dialog` into
// its own class, matching the s200 ThemeEditDialog / s201
// StyleEditorDialog / s210 m1 ImageInfoDialog architecture. Was a
// heap-allocated `new Gtk::Window` self-deleting on hide; now a
// hide-on-close singleton owned by MainWindow. Closes out the two raw
// Buttons in `Canvas_input.cpp` (`dlg_rfp_cnc` / `dlg_rfp_app`) — both
// substrate-registered here, since the abbrevs are useful test
// targets.
//
// Surface
// -------
// Two-section grid: pivot block (X / Y CurvzSpinButton in
// PositionX / PositionY mode, unit labels in column 2), separator,
// rotation block (angle CurvzSpinButton in Angle mode). Right-aligned
// Cancel / Apply button row underneath. Apply carries the
// `suggested-action` CSS class. The pivot spinners are seeded each
// show from the caller's current custom-pivot doc coords; the angle
// spinner is reset to 0° each show.
//
// Lifetime (s200 m1 idiom)
// ------------------------
// Default-constructed once as a MainWindow member; show() drives every
// open. m_built latches on first show — widget tree builds lazily and
// stays in the singleton's tree for the app's lifetime. signal_close_
// request lets the default close-action proceed (hide-on-close → hide
// without destroying).
//
// Why a CommittedFn rather than an apply signal
// ---------------------------------------------
// The dialog is a pure presenter. Apply gathers the three input values
// (pivot_x, pivot_y, angle_deg) and fires the caller-supplied
// CommittedFn — the caller (Canvas, routed through MainWindow) does
// the selection walk, the path/text/image rotation, and the
// CompositeCommand push against the history. Same separation
// StyleEditorDialog uses: dialog owns the inputs, caller owns the
// model-mutating work and the undo seam. CommittedFn fires exactly
// once per Apply; Cancel and the X close-action don't fire it.
//
// Unit-aware spinners
// -------------------
// The pivot spinners are PositionX / PositionY types; they need a
// CanvasModel* to display values in the document's current units
// (px / in / mm / pt). show() takes the model pointer and re-binds
// both spinners via set_model() before each present(). Same pattern
// ThemeEditDialog uses for its dialog-local m_unit_model; the
// difference here is the dialog *borrows* the caller's model (read-
// only, just for unit display) rather than owning its own.
//

#include "CurvzSpinButton.hpp"   // CurvzSpinButton, SpinType
#include "curvz/widgets/Button.hpp"

#include <gtkmm/box.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

#include <functional>

namespace Curvz {

struct CanvasModel;   // forward — defined in CurvzDocument.hpp

class RotateFromPointDialog : public Gtk::Window {
public:
    // Fires once when Apply is clicked, with the three input values.
    // The caller (Canvas) performs the rotation and the undo push.
    // Cancel and the X close-action do NOT fire this.
    using CommittedFn = std::function<void(double pivot_x,
                                           double pivot_y,
                                           double angle_deg)>;

    // s210 m2 — default-constructed; the dialog is a MainWindow member
    // and lives for the app's lifetime. The widget tree builds lazily
    // on first show() via the m_built latch.
    RotateFromPointDialog();

    ~RotateFromPointDialog() override = default;

    RotateFromPointDialog(const RotateFromPointDialog&) = delete;
    RotateFromPointDialog& operator=(const RotateFromPointDialog&) = delete;

    // Present the dialog with a fresh editing session.
    //
    // parent        — transient-for; re-applied each show().
    // initial_pivot — current custom-pivot doc coords (X-right /
    //                 Y-down internal convention; the PositionY spinner
    //                 handles the Y-up display flip).
    // unit_model    — CanvasModel pointer for the pivot spinners to read
    //                 display_unit and px-per-unit from. Borrowed
    //                 read-only; may be null (spinners then display in
    //                 raw doc units, matching CurvzSpinButton's default).
    // on_committed  — fires on Apply with the dialog's final values.
    //                 Stored until the next show() (or cleared on
    //                 Cancel/X paths).
    void show(Gtk::Window& parent,
              double initial_pivot_x,
              double initial_pivot_y,
              const CanvasModel* unit_model,
              CommittedFn on_committed);

private:
    void build();
    void sync_from_state();
    void on_cancel();
    void on_apply();

    // ── State ───────────────────────────────────────────────────────
    bool               m_built = false;
    double             m_initial_pivot_x = 0.0;
    double             m_initial_pivot_y = 0.0;
    const CanvasModel* m_unit_model = nullptr;
    CommittedFn        m_on_committed;

    // ── Widgets ─────────────────────────────────────────────────────
    CurvzSpinButton*         m_spin_x  = nullptr;
    CurvzSpinButton*         m_spin_y  = nullptr;
    CurvzSpinButton*         m_spin_a  = nullptr;
    curvz::widgets::Button*  m_btn_cancel = nullptr;
    curvz::widgets::Button*  m_btn_apply  = nullptr;
};

} // namespace Curvz
