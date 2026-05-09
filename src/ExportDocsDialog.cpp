#include "ExportDocsDialog.hpp"
#include "CurvzProject.hpp"
#include "CurvzDocument.hpp"
#include "CurvzLog.hpp"
#include "MainWindow.hpp"  // s126: per-purpose last-folder accessors
#include "PngExporter.hpp"
#include "RefptExporter.hpp"  // s176 m1: sidecar refpt-coord export
#include "SvgWriter.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2

#include <gtkmm/adjustment.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>
#include <giomm/file.h>
#include <giomm/listmodel.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <sigc++/connection.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Curvz {

namespace {

// ── DPI presets ──────────────────────────────────────────────────────────────
// Trimmed list per S104 design conversation. "Custom" sentinel triggers
// the per-DPI numeric spin. Order is the order they appear in the dropdown.
constexpr int    kDpiPresets[]      = { 72, 96, 100, 144, 300, 600 };
constexpr int    kDpiPresetCount    = sizeof(kDpiPresets) / sizeof(kDpiPresets[0]);
constexpr int    kDpiCustomIndex    = kDpiPresetCount;  // last item
constexpr int    kDpiCustomDefault  = 100;

// ── Units dropdown order ─────────────────────────────────────────────────────
// px is index 0 — when units=px the DPI row hides entirely. The other
// three units all need a DPI to convert to pixels.
constexpr const char* kUnitItems[]  = { "px", "mm", "in", "pt" };
constexpr int         kUnitCount    = 4;

// ── px conversion ───────────────────────────────────────────────────────────
// chosen_px = value × DPI conversion. For "in" multiply by DPI directly;
// for "mm" go via inches (25.4 mm/in); for "pt" go via inches (72 pt/in).
double to_px(double value, const std::string& units, int dpi) {
    if (units == "px") return value;
    if (units == "in") return value * dpi;
    if (units == "mm") return value * dpi / 25.4;
    if (units == "pt") return value * dpi / 72.0;
    return value;  // unknown — treat as px
}

// Strip ".svg" or any extension off a filename, returning the stem.
// Used to make the per-doc output filename. doc.filename always carries
// ".svg" today (see CurvzProject.cpp), but be defensive about other
// extensions and the no-extension case.
std::string strip_ext(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return name;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos && dot < slash) return name;  // dot is in dir, not stem
    return name.substr(0, dot);
}

}  // namespace

// ─── ctor ───────────────────────────────────────────────────────────────────

ExportDocsDialog::ExportDocsDialog(Gtk::Window&  parent,
                                   CurvzProject& project,
                                   DoneCallback  done_cb)
    : m_parent(&parent),
      m_project(&project),
      m_done_cb(std::move(done_cb))
{
    build();
}

// ─── build ──────────────────────────────────────────────────────────────────

void ExportDocsDialog::build() {
    m_window = Gtk::make_managed<Gtk::Window>();
    curvz::utils::set_name(m_window, "dlg_xd", "export_docs_dialog_root");
    m_window->set_title("Export Documents");
    m_window->set_transient_for(*m_parent);
    curvz::utils::apply_motif_class_from_parent(*m_window, *m_parent);  // s117 m18 v2
    m_window->set_modal(true);
    m_window->set_default_size(560, 620);

    // Self-managed lifecycle: on close, fire done_cb and delete-this on idle.
    m_window->signal_close_request().connect(
        [this]() {
            // Disconnect from the window before delete-this so subsequent
            // signals on the dialog widgets (in flight from the close
            // animation) don't reach freed memory.
            if (m_done_cb) {
                m_done_cb(m_export_ran, m_folder_entry
                          ? std::string(m_folder_entry->get_text())
                          : std::string());
            }
            Glib::signal_idle().connect_once([this]() { delete this; });
            return false;  // allow close
        },
        /*after=*/false);

    auto root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    root->set_margin(16);
    m_window->set_child(*root);

    // ── Description ───────────────────────────────────────────────────────
    auto desc = Gtk::make_managed<Gtk::Label>(
        "Export selected documents from this project as SVG or PNG. "
        "Each output uses the chosen size; the other dimension is "
        "computed from each document's aspect ratio.");
    desc->set_xalign(0.0f);
    desc->set_wrap(true);
    desc->set_max_width_chars(60);
    desc->add_css_class("dim-label");
    root->append(*desc);

    build_targets_section(*root);
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_format_section(*root);
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_size_section(*root);
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_refpts_section(*root);  // s176 m1: sidecar coord export
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_output_section(*root);

    // Status — reserved row that's always present so the dialog doesn't
    // re-layout when an error/success message appears.
    m_status = Gtk::make_managed<Gtk::Label>("");
    curvz::utils::set_name(m_status, "dlg_xd_sts", "export_docs_dialog_status_lbl");
    m_status->set_xalign(0.0f);
    m_status->set_wrap(true);
    m_status->add_css_class("dim-label");
    root->append(*m_status);

    build_footer(*root);

    update_visibility();
    m_window->present();
}

