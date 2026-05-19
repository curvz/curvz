// include/TranslateDialog.hpp ──────────────────────────────────────────────
//
// s205 m4 — Translate hub dialog.
//
// One place that gathers the four affine verbs (Move / Scale / Rotate / Skew)
// behind a single pivot picker. The picker IS the refpt every verb shares.
// Edits to the picker write through Canvas::set_custom_pivot, so the canvas
// pivot crosshair, the inspector pivot picker, and any right-click pivot
// popover all stay in sync — the dialog is just another surface on the
// same pivot state.
//
// Verb switcher: dropdown ("Move", "Scale", "Rotate", "Skew"). One parameter
// row visible at a time via Gtk::Stack. Apply commits one undo command,
// resets fields to neutral, keeps the dialog open so the user can chain
// transforms (the natural use case for this hub).
//
// Move semantics: the only Move interpretation that makes pivot-coherent
// sense is "move ANCHOR to DESTINATION". Anchor = the refpt position
// (whatever the picker picked, doc-space). Destination = the X/Y the user
// types. Translation vector = destination - anchor. This is why Move uses
// the same picker — refpt is the operand for every verb in the dialog.
//
// Non-modal by design. Dialog can stay open across multiple Applies and
// across canvas interactions; user can drag pivot on canvas, see picker
// update, type a new value, Apply. The leaves it operates on are re-
// collected from the current canvas selection at each Apply, so the
// dialog tracks "whatever is selected" rather than capturing once at
// open-time.

#pragma once

#include "CurvzSpinButton.hpp"
#include "widgets/RefPointPicker.hpp"

#include <gtkmm/box.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/stack.h>
#include <gtkmm/window.h>

#include <functional>

// s212 m3 — forward-declare the substrate widget types used by
// pointer-held members below. Full includes live in the .cpp (the
// s208 m5 header-coupling discipline). Avoids pulling
// ScriptableWidget + Scriptable + ScriptValue into every TU that
// includes TranslateDialog.hpp.
namespace curvz::widgets {
class Button;
class DropDown;
class SpinButton;
class ToggleButton;
}

namespace Curvz {

struct CanvasModel;
struct PathData;
struct CurvzProject;
struct SceneNode;
class Canvas;
class CommandHistory;

class TranslateDialog : public Gtk::Window {
public:
    TranslateDialog();

    // present(parent, canvas, history, model): builds the dialog UI
    // against the current canvas + active doc's model. Safe to call
    // multiple times — rebuilds the picker (and its unit-aware spinners)
    // against the supplied model so doc switches don't leave stale
    // CanvasModel* references behind.
    //
    // canvas is captured for the lifetime of the dialog instance (until
    // app shutdown — it's a MainWindow member). Same for history. Both
    // pointers stay valid across doc switches; the CanvasModel* does NOT
    // — it lives on the active doc, so we rebuild any model-bound widget
    // each present().
    void present(Gtk::Window& parent,
                 Canvas* canvas,
                 CommandHistory* history,
                 CurvzProject* project,
                 const CanvasModel* model,
                 double ruler_origin_x,
                 double ruler_origin_y);

private:
    // ── Verb enum ────────────────────────────────────────────────────────
    enum class Verb { Move, Scale, Rotate, Skew };

    // ── Build helpers (run once in ctor, parameter rows are static) ───────
    void build_root();
    void build_picker_row();   // top: picker only; populated per-present()
    void build_action_row();   // verb dropdown
    void build_param_stack();  // stack of 4 parameter rows
    void build_button_row();   // Apply, Close

    Gtk::Box*  build_move_row();
    Gtk::Box*  build_scale_row();
    Gtk::Box*  build_rotate_row();
    Gtk::Box*  build_skew_row();

    // ── Apply handlers ───────────────────────────────────────────────────
    // Each pulls fresh state at apply-time:
    //   • re-collects path leaves from current canvas selection
    //   • re-resolves pivot from Canvas::custom_pivot_*() (or bbox centre
    //     if no custom pivot is set)
    //   • runs the verb's affine math against those leaves
    //   • pushes one undo command
    //   • resets the verb's parameter fields to neutral
    void apply_current_verb();
    void do_move();
    void do_scale();
    void do_rotate();
    void do_skew();

