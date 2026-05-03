#pragma once
#include "CurvzEntry.hpp"
#include "TemplateLibrary.hpp"
#include <functional>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

namespace Curvz {

// ── SaveAsTemplateDialog ──────────────────────────────────────────────────────
// Modal dialog collecting metadata for saving the active document as a
// user-global template. Three fields: Name, Category, Description.
//
// Category is a Gtk::DropDown populated with existing user categories plus a
// trailing "Other…" option. Selecting "Other…" reveals a text entry for a
// new category name. The dialog pre-populates the dropdown with any existing
// user categories returned by templates::user_categories() at show-time.
//
// Description is a single-line Gtk::Entry (not multi-line by decision).
//
// Flow:
//   dlg.show(parent, [](TemplateMeta meta) { ... });
//
// The callback runs only on Create. On Cancel, no callback is invoked.
// The dialog does NOT call templates::save() itself — the caller is
// responsible for checking user_bundle_exists(), prompting for overwrite,
// and invoking save(). This keeps the dialog UI-only and the overwrite
// prompt parent-owned.

class SaveAsTemplateDialog : public Gtk::Window {
public:
    using Callback = std::function<void(templates::TemplateMeta)>;

    SaveAsTemplateDialog();

    // Show modal, attached to parent. `callback` runs on Create.
    // `suggested_name` pre-fills the name entry (typically derived from
    // the active document's filename).
    void show(Gtk::Window& parent, const std::string& suggested_name,
              Callback cb);

private:
    // ── Layout ────────────────────────────────────────────────────────────
    void build_layout();

    // Populate m_category_dropdown from templates::user_categories(),
    // append "Other…" at the end. Called at show() time so new categories
    // created via this dialog appear on subsequent opens.
    void refresh_categories();

    // Called when the category dropdown's selection changes. If the user
    // picked "Other…", reveal m_other_entry; otherwise hide it.
    void on_category_selected();

    // ── Actions ───────────────────────────────────────────────────────────
    void on_create();
    void on_cancel();

    // Gather form state into a TemplateMeta. Returns false if validation
    // fails (empty name, or "Other…" selected with empty category entry);
    // in that case an error indicator is shown on the offending field.
    bool gather(templates::TemplateMeta& out);

    // ── Widgets ───────────────────────────────────────────────────────────
    Gtk::Grid                     m_form;
    CurvzEntry                    m_name_entry;
    Gtk::DropDown                 m_category_dropdown;
    Glib::RefPtr<Gtk::StringList> m_category_model;
    CurvzEntry                    m_other_entry;      // revealed for "Other…"
    Gtk::Label                    m_other_label;      // lives on the same row as other_entry
    CurvzEntry                    m_desc_entry;
    Gtk::Button                   m_btn_cancel{"Cancel"};
    Gtk::Button                   m_btn_create{"Save Template"};

    // Cached list of existing user categories (same order as the dropdown,
    // minus the trailing "Other…"). Used by gather() to map dropdown index
    // back to a category name.
    std::vector<std::string>      m_categories;

    Callback                      m_callback;
    bool                          m_built = false;
};

} // namespace Curvz