// ─── targets ────────────────────────────────────────────────────────────────

void ExportDocsDialog::build_targets_section(Gtk::Box& root) {
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

    m_targets_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    m_targets_box->set_margin(6);
    scroller->set_child(*m_targets_box);

    const auto& docs = m_project->documents;
    const int active = m_project->active_doc_index;
    for (std::size_t i = 0; i < docs.size(); ++i) {
        if (!docs[i]) {
            m_target_checks.push_back(nullptr);
            continue;
        }
        std::string label = doc_display_name(*docs[i], i);
        if (static_cast<int>(i) == active) {
            label += "  (active)";
        }
        auto* cb = Gtk::make_managed<Gtk::CheckButton>(label);
        cb->set_active(true);  // default: all selected
        // s176 m1: per-target toggle drives refpts summary refresh
        // (the count + origin list shown in the refpts section is a
        // function of which targets are checked).
        cb->signal_toggled().connect(
            [this]() { refresh_refpts_info(); });
        m_targets_box->append(*cb);
        m_target_checks.push_back(cb);
    }

    auto btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto btn_all = Gtk::make_managed<Gtk::Button>("Select all");
    auto btn_none = Gtk::make_managed<Gtk::Button>("Select none");
    btn_all->signal_clicked().connect(sigc::mem_fun(*this, &ExportDocsDialog::on_select_all));
    btn_none->signal_clicked().connect(sigc::mem_fun(*this, &ExportDocsDialog::on_select_none));
    btn_row->append(*btn_all);
    btn_row->append(*btn_none);
    root.append(*btn_row);
}

// ─── format ─────────────────────────────────────────────────────────────────

void ExportDocsDialog::build_format_section(Gtk::Box& root) {
    auto title = Gtk::make_managed<Gtk::Label>("Format");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    root.append(*title);

    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    m_radio_svg = Gtk::make_managed<Gtk::CheckButton>("SVG");
    m_radio_png = Gtk::make_managed<Gtk::CheckButton>("PNG");
    curvz::utils::set_name(m_radio_svg, "dlg_xd_rsvg", "export_docs_dialog_radio_svg");
    curvz::utils::set_name(m_radio_png, "dlg_xd_rpng", "export_docs_dialog_radio_png");
    m_radio_png->set_group(*m_radio_svg);
    m_radio_svg->set_active(true);

    m_radio_svg->signal_toggled().connect(
        [this]() { update_visibility(); });
    m_radio_png->signal_toggled().connect(
        [this]() { update_visibility(); });

    row->append(*m_radio_svg);
    row->append(*m_radio_png);
    root.append(*row);
}

// ─── size ───────────────────────────────────────────────────────────────────

