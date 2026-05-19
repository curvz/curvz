// ImageInfoDialog.cpp ────────────────────────────────────────────────────────
//
// s210 m1 — implementation of the Image-Info read-only properties
// dialog. See ImageInfoDialog.hpp for the rationale and lifetime model.
//
// All visual decisions (frameless flat-class Entries, dim-label name
// column, right-aligned Close, 480px width, non-resizable) are copied
// verbatim from the s125 m1g/m1h/m1j inline form that previously lived
// in Canvas.cpp's right-click handler. The s125 m1j commentary on
// why-Entry-not-Label is preserved as inline comments — it's earned
// context, not boilerplate.
//
#include "ImageInfoDialog.hpp"

#include "widgets/Entry.hpp"   // s214 m2 — substrate Entry
#include "curvz_utils.hpp"   // set_name, apply_motif_class_from_parent
#include "CurvzLog.hpp"

#include <glibmm/main.h>     // signal_idle

namespace Curvz {

namespace {

// Row roster, in display order. Member-pointer-to-string lets
// sync_from_data() walk the roster + m_data side-by-side without naming
// each field three times.
struct RowSpec {
    const char* label;
    std::string ImageInfo::* field;
    bool optional;   // hide the row when the field is empty
};

constexpr std::size_t k_row_count = 9;

// Format / Depth are "optional" — the original inline form conditionally
// appended them when meta.format / meta.depth were non-empty. We build
// the row tree once and toggle set_visible() per-show to preserve the
// same blank-when-unknown UX without rebuilding.
RowSpec row_specs[k_row_count] = {
    {"Name",      &ImageInfo::filename,    false},
    {"Path",      &ImageInfo::full_path,   false},
    {"Pixels",    &ImageInfo::pixels,      false},
    {"Format",    &ImageInfo::format,      true},
    {"Depth",     &ImageInfo::depth,       true},
    {"File size", &ImageInfo::file_size,   false},
    {"Modified",  &ImageInfo::modified,    false},
    {"Placed",    &ImageInfo::placed_size, false},
    {"Linkage",   &ImageInfo::linkage,     false},
};

} // namespace

// ── ctor ──────────────────────────────────────────────────────────────────
//
// s210 m1 — default-constructed once as a MainWindow member; show() is
// what callers invoke per right-click. The widget tree builds lazily on
// the first show() via the m_built latch and stays in the tree for the
// app's lifetime; subsequent show()s only refresh values via
// sync_from_data().
ImageInfoDialog::ImageInfoDialog() {
    set_title("Image Info");
    // The original inline form was modal=true. The s200/s201 precedent
    // is non-modal, but those are *editors* — keeping a sibling window
    // accessible while editing is genuinely useful there. This dialog
    // is read-only / dismissable, and the user opened it from a
    // canvas right-click; modal-true matches the prior behaviour and
    // the read-only-popup convention. Leave modal as-is.
    set_modal(true);
    set_resizable(false);
    // s125 m1j: with read-only Entry values (no wrap), the measure pass
    // is stable — no width-for-height feedback loop. The earlier (m1g)
    // attempt at set_default_size + set_resizable(false) + wrapping
    // labels caused continuous "needs at least N" warnings on hover.
    // 480px is the width that fits typical paths without horizontal
    // scroll while staying comfortable on small screens.
    set_default_size(480, -1);
    // s210 m1 — the singleton shape Curvz uses for long-lived dialogs.
    // close() now hides the window; the next show() re-presents it
    // populated from the new payload.
    set_hide_on_close(true);
    // Window name + long-name annotation kept for CSS hooks and
    // widget_names_sync ingestion. The substrate widgets register
    // themselves separately on construction; this is purely the
    // GTK-side name (per the s208 m5 "both naming systems coexist"
    // precedent).
    curvz::utils::set_name(*this, "dlg_imginfo", "image_info_dialog_root");
}

// ── show ──────────────────────────────────────────────────────────────────
void ImageInfoDialog::show(Gtk::Window& parent, ImageInfo data) {
    m_data = std::move(data);

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);

    if (!m_built) {
        m_built = true;
        build();
    }
    sync_from_data();   // always re-sync; build() populates from empty defaults

    present();

    // s125 m1h: grab focus AFTER present(), and defer through signal_idle
    // so we run after GTK4's initial focus-traversal walk. Without the
    // defer, selectable text claims focus during the first arrange pass
    // and our grab_focus() is silently overridden — the dialog would
    // open with a selection cursor on the first value field instead of
    // the Close button. Calling set_focus on the window plus a deferred
    // grab_focus covers both the window-level focus and the widget-
    // level focus state. The idle fires on the next event-loop
    // iteration, before any user input can reach the dialog. With the
    // hide-on-close singleton the dialog now outlives the idle for
    // certain (it's a MainWindow member), so the capture-by-pointer is
    // safe across all subsequent re-opens.
    if (m_btn_close) {
        // Bind via the GTK widget surface, not the substrate wrapper,
        // because set_focus / grab_focus are GTK-side operations.
        Gtk::Widget* w = m_btn_close;
        set_focus(*w);
        auto* btn = m_btn_close;
        Glib::signal_idle().connect_once([btn]() { btn->grab_focus(); });
    }
}

