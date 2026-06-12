#include "TextStyleEditorDialog.hpp"

#include "curvz_utils.hpp"
#include "style/TextStyleLibrary.hpp"
#include "UnitSystem.hpp"
#include "CurvzLog.hpp"

#include "color/Paint.hpp"
#include "color/Color.hpp"

#include <gtkmm/box.h>
#include <gtkmm/grid.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>

#include <cairomm/context.h>
#include <pango/pangocairo.h>
#include <glib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>
#include <vector>

namespace Curvz {

namespace {

constexpr const char* kCategorySentinelNew  = "+ New category…";
constexpr const char* kCategoryUncategorised = "(uncategorised)";
constexpr const char* kParentRoot            = "(none — root)";
constexpr const char* kInherit               = "(inherit)";

// Session-stable list of font families from the default Pango map, sorted
// case-insensitively. Same enumeration the StyleBar font popover uses.
std::vector<std::string> enumerate_families() {
    std::vector<std::string> families;
    PangoFontMap* fm = pango_cairo_font_map_get_default();
    PangoFontFamily** fams = nullptr;
    int n = 0;
    pango_font_map_list_families(fm, &fams, &n);
    families.reserve(n);
    for (int i = 0; i < n; ++i)
        families.emplace_back(pango_font_family_get_name(fams[i]));
    g_free(fams);
    std::sort(families.begin(), families.end(),
              [](const std::string& a, const std::string& b) {
                  return g_ascii_strcasecmp(a.c_str(), b.c_str()) < 0;
              });
    return families;
}

Glib::RefPtr<Gtk::StringList> make_string_list(const std::vector<std::string>& v) {
    auto sl = Gtk::StringList::create({});
    for (const auto& s : v) sl->append(s);
    return sl;
}

// s342 — the colour MenuButton's swatch face: a rounded chip filled with the
// current colour, or a hatched "no fill" mark when None is set. Cheap; redrawn
// on every colour/None change via queue_draw.
void draw_colour_swatch(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h,
                        const color::Color& c, bool is_none) {
    const double r = 3.0;
    cr->arc(r, r, r, M_PI, 1.5 * M_PI);
    cr->arc(w - r, r, r, 1.5 * M_PI, 2.0 * M_PI);
    cr->arc(w - r, h - r, r, 0.0, 0.5 * M_PI);
    cr->arc(r, h - r, r, 0.5 * M_PI, M_PI);
    cr->close_path();
    if (is_none) {
        cr->set_source_rgb(0.18, 0.18, 0.18);
        cr->fill_preserve();
        cr->set_source_rgb(0.85, 0.25, 0.25);
        cr->set_line_width(1.5);
        cr->move_to(3.0, h - 3.0);
        cr->line_to(w - 3.0, 3.0);
        cr->stroke_preserve();
    } else {
        cr->set_source_rgb(c.r, c.g, c.b);
        cr->fill_preserve();
    }
    cr->set_source_rgba(1.0, 1.0, 1.0, 0.25);
    cr->set_line_width(1.0);
    cr->stroke();
}

}  // namespace

// ── ctor ──────────────────────────────────────────────────────────────────
TextStyleEditorDialog::TextStyleEditorDialog() {
    set_modal(false);
    set_resizable(false);
    set_default_size(520, -1);
    set_hide_on_close(true);
    curvz::utils::set_name(*this, "dlg_tse", "text_style_editor_dialog_window");

    signal_close_request().connect(
        [this]() -> bool {
            LOG_DEBUG("TextStyleEditorDialog: close-request — discarding "
                      "pending commit callback");
            m_on_committed = nullptr;
            return false;  // hide-on-close
        }, /*after=*/false);
}

// ── show ────────────────────────────────────────────────────────────────────
void TextStyleEditorDialog::show(Gtk::Window& parent,
                                 const style::TextStyleLibrary& library,
                                 CanvasModel* canvas_model,
                                 const std::vector<std::string>& user_categories,
                                 Mode mode,
                                 style::TextStyle initial,
                                 CommittedFn on_committed) {
    m_library      = &library;
    m_canvas_model = canvas_model;
    m_mode         = mode;
    m_working      = std::move(initial);
    m_on_committed = std::move(on_committed);

    // Category-order list for this show (parallel to the dropdown).
    m_category_order.clear();
    m_category_order.push_back(std::string());            // "" = uncategorised
    for (const auto& c : user_categories) {
        if (c.empty()) continue;
        m_category_order.push_back(c);
    }
    m_category_order.push_back(std::string(kCategorySentinelNew));

    const char* title =
        (m_mode == Mode::Edit)      ? "Edit text style" :
        (m_mode == Mode::Duplicate) ? "Edit a copy"     :
                                      "New text style";
    set_title(title);

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);

