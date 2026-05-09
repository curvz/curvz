#pragma once
#include "CurvzSpinButton.hpp"
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>

namespace Curvz {

struct CanvasModel;

// ── BlendDialog ──────────────────────────────────────────────────────────────
// Non-modal dialog shown from MainWindow::on_blend() before the Blend
// operation commits. Collects:
//   - Steps (1..50)
//   - Reverse direction (swap A/B at apply time)
//   - Stroke width range — checkbox + start/end spins (Width type, in
//     doc units). When unchecked, stroke widths interpolate A→B natively.
//   - Node-count mismatch warning banner + an Equalize button. M3
//     renders the banner and leaves the button disabled-with-tooltip
//     (M4 implements De Casteljau midpoint insertion).
//
// The dialog DOES NOT perform the Blend itself — it returns a Result
// via apply_cb and the caller orchestrates. This keeps the "dialog as
// passive data-collector" contract matching StepRepeatPopover.
//
// Non-modality note: canvas must stay interactive so the user can
// click another object and cancel/retry without being blocked.
class BlendDialog : public Gtk::Window {
public:
    struct Result {
        int    steps;                // 1..50
        bool   reverse;              // swap A and B at apply
        bool   stroke_w_override;    // checkbox state
        double stroke_w_start;       // doc units
        double stroke_w_end;         // doc units
    };

    using ApplyCb = std::function<void(Result)>;
    // Equalize callback — invoked when the user clicks the Equalize button
    // in the node-count-mismatch warning banner. Implementation lives
    // outside the dialog (MainWindow orchestrates Canvas::equalize_blend_sources).
    // The callback returns the resulting node count on both sides so the
    // dialog can refresh its banner state.
    using EqualizeCb = std::function<int()>;

    BlendDialog();

    // Show the dialog against the active CanvasModel (for unit labels).
    // a_count / b_count: node counts of the two selected paths. If they
    //   don't match, the warning banner becomes visible. Set both to
    //   the same non-zero value to suppress the banner.
    // a_stroke_w / b_stroke_w: current stroke widths of A and B (doc
    //   units) — used to seed the stroke-width range when the override
    //   is first toggled on.
    void show(Gtk::Window&       parent,
              const CanvasModel* model,
              int                a_count,
              int                b_count,
              double             a_stroke_w,
              double             b_stroke_w,
              ApplyCb            apply_cb,
              EqualizeCb         equalize_cb = nullptr);

private:
    void on_ok();
    void on_cancel();

    // Rebuild the model-dependent spins (stroke-width start/end) against
    // the current CanvasModel. Called on every show().
    void build_model_spins(const CanvasModel* model);

    void refresh_stroke_row_sensitive();

    // ── Layout ────────────────────────────────────────────────────────────
    Gtk::Box  m_outer{Gtk::Orientation::VERTICAL};
    Gtk::Grid m_grid;

    // Warning banner — hidden when counts match.
    Gtk::Box   m_warn_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Label m_warn_lbl;
    Gtk::Button m_btn_equalize{"Equalize"};

    // Steps
    Gtk::SpinButton m_steps;

    // Reverse
    Gtk::CheckButton m_reverse{"Reverse direction (swap A↔B)"};

    // Stroke width override
    Gtk::CheckButton m_stroke_override{"Override stroke width range"};
    Gtk::Box         m_stroke_start_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Box         m_stroke_end_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Label       m_stroke_start_lbl;
    Gtk::Label       m_stroke_end_lbl;
    CurvzSpinButton* m_stroke_start = nullptr;
    CurvzSpinButton* m_stroke_end   = nullptr;

    // Buttons
    Gtk::Box    m_btn_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Button m_btn_cancel{"Cancel"};
    Gtk::Button m_btn_ok{"Blend"};

    // Sticky last values across invocations.
    int    m_last_steps             = 4;
    bool   m_last_reverse           = false;
    bool   m_last_stroke_override   = false;
    double m_last_stroke_start      = 0.0;
    double m_last_stroke_end        = 0.0;

    // Seed defaults (per-show).
    double m_seed_a_stroke_w = 0.0;
    double m_seed_b_stroke_w = 0.0;
    int    m_seed_a_count    = 0;
    int    m_seed_b_count    = 0;

    bool    m_loading = false;
    ApplyCb    m_apply_cb;
    EqualizeCb m_equalize_cb;
};

} // namespace Curvz
