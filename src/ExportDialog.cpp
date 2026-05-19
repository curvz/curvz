// Curvz::ExportDialog — unified Export dialog (File ▸ Export…).
//
// Two-tabbed dialog: Documents (per-doc SVG / PNG / refpt-coords) and
// Icon Theme (freedesktop bundle). One File ▸ Export… menu entry,
// hotkey Ctrl+Shift+T. See the header docstring for layout and history.
//
// s233 m1 — Tier 1 substrate sweep. 21 of 23 raw make_managed<Gtk::*>
// sites in this file migrated to curvz::widgets::* substrate, folding
// each abbrev into the substrate ctor. The 2 remaining raw sites are
// the per-doc in-loop CheckButtons (line 223 Documents tab targets,
// line 1130 Icon Theme tab checklist) — both Reading-C-blocked under
// the row-builder shared-abbrev pattern, addressed eventually via the
// `objects` Scriptable arc's m4+ surface or a per-doc model
// Scriptable. The two anonymous Select all / Select none buttons in
// docs_build_targets gained fresh abbrevs (`dlg_xu_d_sa` / `dlg_xu_d_sn`)
// — they're useful test hooks. Existing curvz::utils::set_name calls
// are preserved verbatim so widget_names_sync's long-name lookup
// keeps working; the substrate ctor handles the script registry side.
//
// Also flipped set_modal(true) → set_modal(false) in build(). The
// dialog being modal blocked the Scripter from listing the registry
// while the dialog is open, which is the established verification
// channel for substrate sweeps. The export operation itself is
// synchronous within the click handler, so the non-modal flip
// doesn't open any race window — the user can't interleave canvas
// edits with an export run. Brings ExportDialog into line with
// TranslateDialog / MacroManagerWindow / ScripterWindow which are
// all non-modal self-managed windows.

#include "ExportDialog.hpp"

#include "CurvzDocument.hpp"
#include "CurvzLog.hpp"
#include "CurvzProject.hpp"
#include "GResourceExporter.hpp"
#include "MainWindow.hpp"
#include "PngExporter.hpp"
#include "RefptExporter.hpp"
#include "SvgWriter.hpp"
#include "curvz_utils.hpp"

// s233 m1 — substrate wrappers for the 21-site sweep.
#include "widgets/Button.hpp"
#include "widgets/CheckButton.hpp"
#include "widgets/DropDown.hpp"
#include "widgets/Entry.hpp"
#include "widgets/SpinButton.hpp"

#include <gtkmm/adjustment.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/grid.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>
#include <giomm/file.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <glib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Curvz {

namespace {

// ── DPI presets ─────────────────────────────────────────────────────────────
// Trimmed list per S104 design conversation. "Custom" sentinel triggers
// the per-DPI numeric spin. Order is the order they appear in the dropdown.
constexpr int    kDpiPresets[]      = { 72, 96, 100, 144, 300, 600 };
constexpr int    kDpiPresetCount    = sizeof(kDpiPresets) / sizeof(kDpiPresets[0]);
constexpr int    kDpiCustomIndex    = kDpiPresetCount;  // last item
constexpr int    kDpiCustomDefault  = 100;

// ── Units dropdown order ────────────────────────────────────────────────────
constexpr const char* kUnitItems[]  = { "px", "mm", "in", "pt" };
constexpr int         kUnitCount    = 4;

// ── px conversion ───────────────────────────────────────────────────────────
double to_px(double value, const std::string& units, int dpi) {
    if (units == "px") return value;
    if (units == "in") return value * dpi;
    if (units == "mm") return value * dpi / 25.4;
    if (units == "pt") return value * dpi / 72.0;
    return value;
}

// Strip ".svg" or any extension off a filename, returning the stem.
std::string strip_ext(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return name;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos && dot < slash) return name;
    return name.substr(0, dot);
}

// ── Refpt format dropdown items ─────────────────────────────────────────────
constexpr const char* kRefptFormatItems[] = {
    "CSV (.csv)",
    "JSON (.json)",
    "G-code (.nc)",
    "DXF (.dxf)",
};
constexpr int kRefptFormatCount =
    sizeof(kRefptFormatItems) / sizeof(kRefptFormatItems[0]);

RefptExporter::Format refpt_format_for_index(unsigned idx) {
    switch (idx) {
        case 0: return RefptExporter::Format::Csv;
        case 1: return RefptExporter::Format::Json;
        case 2: return RefptExporter::Format::Gcode;
        case 3: return RefptExporter::Format::Dxf;
    }
    return RefptExporter::Format::Csv;
}

}  // namespace

// ─── ctor ───────────────────────────────────────────────────────────────────

ExportDialog::ExportDialog(Gtk::Window&  parent,
                           CurvzProject& project,
                           DoneCallback  done_cb)
    : m_parent(&parent),
      m_project(&project),
      m_done_cb(std::move(done_cb))
{
    build();
}

// ─── build ──────────────────────────────────────────────────────────────────

void ExportDialog::build() {
    m_window = Gtk::make_managed<Gtk::Window>();
    curvz::utils::set_name(m_window, "dlg_xu", "export_unified_dialog_root");
    m_window->set_title("Export");
    m_window->set_transient_for(*m_parent);
    curvz::utils::apply_motif_class_from_parent(*m_window, *m_parent);
    // s233 m1 — was set_modal(true). Flipped to non-modal alongside the
    // substrate sweep so the Scripter can list registry entries while
    // the dialog is open (modal blocks every other window, including
    // Scripter dispatch). The export operation itself is synchronous
    // within the Export-button click handler, so non-modal-while-
    // configuring doesn't open any interleaving-edits-with-export race;
    // the user can't mutate the project mid-export. Matches the
    // self-managed non-modal lifecycle TranslateDialog /
    // MacroManagerWindow / ScripterWindow already use. transient_for
    // is preserved — keeps the export window stacked above the main
    // window with the right window-manager hints.
    m_window->set_modal(false);
    m_window->set_default_size(580, 720);

    // Self-managed lifecycle. signal_close_request (not signal_hide)
    // per the s126 lifecycle banked rule: GTK4's Window::close() can
    // destroy a stacked-modal dialog without emitting signal_hide.
    m_window->signal_close_request().connect(
        [this]() {
            if (m_done_cb) {
                m_done_cb(m_export_ran, m_last_export_dir);
            }
            Glib::signal_idle().connect_once([this]() { delete this; });
            return false;  // allow close
        },
        /*after=*/false);

    auto root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    root->set_margin(16);
    m_window->set_child(*root);

    // ── Notebook with two tabs ────────────────────────────────────────────
    m_notebook = Gtk::make_managed<Gtk::Notebook>();
    curvz::utils::set_name(m_notebook, "dlg_xu_nb", "export_unified_dialog_notebook");
    m_notebook->set_vexpand(true);
    m_notebook->set_hexpand(true);
    m_notebook->set_scrollable(true);

    // Tab 1 — Documents
    m_docs_tab = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    curvz::utils::set_name(m_docs_tab, "dlg_xu_docs", "export_unified_dialog_docs_tab");
    m_docs_tab->set_margin(12);
    build_documents_tab(*m_docs_tab);
    auto docs_label = Gtk::make_managed<Gtk::Label>("Documents");
    m_notebook->append_page(*m_docs_tab, *docs_label);

    // Tab 2 — Icon Theme
    m_theme_tab = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    curvz::utils::set_name(m_theme_tab, "dlg_xu_theme", "export_unified_dialog_theme_tab");
    m_theme_tab->set_margin(12);
    build_icon_theme_tab(*m_theme_tab);
    auto theme_label = Gtk::make_managed<Gtk::Label>("Icon Theme");
    m_notebook->append_page(*m_theme_tab, *theme_label);

    root->append(*m_notebook);

    // ── Footer (Close only — Export buttons live per-tab) ─────────────────
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_footer(*root);

    // First-paint state.
    docs_update_visibility();

    m_window->present();
}

