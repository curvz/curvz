#pragma once
#include "GResourceExporter.hpp"
#include "CurvzProject.hpp"
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/entry.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/separator.h>
#include <gtkmm/grid.h>
#include <functional>
#include <vector>

// ── ExportDialog ──────────────────────────────────────────────────────────────
// Modal dialog for exporting an icon theme.
//
// Shows:
//   • Theme name / comment entries
//   • Scrollable checklist of documents (pre-ticked if they have name+category)
//   • Documents missing name or category shown greyed with a warning note
//   • Output folder entry + Browse button
//   • Export button → runs GResourceExporter, shows progress bar
//   • Result summary (icons written / skipped)

namespace Curvz {

class ExportDialog : public Gtk::Window {
public:
    using DoneCallback = std::function<void(bool success, std::string output_dir)>;

    ExportDialog();

    // Populate from project and show modal attached to parent.
    // done_cb is called when the dialog closes (whether export ran or not).
    void show(Gtk::Window& parent, CurvzProject& project, DoneCallback done_cb = nullptr);

private:
    void build_ui();
    void on_browse();
    void on_export();
    void on_close();
    void set_busy(bool busy);
    void show_result(const ExportResult& result);

    // ── Icon checklist row ────────────────────────────────────────────────
    struct DocRow {
        CurvzDocument*    doc     = nullptr;
        Gtk::CheckButton* check   = nullptr;  // managed
        bool              valid   = false;    // has name + category
    };

    // ── Project ref ───────────────────────────────────────────────────────
    CurvzProject* m_project = nullptr;
    DoneCallback  m_done_cb;

    // ── UI widgets ────────────────────────────────────────────────────────
    Gtk::Box    m_root{Gtk::Orientation::VERTICAL};

    // Header
    Gtk::Grid   m_meta_grid;
    Gtk::Entry  m_theme_name_entry;
    Gtk::Entry  m_theme_comment_entry;

    // Doc checklist
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_list_box{Gtk::Orientation::VERTICAL};

    // Output folder
    Gtk::Box    m_folder_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Entry  m_folder_entry;
    Gtk::Button m_browse_btn{"Browse…"};

    // Warning label (shown when some docs lack name/category)
    Gtk::Label m_warn_label;

    // Progress
    Gtk::ProgressBar m_progress;
    Gtk::Label       m_status_label;

    // Buttons
    Gtk::Box    m_btn_row{Gtk::Orientation::HORIZONTAL};
    Gtk::Button m_cancel_btn{"Cancel"};
    Gtk::Button m_export_btn{"Export Theme…"};

    // ── State ─────────────────────────────────────────────────────────────
    std::vector<DocRow> m_rows;
    bool                m_exported = false;
};

} // namespace Curvz
