#pragma once
#include <gtkmm/box.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include "widgets/DropDown.hpp"
#include "widgets/Scale.hpp"
#include "widgets/SpinButton.hpp"

namespace Curvz {

// ── WarpPopover ──────────────────────────────────────────────────────────────
// s154 m4a: defaults editor for new Warps, hosted on the toolbar's Warp
// button right-click.
//
// Trial deliverable. The same four controls also live in the inspector's
// Application ▸ Warp subsection (s146 m3). Both surfaces edit AppPreferences
// independently — last write wins, no live sync between them. The point of
// shipping both is to compare which feels right; once a winner is chosen
// the loser retires.
//
// Form: Top anchors (2..4 spin), Bottom anchors (2..4 spin), Preset
// (8-entry dropdown), Quality (1..16 slider), explicit Done button.
// Edits write directly to AppPreferences setters as the user changes
// them — no Apply/Cancel semantics; the Done button is dismissal only.
// The toolbar's Warp left-click is what actually fires the warp
// operation, reading from AppPreferences exactly as it has since
// s146 m3.
//
// s155 fix: previously the popover used set_autohide(true), which
// broke after the user picked a preset — GTK4's nested-popover
// dismissal sequence (the dropdown's internal popover closing)
// corrupted the outer popover's autohide grab so neither outside-click
// nor Esc would dismiss it. Fix: set_autohide(false) + explicit Done
// button + Esc key controller. Same dismissal shape as
// StepRepeatPopover (s154 m2). The dropdown is preserved because it's
// the natural control for "pick one from a labelled list."
//
// No "(Custom)" entry in the preset dropdown — Application defaults
// describe the shape the next NEW warp will start with. Per-instance
// shape editing of an existing warp lives in the inspector's
// Object ▸ Warp section, untouched by this milestone.

class WarpPopover : public Gtk::Popover {
public:
    WarpPopover();

    // Show the popover anchored to `anchor` (typically the Warp toolbar
    // button). Refreshes control values from AppPreferences each time so
    // that edits made via the inspector subsection between shows are
    // reflected. Mirror direction (popover edits) writes back to
    // AppPreferences via the setters; the inspector picks them up on
    // its next rebuild.
    void show(Gtk::Widget& anchor);

private:
    // Refresh control values from AppPreferences. Re-entrancy guard via
    // m_loading so the on-change handlers don't write back during a
    // programmatic refresh.
    void refresh_from_prefs();

    // Build the form once (called from ctor).
    void build_form();

    Gtk::Box*                    m_outer        = nullptr;
    curvz::widgets::SpinButton*  m_top_spin     = nullptr;
    curvz::widgets::SpinButton*  m_bot_spin     = nullptr;
    curvz::widgets::DropDown*    m_preset_dd    = nullptr;
    curvz::widgets::Scale*       m_quality_sc   = nullptr;
    bool                         m_loading      = false;
};

} // namespace Curvz