// ─── Documents tab ──────────────────────────────────────────────────────────

void ExportDialog::build_documents_tab(Gtk::Box& tab_root) {
    auto desc = Gtk::make_managed<Gtk::Label>(
        "Export selected documents from this project as SVG, PNG, or refpt "
        "coordinates. SVG/PNG outputs use the chosen size; the other "
        "dimension comes from each document's aspect ratio.");
    desc->set_xalign(0.0f);
    desc->set_wrap(true);
    desc->set_max_width_chars(60);
    desc->add_css_class("dim-label");
    tab_root.append(*desc);

    docs_build_targets(tab_root);
    tab_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    docs_build_format(tab_root);
    tab_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    docs_build_size(tab_root);
    tab_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    docs_build_refpts(tab_root);
    tab_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    docs_build_output(tab_root);
    docs_build_status_and_button(tab_root);
}

// ─── Documents: targets ─────────────────────────────────────────────────────

void ExportDialog::docs_build_targets(Gtk::Box& root) {
    auto title = Gtk::make_managed<Gtk::Label>("Documents");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    root.append(*title);

    auto scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_min_content_height(140);
    scroller->set_max_content_height(220);
    scroller->set_has_frame(true);
    scroller->set_vexpand(false);
    root.append(*scroller);

    m_docs_targets_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    m_docs_targets_box->set_margin(6);
    scroller->set_child(*m_docs_targets_box);

    const auto& docs = m_project->documents;
    const int active = m_project->active_doc_index;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        if (!docs[i]) {
            m_docs_target_checks.push_back(nullptr);
            continue;
        }
        std::string label = doc_display_name(*docs[i], i);
        if (static_cast<int>(i) == active) {
            label += "  (active)";
        }
        auto* cb = Gtk::make_managed<Gtk::CheckButton>(label);
        cb->set_active(true);
        cb->signal_toggled().connect(
            [this]() { docs_refresh_refpts_info(); });
        m_docs_targets_box->append(*cb);
        m_docs_target_checks.push_back(cb);
    }

    auto btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    // s233 m1 — registered substrate. Useful test hooks: a script that
    // wants to drive "no docs selected" can `dlg_xu_d_sn click`. Fresh
    // abbrevs in the dlg_xu_d_* neighbourhood; long-name annotation
    // tracked via set_name below.
    auto btn_all  = Gtk::make_managed<curvz::widgets::Button>(
                        "dlg_xu_d_sa", "Select all");
    auto btn_none = Gtk::make_managed<curvz::widgets::Button>(
                        "dlg_xu_d_sn", "Select none");
    curvz::utils::set_name(btn_all,  "dlg_xu_d_sa",
                           "export_unified_dialog_docs_select_all_btn");
    curvz::utils::set_name(btn_none, "dlg_xu_d_sn",
                           "export_unified_dialog_docs_select_none_btn");
    btn_all->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDialog::docs_on_select_all));
    btn_none->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDialog::docs_on_select_none));
    btn_row->append(*btn_all);
    btn_row->append(*btn_none);
    root.append(*btn_row);
}

// ─── Documents: format ──────────────────────────────────────────────────────

void ExportDialog::docs_build_format(Gtk::Box& root) {
    auto title = Gtk::make_managed<Gtk::Label>("Format");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    root.append(*title);

    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    // s233 m1 — registered substrate. Existing abbrevs preserved.
    m_docs_radio_svg   = Gtk::make_managed<curvz::widgets::CheckButton>(
                            "dlg_xu_d_rsvg", "SVG");
    m_docs_radio_png   = Gtk::make_managed<curvz::widgets::CheckButton>(
                            "dlg_xu_d_rpng", "PNG");
    m_docs_radio_refpt = Gtk::make_managed<curvz::widgets::CheckButton>(
                            "dlg_xu_d_rref", "Refpt coordinates");
    curvz::utils::set_name(m_docs_radio_svg,   "dlg_xu_d_rsvg",  "export_unified_dialog_docs_radio_svg");
    curvz::utils::set_name(m_docs_radio_png,   "dlg_xu_d_rpng",  "export_unified_dialog_docs_radio_png");
    curvz::utils::set_name(m_docs_radio_refpt, "dlg_xu_d_rref",  "export_unified_dialog_docs_radio_refpt");
    m_docs_radio_png->set_group(*m_docs_radio_svg);
    m_docs_radio_refpt->set_group(*m_docs_radio_svg);
    m_docs_radio_svg->set_active(true);

    m_docs_radio_svg->signal_toggled().connect(
        [this]() { docs_update_visibility(); });
    m_docs_radio_png->signal_toggled().connect(
        [this]() { docs_update_visibility(); });
    m_docs_radio_refpt->signal_toggled().connect(
        [this]() { docs_update_visibility(); docs_refresh_refpts_info(); });

    row->append(*m_docs_radio_svg);
    row->append(*m_docs_radio_png);
    row->append(*m_docs_radio_refpt);
    root.append(*row);
}

// ─── Documents: size ────────────────────────────────────────────────────────