    if (!m_built) {
        m_built = true;
        build();           // build() ends by calling sync_from_working()
    } else {
        sync_from_working();
    }

    present();
}

// ── build ─────────────────────────────────────────────────────────────────
Gtk::Label* TextStyleEditorDialog::make_section_label(const char* text) {
    auto* lbl = Gtk::make_managed<Gtk::Label>(text);
    lbl->set_xalign(0.0f);
    lbl->add_css_class("curvz-inspector-group-title");  // toned chrome (s329)
    lbl->set_margin_top(6);
    return lbl;
}

Gtk::Box* TextStyleEditorDialog::make_row(Gtk::Box& root, const char* label,
                                          Gtk::Widget& control,
                                          Gtk::CheckButton* override_check) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    if (override_check) {
        override_check->set_label(label);
        override_check->set_tooltip_text("Override this field (else inherit)");
        override_check->set_size_request(150, -1);
        override_check->set_halign(Gtk::Align::START);
        row->append(*override_check);
    } else {
        auto* lbl = Gtk::make_managed<Gtk::Label>(label);
        lbl->set_xalign(0.0f);
        lbl->set_size_request(150, -1);
        row->append(*lbl);
    }
    control.set_hexpand(true);
    row->append(control);
    root.append(*row);
    return row;
}