// ── build ─────────────────────────────────────────────────────────────────
//
// One-shot widget-tree construction. Called from the first show().
// Mirrors the s125 m1g inline form 1:1; only the lifetime story has
// changed (heap-allocated → singleton member).
void ImageInfoDialog::build() {
    auto* outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    m_grid = Gtk::make_managed<Gtk::Grid>();
    m_grid->set_row_spacing(6);
    m_grid->set_column_spacing(12);
    m_grid->set_margin(16);
    m_grid->set_margin_bottom(8);

    // s125 m1j: value column is a read-only Gtk::Entry, not a selectable
    // Gtk::Label. The Label approach (m1g/m1h) produced a "Trying to
    // measure dlg_imginfo for height of N, but it needs at least M"
    // warning storm on hover — selectable+wrapping labels in a non-fixed
    // container have an unstable measure cycle, and tooltip hover-
    // timeout re-measurements hit it on every hover. Entries don't
    // wrap, scroll horizontally for long values, and have full keyboard
    // selection (double-click word, triple-click all, Ctrl+A, Ctrl+C).
    // They also visually signal "this is data you can interact with"
    // better than a static label, which is what we want here.
    m_row_labels.reserve(k_row_count);
    m_value_entries.reserve(k_row_count);
    for (std::size_t i = 0; i < k_row_count; ++i) {
        add_row(row_specs[i].label, static_cast<int>(i));
    }

    outer->append(*m_grid);

    // Close button row — right-aligned, takes initial focus so the
    // selectable text doesn't get a focus ring on open.
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    btn_row->set_margin_start(16);
    btn_row->set_margin_end(16);
    btn_row->set_margin_bottom(12);
    btn_row->set_halign(Gtk::Align::END);

    // s210 m1 — substrate-registered Close button. The abbrev
    // `dlg_imginfo_close` is what makes this dialog script-addressable
    // ("click the Close button on the Image Info dialog"). The s208 m5
    // precedent says the set_name long-name annotation can stay as a
    // sibling comment for widget_names_sync ingestion; the substrate
    // wrapper itself doesn't take a long-name parameter.
    m_btn_close = Gtk::make_managed<curvz::widgets::Button>(
            "dlg_imginfo_close");
    // long-name: image_info_dialog_close_btn
    m_btn_close->set_label("Close");
    m_btn_close->signal_clicked().connect([this]() { close(); });
    btn_row->append(*m_btn_close);
    outer->append(*btn_row);

    set_child(*outer);

    // Enter activates Close (default widget). GTK4 doesn't auto-bind
    // Esc for transient windows; the X button + Close button cover
    // dismissal.
    Gtk::Widget* w = m_btn_close;
    w->set_receives_default(true);
    set_default_widget(*w);

    LOG_DEBUG("ImageInfoDialog: built widget tree");
}

void ImageInfoDialog::add_row(const char* name, int row) {
    auto* lbl_name = Gtk::make_managed<Gtk::Label>(name);
    lbl_name->set_halign(Gtk::Align::END);
    lbl_name->set_valign(Gtk::Align::CENTER);
    lbl_name->add_css_class("dim-label");   // GTK4 standard dim style

    // s214 m2: unregistered substrate Entry — N row instances per dialog
    // build, sharing the role of "row N of an info display." Per-row
    // addressability is model-Scriptables territory (the image
    // SceneNode's metadata, addressed via iid), not per-widget.
    auto* ent_val = Gtk::make_managed<curvz::widgets::Entry>(
        curvz::scripting::unregistered);
    ent_val->set_editable(false);
    ent_val->set_can_focus(true);   // need focus for keyboard selection
    ent_val->set_hexpand(true);
    // Frameless visual treatment — looks more like a value display
    // than a text-entry field, while keeping all the selection
    // affordances. `.flat` is the GTK4 standard "show me as
    // transparent / no border" class.
    ent_val->add_css_class("flat");
    // Width hint — typical fields fit comfortably; long paths scroll
    // horizontally rather than blowing out the dialog.
    ent_val->set_width_chars(48);

    m_grid->attach(*lbl_name, 0, row, 1, 1);
    m_grid->attach(*ent_val,  1, row, 1, 1);

    m_row_labels.push_back(lbl_name);
    m_value_entries.push_back(ent_val);
}

// ── sync_from_data ────────────────────────────────────────────────────────
//
// Refresh every value Entry from m_data. Toggles visibility on the two
// optional rows (format, depth) to preserve the blank-when-unknown UX
// of the original inline form — those rows simply weren't appended
// when the meta read came back empty. set_visible() on both halves
// of the row keeps the grid layout coherent.
void ImageInfoDialog::sync_from_data() {
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