    // ── Picker rebuild / sync ────────────────────────────────────────────
    // Rebuild the picker against the supplied model. Called from
    // present() — same idiom as OffsetPathDialog::build_distance_spin.
    void rebuild_picker(const CanvasModel* model,
                        double ruler_origin_x,
                        double ruler_origin_y);

    // Refresh the picker's bbox from the current canvas selection.
    // Cheap; used at present() time and after every Apply so the preset
    // points re-evaluate against geometry-after-transform.
    void refresh_picker_bbox();

    // Resolve current pivot from canvas state. Returns false if there's
    // no valid pivot AND no valid bbox to fall back to.
    bool current_pivot(double& out_px, double& out_py);

    // ── Members ──────────────────────────────────────────────────────────
    Canvas*         m_canvas  = nullptr;
    CommandHistory* m_history = nullptr;
    CurvzProject*   m_project = nullptr;

    // Root layout (built once)
    Gtk::Box       m_root{Gtk::Orientation::VERTICAL};
    Gtk::Box       m_picker_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Separator m_picker_sep;
    Gtk::Box       m_action_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Stack     m_param_stack;
    Gtk::Separator m_btn_sep;
    Gtk::Box       m_btn_row{Gtk::Orientation::HORIZONTAL};

    // s212 m3 — Close/Apply buttons flipped from value-held
    // Gtk::Button members to substrate-pointer-held (mirrors s198 m1
    // Toolbar zoom_spin/width_spin pattern). The substrate ctor takes
    // the abbrev as a build-time arg, so the previous
    // curvz::utils::set_name(...) calls collapse into the ctor. Build
    // sites moved to make_managed in build_button_row().
    curvz::widgets::Button* m_btn_close = nullptr;  // dlg_xlt_cls
    curvz::widgets::Button* m_btn_apply = nullptr;  // dlg_xlt_app

    // s212 m3 — Verb dropdown flipped from value-held to substrate-
    // pointer-held. Same pattern as Close/Apply above. Built in
    // build_action_row(), still set_model'd with the
    // ("Move","Scale","Rotate","Skew") vector at construction time.
    curvz::widgets::DropDown* m_verb_drop = nullptr;  // dlg_xlt_verb

    // Picker — rebuilt per-present() against the active CanvasModel.
    // Held by raw pointer because gtkmm Box::remove + make_managed re-
    // construction matches the OffsetPathDialog pattern.
    curvz::widgets::RefPointPicker* m_picker = nullptr;

    // ── Move row params ──────────────────────────────────────────────────
    Gtk::Box*        m_move_row     = nullptr;
    CurvzSpinButton* m_move_dx_spin = nullptr;
    CurvzSpinButton* m_move_dy_spin = nullptr;

    // ── Scale row params (s212 m3: substrate) ────────────────────────────
    Gtk::Box*                       m_scale_row      = nullptr;
    curvz::widgets::SpinButton*     m_scale_sx_spin  = nullptr;  // dlg_xlt_sx
    curvz::widgets::SpinButton*     m_scale_sy_spin  = nullptr;  // dlg_xlt_sy
    curvz::widgets::ToggleButton*   m_scale_link_btn = nullptr;  // dlg_xlt_sln
    bool                            m_scale_linked   = true;

    // ── Rotate row params (s212 m3: substrate) ───────────────────────────
    Gtk::Box*                       m_rotate_row     = nullptr;
    curvz::widgets::SpinButton*     m_rotate_a_spin  = nullptr;  // dlg_xlt_ra

    // ── Skew row params (s212 m3: substrate) ─────────────────────────────
    Gtk::Box*                       m_skew_row       = nullptr;
    curvz::widgets::ToggleButton*   m_skew_h_btn     = nullptr;  // dlg_xlt_kh  (grouped pair)
    curvz::widgets::ToggleButton*   m_skew_v_btn     = nullptr;  // dlg_xlt_kv
    curvz::widgets::SpinButton*     m_skew_a_spin    = nullptr;  // dlg_xlt_ka

    // Reusable scratch for the spinner re-target on doc switch (and
    // because CurvzSpinButton wants a CanvasModel* by-construction).
    // Tracked so we can rebuild on present().
    const CanvasModel* m_active_model = nullptr;
};

} // namespace Curvz