void ExportDocsDialog::build_size_section(Gtk::Box& root) {
    auto title = Gtk::make_managed<Gtk::Label>("Size");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    root.append(*title);

    // Fit-to row
    {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto lbl = Gtk::make_managed<Gtk::Label>("Fit to:");
        lbl->set_xalign(0.0f);
        lbl->set_size_request(60, -1);
        row->append(*lbl);
        m_radio_fit_w = Gtk::make_managed<Gtk::CheckButton>("Width");
        m_radio_fit_h = Gtk::make_managed<Gtk::CheckButton>("Height");
        curvz::utils::set_name(m_radio_fit_w, "dlg_xd_rfw", "export_docs_dialog_radio_fit_w");
        curvz::utils::set_name(m_radio_fit_h, "dlg_xd_rfh", "export_docs_dialog_radio_fit_h");
        m_radio_fit_h->set_group(*m_radio_fit_w);
        m_radio_fit_w->set_active(true);
        row->append(*m_radio_fit_w);
        row->append(*m_radio_fit_h);
        root.append(*row);
    }

    // Value + units row
    {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        auto lbl = Gtk::make_managed<Gtk::Label>("Value:");
        lbl->set_xalign(0.0f);
        lbl->set_size_request(60, -1);
        row->append(*lbl);

        // Value spin: 0.01..10000, 2dp.
        // Default: active document's canvas width in px. Falls back to 256
        // when there's no active doc (degenerate — the dialog shouldn't
        // open in that state, but be defensive). Using the active doc's
        // width lines up with the default fit-side (Width) and units (px)
        // so "click Export" without touching anything writes a 1:1 PNG.
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
        m_value_spin = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 2);
        curvz::utils::set_name(m_value_spin, "dlg_xd_val", "export_docs_dialog_value_spn");
        m_value_spin->set_width_chars(8);
        row->append(*m_value_spin);

        // Units dropdown.
        std::vector<Glib::ustring> unit_items;
        for (int i = 0; i < kUnitCount; ++i) unit_items.emplace_back(kUnitItems[i]);
        auto unit_list = Gtk::StringList::create(unit_items);
        m_units_drop = Gtk::make_managed<Gtk::DropDown>(unit_list);
        curvz::utils::set_name(m_units_drop, "dlg_xd_unt", "export_docs_dialog_units_dd");
        m_units_drop->set_selected(0);  // px
        m_units_drop->property_selected().signal_changed().connect(
            [this]() { update_visibility(); });
        row->append(*m_units_drop);
        root.append(*row);
    }

    // DPI row — hidden when units == px.
    {
        m_dpi_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        m_dpi_label = Gtk::make_managed<Gtk::Label>("DPI:");
        m_dpi_label->set_xalign(0.0f);
        m_dpi_label->set_size_request(60, -1);
        m_dpi_row->append(*m_dpi_label);

        std::vector<Glib::ustring> dpi_items;
        for (int i = 0; i < kDpiPresetCount; ++i) {
            dpi_items.push_back(Glib::ustring::format(kDpiPresets[i]));
        }
        dpi_items.emplace_back("Custom…");
        auto dpi_list = Gtk::StringList::create(dpi_items);
        m_dpi_drop = Gtk::make_managed<Gtk::DropDown>(dpi_list);
        curvz::utils::set_name(m_dpi_drop, "dlg_xd_dpi", "export_docs_dialog_dpi_dd");
        // Default to 96 (web/screen standard) — common across all units.
        for (int i = 0; i < kDpiPresetCount; ++i) {
            if (kDpiPresets[i] == 96) {
                m_dpi_drop->set_selected(i);
                break;
            }
        }
        m_dpi_drop->property_selected().signal_changed().connect(
            [this]() { update_visibility(); });
        m_dpi_row->append(*m_dpi_drop);

        // Custom DPI spin: 1..2400 (covers everything up to high-end print).
        auto adj = Gtk::Adjustment::create(kDpiCustomDefault, 1.0, 2400.0, 1.0, 10.0, 0.0);
        m_dpi_custom = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
        curvz::utils::set_name(m_dpi_custom, "dlg_xd_dpc", "export_docs_dialog_dpi_custom_spn");
        m_dpi_custom->set_width_chars(6);
        m_dpi_custom->set_visible(false);
        m_dpi_row->append(*m_dpi_custom);

        root.append(*m_dpi_row);
    }
}

// ─── refpts (s176 m1) ───────────────────────────────────────────────────────
//
// Sidecar coordinate export. When the checkbox is on, every selected doc
// produces an additional file `<docname>.refpts.<ext>` alongside its
// SVG/PNG output, holding the doc's refpt coordinates in the chosen
// format. The format dropdown is hidden when the checkbox is off.
//
// m1 ships with CSV only. m2 will add JSON, G-code, and DXF entries.
// The dropdown is plumbed for the full set today so adding the m2
// formats is a one-line addition to kRefptFormatItems.
//
// The info label below the dropdown is intentionally lightweight in m1 —
// "writes alongside each output" — rather than a per-doc count. Counts
// are reported in the log at write time, not pre-computed in the UI.
// Keeps the dialog simple and the wiring shallow.

namespace {
// m1: CSV is the only entry. The position-to-Format mapping below uses
// the dropdown index. m2 will extend this list and the mapping below;
// the section build code itself doesn't need to change.
constexpr const char* kRefptFormatItems[] = { "CSV (.csv)" };
constexpr int         kRefptFormatCount   =
    sizeof(kRefptFormatItems) / sizeof(kRefptFormatItems[0]);

RefptExporter::Format refpt_format_for_index(unsigned idx) {
    // m1: only CSV. m2 extends.
    (void)idx;
    return RefptExporter::Format::Csv;
}
}  // namespace

