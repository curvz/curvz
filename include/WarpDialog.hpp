#pragma once
#include "SceneNode.hpp"
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

namespace Curvz {

// ── WarpDialog ───────────────────────────────────────────────────────────────
// Non-modal dialog shown from MainWindow::on_warp_make() and
// MainWindow::on_warp_edit(). The live Warp node is mutated in-place as
// the user changes controls, so the canvas shows the warp updating in
// real time underneath the dialog.
//
// Controls:
//   - Top anchor count (2..4)      : Gtk::SpinButton
//   - Bottom anchor count (2..4)   : Gtk::SpinButton
//   - Preset dropdown              : Flat, Arc Up, Arc Down, Bulge,
//                                    Squeeze, Perspective Near,
//                                    Perspective Far, Wave
//   - Quality slider (1..16)       : Gtk::Scale with ticks at 1/4/8/16
//   - Apply / Cancel
//
// Lifecycle:
//   show(parent, warp, glyph_bbox, apply_cb, cancel_cb)
//     - warp: pointer to the live Warp node in the doc tree. Dialog
//       writes through to warp->warp_env_top / warp_env_bottom /
//       warp_quality, flags warp_cache_dirty, and calls the canvas
//       redraw hook on each change.
//     - glyph_bbox: (x, y, w, h) of the source's bbox in doc space
//       (Y-down). Preset shape functions use this to size envelope
//       anchors and handles.
//     - apply_cb: invoked when user clicks Apply — passes the final
//       Result struct back to caller (for EditWarpCommand / orchestrated
//       MakeWarpCommand commit).
//     - cancel_cb: invoked when user clicks Cancel or closes the
//       window — caller should revert envelope + quality to pre-dialog
//       snapshot (edit flow) or rip the scratch Warp out of the tree
//       (make flow).
//
// Both callbacks fire at most once per show(). Internal state (dropdown
// selection, anchor counts) is reset per show from the passed Warp.
class WarpDialog : public Gtk::Window {
public:
    struct Result {
        PathData env_top;
        PathData env_bottom;
        int      quality;     // 1..16
    };

    using ApplyCb  = std::function<void(Result)>;
    using CancelCb = std::function<void()>;
    // Live-update callback — fires on every control change so the caller
    // can writethrough to the live Warp node and trigger a canvas redraw.
    // Distinct from apply so changes are visible without commit.
    using UpdateCb = std::function<void(Result)>;

    WarpDialog();

    void show(Gtk::Window& parent,
              const SceneNode* warp,
              double  glyph_bx, double glyph_by,
              double  glyph_bw, double glyph_bh,
              UpdateCb update_cb,
              ApplyCb  apply_cb,
              CancelCb cancel_cb);

private:
    void on_ok();
    void on_cancel();
    void on_close_request_handled();

    // Rebuild envelope PathData from current dropdown + anchor counts +
    // bbox. Pushes the result to the update callback.
    void emit_update();

    // ── Layout ────────────────────────────────────────────────────────────
    Gtk::Box  m_outer{Gtk::Orientation::VERTICAL};
    Gtk::Grid m_grid;

    // Anchor-count spinners
    Gtk::SpinButton m_top_count;
    Gtk::SpinButton m_bot_count;

    // Preset dropdown
    Gtk::DropDown m_preset;
    Glib::RefPtr<Gtk::StringList> m_preset_model;

    // Quality slider
    Gtk::Scale m_quality;

    // Buttons
    Gtk::Box    m_btn_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Button m_btn_cancel{"Cancel"};
    Gtk::Button m_btn_ok{"Apply"};

    // Seed / sticky state.
    double m_bx = 0.0, m_by = 0.0, m_bw = 1.0, m_bh = 1.0;
    int    m_last_top_count = 2;
    int    m_last_bot_count = 2;
    int    m_last_preset    = 0;     // 0 = Flat
    int    m_last_quality   = 4;

    bool   m_loading   = false;
    bool   m_committed = false;      // true after on_ok fires, suppresses cancel
    UpdateCb m_update_cb;
    ApplyCb  m_apply_cb;
    CancelCb m_cancel_cb;
};

} // namespace Curvz
