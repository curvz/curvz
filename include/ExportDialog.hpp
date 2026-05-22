#pragma once
//
// Curvz::ExportDialog — unified Export dialog.
//
// File ▸ Export… (Ctrl+Shift+T). Tabbed dialog with two tabs:
//
//   Documents   — flat per-doc SVG / PNG / refpt-coordinates output
//                 with user-chosen size (px / mm / in / pt + DPI),
//                 fit-side, refpt format selector. One export run
//                 produces one kind of output per the s178 three-way
//                 format radio.
//
//   Icon Theme  — freedesktop icon-theme bundle, scalable + 16x16
//                 directories, gresource.xml + index.theme + README.
//
// History.
// ────────
//
// Pre-s179 these were two separate dialogs reachable from two
// separate menu entries (File ▸ Export Icon Theme… and Project ▸
// Export Documents…). The menu split was an artifact of the order
// each shipped, not a model the user benefited from. s179 m1
// unified them via additive-prove-retire-consolidate migration
// discipline; m1a-c shipped the new tabbed parent alongside the
// old dialogs, m1d retired the old menu/action wiring, deleted the
// old source files, and lifted this class out of its temporary
// `Curvz::migration` namespace into the regular `Curvz` namespace.
//
// Lifetime.
// ─────────
//
// Self-managed. Constructor presents the window; signal_close_request
// fires done_cb and schedules `delete this` on the next idle. Same
// pattern as ThemesDialog and StyleEditorDialog. The s126 lifecycle
// rule applies: signal_close_request, not signal_hide.
//
// Layout.
// ───────
//
//   ┌─ Export ─────────────────────────────────────────────────┐
//   │ ┌─ Documents ─┬─ Icon Theme ─┐                           │
//   │ │ <tab body>                                              │
//   │ │                                            [Export]    │
//   │ └─────────────────────────────────────────────────────┘  │
//   │                                          [Close]         │
//   └─────────────────────────────────────────────────────────┘
//
// The tab-local Export button fires the operation. The dialog-level
// Close button is the only "dismiss" path — Cancel is dropped because
// Close already serves that role in the unified parent. Esc / X / Close
// all route through signal_close_request.
//

#include "CurvzSpinButton.hpp"
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/window.h>
#include <string>
#include <vector>

namespace Curvz {

class CurvzProject;
class CurvzDocument;
class MainWindow;
struct ExportResult; // defined in GResourceExporter.hpp; forward-decl
                     // here so the header doesn't drag the exporter
                     // include in.

class ExportDialog {
public:
  using DoneCallback =
      std::function<void(bool success, std::string output_dir)>;

  // Construct + present.
  //
  // parent  — transient-for; required.
  // project — borrowed; must outlive the dialog.
  // done_cb — called once on close. Optional.
  ExportDialog(Gtk::Window &parent, CurvzProject &project,
               DoneCallback done_cb = nullptr);

private:
  // ── Build ──────────────────────────────────────────────────────────
  void build();
  void build_documents_tab(Gtk::Box &tab_root);
  void build_icon_theme_tab(Gtk::Box &tab_root);
  void build_footer(Gtk::Box &root);

  // Documents-tab sub-section builders. Each writes its section
  // into the supplied tab Box.
  void docs_build_targets(Gtk::Box &tab);
  void docs_build_format(Gtk::Box &tab);
  void docs_build_size(Gtk::Box &tab);
  void docs_build_refpts(Gtk::Box &tab);
  void docs_build_output(Gtk::Box &tab);
  void docs_build_status_and_button(Gtk::Box &tab);

  // Icon-Theme-tab sub-section builders. Each writes its section
  // into the supplied tab Box.
  void theme_build_metadata(Gtk::Box &tab);
  void theme_build_checklist(Gtk::Box &tab);
  void theme_build_output(Gtk::Box &tab);
  void theme_build_progress(Gtk::Box &tab);
  void theme_build_status_and_button(Gtk::Box &tab);

  // theme_populate fills the icon checklist, picks a default theme
  // name, and sets the default folder. Called once at build time;
  // the dialog is one-shot per construction, so a single populate
  // call covers the lifetime.
  void theme_populate();

  // ── Documents-tab wiring ───────────────────────────────────────────

  // Recompute UI sensitivity based on current radio/dropdown selection.
  // Hides/shows the DPI row based on units; hides/shows the custom-DPI
  // spin based on dropdown selection. Toggles size-vs-refpts section
  // based on the format radio.
  void docs_update_visibility();

  std::string docs_current_units() const;
  int docs_current_dpi() const;
  int docs_chosen_dim_px() const;

  void docs_refresh_refpts_info();