void ExportDocsDialog::build_refpts_section(Gtk::Box& root) {
    auto title = Gtk::make_managed<Gtk::Label>("Refpt coordinates");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    root.append(*title);

    // Checkbox row — enables the section.
    m_refpts_check = Gtk::make_managed<Gtk::CheckButton>(
        "Export refpt coordinates as sidecar file");
    curvz::utils::set_name(m_refpts_check, "dlg_xd_rpc",
                           "export_docs_dialog_refpts_check");
    m_refpts_check->set_active(false);  // default off — opt-in feature
    m_refpts_check->signal_toggled().connect(
        [this]() { refresh_refpts_info(); });
    root.append(*m_refpts_check);

    // Format row — hidden when checkbox is off.
    m_refpts_format_row =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto fmt_label = Gtk::make_managed<Gtk::Label>("Format:");
    fmt_label->set_xalign(0.0f);
    fmt_label->set_size_request(60, -1);
    m_refpts_format_row->append(*fmt_label);

    std::vector<Glib::ustring> fmt_items;
    for (int i = 0; i < kRefptFormatCount; ++i)
        fmt_items.emplace_back(kRefptFormatItems[i]);
    auto fmt_list = Gtk::StringList::create(fmt_items);
    m_refpts_format_drop = Gtk::make_managed<Gtk::DropDown>(fmt_list);
    curvz::utils::set_name(m_refpts_format_drop, "dlg_xd_rpf",
                           "export_docs_dialog_refpts_format_dd");
    m_refpts_format_drop->set_selected(0);
    m_refpts_format_row->append(*m_refpts_format_drop);

    root.append(*m_refpts_format_row);

    // Info label. Stays present even when section is collapsed (acts as
    // explanatory text for the checkbox itself).
    m_refpts_info_label = Gtk::make_managed<Gtk::Label>(
        "When enabled, writes <docname>.refpts.<ext> alongside each "
        "exported file. Coordinates are origin-translated to the "
        "promoted refpt (or canvas 0,0 if none), Y-up, in document units.");
    curvz::utils::set_name(m_refpts_info_label, "dlg_xd_rpi",
                           "export_docs_dialog_refpts_info_lbl");
    m_refpts_info_label->set_xalign(0.0f);
    m_refpts_info_label->set_wrap(true);
    m_refpts_info_label->set_max_width_chars(60);
    m_refpts_info_label->add_css_class("dim-label");
    root.append(*m_refpts_info_label);

    refresh_refpts_info();
}

void ExportDocsDialog::refresh_refpts_info() {
    const bool on = m_refpts_check && m_refpts_check->get_active();
    if (m_refpts_format_row) m_refpts_format_row->set_visible(on);
    if (!m_refpts_info_label) return;

    if (!on) {
        // Off: explanatory text only. The user hasn't opted in yet, so
        // a count would just be noise.
        m_refpts_info_label->set_text(
            "When enabled, writes <docname>.refpts.<ext> alongside each "
            "exported file. Coordinates are origin-translated to the "
            "promoted refpt (or canvas 0,0 if none), Y-up, in document units.");
        return;
    }

    // On: live summary. Walk the currently-checked docs and build a
    // count + per-doc origin breadcrumb. The breadcrumb is the user's
    // preview of "what origin will appear in the metadata header for
    // each sidecar" — same UX leak class as the s175 m3 TOP inspector
    // fix, prevented at source by reading from the resolved origin name
    // (RefptExporter::summarize) rather than rendering raw iids.
    if (!m_project) {
        m_refpts_info_label->set_text("Refpts: no project.");
        return;
    }

    std::size_t total_refpts = 0;
    std::size_t docs_with_refpts = 0;
    std::size_t docs_checked = 0;
    std::ostringstream origins_line;

    const auto& docs = m_project->documents;
    for (std::size_t i = 0; i < docs.size() && i < m_target_checks.size();
         ++i) {
        auto* cb = m_target_checks[i];
        if (!cb || !cb->get_active()) continue;
        if (!docs[i]) continue;
        ++docs_checked;

        const auto s = RefptExporter::summarize(*docs[i]);
        total_refpts += s.refpt_count;
        if (s.refpt_count > 0) {
            ++docs_with_refpts;
            if (origins_line.tellp() > 0) origins_line << ", ";
            // Each entry: "<doc-display-name>: <count> from <origin>"
            const std::string disp = doc_display_name(*docs[i], i);
            origins_line << "\"" << disp << "\": "
                         << s.refpt_count << " from "
                         << s.origin_name;
        }
    }

    std::ostringstream summary;
    if (docs_checked == 0) {
        summary << "Refpts: no documents checked.";
    } else if (total_refpts == 0) {
        summary << "Refpts: 0 across " << docs_checked
                << (docs_checked == 1 ? " document" : " documents")
                << " — sidecar files will be skipped.";
    } else {
        summary << "Refpts: " << total_refpts << " across "
                << docs_with_refpts << " of " << docs_checked
                << (docs_checked == 1 ? " document" : " documents")
                << ".  " << origins_line.str() << ".";
    }
    m_refpts_info_label->set_text(summary.str());
}



