#include "SaveAsTemplateDialog.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2
#include <gtkmm/separator.h>

namespace Curvz {

static constexpr const char* k_other_label = "Other…";

// ── Helper: labelled row in the form grid ─────────────────────────────────────
static void form_row(Gtk::Grid& g, int row, const char* lbl, Gtk::Widget& w) {
    auto* label = Gtk::make_managed<Gtk::Label>(lbl);
    label->set_xalign(1.0f);
    label->set_margin_end(8);
    label->add_css_class("section-label");
    g.attach(*label, 0, row);
    g.attach(w, 1, row);
}

// ── Constructor ───────────────────────────────────────────────────────────────
SaveAsTemplateDialog::SaveAsTemplateDialog() {
    curvz::utils::set_name(*this, "dlg_sat", "save_as_template_dialog_root");
    set_title("Save as Template");
    set_default_size(460, -1);
    set_resizable(false);
    set_hide_on_close(true);
    add_css_class("save-as-template-dialog");
    build_layout();
}

// ── build_layout ──────────────────────────────────────────────────────────────
void SaveAsTemplateDialog::build_layout() {
    if (m_built) return;
    m_built = true;

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    root->set_margin(16);
    root->set_spacing(12);
    set_child(*root);

    // ── Form grid ─────────────────────────────────────────────────────────
    m_form.set_row_spacing(8);
    m_form.set_column_spacing(8);
    root->append(m_form);

    // Name (row 0)
    m_name_entry.set_hexpand(true);
    m_name_entry.set_placeholder_text("e.g. Symbolic Icon 24");
    curvz::utils::set_name(m_name_entry, "dlg_sat_nm", "save_as_template_dialog_name_entry");
    form_row(m_form, 0, "Name", m_name_entry);

    // Category dropdown (row 1)
    m_category_model = Gtk::StringList::create({});
    m_category_dropdown.set_model(m_category_model);
    m_category_dropdown.set_hexpand(true);
    m_category_dropdown.property_selected().signal_changed().connect(
        sigc::mem_fun(*this, &SaveAsTemplateDialog::on_category_selected));
    curvz::utils::set_name(m_category_dropdown, "dlg_sat_cat", "save_as_template_dialog_category_dd");
    form_row(m_form, 1, "Category", m_category_dropdown);

    // Other (new-category) entry (row 2) — hidden until "Other…" is selected
    m_other_label.set_text("New category");
    m_other_label.set_xalign(1.0f);
    m_other_label.set_margin_end(8);
    m_other_label.add_css_class("section-label");
    m_other_entry.set_hexpand(true);
    m_other_entry.set_placeholder_text("e.g. posters");
    curvz::utils::set_name(m_other_entry, "dlg_sat_oth", "save_as_template_dialog_other_entry");
    m_form.attach(m_other_label, 0, 2);
    m_form.attach(m_other_entry, 1, 2);
    m_other_label.set_visible(false);
    m_other_entry.set_visible(false);

    // Description (row 3)
    m_desc_entry.set_hexpand(true);
    m_desc_entry.set_placeholder_text("Optional — brief description");
    curvz::utils::set_name(m_desc_entry, "dlg_sat_dsc", "save_as_template_dialog_description_entry");
    form_row(m_form, 3, "Description", m_desc_entry);

    // ── Separator ─────────────────────────────────────────────────────────
    auto* sep = Gtk::make_managed<Gtk::Separator>();
    root->append(*sep);

    // ── Buttons ───────────────────────────────────────────────────────────
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    btn_row->set_spacing(8);
    btn_row->set_halign(Gtk::Align::END);
    m_btn_create.add_css_class("suggested-action");
    curvz::utils::set_name(m_btn_cancel, "dlg_sat_cnc", "save_as_template_dialog_cancel_btn");
    curvz::utils::set_name(m_btn_create, "dlg_sat_cre", "save_as_template_dialog_create_btn");
    btn_row->append(m_btn_cancel);
    btn_row->append(m_btn_create);
    root->append(*btn_row);

    m_btn_cancel.signal_clicked().connect(
        sigc::mem_fun(*this, &SaveAsTemplateDialog::on_cancel));
    m_btn_create.signal_clicked().connect(
        sigc::mem_fun(*this, &SaveAsTemplateDialog::on_create));
}

// ── refresh_categories ────────────────────────────────────────────────────────
void SaveAsTemplateDialog::refresh_categories() {
    m_categories = templates::user_categories();

    // Rebuild the model: existing categories + "Other…" appended.
    // Gtk::StringList doesn't have a bulk-replace — remove all then append.
    while (m_category_model->get_n_items() > 0)
        m_category_model->remove(0);
    for (const auto& c : m_categories)
        m_category_model->append(c);
    m_category_model->append(k_other_label);

    // Default selection: prefer "icons" (most common icon-authoring case);
    // otherwise the first real category; otherwise index 0 = "Other…" (only
    // reachable if user_categories() returned empty, which ensure_user_root
    // normally prevents).
    guint sel = 0;
    for (size_t i = 0; i < m_categories.size(); ++i) {
        if (m_categories[i] == "icons") { sel = (guint)i; break; }
    }
    m_category_dropdown.set_selected(sel);
    on_category_selected();
}

// ── on_category_selected ──────────────────────────────────────────────────────
void SaveAsTemplateDialog::on_category_selected() {
    guint idx = m_category_dropdown.get_selected();
    // "Other…" is always the last row
    bool is_other = (idx == (guint)m_categories.size());
    m_other_label.set_visible(is_other);
    m_other_entry.set_visible(is_other);
    if (is_other)
        m_other_entry.grab_focus();
}

// ── gather ────────────────────────────────────────────────────────────────────
bool SaveAsTemplateDialog::gather(templates::TemplateMeta& out) {
    std::string name = m_name_entry.get_text();
    // Trim whitespace (users type trailing spaces by accident)
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    };
    trim(name);
    if (name.empty()) {
        m_name_entry.grab_focus();
        return false;
    }