void TextStyleEditorDialog::build() {
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    root->set_margin(12);
    set_child(*root);

    // ── Local layout helpers ────────────────────────────────────────────
    // An override-checkbox (whose label names the field) + its pt spin + the
    // "pt" unit label, packed tight into one cell.
    auto ov_spin_cell = [](Gtk::CheckButton* ov, const char* label,
                           CurvzSpinButton* sp) -> Gtk::Box* {
        ov->set_label(label);
        auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        sp->set_hexpand(true);
        cell->append(*ov);
        cell->append(*sp);
        if (auto* u = sp->get_unit_label()) cell->append(*u);
        return cell;
    };
    // A plain "label  control" cell (control hexpands).
    auto label_cell = [](const char* label, Gtk::Widget& ctrl) -> Gtk::Box* {
        auto* cell = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_xalign(0.0f);
        ctrl.set_hexpand(true);
        cell->append(*l);
        cell->append(ctrl);
        return cell;
    };
    // Two cells, side by side, each half the row.
    auto pair_row = [&](Gtk::Box* a, Gtk::Box* b) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
        a->set_hexpand(true);
        b->set_hexpand(true);
        row->append(*a);
        row->append(*b);
        root->append(*row);
    };

    // ── Identity ────────────────────────────────────────────────────────
    root->append(*make_section_label("IDENTITY"));

    m_name_entry = Gtk::make_managed<Gtk::Entry>();
    m_name_entry->set_placeholder_text("Style name");
    curvz::utils::set_name(m_name_entry, "dlg_tse_name",
                           "text_style_editor_dialog_name_entry");
    make_row(*root, "Name", *m_name_entry);

    // Category dropdown + inline new-category entry in ONE row (the entry shares
    // the row's width, so revealing it never needs the dialog to grow taller).
    m_category_dd = Gtk::make_managed<curvz::widgets::DropDown>(
        "dlg_tse_cat", std::vector<Glib::ustring>{kCategoryUncategorised});
    m_category_new_entry = Gtk::make_managed<Gtk::Entry>();
    m_category_new_entry->set_placeholder_text("New category name");
    m_category_new_entry->set_visible(false);
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* lbl = Gtk::make_managed<Gtk::Label>("Category");
        lbl->set_xalign(0.0f);
        lbl->set_size_request(110, -1);
        row->append(*lbl);
        m_category_dd->set_hexpand(true);
        m_category_new_entry->set_hexpand(true);
        row->append(*m_category_dd);
        row->append(*m_category_new_entry);
        root->append(*row);
    }
    m_category_dd->property_selected().signal_changed().connect([this]() {
        if (m_syncing) return;
        if (!m_category_dd || !m_category_new_entry) return;
        const auto idx = m_category_dd->get_selected();
        if (idx == GTK_INVALID_LIST_POSITION ||
            static_cast<std::size_t>(idx) >= m_category_order.size()) return;
        const bool is_sentinel = (m_category_order[idx] == kCategorySentinelNew);
        m_category_new_entry->set_visible(is_sentinel);
        if (is_sentinel) {
            m_category_new_entry->set_text("");
            m_category_new_entry->grab_focus();
        }
    });

    m_parent_dd = Gtk::make_managed<curvz::widgets::DropDown>(
        "dlg_tse_parent", std::vector<Glib::ustring>{kParentRoot});
    make_row(*root, "Inherits from", *m_parent_dd);
    m_parent_dd->property_selected().signal_changed().connect([this]() {
        if (m_syncing) return;
        apply_inherited_previews();
    });

    // ── Character ─────────────────────────────────────────────────────────
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    root->append(*make_section_label("CHARACTER"));

    // Family (inline (inherit) at index 0).
    {
        m_family_order.clear();
        m_family_order.push_back(std::string());  // "" = inherit
        std::vector<Glib::ustring> opts{kInherit};
        for (const auto& f : enumerate_families()) {
            m_family_order.push_back(f);
            opts.emplace_back(f);
        }
        m_family_dd = Gtk::make_managed<curvz::widgets::DropDown>(
            "dlg_tse_family", opts);
        make_row(*root, "Family", *m_family_dd);
    }

    // Size + Letter spacing share a row (both pt; letter spacing is a CHARACTER
    // trait — per-glyph tracking, not paragraph — so it lives here).
    m_size_ov  = Gtk::make_managed<Gtk::CheckButton>();
    m_size_sp  = Gtk::make_managed<CurvzSpinButton>("dlg_tse_size", SpinType::Width);
    m_size_sp->set_unit_override(Unit::Pt);
    m_track_ov = Gtk::make_managed<Gtk::CheckButton>();
    m_track_sp = Gtk::make_managed<CurvzSpinButton>("dlg_tse_track", SpinType::Width);
    m_track_sp->set_unit_override(Unit::Pt);
    m_track_sp->set_tooltip_text("Letter spacing (tracking)");
    pair_row(ov_spin_cell(m_size_ov, "Size", m_size_sp),
             ov_spin_cell(m_track_ov, "Spacing", m_track_sp));
    m_size_ov->signal_toggled().connect([this]() {
        if (m_syncing) return;
        set_size_override(m_size_ov->get_active(), true);
    });
    m_track_ov->signal_toggled().connect([this]() {
        if (m_syncing) return;
        set_track_override(m_track_ov->get_active(), true);
    });

    // Bold + Italic share a row.
    m_bold_dd = Gtk::make_managed<curvz::widgets::DropDown>(
        "dlg_tse_bold", std::vector<Glib::ustring>{kInherit, "Off", "On"});
    m_italic_dd = Gtk::make_managed<curvz::widgets::DropDown>(
        "dlg_tse_italic", std::vector<Glib::ustring>{kInherit, "Off", "On"});
    pair_row(label_cell("Bold", *m_bold_dd), label_cell("Italic", *m_italic_dd));

    // Colour — a compact swatch MenuButton; the full picker lives in its
    // popover (mirrors the StyleBar fill chip) so it no longer dominates the
    // dialog. The swatch face reflects the live colour / None state.
    m_colour_ov     = Gtk::make_managed<Gtk::CheckButton>("Colour");
    m_colour_none   = Gtk::make_managed<Gtk::CheckButton>("None");
    m_colour_btn    = Gtk::make_managed<Gtk::MenuButton>();
    m_colour_swatch = Gtk::make_managed<Gtk::DrawingArea>();
    m_colour_swatch->set_content_width(40);
    m_colour_swatch->set_content_height(18);
    m_colour_swatch->set_draw_func(
        [this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            draw_colour_swatch(cr, w, h, m_colour_value,
                               m_colour_none && m_colour_none->get_active());
        });
    m_colour_btn->set_child(*m_colour_swatch);
    m_colour_btn->set_tooltip_text("Pick the style's text colour");
    {
        auto* pop = Gtk::make_managed<Gtk::Popover>();
        m_colour_picker = Gtk::make_managed<CurvzColorPicker>();
        m_colour_picker->set_with_alpha(false);  // Pango foreground is opaque RGB
        m_colour_picker->set_initial(color::Color(0.0, 0.0, 0.0, 1.0));
        m_colour_picker->signal_changed().connect([this](color::Color c) {
            m_colour_value = c;
            if (m_colour_none) m_colour_none->set_active(false);  // a solid pick
            if (m_colour_swatch) m_colour_swatch->queue_draw();
        });
        pop->set_child(*m_colour_picker);
        m_colour_btn->set_popover(*pop);
    }
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        m_colour_ov->set_label("Colour");
        m_colour_ov->set_size_request(110, -1);
        row->append(*m_colour_ov);
        m_colour_btn->set_hexpand(false);
        row->append(*m_colour_btn);
        row->append(*m_colour_none);
        root->append(*row);
    }
    m_colour_ov->signal_toggled().connect([this]() {
        if (m_syncing) return;
        set_colour_override(m_colour_ov->get_active(), true);
    });
    m_colour_none->signal_toggled().connect([this]() {
        if (m_syncing) return;
        if (m_colour_swatch) m_colour_swatch->queue_draw();
    });

    // ── Paragraph ───────────────────────────────────────────────────────
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    root->append(*make_section_label("PARAGRAPH"));

    // Alignment + Leading share a row.
    m_align_dd = Gtk::make_managed<curvz::widgets::DropDown>(
        "dlg_tse_align",
        std::vector<Glib::ustring>{kInherit, "Left", "Center", "Right", "Justify"});
    m_leading_ov = Gtk::make_managed<Gtk::CheckButton>();
    m_leading_sp = Gtk::make_managed<CurvzSpinButton>("dlg_tse_leading", SpinType::Width);
    m_leading_sp->set_unit_override(Unit::Pt);
    m_leading_sp->set_tooltip_text("Line height in points (0 = auto / metric)");
    pair_row(label_cell("Align", *m_align_dd),
             ov_spin_cell(m_leading_ov, "Leading", m_leading_sp));
    m_leading_ov->signal_toggled().connect([this]() {
        if (m_syncing) return;
        set_leading_override(m_leading_ov->get_active(), true);
    });

    // Indents — one override gates the trio; each field carries a header label
    // ABOVE it (Left / Right / First line) so the three boxes are unambiguous.
    m_indent_ov = Gtk::make_managed<Gtk::CheckButton>("Indents");
    m_indent_left_sp  = Gtk::make_managed<CurvzSpinButton>("dlg_tse_ind_l", SpinType::Distance, m_canvas_model);
    m_indent_right_sp = Gtk::make_managed<CurvzSpinButton>("dlg_tse_ind_r", SpinType::Distance, m_canvas_model);
    m_indent_first_sp = Gtk::make_managed<CurvzSpinButton>("dlg_tse_ind_f", SpinType::Distance, m_canvas_model);
    m_indent_first_sp->set_tooltip_text("First-line indent (negative = hanging)");
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        m_indent_ov->set_valign(Gtk::Align::END);  // align with the spin row
        m_indent_ov->set_size_request(110, -1);
        row->append(*m_indent_ov);

        auto* grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_column_spacing(6);
        grid->set_row_spacing(2);
        grid->set_hexpand(true);
        auto hdr = [&](const char* t, int col) {
            auto* l = Gtk::make_managed<Gtk::Label>(t);
            l->set_xalign(0.0f);
            l->add_css_class("dim-label");
            grid->attach(*l, col, 0, 1, 1);
        };
        hdr("Left", 0);
        hdr("Right", 1);
        hdr("First line", 2);
        m_indent_left_sp->set_hexpand(true);
        m_indent_right_sp->set_hexpand(true);
        m_indent_first_sp->set_hexpand(true);
        grid->attach(*m_indent_left_sp,  0, 1, 1, 1);
        grid->attach(*m_indent_right_sp, 1, 1, 1, 1);
        grid->attach(*m_indent_first_sp, 2, 1, 1, 1);
        if (auto* u = m_indent_left_sp->get_unit_label()) grid->attach(*u, 3, 1, 1, 1);
        row->append(*grid);
        root->append(*row);
    }
    m_indent_ov->signal_toggled().connect([this]() {
        if (m_syncing) return;
        set_indent_override(m_indent_ov->get_active(), true);
    });

    // ── Buttons ───────────────────────────────────────────────────────────
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    {
        auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        btn_row->set_halign(Gtk::Align::END);
        btn_row->set_margin_top(6);
        m_btn_cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        curvz::utils::set_name(m_btn_cancel, "dlg_tse_cancel",
                               "text_style_editor_dialog_cancel");
        m_btn_ok = Gtk::make_managed<Gtk::Button>("OK");
        curvz::utils::set_name(m_btn_ok, "dlg_tse_ok",
                               "text_style_editor_dialog_ok");
        m_btn_ok->add_css_class("suggested-action");
        m_btn_cancel->signal_clicked().connect(
            sigc::mem_fun(*this, &TextStyleEditorDialog::on_cancel));
        m_btn_ok->signal_clicked().connect(
            sigc::mem_fun(*this, &TextStyleEditorDialog::on_ok));
        btn_row->append(*m_btn_cancel);
        btn_row->append(*m_btn_ok);
        root->append(*btn_row);
    }

    sync_from_working();
}