void ExportDialog::docs_build_size(Gtk::Box& root) {
    m_docs_size_section =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto& host = *m_docs_size_section;

    auto title = Gtk::make_managed<Gtk::Label>("Size");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    host.append(*title);

    // Fit-to row
    {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto lbl = Gtk::make_managed<Gtk::Label>("Fit to:");
        lbl->set_xalign(0.0f);
        lbl->set_size_request(60, -1);
        row->append(*lbl);
        // s233 m1 — registered substrate. Existing abbrevs preserved.
        m_docs_radio_fit_w = Gtk::make_managed<curvz::widgets::CheckButton>(
                                "dlg_xu_d_rfw", "Width");
        m_docs_radio_fit_h = Gtk::make_managed<curvz::widgets::CheckButton>(
                                "dlg_xu_d_rfh", "Height");
        curvz::utils::set_name(m_docs_radio_fit_w, "dlg_xu_d_rfw", "export_unified_dialog_docs_radio_fit_w");
        curvz::utils::set_name(m_docs_radio_fit_h, "dlg_xu_d_rfh", "export_unified_dialog_docs_radio_fit_h");
        m_docs_radio_fit_h->set_group(*m_docs_radio_fit_w);
        m_docs_radio_fit_w->set_active(true);
        row->append(*m_docs_radio_fit_w);
        row->append(*m_docs_radio_fit_h);
        host.append(*row);
    }

    // Value + units row
    {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto lbl = Gtk::make_managed<Gtk::Label>("Value:");
        lbl->set_xalign(0.0f);
        lbl->set_size_request(60, -1);
        row->append(*lbl);

        // Default: active document's canvas width in px. Falls back to 256
        // when there's no active doc.
        double default_value = 256.0;
        if (m_project) {
            int idx = m_project->active_doc_index;
            if (idx >= 0 && idx < (int)m_project->documents.size()
                && m_project->documents[idx]) {
                int cw = m_project->documents[idx]->canvas_width();
                if (cw > 0) default_value = (double)cw;
            }
        }
        auto adj = Gtk::Adjustment::create(default_value, 0.01, 10000.0, 1.0, 10.0, 0.0);
        // s233 m1 — registered substrate (s189 m2 Adjustment-taking form).
        m_docs_value_spin = Gtk::make_managed<curvz::widgets::SpinButton>(
                                "dlg_xu_d_val", adj, 1.0, 2);
        curvz::utils::set_name(m_docs_value_spin, "dlg_xu_d_val", "export_unified_dialog_docs_value_spn");
        m_docs_value_spin->set_width_chars(8);
        row->append(*m_docs_value_spin);

        // Units dropdown
        std::vector<Glib::ustring> unit_items;
        for (int i = 0; i < kUnitCount; ++i) unit_items.emplace_back(kUnitItems[i]);
        auto unit_list = Gtk::StringList::create(unit_items);
        // s233 m1 — registered substrate (s189 m1 StringList-taking form).
        m_docs_units_drop = Gtk::make_managed<curvz::widgets::DropDown>(
                                "dlg_xu_d_unt", unit_list);
        curvz::utils::set_name(m_docs_units_drop, "dlg_xu_d_unt", "export_unified_dialog_docs_units_dd");
        m_docs_units_drop->set_selected(0);  // px
        m_docs_units_drop->property_selected().signal_changed().connect(
            [this]() { docs_update_visibility(); });
        row->append(*m_docs_units_drop);
        host.append(*row);
    }

    // DPI row — hidden when units == px.
    {
        m_docs_dpi_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        m_docs_dpi_label = Gtk::make_managed<Gtk::Label>("DPI:");
        m_docs_dpi_label->set_xalign(0.0f);
        m_docs_dpi_label->set_size_request(60, -1);
        m_docs_dpi_row->append(*m_docs_dpi_label);

        std::vector<Glib::ustring> dpi_items;
        for (int i = 0; i < kDpiPresetCount; ++i) {
            dpi_items.push_back(Glib::ustring::format(kDpiPresets[i]));
        }
        dpi_items.emplace_back("Custom…");
        auto dpi_list = Gtk::StringList::create(dpi_items);
        // s233 m1 — registered substrate (s189 m1 StringList-taking form).
        m_docs_dpi_drop = Gtk::make_managed<curvz::widgets::DropDown>(
                              "dlg_xu_d_dpi", dpi_list);
        curvz::utils::set_name(m_docs_dpi_drop, "dlg_xu_d_dpi", "export_unified_dialog_docs_dpi_dd");
        // Default to 96 (web/screen standard).
        for (int i = 0; i < kDpiPresetCount; ++i) {
            if (kDpiPresets[i] == 96) {
                m_docs_dpi_drop->set_selected(i);
                break;
            }
        }
        m_docs_dpi_drop->property_selected().signal_changed().connect(
            [this]() { docs_update_visibility(); });
        m_docs_dpi_row->append(*m_docs_dpi_drop);

        // Custom DPI spin: 1..2400.
        auto adj = Gtk::Adjustment::create(kDpiCustomDefault, 1.0, 2400.0, 1.0, 10.0, 0.0);
        // s233 m1 — registered substrate (s189 m2 Adjustment-taking form).
        m_docs_dpi_custom = Gtk::make_managed<curvz::widgets::SpinButton>(
                                "dlg_xu_d_dpc", adj, 1.0, 0);
        curvz::utils::set_name(m_docs_dpi_custom, "dlg_xu_d_dpc", "export_unified_dialog_docs_dpi_custom_spn");
        m_docs_dpi_custom->set_width_chars(6);
        m_docs_dpi_custom->set_visible(false);
        m_docs_dpi_row->append(*m_docs_dpi_custom);

        host.append(*m_docs_dpi_row);
    }

    root.append(host);
}

// ─── Documents: refpts ──────────────────────────────────────────────────────

void ExportDialog::docs_build_refpts(Gtk::Box& root) {
    m_docs_refpts_section =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    auto& host = *m_docs_refpts_section;

    // Format row.
    m_docs_refpts_format_row =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto fmt_label = Gtk::make_managed<Gtk::Label>("Format:");
    fmt_label->set_xalign(0.0f);
    fmt_label->set_size_request(60, -1);
    m_docs_refpts_format_row->append(*fmt_label);

    std::vector<Glib::ustring> fmt_items;
    for (int i = 0; i < kRefptFormatCount; ++i)
        fmt_items.emplace_back(kRefptFormatItems[i]);
    auto fmt_list = Gtk::StringList::create(fmt_items);
    // s233 m1 — registered substrate (s189 m1 StringList-taking form).
    m_docs_refpts_format_drop = Gtk::make_managed<curvz::widgets::DropDown>(
                                    "dlg_xu_d_rpf", fmt_list);
    curvz::utils::set_name(m_docs_refpts_format_drop, "dlg_xu_d_rpf",
                           "export_unified_dialog_docs_refpts_format_dd");
    m_docs_refpts_format_drop->set_selected(0);
    m_docs_refpts_format_row->append(*m_docs_refpts_format_drop);
    host.append(*m_docs_refpts_format_row);

    // Info label — live count summary.
    m_docs_refpts_info_label = Gtk::make_managed<Gtk::Label>("");
    curvz::utils::set_name(m_docs_refpts_info_label, "dlg_xu_d_rpi",
                           "export_unified_dialog_docs_refpts_info_lbl");
    m_docs_refpts_info_label->set_xalign(0.0f);
    m_docs_refpts_info_label->set_wrap(true);
    m_docs_refpts_info_label->set_max_width_chars(60);
    m_docs_refpts_info_label->add_css_class("dim-label");
    host.append(*m_docs_refpts_info_label);

    root.append(host);

    docs_refresh_refpts_info();
}

