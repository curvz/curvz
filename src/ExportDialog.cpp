#include "ExportDialog.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <gtkmm/filedialog.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <giomm/liststore.h>
#include <giomm/file.h>
#include <glibmm/main.h>
#include <glib.h>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
namespace Curvz {

// ── Constructor ───────────────────────────────────────────────────────────────

ExportDialog::ExportDialog() {
    curvz::utils::set_name(*this, "dlg_xp", "export_dialog_root");
    set_title("Export Icon Theme");
    set_modal(true);
    set_resizable(true);
    set_default_size(520, 560);
    set_hide_on_close(true);   // keep widget tree alive for reuse; match NewDocumentDialog pattern
    build_ui();
    set_child(m_root);
}

// ── UI Construction ───────────────────────────────────────────────────────────

void ExportDialog::build_ui() {
    m_root.set_margin(16);
    m_root.set_spacing(10);

    // ── Section: Theme metadata ───────────────────────────────────────────
    auto* meta_lbl = Gtk::make_managed<Gtk::Label>();
    meta_lbl->set_markup("<b>Theme</b>");
    meta_lbl->set_xalign(0.0f);
    m_root.append(*meta_lbl);

    m_meta_grid.set_row_spacing(6);
    m_meta_grid.set_column_spacing(10);

    auto* name_lbl = Gtk::make_managed<Gtk::Label>("Name");
    name_lbl->set_xalign(1.0f);
    m_meta_grid.attach(*name_lbl, 0, 0);
    m_theme_name_entry.set_placeholder_text("MyApp");
    m_theme_name_entry.set_hexpand(true);
    curvz::utils::set_name(m_theme_name_entry, "dlg_xp_nm", "export_dialog_theme_name_entry");
    m_meta_grid.attach(m_theme_name_entry, 1, 0);

    auto* comment_lbl = Gtk::make_managed<Gtk::Label>("Comment");
    comment_lbl->set_xalign(1.0f);
    m_meta_grid.attach(*comment_lbl, 0, 1);
    m_theme_comment_entry.set_placeholder_text("Icons for MyApp");
    m_theme_comment_entry.set_hexpand(true);
    curvz::utils::set_name(m_theme_comment_entry, "dlg_xp_cmt", "export_dialog_theme_comment_entry");
    m_meta_grid.attach(m_theme_comment_entry, 1, 1);

    m_root.append(m_meta_grid);

    // ── Section: Icon checklist ───────────────────────────────────────────
    auto* sep1 = Gtk::make_managed<Gtk::Separator>();
    m_root.append(*sep1);

    auto* icons_lbl = Gtk::make_managed<Gtk::Label>();
    icons_lbl->set_markup("<b>Icons to export</b>");
    icons_lbl->set_xalign(0.0f);
    m_root.append(*icons_lbl);

    m_list_box.set_spacing(4);
    m_scroll.set_child(m_list_box);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_min_content_height(180);
    m_scroll.set_max_content_height(280);
    m_scroll.set_vexpand(true);
    m_root.append(m_scroll);

    // Warning label for docs without name/category
    m_warn_label.set_xalign(0.0f);
    m_warn_label.add_css_class("dim-label");
    m_warn_label.set_wrap(true);
    m_warn_label.set_visible(false);
    curvz::utils::set_name(m_warn_label, "dlg_xp_warn", "export_dialog_warn_lbl");
    m_root.append(m_warn_label);

    // ── Section: Output folder ────────────────────────────────────────────
    auto* sep2 = Gtk::make_managed<Gtk::Separator>();
    m_root.append(*sep2);

    auto* folder_lbl = Gtk::make_managed<Gtk::Label>();
    folder_lbl->set_markup("<b>Output folder</b>");
    folder_lbl->set_xalign(0.0f);
    m_root.append(*folder_lbl);

    m_folder_row.set_spacing(6);
    m_folder_entry.set_placeholder_text("Choose a folder…");
    m_folder_entry.set_hexpand(true);
    curvz::utils::set_name(m_folder_entry, "dlg_xp_fld", "export_dialog_folder_entry");
    curvz::utils::set_name(m_browse_btn, "dlg_xp_brw", "export_dialog_browse_btn");
    m_folder_row.append(m_folder_entry);
    m_folder_row.append(m_browse_btn);
    m_root.append(m_folder_row);

    m_browse_btn.signal_clicked().connect(sigc::mem_fun(*this, &ExportDialog::on_browse));

    // ── Progress ──────────────────────────────────────────────────────────
    m_progress.set_visible(false);
    curvz::utils::set_name(m_progress, "dlg_xp_prg", "export_dialog_progress_bar");
    m_root.append(m_progress);

    m_status_label.set_xalign(0.0f);
    m_status_label.set_visible(false);
    m_status_label.set_wrap(true);
    curvz::utils::set_name(m_status_label, "dlg_xp_sts", "export_dialog_status_lbl");
    m_root.append(m_status_label);

    // ── Buttons ───────────────────────────────────────────────────────────
    auto* sep3 = Gtk::make_managed<Gtk::Separator>();
    m_root.append(*sep3);

    m_btn_row.set_spacing(8);
    m_btn_row.set_halign(Gtk::Align::END);

    m_export_btn.add_css_class("suggested-action");
    curvz::utils::set_name(m_cancel_btn, "dlg_xp_cnc", "export_dialog_cancel_btn");
    curvz::utils::set_name(m_export_btn, "dlg_xp_exp", "export_dialog_export_btn");
    m_btn_row.append(m_cancel_btn);
    m_btn_row.append(m_export_btn);
    m_root.append(m_btn_row);

    m_cancel_btn.signal_clicked().connect(sigc::mem_fun(*this, &ExportDialog::on_close));
    m_export_btn.signal_clicked().connect(sigc::mem_fun(*this, &ExportDialog::on_export));
}

// ── Populate & show ───────────────────────────────────────────────────────────

void ExportDialog::show(Gtk::Window& parent, CurvzProject& project, DoneCallback done_cb) {
    m_project  = &project;
    m_done_cb  = done_cb;
    m_exported = false;
    // Remove existing list rows first, then clear stale pointers
    while (auto* child = m_list_box.get_first_child())
        m_list_box.remove(*child);
    m_rows.clear();

    // Default output folder: project dir / export
    std::string default_out = project.directory + "/export";
    m_folder_entry.set_text(default_out);

    // Default theme name from project directory name
    std::string proj_name = fs::path(project.directory).filename().string();
    if (m_theme_name_entry.get_text().empty())
        m_theme_name_entry.set_text(proj_name);

    int invalid_count = 0;

    for (auto& doc_ptr : project.documents) {
        CurvzDocument* doc = doc_ptr.get();
        bool valid = !doc->export_name.empty() && !doc->export_category.empty();
        if (!valid) invalid_count++;

        // Build row: [checkbox] [icon name] [category tag or warning]
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        row->set_spacing(8);
        row->set_margin_start(4);

        auto* check = Gtk::make_managed<Gtk::CheckButton>();
        check->set_active(valid);  // pre-tick valid icons only
        check->set_sensitive(valid);
        row->append(*check);

        // Icon name or filename fallback
        std::string display_name = doc->export_name.empty()
            ? fs::path(doc->filename).stem().string()
            : doc->export_name;
        auto* name_lbl = Gtk::make_managed<Gtk::Label>(display_name);
        name_lbl->set_xalign(0.0f);
        name_lbl->set_hexpand(true);
        if (!valid) name_lbl->add_css_class("dim-label");
        row->append(*name_lbl);

        // Category badge or missing warning
        std::string badge_text = doc->export_category.empty()
            ? "⚠ no category"
            : doc->export_category;
        auto* badge = Gtk::make_managed<Gtk::Label>(badge_text);
        badge->set_xalign(1.0f);
        if (!valid) badge->add_css_class("dim-label");
        else        badge->add_css_class("caption");
        row->append(*badge);

        m_list_box.append(*row);

        DocRow dr;
        dr.doc   = doc;
        dr.check = check;
        dr.valid = valid;
        m_rows.push_back(dr);
    }

    // Show warning if some docs are unassigned
    if (invalid_count > 0) {
        m_warn_label.set_text(
            std::to_string(invalid_count) +
            " icon(s) have no name or category set and will be skipped. "
            "Assign them in the Document inspector.");
        m_warn_label.set_visible(true);
    } else {
        m_warn_label.set_visible(false);
    }

    m_status_label.set_visible(false);
    m_progress.set_visible(false);
    set_busy(false);

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    present();
}

// ── Actions ───────────────────────────────────────────────────────────────────

void ExportDialog::on_browse() {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Choose Export Folder");

    // Pre-fill with current folder entry if it's a valid path
    std::string current = m_folder_entry.get_text();
    if (!current.empty()) {
        try {
            auto init = Gio::File::create_for_path(current);
            dialog->set_initial_folder(init);
        } catch (...) {}
    }

    dialog->select_folder(*this,
        [this, dialog](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto folder = dialog->select_folder_finish(result);
                if (folder)
                    m_folder_entry.set_text(folder->get_path());
            } catch (...) {}
        });
}