// ── rebuild_parent_options ──────────────────────────────────────────────────
void TextStyleEditorDialog::rebuild_parent_options() {
    if (!m_parent_dd || !m_library) return;

    // Descendant test: candidate C is a descendant of the edited id E if
    // walking C's parent chain reaches E (visited-guarded). New/Duplicate have
    // no own id yet (E empty) -> nothing excluded.
    const style::TextStyleId edited =
        (m_mode == Mode::Edit) ? m_working.header.id : std::string();
    auto is_descendant = [this](const style::TextStyleId& start,
                                const style::TextStyleId& ancestor) -> bool {
        if (ancestor.empty()) return false;
        std::vector<style::TextStyleId> seen;
        style::TextStyleId cur = start;
        while (!cur.empty()) {
            if (cur == ancestor) return true;
            if (std::find(seen.begin(), seen.end(), cur) != seen.end()) break;
            seen.push_back(cur);
            const style::TextStyle* s = m_library->find_text_style(cur);
            if (!s) break;
            cur = s->header.parent_id;
        }
        return false;
    };

    m_parent_order.clear();
    std::vector<std::string> labels;
    m_parent_order.push_back(std::string());     // index 0 = root / none
    labels.emplace_back(kParentRoot);

    auto consider = [&](const style::TextStyle* s) {
        if (!s) return;
        const style::TextStyleId& id = s->header.id;
        if (!edited.empty() && id == edited) return;          // not self
        if (is_descendant(id, edited)) return;                // no cycles
        m_parent_order.push_back(id);
        labels.push_back(s->header.name.empty() ? id : s->header.name);
    };
    for (const auto& cat : m_library->app_categories())
        for (const auto* s : m_library->app_styles_in_category(cat)) consider(s);
    for (const auto& cat : m_library->user_categories())
        for (const auto* s : m_library->user_styles_in_category(cat)) consider(s);

    m_parent_dd->set_model(make_string_list(labels));
    // Select the working parent (or fall back to root).
    guint sel = 0;
    for (std::size_t i = 0; i < m_parent_order.size(); ++i)
        if (m_parent_order[i] == m_working.header.parent_id) { sel = (guint)i; break; }
    m_parent_dd->set_selected(sel);
}

