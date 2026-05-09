#pragma once
//
// ExportDocsDialog — Project → Export Documents… (S104 m1).
//
// Per-document SVG/PNG export. Distinct from the icon-theme exporter
// (ExportDialog → File → Export Icon Theme…) which writes a freedesktop
// icon-theme bundle of fixed sizes. This dialog targets the general
// "I want flat output files for these N docs" use case.
//
// Layout
// ───────
//
//   ┌─ Export Documents ──────────────────────────────────────┐
//   │ Description label (what gets written)                   │
//   │                                                          │
//   │  Documents                                               │
//   │  [ ☑ Doc 1 (active)                                ]    │
//   │  [ ☑ Doc 2                                         ]    │
//   │  [ ☑ Doc 3                                         ]    │
//   │  [Select all] [Select none]                             │
//   │                                                          │
//   │  Format     (•) SVG    ( ) PNG                          │
//   │                                                          │
//   │  Size                                                    │
//   │   Fit to:  (•) Width    ( ) Height                      │
//   │   Value:   [256.00] [px ▾]                              │
//   │   DPI:     [96 ▾]   [custom_spin]   (hidden when px)    │
//   │                                                          │
//   │  Output folder                                           │
//   │  [/home/scott/…                          ] [Browse…]    │
//   │                                                          │
//   │  [status / progress text]                               │
//   │                                                          │
//   │                              [Cancel] [Export]          │
//   └──────────────────────────────────────────────────────────┘
//
// Sizing model
// ─────────────
//
// User picks one side (Width or Height) and an output dimension. The
// other side is derived from the doc's aspect ratio per-doc:
//
//   if fit_side == Width:   out_w = chosen_px;   out_h = out_w * (ch/cw)
//   if fit_side == Height:  out_h = chosen_px;   out_w = out_h * (cw/ch)
//
// "chosen_px" comes from the value+units control:
//
//   units == px:   chosen_px = value          (DPI hidden, irrelevant)
//   units == mm:   chosen_px = value * dpi / 25.4
//   units == in:   chosen_px = value * dpi
//   units == pt:   chosen_px = value * dpi / 72.0
//
// DPI is one of {72, 96, 100, 144, 300, 600, Custom}. Custom enables a
// numeric spin (1..2400, default 100). Trimmed deliberately — Scott's
// prepress muscle memory pushes 100/144/300; the rest cover web/print
// defaults; the custom field handles edge cases.
//
// SVG path doesn't use DPI (it's a vector format). The size-and-units
// row still drives the SVG output: write_svg_file_with_export_meta()
// stamps the user's intent into data-curvz-export-* attrs on the root
// <svg> for downstream tooling and round-trip on re-import.
//
// Filename rule
// ──────────────
//
// "<doc.filename>.<ext>" or "Document_<i>.<ext>" if filename empty.
// Collisions in the output dir get " (2)", " (3)" suffixes — same
// pattern as theme/style import name disambiguation.
//
// Lifetime
// ─────────
//
// Self-managed. Constructor does present(); the dialog deletes itself
// via signal_close_request → idle delete-this. Same pattern as
// ThemesDialog and StyleEditorDialog.
//

#include <functional>
#include <string>
#include <vector>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/window.h>

namespace Curvz {

class CurvzProject;
class CurvzDocument;

class ExportDocsDialog {
public:
    using DoneCallback = std::function<void(bool success, std::string output_dir)>;

    // Construct + present.
    //
    // parent  — transient-for; required.
    // project — borrowed; must outlive the dialog.
    // done_cb — called once on close (export completed or cancelled).
    //           Optional.
    //
    // Last-folder memory is keyed "export-docs" and is read/written
    // through the parent MainWindow's get_last_folder/set_last_folder
    // (see main_window() helper). No caller plumbing required —
    // construct as before and the dialog persists its own choice.
    ExportDocsDialog(Gtk::Window& parent,
                     CurvzProject& project,
                     DoneCallback done_cb);

private:
    // ── Build ──────────────────────────────────────────────────────────
    void build();
    void build_targets_section(Gtk::Box& root);
    void build_format_section(Gtk::Box& root);
    void build_size_section(Gtk::Box& root);
    void build_refpts_section(Gtk::Box& root);  // s176 m1
    void build_output_section(Gtk::Box& root);
    void build_footer(Gtk::Box& root);

    // ── Wiring helpers ─────────────────────────────────────────────────

    // Recompute UI sensitivity based on current radio/dropdown selection.
    // Hides/shows the DPI row based on units; hides/shows the custom-DPI
    // spin based on dropdown selection.
    void update_visibility();

