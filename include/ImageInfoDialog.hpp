#pragma once
//
// ImageInfoDialog — read-only properties dialog for an image SceneNode.
//
// s210 m1 — lifted out of Canvas.cpp's right-click handler into its own
// class, matching the s200 ThemeEditDialog / s201 StyleEditorDialog
// architecture. Was the last raw `new Gtk::Window` self-deleting dialog
// in Canvas.cpp; the Close button was the last raw `Gtk::Button` in the
// file (it had a useful abbrev — `dlg_imginfo_close` — which is why the
// s209 m5 pass left it for this conversion rather than tagging it with
// `unregistered_t`).
//
// Surface
// -------
// Nine read-only rows in a name/value grid (filename, path, pixel size,
// format, depth, file size, modified, placed-size, linkage) plus a
// right-aligned Close button. Format and Depth hide via set_visible()
// when the meta read can't determine them (blank-when-unknown UX,
// matching the pre-conversion inline form). Each value is a frameless
// flat Gtk::Entry rather than a Gtk::Label — see the s125 m1j
// commentary preserved in the implementation for the measure-loop /
// hover-warning story behind that choice.
//
// Lifetime (s200 m1 idiom)
// ------------------------
// Hide-on-close singleton owned by MainWindow. One instance lives for
// the app's lifetime; the widget tree builds once on first show() via
// the m_built latch, and subsequent show()s repopulate values via
// sync_from_data(). signal_close_request lets the default close-action
// proceed (set_hide_on_close(true) → hide).
//
// This replaces the prior heap-allocated form
// (`new Gtk::Window` → `signal_hide → delete dlg`) inside Canvas.cpp.
// Caller now just calls show() with a fresh ImageInfo payload.
//
// Why values via show() rather than a SceneNode pointer
// -----------------------------------------------------
// The dialog is a pure presenter. Canvas does the file-system reads
// (read_image_meta, last_write_time, format_file_size, the canvas-units
// printf) and hands the fully-baked strings over via the ImageInfo POD.
// The dialog never reads the disk, never sees a SceneNode, never knows
// about ImageMeta. Same separation StyleEditorDialog uses (caller hands
// over a style::Style; the dialog edits it as a presenter).
//

#include "ImageInfo.hpp"

#include "curvz/widgets/Button.hpp"

#include <gtkmm/box.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/window.h>

#include <vector>

namespace Curvz {

class ImageInfoDialog : public Gtk::Window {
public:
    // s210 m1 — default-constructed; the dialog is a MainWindow member
    // and lives for the app's lifetime. The widget tree builds lazily
    // on first show() via the m_built latch.
    ImageInfoDialog();

    ~ImageInfoDialog() override = default;

    ImageInfoDialog(const ImageInfoDialog&) = delete;
    ImageInfoDialog& operator=(const ImageInfoDialog&) = delete;

    // Present the dialog with a fresh set of values.
    //
    // parent — transient-for. Re-applied each show() so the dialog
    //          tracks whichever MainWindow is hosting it (mirrors the
    //          s200/s201 multi-window-ready convention).
    // data   — pre-baked strings; see ImageInfo.hpp. The dialog stores
    //          this in m_data so sync_from_data() can re-read on each
    //          show() after first build.
    void show(Gtk::Window& parent, ImageInfo data);

private:
    void build();

    // Build / refresh the rows. build() creates the full row roster
    // once on first show() (nine rows: filename, path, pixels, format,
    // depth, file size, modified, placed, linkage). sync_from_data()
    // refreshes the Entry values from m_data on every show() and
    // toggles visibility on the two optional rows (format, depth) per
    // the same blank-when-unknown convention the original inline form
    // used. Build-once stays clean; the visual roster matches the
    // pre-conversion behaviour.
    void sync_from_data();

    // Append one name/value (label + Entry) row to m_grid. The Entry
    // is appended to m_value_entries in the same order; sync_from_data
    // walks both in lockstep with the m_data fields. The label widget
    // pointer is captured into m_row_labels so that optional rows can
    // hide both halves together (the optional-flag lives in the
    // anonymous-namespace row_specs table in the .cpp — sync reads it
    // there). Only called from build().
    void add_row(const char* name, int row);

    // ── State ───────────────────────────────────────────────────────
    // s210 m1 — m_built guards build() so the widget tree constructs
    // once on first show() and stays in the singleton's tree until app
    // shutdown. Subsequent show() calls only run sync_from_data().
    bool      m_built = false;
    ImageInfo m_data;

    // ── Widgets ─────────────────────────────────────────────────────
    // The value-side Entries and the name-side Labels, in row order.
    // sync_from_data() walks these and m_data side-by-side. Two
    // parallel vectors (rather than a struct) because the Entries also
    // get poked individually by build() to set frameless styling, while
    // the Labels are write-once at build().
    Gtk::Grid*                m_grid = nullptr;
    std::vector<Gtk::Label*>  m_row_labels;
    std::vector<Gtk::Entry*>  m_value_entries;

    // Close button — substrate-registered as `dlg_imginfo_close` for
    // script-driven testing. This was the last raw Gtk::Button in
    // Canvas.cpp; lifting the dialog converts it.
    curvz::widgets::Button*  m_btn_close = nullptr;
};

} // namespace Curvz