void ExportDialog::docs_refresh_refpts_info() {
    if (!m_docs_refpts_info_label) return;

    if (!m_project) {
        m_docs_refpts_info_label->set_text("Refpts: no project.");
        return;
    }

    std::size_t total_refpts     = 0;
    std::size_t docs_with_refpts = 0;
    std::size_t docs_checked     = 0;
    std::ostringstream origins_line;

    const auto& docs = m_project->documents;
    for (std::size_t i = 0; i < docs.size() && i < m_docs_target_checks.size();
         ++i) {
        auto* cb = m_docs_target_checks[i];
        if (!cb || !cb->get_active()) continue;
        if (!docs[i]) continue;
        ++docs_checked;

        const auto s = RefptExporter::summarize(*docs[i]);
        total_refpts += s.refpt_count;
        if (s.refpt_count > 0) {
            ++docs_with_refpts;
            if (origins_line.tellp() > 0) origins_line << ", ";
            const std::string disp = doc_display_name(*docs[i], i);
            origins_line << "\"" << disp << "\": " << s.refpt_count;
        }
    }

    std::ostringstream summary;
    if (docs_checked == 0) {
        summary << "Refpts: no documents checked.";
    } else if (total_refpts == 0) {
        summary << "Refpts: 0 across " << docs_checked
                << (docs_checked == 1 ? " document" : " documents")
                << " — header-only files will be written.";
    } else {
        summary << "Refpts: " << total_refpts << " across "
                << docs_with_refpts << " of " << docs_checked
                << (docs_checked == 1 ? " document" : " documents")
                << ".  " << origins_line.str() << ".";
    }
    m_docs_refpts_info_label->set_text(summary.str());
}

// ─── Documents: output folder ───────────────────────────────────────────────

void ExportDialog::docs_build_output(Gtk::Box& root) {
    auto title = Gtk::make_managed<Gtk::Label>("Output folder");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    root.append(*title);

    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    // s233 m1 — registered substrate.
    m_docs_folder_entry = Gtk::make_managed<curvz::widgets::Entry>("dlg_xu_d_fld");
    curvz::utils::set_name(m_docs_folder_entry, "dlg_xu_d_fld", "export_unified_dialog_docs_folder_entry");
    m_docs_folder_entry->set_hexpand(true);
    m_docs_folder_entry->set_placeholder_text("Choose a folder…");

    // Pre-fill priority. Last-used folder ("export-docs" key) beats
    // project directory beats nothing.
    namespace fs = std::filesystem;
    std::string prefill;
    if (auto* mw = main_window()) {
        std::string remembered = mw->get_last_folder("export-docs");
        std::error_code ec;
        if (!remembered.empty() && fs::is_directory(remembered, ec)) {
            prefill = remembered;
        }
    }
    if (prefill.empty() && m_project && !m_project->directory.empty()) {
        prefill = m_project->directory;
    }
    if (!prefill.empty()) {
        m_docs_folder_entry->set_text(prefill);
    }

    // s233 m1 — registered substrate.
    m_docs_btn_browse = Gtk::make_managed<curvz::widgets::Button>(
                            "dlg_xu_d_brw", "Browse…");
    curvz::utils::set_name(m_docs_btn_browse, "dlg_xu_d_brw", "export_unified_dialog_docs_browse_btn");
    m_docs_btn_browse->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDialog::docs_on_browse));

    row->append(*m_docs_folder_entry);
    row->append(*m_docs_btn_browse);
    root.append(*row);
}

// ─── Documents: status + per-tab Export button ──────────────────────────────

void ExportDialog::docs_build_status_and_button(Gtk::Box& root) {
    // Status — reserved row that's always present so the tab doesn't
    // re-layout when an error/success message appears.
    m_docs_status = Gtk::make_managed<Gtk::Label>("");
    curvz::utils::set_name(m_docs_status, "dlg_xu_d_sts", "export_unified_dialog_docs_status_lbl");
    m_docs_status->set_xalign(0.0f);
    m_docs_status->set_wrap(true);
    m_docs_status->add_css_class("dim-label");
    root.append(*m_docs_status);

    // Per-tab Export button (no Cancel — dialog Close serves that role).
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_halign(Gtk::Align::END);
    row->set_margin_top(8);

    // s233 m1 — registered substrate.
    m_docs_btn_export = Gtk::make_managed<curvz::widgets::Button>(
                            "dlg_xu_d_exp", "Export");
    curvz::utils::set_name(m_docs_btn_export, "dlg_xu_d_exp", "export_unified_dialog_docs_export_btn");
    m_docs_btn_export->add_css_class("suggested-action");
    m_docs_btn_export->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDialog::docs_on_export));

    row->append(*m_docs_btn_export);
    root.append(*row);
}

// ─── Documents: visibility wiring ───────────────────────────────────────────

void ExportDialog::docs_update_visibility() {
    const bool refpt_mode =
        m_docs_radio_refpt && m_docs_radio_refpt->get_active();
    if (m_docs_size_section)   m_docs_size_section->set_visible(!refpt_mode);
    if (m_docs_refpts_section) m_docs_refpts_section->set_visible(refpt_mode);

    const bool need_dpi = (docs_current_units() != "px");
    if (m_docs_dpi_row) m_docs_dpi_row->set_visible(need_dpi);

    const bool custom = need_dpi && m_docs_dpi_drop &&
                        (static_cast<int>(m_docs_dpi_drop->get_selected()) == kDpiCustomIndex);
    if (m_docs_dpi_custom) m_docs_dpi_custom->set_visible(custom);

    if (m_docs_status) m_docs_status->set_text("");
}

std::string ExportDialog::docs_current_units() const {
    if (!m_docs_units_drop) return "px";
    auto sel = static_cast<int>(m_docs_units_drop->get_selected());
    if (sel < 0 || sel >= kUnitCount) return "px";
    return kUnitItems[sel];
}

int ExportDialog::docs_current_dpi() const {
    if (!m_docs_dpi_drop) return kDpiCustomDefault;
    auto sel = static_cast<int>(m_docs_dpi_drop->get_selected());
    if (sel >= 0 && sel < kDpiPresetCount) return kDpiPresets[sel];
    if (sel == kDpiCustomIndex && m_docs_dpi_custom) {
        return std::max(1, m_docs_dpi_custom->get_value_as_int());
    }
    return kDpiCustomDefault;
}

int ExportDialog::docs_chosen_dim_px() const {
    if (!m_docs_value_spin) return 256;
    double v = m_docs_value_spin->get_value();
    if (v <= 0.0) return 1;
    double px = to_px(v, docs_current_units(), docs_current_dpi());
    int rounded = static_cast<int>(std::round(px));
    return std::max(1, rounded);
}

// ─── Documents: select all/none ─────────────────────────────────────────────

void ExportDialog::docs_on_select_all() {
    for (auto* cb : m_docs_target_checks) {
        if (cb) cb->set_active(true);
    }
}

void ExportDialog::docs_on_select_none() {
    for (auto* cb : m_docs_target_checks) {
        if (cb) cb->set_active(false);
    }
}

// ─── Documents: browse ──────────────────────────────────────────────────────