void ExportDocsDialog::build_output_section(Gtk::Box& root) {
    auto title = Gtk::make_managed<Gtk::Label>("Output folder");
    title->set_xalign(0.0f);
    title->add_css_class("heading");
    root.append(*title);

    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_folder_entry = Gtk::make_managed<Gtk::Entry>();
    curvz::utils::set_name(m_folder_entry, "dlg_xd_fld", "export_docs_dialog_folder_entry");
    m_folder_entry->set_hexpand(true);
    m_folder_entry->set_placeholder_text("Choose a folder…");

    // Pre-fill priority: most recent intent wins. Last-used folder for this
    // purpose ("export-docs") beats the project directory beats nothing.
    // s126 — keyed separately from "place-image" / "open-image" / "save-as"
    // so export targets don't pollute import pickers and vice-versa.
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
        m_folder_entry->set_text(prefill);
    }

    m_btn_browse = Gtk::make_managed<Gtk::Button>("Browse…");
    curvz::utils::set_name(m_btn_browse, "dlg_xd_brw", "export_docs_dialog_browse_btn");
    m_btn_browse->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDocsDialog::on_browse));

    row->append(*m_folder_entry);
    row->append(*m_btn_browse);
    root.append(*row);
}

// ─── footer ─────────────────────────────────────────────────────────────────

void ExportDocsDialog::build_footer(Gtk::Box& root) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_halign(Gtk::Align::END);
    row->set_margin_top(8);

    m_btn_cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    m_btn_export = Gtk::make_managed<Gtk::Button>("Export");
    curvz::utils::set_name(m_btn_cancel, "dlg_xd_cnc", "export_docs_dialog_cancel_btn");
    curvz::utils::set_name(m_btn_export, "dlg_xd_exp", "export_docs_dialog_export_btn");
    m_btn_export->add_css_class("suggested-action");

    m_btn_cancel->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDocsDialog::on_cancel));
    m_btn_export->signal_clicked().connect(
        sigc::mem_fun(*this, &ExportDocsDialog::on_export));

    row->append(*m_btn_cancel);
    row->append(*m_btn_export);
    root.append(*row);
}

// ─── visibility wiring ──────────────────────────────────────────────────────

void ExportDocsDialog::update_visibility() {
    // DPI row hides for px units (size value is already px).
    const bool need_dpi = (current_units() != "px");
    if (m_dpi_row) m_dpi_row->set_visible(need_dpi);

    // Custom DPI spin hides unless dropdown selection is "Custom…".
    const bool custom = need_dpi && m_dpi_drop &&
                        (static_cast<int>(m_dpi_drop->get_selected()) == kDpiCustomIndex);
    if (m_dpi_custom) m_dpi_custom->set_visible(custom);

    // Status clears whenever the user adjusts inputs.
    if (m_status) m_status->set_text("");
}

std::string ExportDocsDialog::current_units() const {
    if (!m_units_drop) return "px";
    auto sel = static_cast<int>(m_units_drop->get_selected());
    if (sel < 0 || sel >= kUnitCount) return "px";
    return kUnitItems[sel];
}

int ExportDocsDialog::current_dpi() const {
    if (!m_dpi_drop) return kDpiCustomDefault;
    auto sel = static_cast<int>(m_dpi_drop->get_selected());
    if (sel >= 0 && sel < kDpiPresetCount) return kDpiPresets[sel];
    if (sel == kDpiCustomIndex && m_dpi_custom) {
        return std::max(1, m_dpi_custom->get_value_as_int());
    }
    return kDpiCustomDefault;
}

int ExportDocsDialog::chosen_dim_px() const {
    if (!m_value_spin) return 256;
    double v = m_value_spin->get_value();
    if (v <= 0.0) return 1;
    double px = to_px(v, current_units(), current_dpi());
    int rounded = static_cast<int>(std::round(px));
    return std::max(1, rounded);
}

// ─── target select all/none ─────────────────────────────────────────────────

void ExportDocsDialog::on_select_all() {
    for (auto* cb : m_target_checks) {
        if (cb) cb->set_active(true);
    }
}

void ExportDocsDialog::on_select_none() {
    for (auto* cb : m_target_checks) {
        if (cb) cb->set_active(false);
    }
}

// ─── browse ─────────────────────────────────────────────────────────────────

void ExportDocsDialog::on_browse() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Choose Output Folder");

    std::string current = m_folder_entry ? std::string(m_folder_entry->get_text()) : std::string();
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
                if (folder && m_folder_entry) {
                    std::string chosen = folder->get_path();
                    m_folder_entry->set_text(chosen);
                    // s126: remember for next time. We also save on
                    // successful export, but saving here too means the
                    // memory survives a Cancel-without-export.
                    if (auto* mw = main_window()) {
                        mw->set_last_folder("export-docs", chosen);
                    }
                }
            } catch (...) {
                // Cancelled or no folder — leave entry alone.
            }
        });
}