// ── inherited previews ──────────────────────────────────────────────────────
void TextStyleEditorDialog::apply_inherited_previews() {
    if (!m_library) return;
    style::TextStyleId parent;
    if (m_parent_dd) {
        const auto idx = m_parent_dd->get_selected();
        if (idx != GTK_INVALID_LIST_POSITION &&
            static_cast<std::size_t>(idx) < m_parent_order.size())
            parent = m_parent_order[idx];
    }
    const style::ResolvedTextStyle r = m_library->resolve(parent);

    // s346 — CurvzSpinButton's internal value IS doc-px (the widget owns the
    // pt display conversion via the unit override); wrapping it in
    // from_px/to_px here double-converted. The read and write errors
    // cancelled INSIDE the dialog — it always displayed the typed number —
    // while the committed value was inflated by 96/72, so styled text
    // rendered (and the s346 chip face reported) 4/3 of the typed size.
    // The indent rows below were always raw-px, the correct idiom.
    if (m_size_sp && m_size_ov && !m_size_ov->get_active())
        m_size_sp->set_internal_value(r.size);
    if (m_leading_sp && m_leading_ov && !m_leading_ov->get_active())
        m_leading_sp->set_internal_value(r.leading_px);
    if (m_track_sp && m_track_ov && !m_track_ov->get_active())
        m_track_sp->set_internal_value(r.letter_spacing);
    if (m_indent_ov && !m_indent_ov->get_active()) {
        if (m_indent_left_sp)  m_indent_left_sp->set_internal_value(r.indent_left_px);
        if (m_indent_right_sp) m_indent_right_sp->set_internal_value(r.indent_right_px);
        if (m_indent_first_sp) m_indent_first_sp->set_internal_value(r.indent_first_px);
    }
    if (m_colour_ov && !m_colour_ov->get_active() && m_colour_none) {
        bool is_none = std::holds_alternative<color::None>(r.colour);
        m_colour_none->set_active(is_none);
        color::Color c = color::Color::black();
        if (const auto* sol = std::get_if<color::Solid>(&r.colour)) c = sol->color;
        m_colour_value = c;
        if (m_colour_picker) m_colour_picker->set_initial(c);  // popover starts here
        if (m_colour_swatch) m_colour_swatch->queue_draw();
    }
}