    // Read the current units string from the units dropdown.
    // Returns one of "px", "mm", "in", "pt".
    std::string current_units() const;

    // Read the effective DPI from the DPI controls. Returns the dropdown
    // value when a preset is selected, or the custom spin's value when
    // "Custom…" is selected.
    int current_dpi() const;

    // Compute the export dimension in pixels from the value+units+DPI
    // controls. For px-units the DPI is ignored; for physical units
    // (mm/in/pt) the conversion uses current_dpi().
    int chosen_dim_px() const;

    // ── Action handlers ────────────────────────────────────────────────
    void on_select_all();
    void on_select_none();
    void on_browse();
    void on_export();
    void on_cancel();

    // Per-doc plan resolved once at on_export time and reused by
    // perform_export. Each entry is a fully-formed target path plus the
    // source doc pointer. We compute these up front so the same set of
    // targets is checked for collisions and written — no chance of the
    // collision scan and the write loop disagreeing about filenames.
    struct DocTarget {
        CurvzDocument* doc = nullptr;
        std::string    path;       // dir/stem.ext, no disambiguation
        int            doc_index = 0;
    };

    // Run the export. Called either directly (no collisions) or from the
    // confirm callback (user picked Replace). overwrite_existing=true
    // means writes go straight to target paths — files may be replaced.
    // Reads sizing controls fresh to pick up any changes since on_export
    // was clicked (the confirm dialog yields the main loop).
    void perform_export(const std::vector<DocTarget>& targets);

    // Resolve the MainWindow that owns this dialog, when there is one.
    // Returns nullptr if the parent isn't a MainWindow (theoretical — in
    // practice always invoked from MainWindow::on_export_docs). Only used
    // for the per-purpose last-folder API; absence just means we skip the
    // remember/restore.
    class MainWindow* main_window() const;

    // Display-name helper — same rule as ThemesDialog.
    static std::string doc_display_name(const class CurvzDocument& doc,
                                        std::size_t fallback_index);

    // ── State ──────────────────────────────────────────────────────────
    Gtk::Window*    m_window  = nullptr;
    Gtk::Window*    m_parent  = nullptr;
    CurvzProject*   m_project = nullptr;
    DoneCallback    m_done_cb;
    bool            m_export_ran = false;  // for done_cb result flag

    // Targets
    Gtk::Box*                       m_targets_box = nullptr;
    std::vector<Gtk::CheckButton*>  m_target_checks;  // parallel to project->documents

    // Format
    Gtk::CheckButton* m_radio_svg = nullptr;
    Gtk::CheckButton* m_radio_png = nullptr;

    // Size — fit side
    Gtk::CheckButton* m_radio_fit_w = nullptr;
    Gtk::CheckButton* m_radio_fit_h = nullptr;

    // Size — value + units + DPI
    Gtk::SpinButton* m_value_spin   = nullptr;  // 1..10000, 2dp
    Gtk::DropDown*   m_units_drop   = nullptr;  // px / mm / in / pt
    Gtk::Box*        m_dpi_row      = nullptr;  // hidden when units=px
    Gtk::Label*      m_dpi_label    = nullptr;
    Gtk::DropDown*   m_dpi_drop     = nullptr;  // 72/96/100/144/300/600/Custom
    Gtk::SpinButton* m_dpi_custom   = nullptr;  // hidden unless Custom

    // Output
    Gtk::Entry*  m_folder_entry = nullptr;
    Gtk::Button* m_btn_browse   = nullptr;

    // Refpts (s176 m1) — sidecar coordinate export. Only the CSV format
    // is wired in m1; m2 will add JSON/G-code/DXF entries to the dropdown.
    // The checkbox enables the section; when off the dropdown is hidden
    // and no sidecar files are written. The "info" label below the
    // dropdown shows a count of refpts that will be exported across the
    // currently selected docs (refreshed on target/origin change so the
    // user sees the consequence of their selection before clicking Export).
    Gtk::CheckButton* m_refpts_check = nullptr;
    Gtk::DropDown*    m_refpts_format_drop = nullptr;
    Gtk::Box*         m_refpts_format_row  = nullptr;
    Gtk::Label*       m_refpts_info_label  = nullptr;

    // Update the refpts info label and dropdown sensitivity based on
    // current selection + checkbox state. Cheap to call on any change.
    void refresh_refpts_info();

    // Footer
    Gtk::Label*  m_status        = nullptr;
    Gtk::Button* m_btn_cancel    = nullptr;
    Gtk::Button* m_btn_export    = nullptr;
};

} // namespace Curvz