// ─── cancel ─────────────────────────────────────────────────────────────────

void ExportDocsDialog::on_cancel() {
    if (m_window) m_window->close();
}

std::string ExportDocsDialog::doc_display_name(const CurvzDocument& doc,
                                               std::size_t fallback_index) {
    if (!doc.filename.empty()) return doc.filename;
    return "Document " + std::to_string(fallback_index + 1);
}

MainWindow* ExportDocsDialog::main_window() const {
    return dynamic_cast<MainWindow*>(m_parent);
}

// ─── export ─────────────────────────────────────────────────────────────────

void ExportDocsDialog::on_export() {
    if (!m_project || !m_status) return;

    // Validate output dir.
    std::string out_dir = m_folder_entry ? std::string(m_folder_entry->get_text()) : std::string();
    if (out_dir.empty()) {
        m_status->set_text("Please choose an output folder.");
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(out_dir, ec) || !fs::is_directory(out_dir, ec)) {
        // Try to create it. If that fails, complain.
        if (!fs::create_directories(out_dir, ec)) {
            m_status->set_text("Output folder is not a valid directory.");
            return;
        }
    }

    // Validate selected docs.
    std::vector<int> selected_indices;
    for (std::size_t i = 0; i < m_target_checks.size(); ++i) {
        if (m_target_checks[i] && m_target_checks[i]->get_active()) {
            selected_indices.push_back(static_cast<int>(i));
        }
    }
    if (selected_indices.empty()) {
        m_status->set_text("Select at least one document to export.");
        return;
    }

    const bool   want_svg = m_radio_svg && m_radio_svg->get_active();
    const std::string ext = want_svg ? ".svg" : ".png";

    // s126 diagnostic — confirms which branch fires when Scott reports
    // "PNG export still calls file svg". Logs both radio states + the
    // computed want_svg so we can see if the radio group is decoupled.
    LOG_INFO("ExportDocsDialog::on_export "
             "radio_svg.active={} radio_png.active={} → want_svg={} (ext={})",
             m_radio_svg && m_radio_svg->get_active(),
             m_radio_png && m_radio_png->get_active(),
             want_svg, ext);

    // Resolve per-doc target paths up front (un-disambiguated). The same
    // list feeds both the collision scan and the write loop, so the user
    // sees a Replace prompt for exactly the files we'd overwrite.
    //
    // Stem rules: doc.filename minus its extension; falls back to
    // "Document_<n>" for unsaved docs. Two docs in the project that
    // happen to share a stem will land on the same target path —
    // that's a project-level naming collision, not an on-disk one,
    // and the second write will silently overwrite the first. Acceptable
    // edge case (existed before this change too) and the user can
    // disambiguate by renaming docs.
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
        m_status->set_text("No valid documents selected.");
        return;
    }

    // Collision scan — anything already on disk?
    std::vector<std::string> collisions;
    for (const auto& t : targets) {
        bool exists = fs::exists(t.path, ec);
        // s126 diagnostic — Scott reports "not overwriting and reporting
        // it overwrote". Either fs::exists is missing the file (path
        // mismatch) or the writer is silently writing somewhere else.
        // Logging every resolved target + existence verdict pins which.
        LOG_INFO("ExportDocsDialog scan: target='{}' exists={}", t.path, exists);
        if (exists) collisions.push_back(t.path);
    }

    if (collisions.empty()) {
        // Clean — go straight to write.
        perform_export(targets);
        return;
    }

    // Build a confirm prompt. One file: name it. Many: count + sample.
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

    // Index 0 = Cancel (also the cancel-on-Esc), 1 = Replace (default).
    // Convention from curvz_utils header: destructive first, affirmative
    // last. The button-list order matches the index returned by show_confirm.
    //
    // s126 — Defer the show_confirm via signal_idle. Calling it
    // synchronously from this Export button-click handler — itself
    // running inside a modal window — leaves the second dialog's
    // present() silently swallowed (the focus chain is still mid-grab
    // from the click). Idle gives GTK a clean tick to settle the
    // outer state before the new dialog tries to claim focus.
    LOG_INFO("ExportDocsDialog: scheduling Replace confirm "
             "({} collisions, {} targets)",
             collisions.size(), targets.size());
    Glib::signal_idle().connect_once(
        [this, detail_str = detail.str(),
         targets = std::move(targets)]() mutable {
            LOG_INFO("ExportDocsDialog: presenting Replace confirm");
            curvz::utils::show_confirm(
                *m_window,
                "Replace files?",
                detail_str,
                {"Cancel", "Replace"},
                /*default_button=*/1,
                /*cancel_button=*/0,
                [this, targets = std::move(targets)](int choice) {
                    LOG_INFO("ExportDocsDialog confirm-cb: "
                             "choice={} targets={}",
                             choice, targets.size());
                    if (choice == 1) {
                        perform_export(targets);
                    } else {
                        // User cancelled. Leave the dialog open so they can
                        // pick a different folder or tweak settings.
                        if (m_status) m_status->set_text(
                            "Cancelled. Choose a different folder or "
                            "rename existing files.");
                    }
                });
        });
}