// ── override setters ────────────────────────────────────────────────────────
void TextStyleEditorDialog::set_size_override(bool on, bool reload_preview) {
    if (m_size_sp) m_size_sp->set_sensitive(on);
    if (!on && reload_preview) apply_inherited_previews();
}
void TextStyleEditorDialog::set_leading_override(bool on, bool reload_preview) {
    if (m_leading_sp) m_leading_sp->set_sensitive(on);
    if (!on && reload_preview) apply_inherited_previews();
}
void TextStyleEditorDialog::set_track_override(bool on, bool reload_preview) {
    if (m_track_sp) m_track_sp->set_sensitive(on);
    if (!on && reload_preview) apply_inherited_previews();
}
void TextStyleEditorDialog::set_indent_override(bool on, bool reload_preview) {
    if (m_indent_left_sp)  m_indent_left_sp->set_sensitive(on);
    if (m_indent_right_sp) m_indent_right_sp->set_sensitive(on);
    if (m_indent_first_sp) m_indent_first_sp->set_sensitive(on);
    if (!on && reload_preview) apply_inherited_previews();
}
void TextStyleEditorDialog::set_colour_override(bool on, bool reload_preview) {
    if (m_colour_none) m_colour_none->set_sensitive(on);
    if (m_colour_btn)  m_colour_btn->set_sensitive(on);
    if (!on && reload_preview) apply_inherited_previews();
}

