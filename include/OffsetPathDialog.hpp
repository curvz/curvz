#pragma once
#include "CurvzEntry.hpp"
#include "CurvzSpinButton.hpp"
#include "math/PathOffset.hpp"
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/grid.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/separator.h>
#include <functional>

namespace Curvz {

struct CanvasModel;

// Modal "Offset Path" dialog.
// Presents distance + side (Outside / Inside / Both).
// Call show(parent, canvas_model, callback) — callback called on Apply, not on Cancel.
//
// The CanvasModel* is passed per-show so the distance spin honors the active
// document's display unit. Each show() rebuilds the spin against the current
// model — cheap, and correct on doc switch.

class OffsetPathDialog : public Gtk::Window {
public:
    struct Options {
        double     distance      = 10.0;   // doc units
        OffsetSide side          = OffsetSide::Outside;
        bool       keep_original = false;
    };

    using Callback = std::function<void(Options)>;

    OffsetPathDialog();
    void show(Gtk::Window& parent,
              const CanvasModel* model,
              Callback cb);

private:
    void on_apply();
    void on_cancel();

    // Build (or rebuild) the distance spin against the active model.
    // Called from show() — replaces any previous spin/unit-label children
    // of m_dist_row in place.
    void build_distance_spin(const CanvasModel* model, double initial);

    Gtk::Box         m_root{Gtk::Orientation::VERTICAL};
    Gtk::Grid        m_grid;
    Gtk::Box         m_dist_row{Gtk::Orientation::HORIZONTAL};
    CurvzSpinButton* m_distance_spin = nullptr;   // rebuilt per-show
    Gtk::DropDown    m_side_drop;
    Gtk::CheckButton m_keep_original{"Keep original path"};
    Gtk::Separator   m_sep;
    Gtk::Box         m_btn_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Button      m_btn_cancel{"Cancel"};
    Gtk::Button      m_btn_apply{"Apply"};

    // Last distance entered (preserved across shows).
    double           m_last_distance = 10.0;

    Callback m_callback;
};

} // namespace Curvz