void ExportDialog::on_export() {
    if (!m_project) return;

    std::string out_dir   = m_folder_entry.get_text();
    std::string theme_name    = m_theme_name_entry.get_text();
    std::string theme_comment = m_theme_comment_entry.get_text();

    if (out_dir.empty()) {
        m_status_label.set_text("Please choose an output folder.");
        m_status_label.set_visible(true);
        return;
    }
    if (theme_name.empty()) theme_name = "MyIcons";

    // Build entry list from checked rows
    std::vector<ExportEntry> entries;
    for (const auto& row : m_rows) {
        ExportEntry e;
        e.doc     = row.doc;
        e.include = row.valid && row.check->get_active();
        entries.push_back(e);
    }

    set_busy(true);
    m_progress.set_fraction(0.0);
    m_progress.set_visible(true);
    m_status_label.set_visible(false);

    // Run export — progress updates pumped via Glib::signal_idle
    // We capture everything by value/copy safe for the lambda lifetime.
    // Because export_theme is synchronous and potentially slow for large sets,
    // we pump GTK events via the progress callback to keep the bar live.
    ExportResult result = export_theme(
        entries, out_dir, theme_name, theme_comment,
        [this](int current, int total, const std::string& /*name*/) {
            if (total > 0)
                m_progress.set_fraction((double)current / total);
            // Pump GTK main loop to keep UI responsive during synchronous export
            while (g_main_context_pending(nullptr))
                g_main_context_iteration(nullptr, FALSE);
        });

    m_exported = true;
    show_result(result);
    set_busy(false);

    if (m_done_cb)
        m_done_cb(result.success, result.output_dir);
}

void ExportDialog::on_close() {
    if (m_done_cb && !m_exported)
        m_done_cb(false, "");
    set_visible(false);
}

void ExportDialog::set_busy(bool busy) {
    m_export_btn.set_sensitive(!busy);
    m_cancel_btn.set_sensitive(!busy);
    m_browse_btn.set_sensitive(!busy);
    m_theme_name_entry.set_sensitive(!busy);
    m_theme_comment_entry.set_sensitive(!busy);
    m_folder_entry.set_sensitive(!busy);
}

void ExportDialog::show_result(const ExportResult& result) {
    m_progress.set_fraction(1.0);
    m_status_label.set_visible(true);

    if (result.success) {
        std::ostringstream msg;
        msg << "✓ Exported " << result.icons_written << " icon"
            << (result.icons_written != 1 ? "s" : "")
            << " to:\n" << result.output_dir;
        if (result.icons_skipped > 0)
            msg << "\n(" << result.icons_skipped << " skipped — missing name or category)";
        m_status_label.set_text(msg.str());
        m_cancel_btn.set_label("Close");
    } else {
        m_status_label.set_text("Export failed: " + result.error_message);
    }
}

} // namespace Curvz