void ExportDialog::docs_on_browse() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Choose Output Folder");

    std::string current = m_docs_folder_entry
        ? std::string(m_docs_folder_entry->get_text()) : std::string();
    if (!current.empty()) {
        try {
            auto init = Gio::File::create_for_path(current);
            dialog->set_initial_folder(init);
        } catch (...) {}
    }

    dialog->select_folder(*m_window,
        [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto folder = dialog->select_folder_finish(result);
                if (folder && m_docs_folder_entry) {
                    std::string chosen = folder->get_path();
                    m_docs_folder_entry->set_text(chosen);
                    if (auto* mw = main_window()) {
                        mw->set_last_folder("export-docs", chosen);
                    }
                }
            } catch (...) {
                // Cancelled — leave entry alone.
            }
        });
}

// ─── Documents: export ──────────────────────────────────────────────────────

void ExportDialog::docs_on_export() {
    if (!m_project || !m_docs_status) return;

    // Validate output dir.
    std::string out_dir = m_docs_folder_entry
        ? std::string(m_docs_folder_entry->get_text()) : std::string();
    if (out_dir.empty()) {
        m_docs_status->set_text("Please choose an output folder.");
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(out_dir, ec) || !fs::is_directory(out_dir, ec)) {
        if (!fs::create_directories(out_dir, ec)) {
            m_docs_status->set_text("Output folder is not a valid directory.");
            return;
        }
    }

    // Validate selected docs.
    std::vector<int> selected_indices;
    for (std::size_t i = 0; i < m_docs_target_checks.size(); ++i) {
        if (m_docs_target_checks[i] && m_docs_target_checks[i]->get_active()) {
            selected_indices.push_back(static_cast<int>(i));
        }
    }
    if (selected_indices.empty()) {
        m_docs_status->set_text("Select at least one document to export.");
        return;
    }

    // Three-way format radio decides the output extension.
    const bool want_svg   = m_docs_radio_svg   && m_docs_radio_svg->get_active();
    const bool want_png   = m_docs_radio_png   && m_docs_radio_png->get_active();
    const bool want_refpt = m_docs_radio_refpt && m_docs_radio_refpt->get_active();
    std::string ext;
    if (want_svg) {
        ext = ".svg";
    } else if (want_png) {
        ext = ".png";
    } else /* want_refpt */ {
        const auto rfmt = m_docs_refpts_format_drop
            ? refpt_format_for_index(m_docs_refpts_format_drop->get_selected())
            : RefptExporter::Format::Csv;
        ext = std::string(".") + RefptExporter::extension(rfmt);
    }

    LOG_INFO("ExportDialog docs_on_export "
             "radio_svg.active={} radio_png.active={} radio_refpt.active={} "
             "→ ext={}",
             want_svg, want_png, want_refpt, ext);

    // Resolve per-doc target paths.
    std::vector<DocTarget> targets;
    targets.reserve(selected_indices.size());
    for (int idx : selected_indices) {
        if (idx < 0 || idx >= (int)m_project->documents.size()) continue;
        CurvzDocument* doc = m_project->documents[idx].get();
        if (!doc) continue;
        std::string stem = strip_ext(doc->filename);
        if (stem.empty()) stem = "Document_" + std::to_string(idx + 1);
        DocTarget t;
        t.doc       = doc;
        t.doc_index = idx;
        t.path      = (fs::path(out_dir) / (stem + ext)).string();
        targets.push_back(std::move(t));
    }
    if (targets.empty()) {
        m_docs_status->set_text("No valid documents selected.");
        return;
    }

    // Collision scan.
    std::vector<std::string> collisions;
    for (const auto& t : targets) {
        bool exists = fs::exists(t.path, ec);
        LOG_INFO("ExportDialog docs scan: target='{}' exists={}",
                 t.path, exists);
        if (exists) collisions.push_back(t.path);
    }

    if (collisions.empty()) {
        docs_perform_export(targets);
        return;
    }

    // Build a confirm prompt.
    std::ostringstream detail;
    if (collisions.size() == 1) {
        fs::path p(collisions.front());
        detail << "\"" << p.filename().string()
               << "\" already exists in this folder.\n\n"
               << "Replace it?";
    } else {
        detail << collisions.size()
               << " files already exist in this folder.\n\n";
        const std::size_t sample = std::min<std::size_t>(collisions.size(), 5);
        for (std::size_t i = 0; i < sample; ++i) {
            fs::path p(collisions[i]);
            detail << "  • " << p.filename().string() << "\n";
        }
        if (collisions.size() > sample) {
            detail << "  …and " << (collisions.size() - sample) << " more\n";
        }
        detail << "\nReplace them all?";
    }

    LOG_INFO("ExportDialog docs: scheduling Replace confirm "
             "({} collisions, {} targets)",
             collisions.size(), targets.size());
    Glib::signal_idle().connect_once(
        [this, detail_str = detail.str(),
         targets = std::move(targets)]() mutable {
            LOG_INFO("ExportDialog docs: presenting Replace confirm");
            curvz::utils::show_confirm(
                *m_window,
                "Replace files?",
                detail_str,
                {"Cancel", "Replace"},
                /*default_button=*/1,
                /*cancel_button=*/0,
                [this, targets = std::move(targets)](int choice) {
                    LOG_INFO("ExportDialog docs confirm-cb: "
                             "choice={} targets={}",
                             choice, targets.size());
                    if (choice == 1) {
                        docs_perform_export(targets);
                    } else {
                        if (m_docs_status) m_docs_status->set_text(
                            "Cancelled. Choose a different folder or "
                            "rename existing files.");
                    }
                });
        });
}

// ─── Documents: perform_export ──────────────────────────────────────────────