// ── sync_from_working ───────────────────────────────────────────────────────
void TextStyleEditorDialog::sync_from_working() {
    m_syncing = true;

    // Re-point the doc-unit indent spins at the active doc's model (the unit
    // display + conversion follow the doc; the pointer can change per show).
    if (m_indent_left_sp)  m_indent_left_sp->set_model(m_canvas_model);
    if (m_indent_right_sp) m_indent_right_sp->set_model(m_canvas_model);
    if (m_indent_first_sp) m_indent_first_sp->set_model(m_canvas_model);

    if (m_name_entry) m_name_entry->set_text(m_working.header.name);

    // Category dropdown.
    if (m_category_dd) {
        std::vector<std::string> labels;
        labels.reserve(m_category_order.size());
        for (const auto& c : m_category_order)
            labels.push_back(c.empty() ? std::string(kCategoryUncategorised) : c);
        // Fold an unknown existing category in just before the sentinel.
        const std::string& cur = m_working.header.category;
        guint sel = 0;
        bool found = false;
        for (std::size_t i = 0; i < m_category_order.size(); ++i)
            if (m_category_order[i] == cur) { sel = (guint)i; found = true; break; }
        if (!found && !cur.empty()) {
            m_category_order.insert(m_category_order.end() - 1, cur);
            labels.insert(labels.end() - 1, cur);
            sel = (guint)(m_category_order.size() - 2);
        }
        m_category_dd->set_model(make_string_list(labels));
        m_category_dd->set_selected(sel);
        if (m_category_new_entry) {
            m_category_new_entry->set_visible(false);
            m_category_new_entry->set_text("");
        }
    }

    // Parent dropdown (rebuilds options + selects).
    rebuild_parent_options();

    // Family.
    if (m_family_dd) {
        guint sel = 0;  // (inherit)
        if (m_working.chars.family) {
            for (std::size_t i = 0; i < m_family_order.size(); ++i)
                if (m_family_order[i] == *m_working.chars.family) { sel = (guint)i; break; }
        }
        m_family_dd->set_selected(sel);
    }

    // Bold / Italic.
    if (m_bold_dd)
        m_bold_dd->set_selected(!m_working.chars.bold ? 0 : (*m_working.chars.bold ? 2 : 1));
    if (m_italic_dd)
        m_italic_dd->set_selected(!m_working.chars.italic ? 0 : (*m_working.chars.italic ? 2 : 1));

    // Size.
    if (m_size_ov) {
        const bool on = m_working.chars.size.has_value();
        m_size_ov->set_active(on);
        if (on && m_size_sp)
            m_size_sp->set_internal_value(*m_working.chars.size);  // s346 — internal IS px
        set_size_override(on, /*reload_preview=*/false);
    }

    // Letter spacing.
    if (m_track_ov) {
        const bool on = m_working.chars.letter_spacing.has_value();
        m_track_ov->set_active(on);
        if (on && m_track_sp)
            m_track_sp->set_internal_value(*m_working.chars.letter_spacing);  // s346 — internal IS px
        set_track_override(on, false);
    }

    // Colour.
    if (m_colour_ov) {
        const bool on = m_working.chars.colour.has_value();
        m_colour_ov->set_active(on);
        if (on && m_colour_none) {
            bool is_none = std::holds_alternative<color::None>(*m_working.chars.colour);
            m_colour_none->set_active(is_none);
            color::Color c = color::Color::black();
            if (const auto* sol = std::get_if<color::Solid>(&*m_working.chars.colour))
                c = sol->color;
            m_colour_value = c;
            if (m_colour_picker) m_colour_picker->set_initial(c);
        }
        if (m_colour_swatch) m_colour_swatch->queue_draw();
        set_colour_override(on, false);
    }

    // Alignment.
    if (m_align_dd) {
        guint sel = 0;
        if (m_working.para.align)
            sel = (guint)(style::para_align_to_ivalue(*m_working.para.align) + 1);
        m_align_dd->set_selected(sel);
    }

    // Leading.
    if (m_leading_ov) {
        const bool on = m_working.para.leading_px.has_value();
        m_leading_ov->set_active(on);
        if (on && m_leading_sp)
            m_leading_sp->set_internal_value(*m_working.para.leading_px);  // s346 — internal IS px
        set_leading_override(on, false);
    }

    // Indents (the trio moves together).
    if (m_indent_ov) {
        const bool on = m_working.para.indent_left_px.has_value() ||
                        m_working.para.indent_right_px.has_value() ||
                        m_working.para.indent_first_px.has_value();
        m_indent_ov->set_active(on);
        if (on) {
            if (m_indent_left_sp)
                m_indent_left_sp->set_internal_value(m_working.para.indent_left_px.value_or(0.0));
            if (m_indent_right_sp)
                m_indent_right_sp->set_internal_value(m_working.para.indent_right_px.value_or(0.0));
            if (m_indent_first_sp)
                m_indent_first_sp->set_internal_value(m_working.para.indent_first_px.value_or(0.0));
        }
        set_indent_override(on, false);
    }

    // Fill the inherited previews into whatever stayed off.
    apply_inherited_previews();

    m_syncing = false;
}