// Actual write loop. Targets are pre-resolved un-disambiguated paths;
// any existing files at these paths get overwritten (the user has
// already consented in on_export, or there were no collisions).
void ExportDocsDialog::perform_export(const std::vector<DocTarget>& targets) {
    LOG_INFO("ExportDocsDialog::perform_export entry: targets={} project={} status={}",
             targets.size(), (void*)m_project, (void*)m_status);
    if (!m_project || !m_status || targets.empty()) {
        LOG_WARN("ExportDocsDialog::perform_export early-return: "
                 "project_null={} status_null={} targets_empty={}",
                 m_project == nullptr, m_status == nullptr, targets.empty());
        return;
    }

    namespace fs = std::filesystem;

    // Re-read sizing controls. The confirm dialog yields the main loop,
    // so in principle the user could change settings between Export click
    // and Replace click. Re-reading is harmless if they didn't and correct
    // if they did.
    const bool   want_svg   = m_radio_svg && m_radio_svg->get_active();
    const bool   fit_width  = m_radio_fit_w && m_radio_fit_w->get_active();
    const std::string units = current_units();
    const double size_value = m_value_spin ? m_value_spin->get_value() : 256.0;
    const int    chosen_px  = chosen_dim_px();

    // s176 m1: refpts sidecar export. Read once before the loop — the
    // checkbox state shouldn't change per-doc. The format index drives
    // the file extension and the writer call inside the loop.
    const bool refpts_on = m_refpts_check && m_refpts_check->get_active();
    const auto refpts_fmt = refpts_on && m_refpts_format_drop
        ? refpt_format_for_index(m_refpts_format_drop->get_selected())
        : RefptExporter::Format::Csv;
    int refpts_written = 0;
    int refpts_failed  = 0;
    int refpts_skipped_empty = 0;

    int written = 0;
    int failed  = 0;
    std::vector<std::string> output_files;
    output_files.reserve(targets.size());

    for (const auto& t : targets) {
        if (!t.doc) continue;

        // s126 diagnostic — pre-write snapshot. Scott reports "not
        // overwriting and reporting it overwrote". Either the writer is
        // lying (returns ok=true without touching disk) or we're looking
        // at the wrong path. Pre/post snapshot of size + mtime pins it:
        //   pre_exists=Y, post mtime unchanged → writer lied (most likely
        //     somewhere in flush error-handling).
        //   pre_exists=N, post_exists=N → wrote to a DIFFERENT path.
        //   pre_exists=N, post_exists=Y → first write, working as intended.
        std::error_code pec;
        bool pre_exists = fs::exists(t.path, pec);
        std::uintmax_t pre_size = 0;
        fs::file_time_type pre_mtime{};
        if (pre_exists) {
            pre_size  = fs::file_size(t.path, pec);
            pre_mtime = fs::last_write_time(t.path, pec);
        }

        // s126: pre-emptive remove of the existing target. If the writer
        // is silently failing to overwrite (some flush-error path swallows
        // the failure and returns ok), this guarantees we either get a
        // fresh write or a clear failure — no stale-file-claimed-fresh
        // confusion. The user has already consented to overwrite by
        // clicking Replace, so the remove is in scope.
        if (pre_exists) {
            std::error_code rec;
            fs::remove(t.path, rec);
            if (rec) {
                LOG_ERROR("ExportDocsDialog: pre-write remove failed for "
                          "'{}': {} — skipping", t.path, rec.message());
                ++failed;
                continue;
            }
        }

        bool ok = false;
        if (want_svg) {
            // SVG: stamp the user's sizing intent into data-curvz-export-*
            // attrs. The "fit_side" string is "width" or "height" so re-
            // import / downstream tooling knows which side the user fixed.
            const std::string fit_side = fit_width ? "width" : "height";
            ok = write_svg_file_with_export_meta(
                *t.doc, t.path, units, size_value, fit_side);
        } else {
            // PNG: chosen_px is the fixed side; the other comes from
            // doc aspect ratio.
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
        }
        if (ok) { ++written; output_files.push_back(t.path); }
        else    { ++failed; }

        // Post-write snapshot. The writer claims success but if size
        // and mtime didn't change, the file on disk is the one the user
        // is staring at thinking the export didn't take.
        std::error_code qec;
        bool post_exists = fs::exists(t.path, qec);
        std::uintmax_t post_size = post_exists ? fs::file_size(t.path, qec) : 0;
        fs::file_time_type post_mtime{};
        if (post_exists) post_mtime = fs::last_write_time(t.path, qec);
        const bool size_changed  = (pre_size  != post_size);
        const bool mtime_changed = (pre_mtime != post_mtime);
        LOG_INFO("ExportDocsDialog write: path='{}' fmt={} writer_ok={} "
                 "pre={{exists={} size={}}} post={{exists={} size={}}} "
                 "size_changed={} mtime_changed={}",
                 t.path, want_svg ? "svg" : "png", ok,
                 pre_exists, pre_size, post_exists, post_size,
                 size_changed, mtime_changed);

        // s176 m1: refpts sidecar. Only attempted on a successful artwork
        // write — there's no value in writing the sidecar for a doc whose
        // primary export failed. If the doc has zero exportable refpts,
        // the sidecar is skipped (logged at info level — not an error,
        // just a no-op for that doc). The sidecar path is the artwork
        // path with the extension swapped: "icon.svg" -> "icon.refpts.csv".
        if (ok && refpts_on) {
            const std::size_t n =
                RefptExporter::count_exportable(*t.doc);
            if (n == 0) {
                ++refpts_skipped_empty;
                LOG_INFO("ExportDocsDialog refpts: '{}' has no refpts — "
                         "skipping sidecar", t.path);
            } else {
                fs::path artwork_path(t.path);
                fs::path sidecar = artwork_path;
                // Replace the artwork extension with ".refpts.<ext>".
                // sidecar.replace_extension expects ".ext"; we build it
                // manually to keep the doubled extension shape ("foo.svg"
                // -> "foo.refpts.csv", not "foo.refpts").
                std::string stem = artwork_path.stem().string();
                std::string parent = artwork_path.parent_path().string();
                std::string ext = RefptExporter::extension(refpts_fmt);
                sidecar = fs::path(parent) /
                          (stem + ".refpts." + ext);

                std::string body =
                    RefptExporter::export_refpts(*t.doc, refpts_fmt);
                std::ofstream out(sidecar, std::ios::binary);
                if (out.good()) {
                    out << body;
                    out.close();
                    if (out.good()) {
                        ++refpts_written;
                        LOG_INFO("ExportDocsDialog refpts: wrote '{}' "
                                 "({} refpts, {} bytes)",
                                 sidecar.string(), n, body.size());
                    } else {
                        ++refpts_failed;
                        LOG_ERROR("ExportDocsDialog refpts: write failed "
                                  "for '{}' (close/flush error)",
                                  sidecar.string());
                    }
                } else {
                    ++refpts_failed;
                    LOG_ERROR("ExportDocsDialog refpts: open failed for "
                              "'{}'", sidecar.string());
                }
            }
        }
    }

    // s176 m1: refpts sidecar summary. Logged after all docs processed
    // so the count appears once per export action, not per-doc. Status-
    // text update happens in the success/failure branches below.
    if (refpts_on) {
        LOG_INFO("ExportDocsDialog refpts summary: written={} failed={} "
                 "skipped_empty={}",
                 refpts_written, refpts_failed, refpts_skipped_empty);
    }

    // Record that the export ran (for the done_cb result flag) and report.
    m_export_ran = (written > 0);

    const std::string out_dir = m_folder_entry
        ? std::string(m_folder_entry->get_text()) : std::string();

    // s126: remember the folder if anything was written. Covers the
    // typed-path case where the user never used Browse (on_browse saves
    // its own choice eagerly).
    if (written > 0 && !out_dir.empty()) {
        if (auto* mw = main_window()) {
            mw->set_last_folder("export-docs", out_dir);
        }
    }

    if (failed == 0) {
        // Success — close directly. An extra OK gate stands between the
        // user and their next action; the file manager and status bar
        // confirm the write. done_cb fires on close so the MainWindow
        // call site still gets its completion notification.
        LOG_INFO("ExportDocsDialog: wrote {} files to '{}'", written, out_dir);
        if (m_window) m_window->close();
    } else if (written == 0) {
        // Total failure — keep dialog open with a status line.
        m_status->set_text("Export failed. Check the output folder permissions and try again.");
        LOG_ERROR("ExportDocsDialog: 0 written, {} failed", failed);
    } else {
        // Partial — keep dialog open with status.
        std::ostringstream s;
        s << "Wrote " << written << " of " << (written + failed) << " files. "
          << failed << " failed (see log).";
        m_status->set_text(s.str());
        LOG_WARN("ExportDocsDialog: partial — {} written, {} failed", written, failed);
    }
}

}  // namespace Curvz