void ExportDialog::docs_perform_export(const std::vector<DocTarget>& targets) {
    LOG_INFO("ExportDialog docs_perform_export entry: "
             "targets={} project={} status={}",
             targets.size(), (void*)m_project, (void*)m_docs_status);
    if (!m_project || !m_docs_status || targets.empty()) {
        LOG_WARN("ExportDialog docs_perform_export early-return: "
                 "project_null={} status_null={} targets_empty={}",
                 m_project == nullptr, m_docs_status == nullptr,
                 targets.empty());
        return;
    }

    namespace fs = std::filesystem;

    // Re-read sizing controls — confirm dialog yields the main loop, so
    // settings could have changed between Export click and Replace click.
    const bool   want_svg   = m_docs_radio_svg   && m_docs_radio_svg->get_active();
    const bool   want_png   = m_docs_radio_png   && m_docs_radio_png->get_active();
    const bool   want_refpt = m_docs_radio_refpt && m_docs_radio_refpt->get_active();
    const bool   fit_width  = m_docs_radio_fit_w && m_docs_radio_fit_w->get_active();
    const std::string units = docs_current_units();
    const double size_value = m_docs_value_spin
        ? m_docs_value_spin->get_value() : 256.0;
    const int    chosen_px  = docs_chosen_dim_px();

    const auto refpts_fmt = m_docs_refpts_format_drop
        ? refpt_format_for_index(m_docs_refpts_format_drop->get_selected())
        : RefptExporter::Format::Csv;

    int written = 0;
    int failed  = 0;
    std::vector<std::string> output_files;
    output_files.reserve(targets.size());

    for (const auto& t : targets) {
        if (!t.doc) continue;

        // Pre-write snapshot.
        std::error_code pec;
        bool pre_exists = fs::exists(t.path, pec);
        std::uintmax_t pre_size = 0;
        fs::file_time_type pre_mtime{};
        if (pre_exists) {
            pre_size  = fs::file_size(t.path, pec);
            pre_mtime = fs::last_write_time(t.path, pec);
        }

        // Pre-emptive remove if file exists (the user has already consented
        // by clicking Replace).
        if (pre_exists) {
            std::error_code rec;
            fs::remove(t.path, rec);
            if (rec) {
                LOG_ERROR("ExportDialog docs: pre-write remove "
                          "failed for '{}': {} — skipping",
                          t.path, rec.message());
                ++failed;
                continue;
            }
        }

        bool ok = false;
        if (want_svg) {
            const std::string fit_side = fit_width ? "width" : "height";
            ok = write_svg_file_with_export_meta(
                *t.doc, t.path, units, size_value, fit_side);
        } else if (want_png) {
            const int cw = t.doc->canvas_width();
            const int ch = t.doc->canvas_height();
            int out_w = 1, out_h = 1;
            if (fit_width) {
                out_w = chosen_px;
                out_h = std::max(1, (int)std::round(
                    chosen_px * (double)ch / (double)cw));
            } else {
                out_h = chosen_px;
                out_w = std::max(1, (int)std::round(
                    chosen_px * (double)cw / (double)ch));
            }
            ok = export_png_sized(*t.doc, t.path, out_w, out_h);
        } else /* want_refpt */ {
            const std::size_t n = RefptExporter::count_exportable(*t.doc);
            std::string body =
                RefptExporter::export_refpts(*t.doc, refpts_fmt);
            std::ofstream out(t.path, std::ios::binary);
            if (out.good()) {
                out << body;
                out.close();
                ok = out.good();
                if (ok) {
                    LOG_INFO("ExportDialog docs refpts: wrote "
                             "'{}' ({} refpts, {} bytes)",
                             t.path, n, body.size());
                } else {
                    LOG_ERROR("ExportDialog docs refpts: "
                              "close/flush failed for '{}'", t.path);
                }
            } else {
                LOG_ERROR("ExportDialog docs refpts: open failed "
                          "for '{}'", t.path);
            }
        }
        if (ok) { ++written; output_files.push_back(t.path); }
        else    { ++failed; }

        // Post-write snapshot.
        std::error_code qec;
        bool post_exists = fs::exists(t.path, qec);
        std::uintmax_t post_size = post_exists ? fs::file_size(t.path, qec) : 0;
        fs::file_time_type post_mtime{};
        if (post_exists) post_mtime = fs::last_write_time(t.path, qec);
        const bool size_changed  = (pre_size  != post_size);
        const bool mtime_changed = (pre_mtime != post_mtime);
        const char* fmt_label = want_svg ? "svg"
                              : want_png ? "png" : "refpts";
        LOG_INFO("ExportDialog docs write: path='{}' fmt={} "
                 "writer_ok={} pre={{exists={} size={}}} "
                 "post={{exists={} size={}}} size_changed={} mtime_changed={}",
                 t.path, fmt_label, ok,
                 pre_exists, pre_size, post_exists, post_size,
                 size_changed, mtime_changed);
    }

    // Roll-up: never demote a previously-successful export. The
    // unified parent shares m_export_ran across both tabs, so a docs
    // run with 0 writes after a successful theme run shouldn't reset
    // the flag.
    if (written > 0) m_export_ran = true;

    const std::string out_dir = m_docs_folder_entry
        ? std::string(m_docs_folder_entry->get_text()) : std::string();

    if (written > 0 && !out_dir.empty()) {
        m_last_export_dir = out_dir;
        if (auto* mw = main_window()) {
            mw->set_last_folder("export-docs", out_dir);
        }
    }

    if (failed == 0) {
        LOG_INFO("ExportDialog docs: wrote {} files to '{}'",
                 written, out_dir);
        if (m_window) m_window->close();
    } else if (written == 0) {
        m_docs_status->set_text(
            "Export failed. Check the output folder permissions and try again.");
        LOG_ERROR("ExportDialog docs: 0 written, {} failed", failed);
    } else {
        std::ostringstream s;
        s << "Wrote " << written << " of " << (written + failed) << " files. "
          << failed << " failed (see log).";
        m_docs_status->set_text(s.str());
        LOG_WARN("ExportDialog docs: partial — {} written, {} failed",
                 written, failed);
    }
}

// ─── Icon Theme tab ─────────────────────────────────────────────────────────
//
// Builds the freedesktop-icon-theme bundle exporter: theme metadata
// (name + comment), per-doc include checklist with category badges,
// output folder, progress bar, status label, Export Theme button.
// The synchronous theme write runs inside the event loop with manual
// pumping for progress visibility; theme_set_busy disables the dialog-
// level Close button + tab-switching while it runs, since `delete this`
// from a user-driven close mid-loop would be unsafe.
//
// theme_populate runs once at build time to fill the checklist and
// pick defaults; this dialog is one-shot per construction.
//
// Last-folder memory keyed "export-theme" (parallel to "export-docs").

void ExportDialog::build_icon_theme_tab(Gtk::Box& tab_root) {
    theme_build_metadata(tab_root);
    tab_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    theme_build_checklist(tab_root);
    tab_root.append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    theme_build_output(tab_root);
    theme_build_progress(tab_root);
    theme_build_status_and_button(tab_root);

    // Fill the checklist and pick defaults from the project.
    theme_populate();
}

// ─── Icon Theme: metadata ───────────────────────────────────────────────────

void ExportDialog::theme_build_metadata(Gtk::Box& tab_root) {
    auto meta_lbl = Gtk::make_managed<Gtk::Label>();
    meta_lbl->set_markup("<b>Theme</b>");
    meta_lbl->set_xalign(0.0f);
    tab_root.append(*meta_lbl);

    m_theme_meta_grid = Gtk::make_managed<Gtk::Grid>();
    m_theme_meta_grid->set_row_spacing(6);
    m_theme_meta_grid->set_column_spacing(10);

    auto name_lbl = Gtk::make_managed<Gtk::Label>("Name");
    name_lbl->set_xalign(1.0f);
    m_theme_meta_grid->attach(*name_lbl, 0, 0);

    // s233 m1 — registered substrate.
    m_theme_name_entry = Gtk::make_managed<curvz::widgets::Entry>("dlg_xu_t_nm");
    m_theme_name_entry->set_placeholder_text("MyApp");
    m_theme_name_entry->set_hexpand(true);
    curvz::utils::set_name(m_theme_name_entry, "dlg_xu_t_nm",
                           "export_unified_dialog_theme_name_entry");
    m_theme_meta_grid->attach(*m_theme_name_entry, 1, 0);

    auto comment_lbl = Gtk::make_managed<Gtk::Label>("Comment");
    comment_lbl->set_xalign(1.0f);
    m_theme_meta_grid->attach(*comment_lbl, 0, 1);

    // s233 m1 — registered substrate.
    m_theme_comment_entry = Gtk::make_managed<curvz::widgets::Entry>("dlg_xu_t_cmt");
    m_theme_comment_entry->set_placeholder_text("Icons for MyApp");
    m_theme_comment_entry->set_hexpand(true);
    curvz::utils::set_name(m_theme_comment_entry, "dlg_xu_t_cmt",
                           "export_unified_dialog_theme_comment_entry");
    m_theme_meta_grid->attach(*m_theme_comment_entry, 1, 1);

    tab_root.append(*m_theme_meta_grid);
}

