#pragma once
#include "CurvzProject.hpp"
#include <gtkmm/window.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/frame.h>
#include <gtkmm/grid.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/printoperation.h>
#include <gtkmm/printcontext.h>
#include <gtkmm/pagesetup.h>
#include <gtkmm/printsettings.h>
#include <vector>

// ── PrintManager ──────────────────────────────────────────────────────────────
// Modal dialog for printing Curvz documents.
//
// Three print styles:
//   Sheet  — grid of icons per page, with name / path / node count / file size
//   Plot   — one icon per page in outline mode with nodes, handles, coordinates
//   Normal — one doc per page, rendered as the canvas currently shows it
//            (full fill/stroke, honours layer + object visibility). Intended
//            for "show off the art" prints. currentColor fills render as a
//            checkerboard pattern and strokes as dashed grey; a colour list
//            footer shows the palette when enabled.
//
// Left panel: scrollable checklist of all project documents.
// Right panel: style selector + options.
// Bottom: Page Setup + Print buttons.

namespace Curvz {

class PrintManager : public Gtk::Window {
public:
    PrintManager();

    // Show modal, attached to parent window.
    void show(Gtk::Window& parent, CurvzProject& project);

private:
    // ── Print styles ─────────────────────────────────────────────────────
    enum class Style { Sheet, Plot, Normal };

    // ── UI builders ──────────────────────────────────────────────────────
    void build_ui();
    void build_doc_list();
    void build_style_panel();
    void update_style_panel();   // swap option widgets when style changes

    // ── Print pipeline ───────────────────────────────────────────────────
    void run_print();

    // signal_draw_page handlers
    void draw_sheet_page(const Cairo::RefPtr<Cairo::Context>& cr,
                         int page_nr, Gtk::PrintContext& ctx);
    void draw_plot_page (const Cairo::RefPtr<Cairo::Context>& cr,
                         int page_nr, Gtk::PrintContext& ctx);

    // Helper: render one doc icon onto cr at (x,y) with given side length (pts)
    void render_icon_cell(const Cairo::RefPtr<Cairo::Context>& cr,
                          const CurvzDocument& doc,
                          double x, double y, double side);

    // Helper: render one doc in outline/plot mode filling the page
    void render_plot(const Cairo::RefPtr<Cairo::Context>& cr,
                     const CurvzDocument& doc,
                     double page_w, double page_h);

    // Helper: render one doc in normal "show off" mode filling the page.
    // Respects layer + object visibility. Clips to artboard. currentColor
    // fills render as a checkerboard pattern, strokes as dashed grey.
    // Header strip above artwork holds user-selected metadata; optional
    // colour list footer shows the palette.
    void render_normal(const Cairo::RefPtr<Cairo::Context>& cr,
                       const CurvzDocument& doc,
                       double page_w, double page_h);

    // ── State ─────────────────────────────────────────────────────────────
    CurvzProject*  m_project  = nullptr;
    Gtk::Window*   m_parent   = nullptr;
    Style          m_style    = Style::Sheet;

    // Selected documents (indices into m_project->documents)
    std::vector<bool> m_selected;

    // Sheet options
    int    m_icons_per_row  = 4;
    int    m_cell_px        = 64;
    bool   m_show_name      = true;
    bool   m_show_path      = false;
    bool   m_show_nodes     = true;
    bool   m_show_filesize  = false;

    // Plot options
    bool   m_plot_handles   = true;
    bool   m_plot_coords    = false;
    bool   m_plot_indices   = false;

    // Normal options — header metadata toggles + footer toggles.
    // All header fields default off ("minimal page furniture"); the user
    // opts in to what they want. Footer toggles (colour list, artboard
    // border) also default off.
    bool   m_normal_hdr_filename   = false;
    bool   m_normal_hdr_dimensions = false;
    bool   m_normal_hdr_nodes      = false;
    bool   m_normal_hdr_layers     = false;
    bool   m_normal_hdr_date       = false;
    bool   m_normal_show_colors    = false;
    bool   m_normal_show_border    = false;

    // ── Widgets ───────────────────────────────────────────────────────────
    Gtk::Box          m_root{Gtk::Orientation::VERTICAL};
    Gtk::Box          m_content{Gtk::Orientation::HORIZONTAL};
    Gtk::Box          m_bottom_bar{Gtk::Orientation::HORIZONTAL};

    // Doc list
    Gtk::ScrolledWindow          m_doc_scroll;
    Gtk::Box                     m_doc_box{Gtk::Orientation::VERTICAL};
    std::vector<Gtk::CheckButton*> m_doc_checks;

    // Style toggle
    Gtk::Box          m_style_bar{Gtk::Orientation::HORIZONTAL};
    Gtk::ToggleButton m_sheet_btn{"Sheet"};
    Gtk::ToggleButton m_plot_btn {"Plot"};
    Gtk::ToggleButton m_normal_btn{"Normal"};

    // Options area (rebuilt on style change)
    Gtk::Frame        m_options_frame;
    Gtk::Box          m_options_box{Gtk::Orientation::VERTICAL};

    // Print settings (persisted across calls)
    Glib::RefPtr<Gtk::PageSetup>    m_page_setup;
    Glib::RefPtr<Gtk::PrintSettings> m_print_settings;
};

} // namespace Curvz
