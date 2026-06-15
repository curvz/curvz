#pragma once
//
// ImportFailureDialog — read-only "this file could not be imported"
// dialog.
//
// s360. Built to the s210 m1 ImageInfoDialog architecture: a Curvz-
// themed Gtk::Window (NOT a Gtk::AlertDialog — see the s125 m1g note in
// Canvas.cpp on why the system alert primitive is wrong: it paints in
// the OS theme and its body text isn't selectable). This dialog inherits
// the app motif via apply_motif_class_from_parent and renders each value
// as a selectable Gtk::Entry so the user can copy the path/reason out
// into a bug report or a terminal.
//
// Why this exists
// ---------------
// Before s360, a failed SVG import was a silent no-op from the user's
// seat: import_svg_to_canvas logged a line to the *file* log (or, in the
// "parsed but nothing drawable" case, nothing at all) and returned. The
// user dropped a file and saw nothing happen. This dialog surfaces the
// failure and names what went wrong.
//
// Surface
// -------
// A short heading ("Couldn't import this file"), then a name/value grid:
// Name, Path, Reason, and an optional Detail row (hidden when empty,
// same blank-when-unknown convention as ImageInfoDialog's Format/Depth).
// A right-aligned Close button takes initial focus.
//
// Lifetime (s200 m1 idiom)
// ------------------------
// Hide-on-close singleton owned by MainWindow. The widget tree builds
// once on first show() via the m_built latch; subsequent show()s
// repopulate via sync_from_data(). set_hide_on_close(true) lets the
// default close action hide rather than destroy.
//

#include "ImportFailure.hpp"

#include "widgets/Button.hpp"

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

#include <vector>

namespace Curvz {

class ImportFailureDialog : public Gtk::Window {
public:
    // Default-constructed once as a MainWindow member; lives for the
    // app's lifetime. Widget tree builds lazily on first show().
    ImportFailureDialog();

    ~ImportFailureDialog() override = default;

    ImportFailureDialog(const ImportFailureDialog&) = delete;
    ImportFailureDialog& operator=(const ImportFailureDialog&) = delete;

    // Present the dialog with a fresh failure payload.
    //
    // parent — transient-for, re-applied each show() so the dialog
    //          tracks whichever MainWindow is hosting it.
    // data   — pre-baked strings; see ImportFailure.hpp.
    void show(Gtk::Window& parent, ImportFailure data);

private:
    void build();
    void sync_from_data();
    void add_row(const char* name, int row);

    // ── State ───────────────────────────────────────────────────────
    bool          m_built = false;
    ImportFailure m_data;

    // ── Widgets ─────────────────────────────────────────────────────
    // Heading above the grid; the value-side Entries and name-side
    // Labels in row order (sync_from_data walks these + m_data in
    // lockstep). Two parallel vectors mirror ImageInfoDialog so the
    // optional Detail row can hide both halves together.
    Gtk::Label*               m_heading = nullptr;
    Gtk::Grid*                m_grid = nullptr;
    std::vector<Gtk::Label*>  m_row_labels;
    std::vector<Gtk::Entry*>  m_value_entries;

    // Close button — substrate-registered as `dlg_importfail_close`
    // for script-driven testing.
    curvz::widgets::Button*   m_btn_close = nullptr;
};

} // namespace Curvz