// ─── Icon Theme: checklist ──────────────────────────────────────────────────

void ExportDialog::theme_build_checklist(Gtk::Box& tab_root) {
    auto icons_lbl = Gtk::make_managed<Gtk::Label>();
    icons_lbl->set_markup("<b>Icons to export</b>");
    icons_lbl->set_xalign(0.0f);
    tab_root.append(*icons_lbl);

    auto scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_min_content_height(180);
    scroller->set_max_content_height(280);
    scroller->set_vexpand(true);

    m_theme_list_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    scroller->set_child(*m_theme_list_box);
    tab_root.append(*scroller);

    // Warning label (shown when some docs lack name/category).
    m_theme_warn_label = Gtk::make_managed<Gtk::Label>();
    m_theme_warn_label->set_xalign(0.0f);
    m_theme_warn_label->add_css_class("dim-label");
    m_theme_warn_label->set_wrap(true);
    m_theme_warn_label->set_visible(false);
    curvz::utils::set_name(m_theme_warn_label, "dlg_xu_t_warn",
                           "export_unified_dialog_theme_warn_lbl");
    tab_root.append(*m_theme_warn_label);
}

// ─── Icon Theme: output folder ──────────────────────────────────────────────

void ExportDialog::theme_build_output(Gtk::Box& tab_root) {
    auto folder_lbl = Gtk::make_managed<Gtk::Label>();
    folder_lbl->set_markup("<b>Output folder</b>");
    folder_lbl->set_xalign(0.0f);
    tab_root.append(*folder_lbl);

    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);

    // s233 m1 — registered substrate.
    m_theme_folder_entry = Gtk::make_managed<curvz::widgets::Entry>("dlg_xu_t_fld");
    m_theme_folder_entry->set_placeholder_text("Choose a folder…");
    m_theme_folder_entry->set_hexpand(true);
    curvz::utils::set_name(m_theme_folder_entry, "dlg_xu_t_fld",
                           "export_unified_dialog_theme_folder_entry");

    // s233 m1 — registered substrate.
    m_theme_btn_browse = Gtk::make_managed<curvz::widgets::Button>(
                             "dlg_xu_t_brw", "Browse…");
    curvz::utils::set_name(m_theme_btn_browse, "dlg_xu_t_brw",
                           "export_unified_dialog_theme_browse_btn");
    m_theme_btn_browse->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDialog::theme_on_browse));

    row->append(*m_theme_folder_entry);
    row->append(*m_theme_btn_browse);
    tab_root.append(*row);
}

// ─── Icon Theme: progress + status ──────────────────────────────────────────

void ExportDialog::theme_build_progress(Gtk::Box& tab_root) {
    m_theme_progress = Gtk::make_managed<Gtk::ProgressBar>();
    m_theme_progress->set_visible(false);
    curvz::utils::set_name(m_theme_progress, "dlg_xu_t_prg",
                           "export_unified_dialog_theme_progress_bar");
    tab_root.append(*m_theme_progress);

    m_theme_status_label = Gtk::make_managed<Gtk::Label>();
    m_theme_status_label->set_xalign(0.0f);
    m_theme_status_label->set_visible(false);
    m_theme_status_label->set_wrap(true);
    curvz::utils::set_name(m_theme_status_label, "dlg_xu_t_sts",
                           "export_unified_dialog_theme_status_lbl");
    tab_root.append(*m_theme_status_label);
}

// ─── Icon Theme: per-tab Export button ──────────────────────────────────────

void ExportDialog::theme_build_status_and_button(Gtk::Box& tab_root) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_halign(Gtk::Align::END);
    row->set_margin_top(8);

    // s233 m1 — registered substrate.
    m_theme_btn_export = Gtk::make_managed<curvz::widgets::Button>(
                             "dlg_xu_t_exp", "Export Theme…");
    curvz::utils::set_name(m_theme_btn_export, "dlg_xu_t_exp",
                           "export_unified_dialog_theme_export_btn");
    m_theme_btn_export->add_css_class("suggested-action");
    m_theme_btn_export->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDialog::theme_on_export));

    row->append(*m_theme_btn_export);
    tab_root.append(*row);
}

// ─── Icon Theme: populate from project ──────────────────────────────────────

void ExportDialog::theme_populate() {
    if (!m_project) return;

    namespace fs = std::filesystem;

    // Default output folder. Last-folder memory ("export-theme") beats
    // project_dir/export beats nothing. New behavior vs legacy — see
    // the section docstring.
    std::string prefill;
    if (auto* mw = main_window()) {
        std::string remembered = mw->get_last_folder("export-theme");
        std::error_code ec;
        if (!remembered.empty() && fs::is_directory(remembered, ec)) {
            prefill = remembered;
        }
    }
    if (prefill.empty() && !m_project->directory.empty()) {
        prefill = m_project->directory + "/export";
    }
    if (m_theme_folder_entry && !prefill.empty()) {
        m_theme_folder_entry->set_text(prefill);
    }

    // Default theme name from project directory name.
    if (m_theme_name_entry && !m_project->directory.empty()) {
        std::string proj_name = fs::path(m_project->directory).filename().string();
        if (m_theme_name_entry->get_text().empty()) {
            m_theme_name_entry->set_text(proj_name);
        }
    }

    // Build checklist rows.
    int invalid_count = 0;
    for (auto& doc_ptr : m_project->documents) {
        CurvzDocument* doc = doc_ptr.get();
        if (!doc) continue;
        bool valid = !doc->export_name.empty() && !doc->export_category.empty();
        if (!valid) ++invalid_count;

        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        row->set_spacing(8);
        row->set_margin_start(4);

        auto* check = Gtk::make_managed<Gtk::CheckButton>();
        check->set_active(valid);
        check->set_sensitive(valid);
        row->append(*check);

        std::string display_name = doc->export_name.empty()
            ? fs::path(doc->filename).stem().string()
            : doc->export_name;
        auto* name_lbl = Gtk::make_managed<Gtk::Label>(display_name);
        name_lbl->set_xalign(0.0f);
        name_lbl->set_hexpand(true);
        if (!valid) name_lbl->add_css_class("dim-label");
        row->append(*name_lbl);

        std::string badge_text = doc->export_category.empty()
            ? "⚠ no category"
            : doc->export_category;
        auto* badge = Gtk::make_managed<Gtk::Label>(badge_text);
        badge->set_xalign(1.0f);
        if (!valid) badge->add_css_class("dim-label");
        else        badge->add_css_class("caption");
        row->append(*badge);

        if (m_theme_list_box) m_theme_list_box->append(*row);

        ThemeDocRow dr;
        dr.doc   = doc;
        dr.check = check;
        dr.valid = valid;
        m_theme_rows.push_back(dr);
    }

    // Warning summary if some docs lack metadata.
    if (m_theme_warn_label) {
        if (invalid_count > 0) {
            m_theme_warn_label->set_text(
                std::to_string(invalid_count) +
                " icon(s) have no name or category set and will be skipped. "
                "Assign them in the Document inspector.");
            m_theme_warn_label->set_visible(true);
        } else {
            m_theme_warn_label->set_visible(false);
        }
    }
}

