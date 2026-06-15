// ImportFailureDialog.cpp ─────────────────────────────────────────────────────
//
// s360 — implementation of the import-failure dialog. See
// ImportFailureDialog.hpp for the rationale and lifetime model. The
// visual treatment (frameless flat-class Entries, dim-label name
// column, right-aligned Close, 480px width, non-resizable, the m1h
// focus-idle dance) is copied from ImageInfoDialog so the two read-only
// info dialogs feel identical.
//
#include "ImportFailureDialog.hpp"

#include "widgets/Entry.hpp"   // substrate Entry
#include "curvz_utils.hpp"     // set_name, apply_motif_class_from_parent
#include "CurvzLog.hpp"

#include <glibmm/main.h>       // signal_idle

namespace Curvz {

namespace {

// Row roster, in display order. Member-pointer-to-string lets
// sync_from_data() walk the roster + m_data side-by-side without naming
// each field three times. Detail is "optional" — its row hides when the
// field is empty (same blank-when-unknown convention as ImageInfoDialog
// Format/Depth).
struct RowSpec {
    const char* label;
    std::string ImportFailure::* field;
    bool optional;
};

constexpr std::size_t k_row_count = 4;

RowSpec row_specs[k_row_count] = {
    {"Name",   &ImportFailure::filename,  false},
    {"Path",   &ImportFailure::full_path, false},
    {"Reason", &ImportFailure::reason,    false},
    {"Detail", &ImportFailure::detail,    true},
};

} // namespace

// ── ctor ──────────────────────────────────────────────────────────────────
ImportFailureDialog::ImportFailureDialog() {
    set_title("Import Failed");
    set_modal(true);
    set_resizable(false);
    set_default_size(480, -1);
    set_hide_on_close(true);
    curvz::utils::set_name(*this, "dlg_importfail", "import_failure_dialog_root");
}

// ── show ──────────────────────────────────────────────────────────────────
void ImportFailureDialog::show(Gtk::Window& parent, ImportFailure data) {
    m_data = std::move(data);

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);

    if (!m_built) {
        m_built = true;
        build();
    }
    sync_from_data();

    present();

    // Same deferred-focus dance as ImageInfoDialog (s125 m1h): grab the
    // Close button AFTER present() via signal_idle so GTK4's initial
    // focus-traversal walk doesn't leave a selection cursor on a value
    // Entry. The singleton outlives the idle (it's a MainWindow member),
    // so capture-by-pointer is safe across re-opens.
    if (m_btn_close) {
        Gtk::Widget* w = m_btn_close;
        set_focus(*w);
        auto* btn = m_btn_close;
        Glib::signal_idle().connect_once([btn]() { btn->grab_focus(); });
    }
}

// ── build ─────────────────────────────────────────────────────────────────
void ImportFailureDialog::build() {
    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    // Heading — a plain prominent line above the grid. Left-aligned,
    // title-class so the motif stylesheet can weight it.
    m_heading = Gtk::make_managed<Gtk::Label>("Couldn't import this file");
    m_heading->set_halign(Gtk::Align::START);
    m_heading->set_margin_start(16);
    m_heading->set_margin_end(16);
    m_heading->set_margin_top(16);
    m_heading->set_margin_bottom(4);
    m_heading->add_css_class("title-4");   // GTK4 standard heading weight
    outer->append(*m_heading);

    m_grid = Gtk::make_managed<Gtk::Grid>();
    m_grid->set_row_spacing(6);
    m_grid->set_column_spacing(12);
    m_grid->set_margin(16);
    m_grid->set_margin_top(8);
    m_grid->set_margin_bottom(8);

    m_row_labels.reserve(k_row_count);
    m_value_entries.reserve(k_row_count);
    for (std::size_t i = 0; i < k_row_count; ++i) {
        add_row(row_specs[i].label, static_cast<int>(i));
    }

    outer->append(*m_grid);

    // Close button row — right-aligned, takes initial focus.
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    btn_row->set_margin_start(16);
    btn_row->set_margin_end(16);
    btn_row->set_margin_bottom(12);
    btn_row->set_halign(Gtk::Align::END);

    m_btn_close = Gtk::make_managed<curvz::widgets::Button>(
            "dlg_importfail_close");
    // long-name: import_failure_dialog_close_btn
    m_btn_close->set_label("Close");
    m_btn_close->signal_clicked().connect([this]() { close(); });
    btn_row->append(*m_btn_close);
    outer->append(*btn_row);

    set_child(*outer);

    Gtk::Widget* w = m_btn_close;
    w->set_receives_default(true);
    set_default_widget(*w);

    LOG_DEBUG("ImportFailureDialog: built widget tree");
}

void ImportFailureDialog::add_row(const char* name, int row) {
    auto* lbl_name = Gtk::make_managed<Gtk::Label>(name);
    lbl_name->set_halign(Gtk::Align::END);
    lbl_name->set_valign(Gtk::Align::CENTER);
    lbl_name->add_css_class("dim-label");

    auto* ent_val = Gtk::make_managed<curvz::widgets::Entry>(
        curvz::scripting::unregistered);
    ent_val->set_editable(false);
    ent_val->set_can_focus(true);
    ent_val->set_hexpand(true);
    ent_val->add_css_class("flat");
    ent_val->set_width_chars(48);

    m_grid->attach(*lbl_name, 0, row, 1, 1);
    m_grid->attach(*ent_val,  1, row, 1, 1);

    m_row_labels.push_back(lbl_name);
    m_value_entries.push_back(ent_val);
}

// ── sync_from_data ────────────────────────────────────────────────────────
void ImportFailureDialog::sync_from_data() {
    for (std::size_t i = 0; i < k_row_count; ++i) {
        const std::string& val = m_data.*(row_specs[i].field);
        m_value_entries[i]->set_text(val);

        if (row_specs[i].optional) {
            const bool show_row = !val.empty();
            m_row_labels[i]->set_visible(show_row);
            m_value_entries[i]->set_visible(show_row);
        }
    }
}

} // namespace Curvz