  // Per-doc plan resolved once at on_export time and reused by
  // docs_perform_export. Each entry is a fully-formed target path
  // plus the source doc pointer.
  struct DocTarget {
    CurvzDocument *doc = nullptr;
    std::string path; // dir/stem.ext, no disambiguation
    int doc_index = 0;
  };

  // ── Action handlers ────────────────────────────────────────────────
  void docs_on_select_all();
  void docs_on_select_none();
  void docs_on_browse();
  void docs_on_export();

  void docs_perform_export(const std::vector<DocTarget> &targets);

  // Theme-tab handlers. theme_set_busy enables/disables inputs during
  // the synchronous export loop; theme_show_result populates the
  // status label with the success or failure summary.
  void theme_on_browse();
  void theme_on_export();
  void theme_set_busy(bool busy);
  void theme_show_result(const ExportResult &result);

  void on_close(); // dialog-level Close button

  // Resolve the MainWindow that owns this dialog, when there is one.
  MainWindow *main_window() const;

  // Display-name helper for the Documents-tab targets list.
  static std::string doc_display_name(const CurvzDocument &doc,
                                      std::size_t fallback_index);

  // ── State ──────────────────────────────────────────────────────────
  Gtk::Window *m_window = nullptr;
  Gtk::Window *m_parent = nullptr;
  CurvzProject *m_project = nullptr;
  DoneCallback m_done_cb;
  bool m_export_ran = false;

  // Remembered output directory of the last successful export (either
  // tab). Used by the close-request handler to pass a meaningful dir
  // to done_cb. Empty until something exports successfully.
  std::string m_last_export_dir;

  // Notebook + tabs
  Gtk::Notebook *m_notebook = nullptr;
  Gtk::Box *m_docs_tab = nullptr;
  Gtk::Box *m_theme_tab = nullptr;

  // Dialog-level footer
  Gtk::Button *m_btn_close = nullptr;

  // ── Documents-tab widgets ──────────────────────────────────────────

  // Targets
  Gtk::Box *m_docs_targets_box = nullptr;
  std::vector<Gtk::CheckButton *>
      m_docs_target_checks; // parallel to project->documents

  // Format — three-way radio
  Gtk::CheckButton *m_docs_radio_svg = nullptr;
  Gtk::CheckButton *m_docs_radio_png = nullptr;
  Gtk::CheckButton *m_docs_radio_refpt = nullptr;

  // Section wrappers
  Gtk::Box *m_docs_size_section = nullptr;
  Gtk::Box *m_docs_refpts_section = nullptr;

  // Size — fit side
  Gtk::CheckButton *m_docs_radio_fit_w = nullptr;
  Gtk::CheckButton *m_docs_radio_fit_h = nullptr;

  // Size — value + units + DPI
  Gtk::SpinButton *m_docs_value_spin = nullptr;
  Gtk::DropDown *m_docs_units_drop = nullptr;
  Gtk::Box *m_docs_dpi_row = nullptr;
  Gtk::Label *m_docs_dpi_label = nullptr;
  Gtk::DropDown *m_docs_dpi_drop = nullptr;
  Gtk::SpinButton *m_docs_dpi_custom = nullptr;

  // Output
  Gtk::Entry *m_docs_folder_entry = nullptr;
  Gtk::Button *m_docs_btn_browse = nullptr;

  // Refpts
  Gtk::DropDown *m_docs_refpts_format_drop = nullptr;
  Gtk::Box *m_docs_refpts_format_row = nullptr;
  Gtk::Label *m_docs_refpts_info_label = nullptr;

  // Status + per-tab Export button
  Gtk::Label *m_docs_status = nullptr;
  Gtk::Button *m_docs_btn_export = nullptr;

  // ── Icon-Theme-tab widgets ─────────────────────────────────────────

  // Per-doc row in the theme checklist.
  struct ThemeDocRow {
    CurvzDocument *doc = nullptr;
    Gtk::CheckButton *check = nullptr; // managed
    bool valid = false;                // has export_name + export_category
  };

  // Metadata
  Gtk::Grid *m_theme_meta_grid = nullptr;
  Gtk::Entry *m_theme_name_entry = nullptr;
  Gtk::Entry *m_theme_comment_entry = nullptr;

  // Doc checklist
  Gtk::Box *m_theme_list_box = nullptr; // scrollable contents
  Gtk::Label *m_theme_warn_label = nullptr;
  std::vector<ThemeDocRow> m_theme_rows;

  // Output folder
  Gtk::Entry *m_theme_folder_entry = nullptr;
  Gtk::Button *m_theme_btn_browse = nullptr;

  // Progress + status
  Gtk::ProgressBar *m_theme_progress = nullptr;
  Gtk::Label *m_theme_status_label = nullptr;

  // Per-tab Export button
  Gtk::Button *m_theme_btn_export = nullptr;
};

} // namespace Curvz