// ─── Icon Theme: browse ─────────────────────────────────────────────────────

void ExportDialog::theme_on_browse() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Choose Export Folder");

    std::string current = m_theme_folder_entry
        ? std::string(m_theme_folder_entry->get_text()) : std::string();
    if (!current.empty()) {
        try {
            auto init = Gio::File::create_for_path(current);
            dialog->set_initial_folder(init);
        } catch (...) {}
    }

    dialog->select_folder(*m_window,
        [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto folder = dialog->select_folder_finish(result);
                if (folder && m_theme_folder_entry) {
                    std::string chosen = folder->get_path();
                    m_theme_folder_entry->set_text(chosen);
                    if (auto* mw = main_window()) {
                        mw->set_last_folder("export-theme", chosen);
                    }
                }
            } catch (...) {
                // Cancelled — leave entry alone.
            }
        });
}

// ─── Icon Theme: export ─────────────────────────────────────────────────────

void ExportDialog::theme_on_export() {
    if (!m_project) return;

    std::string out_dir       = m_theme_folder_entry
        ? std::string(m_theme_folder_entry->get_text()) : std::string();
    std::string theme_name    = m_theme_name_entry
        ? std::string(m_theme_name_entry->get_text()) : std::string();
    std::string theme_comment = m_theme_comment_entry
        ? std::string(m_theme_comment_entry->get_text()) : std::string();

    if (out_dir.empty()) {
        if (m_theme_status_label) {
            m_theme_status_label->set_text("Please choose an output folder.");
            m_theme_status_label->set_visible(true);
        }
        return;
    }
    if (theme_name.empty()) theme_name = "MyIcons";

    // Build entry list from checked rows.
    std::vector<ExportEntry> entries;
    entries.reserve(m_theme_rows.size());
    for (const auto& row : m_theme_rows) {
        ExportEntry e;
        e.doc     = row.doc;
        e.include = row.valid && row.check && row.check->get_active();
        entries.push_back(e);
    }

    theme_set_busy(true);
    if (m_theme_progress) {
        m_theme_progress->set_fraction(0.0);
        m_theme_progress->set_visible(true);
    }
    if (m_theme_status_label) m_theme_status_label->set_visible(false);

    // Synchronous export with main-loop pumping for progress visibility.
    // Same shape as legacy ExportDialog::on_export.
    LOG_INFO("ExportDialog theme: starting export — "
             "name='{}' comment='{}' out='{}' entries={}",
             theme_name, theme_comment, out_dir, entries.size());

    ExportResult result = export_theme(
        entries, out_dir, theme_name, theme_comment,
        [this](int current, int total, const std::string& /*name*/) {
            if (total > 0 && m_theme_progress) {
                m_theme_progress->set_fraction((double)current / (double)total);
            }
            while (g_main_context_pending(nullptr))
                g_main_context_iteration(nullptr, FALSE);
        });

    if (result.success) {
        m_export_ran = true;
        m_last_export_dir = result.output_dir;
        // Persist the folder choice for next time. Mirrors the docs-side
        // s126 behavior; covers the typed-path case where Browse wasn't
        // used.
        if (auto* mw = main_window()) {
            mw->set_last_folder("export-theme", out_dir);
        }
    }

    theme_show_result(result);
    theme_set_busy(false);

    LOG_INFO("ExportDialog theme: result success={} written={} "
             "skipped={} out_dir='{}'",
             result.success, result.icons_written,
             result.icons_skipped, result.output_dir);
}

// ─── Icon Theme: busy guard ─────────────────────────────────────────────────

void ExportDialog::theme_set_busy(bool busy) {
    // Disable theme-tab inputs and the dialog-level Close button.
    // The legacy dialog disabled its own Cancel; we don't have one,
    // but Close still needs disabling so a user-driven dismissal can't
    // run `delete this` while the synchronous export loop is iterating
    // GTK's main loop.
    if (m_theme_btn_export)    m_theme_btn_export->set_sensitive(!busy);
    if (m_theme_btn_browse)    m_theme_btn_browse->set_sensitive(!busy);
    if (m_theme_name_entry)    m_theme_name_entry->set_sensitive(!busy);
    if (m_theme_comment_entry) m_theme_comment_entry->set_sensitive(!busy);
    if (m_theme_folder_entry)  m_theme_folder_entry->set_sensitive(!busy);
    if (m_btn_close)           m_btn_close->set_sensitive(!busy);
    // Disable tab-switching too — flipping to Documents mid-export would
    // visually present an interactive UI on a tab that's really blocked.
    if (m_notebook)            m_notebook->set_sensitive(!busy);
}

// ─── Icon Theme: show_result ────────────────────────────────────────────────

void ExportDialog::theme_show_result(const ExportResult& result) {
    if (m_theme_progress)     m_theme_progress->set_fraction(1.0);
    if (m_theme_status_label) m_theme_status_label->set_visible(true);

    if (result.success) {
        std::ostringstream msg;
        msg << "✓ Exported " << result.icons_written << " icon"
            << (result.icons_written != 1 ? "s" : "")
            << " to:\n" << result.output_dir;
        if (result.icons_skipped > 0) {
            msg << "\n(" << result.icons_skipped
                << " skipped — missing name or category)";
        }
        if (m_theme_status_label) m_theme_status_label->set_text(msg.str());
    } else {
        if (m_theme_status_label) {
            m_theme_status_label->set_text("Export failed: " + result.error_message);
        }
    }
}

// ─── footer ─────────────────────────────────────────────────────────────────

void ExportDialog::build_footer(Gtk::Box& root) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_halign(Gtk::Align::END);

    // s233 m1 — registered substrate.
    m_btn_close = Gtk::make_managed<curvz::widgets::Button>(
                      "dlg_xu_close", "Close");
    curvz::utils::set_name(m_btn_close, "dlg_xu_close", "export_unified_dialog_close_btn");
    m_btn_close->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDialog::on_close));
    row->append(*m_btn_close);

    root.append(*row);
}

// ─── close ──────────────────────────────────────────────────────────────────

void ExportDialog::on_close() {
    if (m_window) m_window->close();
}

// ─── helpers ────────────────────────────────────────────────────────────────

std::string ExportDialog::doc_display_name(const CurvzDocument& doc,
                                           std::size_t fallback_index) {
    if (!doc.filename.empty()) return doc.filename;
    return "Document " + std::to_string(fallback_index + 1);
}

MainWindow* ExportDialog::main_window() const {
    return dynamic_cast<MainWindow*>(m_parent);
}

} // namespace Curvz