    // Category
    guint idx = m_category_dropdown.get_selected();
    std::string category;
    if (idx < (guint)m_categories.size()) {
        category = m_categories[idx];
    } else {
        // "Other…" — read the revealed entry
        category = m_other_entry.get_text();
        trim(category);
        if (category.empty()) {
            m_other_entry.grab_focus();
            return false;
        }
        // Sanitise: the category becomes a directory name, so strip separators.
        for (char& c : category) {
            if (c == '/' || c == '\\') c = '-';
        }
    }

    std::string desc = m_desc_entry.get_text();
    trim(desc);

    out.name        = std::move(name);
    out.category    = std::move(category);
    out.description = std::move(desc);
    // author, created_utc left to save() to fill
    return true;
}

// ── on_create ─────────────────────────────────────────────────────────────────
void SaveAsTemplateDialog::on_create() {
    templates::TemplateMeta meta;
    if (!gather(meta)) {
        LOG_DEBUG("SaveAsTemplateDialog: validation failed");
        return;
    }
    Callback cb = std::move(m_callback);
    m_callback = nullptr;
    hide();
    if (cb) cb(std::move(meta));
}

// ── on_cancel ─────────────────────────────────────────────────────────────────
void SaveAsTemplateDialog::on_cancel() {
    m_callback = nullptr;
    hide();
}

// ── show ──────────────────────────────────────────────────────────────────────
void SaveAsTemplateDialog::show(Gtk::Window& parent,
                                const std::string& suggested_name,
                                Callback cb) {
    m_callback = std::move(cb);
    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);  // s117 m18 v2
    set_modal(true);

    // Reset form state
    m_name_entry.set_text(suggested_name);
    m_other_entry.set_text("");
    m_desc_entry.set_text("");

    refresh_categories();

    // Focus the name entry and select its contents so the user can type over
    m_name_entry.grab_focus();
    m_name_entry.select_region(0, -1);

    present();
}

} // namespace Curvz