// ── harvest_into_working ─────────────────────────────────────────────────────
void TextStyleEditorDialog::harvest_into_working() {
    // Identity. Name never empty (UUIDs must not surface).
    if (m_name_entry) {
        std::string n = m_name_entry->get_text().raw();
        // trim
        auto a = n.find_first_not_of(" \t");
        auto b = n.find_last_not_of(" \t");
        n = (a == std::string::npos) ? std::string() : n.substr(a, b - a + 1);
        m_working.header.name = n.empty() ? std::string("Text style") : n;
    }
    if (m_category_dd) {
        const auto idx = m_category_dd->get_selected();
        if (idx != GTK_INVALID_LIST_POSITION &&
            static_cast<std::size_t>(idx) < m_category_order.size()) {
            const std::string& choice = m_category_order[idx];
            if (choice == kCategorySentinelNew && m_category_new_entry) {
                std::string n = m_category_new_entry->get_text().raw();
                auto a = n.find_first_not_of(" \t");
                auto b = n.find_last_not_of(" \t");
                n = (a == std::string::npos) ? std::string()
                                             : n.substr(a, b - a + 1);
                m_working.header.category = n;  // empty -> (uncategorised)
            } else {
                m_working.header.category = choice;  // "" for uncategorised
            }
        }
    }
    if (m_parent_dd) {
        const auto idx = m_parent_dd->get_selected();
        if (idx != GTK_INVALID_LIST_POSITION &&
            static_cast<std::size_t>(idx) < m_parent_order.size())
            m_working.header.parent_id = m_parent_order[idx];
    }

    // Character.
    if (m_family_dd) {
        const auto idx = m_family_dd->get_selected();
        if (idx == 0 || idx == GTK_INVALID_LIST_POSITION ||
            static_cast<std::size_t>(idx) >= m_family_order.size())
            m_working.chars.family.reset();
        else
            m_working.chars.family = m_family_order[idx];
    }
    if (m_size_ov && m_size_sp) {
        if (m_size_ov->get_active())
            m_working.chars.size = m_size_sp->get_internal_value();  // s346 — already px
        else
            m_working.chars.size.reset();
    }
    if (m_bold_dd) {
        const auto idx = m_bold_dd->get_selected();
        if (idx == 1) m_working.chars.bold = false;
        else if (idx == 2) m_working.chars.bold = true;
        else m_working.chars.bold.reset();
    }
    if (m_italic_dd) {
        const auto idx = m_italic_dd->get_selected();
        if (idx == 1) m_working.chars.italic = false;
        else if (idx == 2) m_working.chars.italic = true;
        else m_working.chars.italic.reset();
    }
    if (m_track_ov && m_track_sp) {
        if (m_track_ov->get_active())
            m_working.chars.letter_spacing =
                m_track_sp->get_internal_value();  // s346 — already px
        else
            m_working.chars.letter_spacing.reset();
    }
    if (m_colour_ov) {
        if (!m_colour_ov->get_active()) {
            m_working.chars.colour.reset();
        } else if (m_colour_none && m_colour_none->get_active()) {
            m_working.chars.colour = color::Paint(color::None{});
        } else {
            color::Color c = m_colour_value;
            c.a = 1.0;  // text fill opaque (matches the capture path)
            m_working.chars.colour = color::Paint(color::Solid(c));
        }
    }

    // Paragraph.
    if (m_align_dd) {
        const auto idx = m_align_dd->get_selected();
        if (idx == 0 || idx == GTK_INVALID_LIST_POSITION)
            m_working.para.align.reset();
        else
            m_working.para.align = style::para_align_from_ivalue((int)idx - 1);
    }
    if (m_leading_ov && m_leading_sp) {
        if (m_leading_ov->get_active())
            m_working.para.leading_px =
                m_leading_sp->get_internal_value();  // s346 — already px
        else
            m_working.para.leading_px.reset();
    }
    if (m_indent_ov) {
        if (m_indent_ov->get_active()) {
            if (m_indent_left_sp)  m_working.para.indent_left_px  = m_indent_left_sp->get_internal_value();
            if (m_indent_right_sp) m_working.para.indent_right_px = m_indent_right_sp->get_internal_value();
            if (m_indent_first_sp) m_working.para.indent_first_px = m_indent_first_sp->get_internal_value();
        } else {
            m_working.para.indent_left_px.reset();
            m_working.para.indent_right_px.reset();
            m_working.para.indent_first_px.reset();
        }
    }

    // NOTE: m_working.para.tabs is intentionally NOT touched here — the dialog
    // does not edit tabs, so an existing tab spec is carried through an edit.
}

// ── on_ok / on_cancel ────────────────────────────────────────────────────────
void TextStyleEditorDialog::on_ok() {
    harvest_into_working();

    // New / Duplicate clear the id so the library mints a fresh txs_<uuid>.
    if (m_mode != Mode::Edit)
        m_working.header.id.clear();

    auto cb = m_on_committed;
    style::TextStyle result = m_working;
    if (cb) cb(std::move(result));

    close();
}

void TextStyleEditorDialog::on_cancel() {
    LOG_DEBUG("TextStyleEditorDialog: cancel — discarding working buffer");
    close();
}

}  // namespace Curvz
