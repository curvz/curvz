#include "StyleEditorDialog.hpp"
#include "curvz_utils.hpp"  // s117 m18 v2

#include "color/FillStyleInterop.hpp"  // to_paint / to_fillstyle
#include "color/ColorRegion.hpp"       // region_name (binding annotation fallback)
#include "color/SwatchLibrary.hpp"
#include "color/Color.hpp"             // S98: colour-picker initial value
#include "CurvzLog.hpp"

#include <giomm/liststore.h>
#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <variant>

namespace Curvz {

namespace {

// ── Helpers ────────────────────────────────────────────────────────────────

// Sentinel string surfaced as the last item in the category dropdown.
// Keep distinct from any plausible user-typed category — the leading
// whitespace + ellipsis is the user-facing affordance and the exact
// string is the discriminant we test on selection.
constexpr const char* kCategorySentinelNew = "+ New category…";
constexpr const char* kCategoryUncategorised = "(uncategorised)";

// Convert a Paint to a FillStyle suitable for PaintEditor consumption.
// SwatchRef collapses to Solid using its cached fallback colour — the
// dialog tracks the binding id separately in m_*_binding_id. Gradient
// round-trips faithfully (S93 m1).
FillStyle paint_to_fill_for_editor(const color::Paint& p) {
    FillStyle fs;
    std::visit(color::Overloaded{
        [&](const color::None&) {
            fs.type = FillStyle::Type::None;
        },
        [&](const color::CurrentColor&) {
            fs.type = FillStyle::Type::CurrentColor;
        },
        [&](const color::Solid& s) {
            fs.type = FillStyle::Type::Solid;
            fs.r = s.color.r;
            fs.g = s.color.g;
            fs.b = s.color.b;
            fs.a = s.color.a;
        },
        [&](const color::SwatchRef& r) {
            // Collapse to Solid using the cached fallback. The binding
            // is preserved in m_*_binding_id by the caller, NOT here.
            fs.type = FillStyle::Type::Solid;
            fs.r = r.fallback.r;
            fs.g = r.fallback.g;
            fs.b = r.fallback.b;
            fs.a = r.fallback.a;
        },
        [&](const color::Gradient& g) {
            // S93 m1: round-trip gradient straight to FillStyle, same
            // mapping as FillStyleInterop::to_fillstyle. Inlined here
            // rather than delegated to keep the binding-aware SwatchRef
            // handling above local.
            fs.type = (g.kind == color::Gradient::Kind::Radial)
                          ? FillStyle::Type::RadialGradient
                          : FillStyle::Type::LinearGradient;
            fs.stops.clear();
            fs.stops.reserve(g.stops.size());
            for (const auto& s : g.stops) {
                GradientStop fs_stop;
                fs_stop.offset = s.offset;
                fs_stop.r = s.color.r;
                fs_stop.g = s.color.g;
                fs_stop.b = s.color.b;
                fs_stop.a = s.color.a;
                fs.stops.push_back(fs_stop);
            }
            fs.g_x1 = g.g_x1; fs.g_y1 = g.g_y1;
            fs.g_x2 = g.g_x2; fs.g_y2 = g.g_y2;
            fs.g_r  = g.g_r;
        },
    }, p);
    return fs;
}

// Convert a FillStyle (from PaintEditor's edits) into a Paint, optionally
// re-attaching a SwatchRef binding. The binding id reattach is what the
// dialog uses when the user hasn't touched the paint — preserving the
// SwatchRef across an OK that only changed name/category/stroke fields.
color::Paint fill_to_paint_with_binding(const FillStyle& fs,
                                         const std::string& binding_id) {
    if (!binding_id.empty() && fs.type == FillStyle::Type::Solid) {
        // Preserve the swatch binding. The fallback is the FillStyle's
        // current colour (which on a clean round-trip equals the
        // swatch's current colour, modulo any swatch edits since the
        // dialog opened — the live cache is fine as a fallback).
        color::Color fb;
        fb.r = fs.r; fb.g = fs.g; fb.b = fs.b; fb.a = fs.a;
        return color::SwatchRef{binding_id, fb};
    }
    switch (fs.type) {
        case FillStyle::Type::None:         return color::None{};
        case FillStyle::Type::CurrentColor: return color::CurrentColor{};
        case FillStyle::Type::Solid: {
            color::Color c;
            c.r = fs.r; c.g = fs.g; c.b = fs.b; c.a = fs.a;
            return color::Solid{c};
        }
        case FillStyle::Type::LinearGradient:
        case FillStyle::Type::RadialGradient: {
            // S93 m1: faithful gradient round-trip. Pre-S93 this branch
            // degraded to "first stop's flat colour" because color::Paint
            // had no gradient arm. Now Paint has a Gradient arm so we
            // copy stops + geometry across. Same mapping as
            // FillStyleInterop::to_paint — inlined here for symmetry
            // with paint_to_fill_for_editor above (both halves of the
            // dialog's local round-trip live together).
            color::Gradient g;
            g.kind = (fs.type == FillStyle::Type::RadialGradient)
                         ? color::Gradient::Kind::Radial
                         : color::Gradient::Kind::Linear;
            g.stops.reserve(fs.stops.size());
            for (const auto& s : fs.stops) {
                g.stops.push_back(color::GradientStop{
                    s.offset, color::Color{ s.r, s.g, s.b, s.a } });
            }
            g.g_x1 = fs.g_x1; g.g_y1 = fs.g_y1;
            g.g_x2 = fs.g_x2; g.g_y2 = fs.g_y2;
            g.g_r  = fs.g_r;
            return g;
        }
    }
    return color::None{};  // unreachable, defensive
}

// Trim leading + trailing ASCII whitespace.
std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

}  // anonymous namespace

// ── ctor ──────────────────────────────────────────────────────────────────
//
// s201 m1 — singleton form. Default-constructed once as a MainWindow
// member; show() is what callers invoke per editing session. The
// widget tree builds lazily on the first show() via the m_built
// latch and stays in the tree for the app's lifetime; subsequent
// show()s only refresh values via sync_from_working().
//
// Mirror of ThemeEditDialog's s200 m1 ctor. Title is set per-show()
// because Mode (Edit / New / Duplicate) drives the title-bar text.
StyleEditorDialog::StyleEditorDialog() {
    // Title is set per-show() (mode-dependent); leave default here
    // and the first show() overwrites it before present().
    // Non-modal so the Scripter window stays accessible while the
    // dialog is open (script-driven testing of the dialog's substrate
    // widgets needs sibling-window focus). OK closes via on_ok →
    // close(); Cancel via on_cancel → close(); X close-request hides
    // without firing on_committed.
    set_modal(false);
    set_resizable(false);
    set_default_size(420, -1);
    // s201 m1 — the singleton shape Curvz uses for long-lived dialogs.
    // close() now hides the window; the next show() re-presents it.
    set_hide_on_close(true);
    curvz::utils::set_name(*this, "dlg_se", "style_editor_dialog_window");

    // s201 m1 — close-request handler. Discards the pending commit
    // callback (cancel semantics on X). Returning false lets the
    // default close-action proceed, which for a hide-on-close window
    // means "hide, don't destroy."
    //
    // We do NOT call force_unregister_subtree here — substrate
    // widgets in this dialog construct exactly once at MainWindow
    // init time (when this dialog is constructed) and live until app
    // shutdown. The s199 m1 force_unregister_subtree discipline was
    // for the heap-allocated lifetime where the widget subtree died
    // on every close; that no longer happens.
    //
    // We do NOT detach the color popovers here — they stay attached
    // for the app's lifetime, same as every other inner widget.
    signal_close_request().connect(
        [this]() -> bool {
            LOG_DEBUG("StyleEditorDialog: close-request — discarding "
                      "pending commit callback");
            // Clear the commit callback so a stale closure can't fire
            // from any subsequent code path. on_committed is re-supplied
            // per show().
            m_on_committed = nullptr;
            return false;  // let hide-on-close proceed
        }, /*after=*/false);
}

// ── show ──────────────────────────────────────────────────────────────────
//
// s201 m1 — replaces the previous parameterised ctor. Sets the editing
// state, builds the widget tree on first call, syncs all widgets from
// the new working style, and presents.
//
// Cancel semantics: if the user closes via X (signal_close_request) or
// via the Cancel button, the pending commit callback is cleared. The
// dialog's next show() starts fresh with whatever style + callback the
// caller passes.
void StyleEditorDialog::show(Gtk::Window& parent,
                             const color::SwatchLibrary& sw_lib,
                             CanvasModel* canvas_model,
                             const std::vector<std::string>& user_categories,
                             Mode mode,
                             style::Style initial,
                             CommittedFn on_committed) {
    m_sw_lib       = &sw_lib;
    m_canvas_model = canvas_model;
    m_mode         = mode;
    m_working      = std::move(initial);
    m_on_committed = std::move(on_committed);

    // Pre-seed binding ids from the incoming Paint variants. The
    // dialog tracks these separately so a SwatchRef survives an OK
    // that didn't touch the paint slot. Reset both first (a previous
    // show() may have left stale ids on a different style).
    m_fill_binding_id.clear();
    m_stroke_binding_id.clear();
    if (auto* r = std::get_if<color::SwatchRef>(&m_working.fill)) {
        m_fill_binding_id = r->id;
    }
    if (auto* r = std::get_if<color::SwatchRef>(&m_working.stroke.paint)) {
        m_stroke_binding_id = r->id;
    }

    // Rebuild the category-order list from this show()'s user_categories.
    // The dropdown's StringList is rebuilt in sync_from_working() to
    // match. "(uncategorised)" first, sentinel last; same shape the
    // heap-allocated ctor used.
    m_category_order.clear();
    m_category_order.push_back(std::string());  // "" = uncategorised
    for (const auto& c : user_categories) {
        if (c.empty()) continue;  // skip — "" is already first
        m_category_order.push_back(c);
    }
    m_category_order.push_back(std::string(kCategorySentinelNew));

    // Title is mode-dependent.
    const char* title =
        (m_mode == Mode::Edit)      ? "Edit style"      :
        (m_mode == Mode::Duplicate) ? "Edit a copy"     :
                                      "New style";
    set_title(title);

    set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*this, parent);

    if (!m_built) {
        m_built = true;
        build();
    } else {
        // s201 m1 — widgets already exist from a prior open; refresh
        // every value from the new working buffer. build()'s initial
        // value-reads from m_working only fire on first open; this
        // is the symmetric refresh for every subsequent open.
        sync_from_working();
    }

    present();
}

// ── build ─────────────────────────────────────────────────────────────────
void StyleEditorDialog::build() {
    m_syncing = true;

    // Attach popovers to the dialog window itself — they need a stable
    // parent that lives for the full dialog lifetime. s201 m1: the
    // singleton form means "full lifetime" is the app's lifetime; no
    // detach() in the close handler (popovers stay attached forever).
    m_fill_popover.attach(*this);
    m_stroke_popover.attach(*this);
    m_shadow_popover.attach(*this);  // S98

    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(12);
    set_child(*root);

    build_identity_section(*root);
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_fill_section(*root);
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_stroke_section(*root);
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_shadow_section(*root);  // S98
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_button_row(*root);

    m_syncing = false;
}

// ── build_identity_section ────────────────────────────────────────────────
void StyleEditorDialog::build_identity_section(Gtk::Box& root) {
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    root.append(*grid);

    // ── Name ──────────────────────────────────────────────────────────
    auto* lbl_name = Gtk::make_managed<Gtk::Label>("Name:");
    lbl_name->set_xalign(0.0f);
    grid->attach(*lbl_name, 0, 0, 1, 1);

    m_name_entry = Gtk::make_managed<CurvzEntry>();
    curvz::utils::set_name(m_name_entry, "dlg_se_nm", "style_editor_dialog_name_entry");
    m_name_entry->set_text(m_working.header.name);
    m_name_entry->set_hexpand(true);
    grid->attach(*m_name_entry, 1, 0, 2, 1);
    m_name_entry->on_commit([this]() {
        if (m_syncing) return;
        // Idempotency guard (s84 m4i fix6 lesson). The entry's
        // on_commit fires on focus-leave including during dialog
        // teardown; comparing against the cached working value
        // before mutating makes the destruction-time fire a no-op.
        std::string typed = trim(m_name_entry->get_text());
        if (typed == m_working.header.name) return;
        m_working.header.name = typed;
    });

    // ── Category ──────────────────────────────────────────────────────
    auto* lbl_cat = Gtk::make_managed<Gtk::Label>("Category:");
    lbl_cat->set_xalign(0.0f);
    grid->attach(*lbl_cat, 0, 1, 1, 1);

    auto string_list = Gtk::StringList::create({});
    int initial_idx = 0;
    for (std::size_t i = 0; i < m_category_order.size(); ++i) {
        const std::string& c = m_category_order[i];
        const std::string display =
            (c == kCategorySentinelNew) ? c :
            (c.empty())                 ? std::string(kCategoryUncategorised) :
                                          c;
        string_list->append(display);
        if (c == m_working.header.category &&
            c != kCategorySentinelNew) {
            initial_idx = static_cast<int>(i);
        }
    }
    m_category_dd = Gtk::make_managed<curvz::widgets::DropDown>(
        "dlg_se_cat", string_list);
    curvz::utils::set_name(m_category_dd, "dlg_se_cat", "style_editor_dialog_category_dd");
    m_category_dd->set_hexpand(true);
    m_category_dd->set_selected(initial_idx);
    grid->attach(*m_category_dd, 1, 1, 1, 1);

    m_category_new_entry = Gtk::make_managed<CurvzEntry>();
    curvz::utils::set_name(m_category_new_entry, "dlg_se_cnew", "style_editor_dialog_category_new_entry");
    m_category_new_entry->set_placeholder_text("New category name");
    m_category_new_entry->set_hexpand(true);
    m_category_new_entry->set_visible(false);
    grid->attach(*m_category_new_entry, 2, 1, 1, 1);

    m_category_dd->property_selected().signal_changed().connect([this]() {
        if (m_syncing) return;
        const auto idx = m_category_dd->get_selected();
        if (idx == GTK_INVALID_LIST_POSITION) return;
        if (idx >= m_category_order.size()) return;
        const std::string& choice = m_category_order[idx];
        if (choice == kCategorySentinelNew) {
            // Reveal the inline new-category entry. Don't mutate
            // m_working.header.category yet — wait for the entry
            // commit to deliver a real value.
            m_category_new_entry->set_visible(true);
            m_category_new_entry->set_text("");
            m_category_new_entry->grab_focus();
        } else {
            m_category_new_entry->set_visible(false);
            m_working.header.category = choice;  // "" for uncategorised
        }
    });

    m_category_new_entry->on_commit([this]() {
        if (m_syncing) return;
        std::string typed = trim(m_category_new_entry->get_text());
        if (typed.empty()) {
            // Empty new-cat treated as "go back to (uncategorised)".
            // Don't snap the dropdown back here — the user can re-pick.
            return;
        }
        if (typed == m_working.header.category) return;
        m_working.header.category = typed;
    });
}

// ── build_fill_section ────────────────────────────────────────────────────
void StyleEditorDialog::build_fill_section(Gtk::Box& root) {
    auto* lbl = Gtk::make_managed<Gtk::Label>("Fill:");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("dim-label");
    root.append(*lbl);

    m_fill_editor = Gtk::make_managed<PaintEditor>(m_fill_popover);
    curvz::utils::set_name(m_fill_editor, "dlg_se_fill", "style_editor_dialog_fill_editor");
    root.append(*m_fill_editor);
    m_fill_editor->set_render_state(compute_render_state(m_working.fill));

    wire_paint_editor(*m_fill_editor, m_working.fill, /*is_stroke=*/false);
}

// ── build_stroke_section ──────────────────────────────────────────────────
void StyleEditorDialog::build_stroke_section(Gtk::Box& root) {
    auto* lbl = Gtk::make_managed<Gtk::Label>("Stroke:");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("dim-label");
    root.append(*lbl);

    // Paint editor first.
    m_stroke_editor = Gtk::make_managed<PaintEditor>(m_stroke_popover);
    curvz::utils::set_name(m_stroke_editor, "dlg_se_strk", "style_editor_dialog_stroke_editor");
    root.append(*m_stroke_editor);
    m_stroke_editor->set_render_state(
        compute_render_state(m_working.stroke.paint));
    wire_paint_editor(*m_stroke_editor, m_working.stroke.paint,
                      /*is_stroke=*/true);

    // Stroke fields grid: width, cap, join, miter, dash, dash offset, align.
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    grid->set_margin_top(4);
    root.append(*grid);

    int row = 0;

    // ── Width ─────────────────────────────────────────────────────────
    auto* lbl_w = Gtk::make_managed<Gtk::Label>("Width:");
    lbl_w->set_xalign(0.0f);
    grid->attach(*lbl_w, 0, row, 1, 1);

    m_stroke_width_sp = Gtk::make_managed<CurvzSpinButton>(
        SpinType::Width, m_canvas_model);
    curvz::utils::set_name(m_stroke_width_sp, "dlg_se_sw", "style_editor_dialog_stroke_width_spn");
    m_stroke_width_sp->with_value(m_working.stroke.width);
    grid->attach(*m_stroke_width_sp, 1, row, 1, 1);
    if (auto* unit = m_stroke_width_sp->get_unit_label()) {
        grid->attach(*unit, 2, row, 1, 1);
    }
    m_stroke_width_sp->signal_internal_changed().connect([this](double v) {
        if (m_syncing) return;
        m_working.stroke.width = v;
    });
    ++row;

    // ── Cap row ───────────────────────────────────────────────────────
    //
    // S87 m1: migrated from Gtk::DropDown to a row of three icon-bearing
    // Gtk::ToggleButtons in a radio group (set_group). Mirrors the
    // toolbar's Stroke popover layout and PropertiesPanel inspector.
    // signal_toggled with `if (m_syncing || !btn.get_active()) return;`
    // guards against the deactivation half of group radio events
    // (toggled fires twice on a group flip — once for the deactivating
    // button, once for the activating one).
    {
        auto* lbl_cap = Gtk::make_managed<Gtk::Label>("Cap:");
        lbl_cap->set_xalign(0.0f);
        grid->attach(*lbl_cap, 0, row, 1, 1);

        auto* cap_row = Gtk::make_managed<Gtk::Box>(
            Gtk::Orientation::HORIZONTAL, 4);

        m_cap_butt_btn   = Gtk::make_managed<curvz::widgets::ToggleButton>("dlg_se_cb");
        m_cap_round_btn  = Gtk::make_managed<curvz::widgets::ToggleButton>("dlg_se_cr");
        m_cap_square_btn = Gtk::make_managed<curvz::widgets::ToggleButton>("dlg_se_cs");
        curvz::utils::set_name(m_cap_butt_btn,   "dlg_se_cb", "style_editor_dialog_cap_butt_btn");
        curvz::utils::set_name(m_cap_round_btn,  "dlg_se_cr", "style_editor_dialog_cap_round_btn");
        curvz::utils::set_name(m_cap_square_btn, "dlg_se_cs", "style_editor_dialog_cap_square_btn");
        m_cap_butt_btn  ->set_icon_name("curvz-cap-butt-symbolic");
        m_cap_round_btn ->set_icon_name("curvz-cap-round-symbolic");
        m_cap_square_btn->set_icon_name("curvz-cap-square-symbolic");
        m_cap_butt_btn  ->set_tooltip_text("Butt");
        m_cap_round_btn ->set_tooltip_text("Round");
        m_cap_square_btn->set_tooltip_text("Square");
        // Radio behaviour: round + square join the butt button's group.
        m_cap_round_btn ->set_group(*m_cap_butt_btn);
        m_cap_square_btn->set_group(*m_cap_butt_btn);
        // Initial active state: select whichever matches the working cap.
        switch (m_working.stroke.cap) {
            case LineCap::Butt:   m_cap_butt_btn  ->set_active(true); break;
            case LineCap::Round:  m_cap_round_btn ->set_active(true); break;
            case LineCap::Square: m_cap_square_btn->set_active(true); break;
        }
        m_cap_butt_btn->signal_toggled().connect([this]() {
            if (m_syncing || !m_cap_butt_btn->get_active()) return;
            m_working.stroke.cap = LineCap::Butt;
        });
        m_cap_round_btn->signal_toggled().connect([this]() {
            if (m_syncing || !m_cap_round_btn->get_active()) return;
            m_working.stroke.cap = LineCap::Round;
        });
        m_cap_square_btn->signal_toggled().connect([this]() {
            if (m_syncing || !m_cap_square_btn->get_active()) return;
            m_working.stroke.cap = LineCap::Square;
        });
        cap_row->append(*m_cap_butt_btn);
        cap_row->append(*m_cap_round_btn);
        cap_row->append(*m_cap_square_btn);
        grid->attach(*cap_row, 1, row, 2, 1);
        ++row;
    }

    // ── Join row ──────────────────────────────────────────────────────
    {
        auto* lbl_join = Gtk::make_managed<Gtk::Label>("Join:");
        lbl_join->set_xalign(0.0f);
        grid->attach(*lbl_join, 0, row, 1, 1);

        auto* join_row = Gtk::make_managed<Gtk::Box>(
            Gtk::Orientation::HORIZONTAL, 4);

        m_join_miter_btn = Gtk::make_managed<curvz::widgets::ToggleButton>("dlg_se_jm");
        m_join_round_btn = Gtk::make_managed<curvz::widgets::ToggleButton>("dlg_se_jr");
        m_join_bevel_btn = Gtk::make_managed<curvz::widgets::ToggleButton>("dlg_se_jb");
        curvz::utils::set_name(m_join_miter_btn, "dlg_se_jm", "style_editor_dialog_join_miter_btn");
        curvz::utils::set_name(m_join_round_btn, "dlg_se_jr", "style_editor_dialog_join_round_btn");
        curvz::utils::set_name(m_join_bevel_btn, "dlg_se_jb", "style_editor_dialog_join_bevel_btn");
        m_join_miter_btn->set_icon_name("curvz-join-miter-symbolic");
        m_join_round_btn->set_icon_name("curvz-join-round-symbolic");
        m_join_bevel_btn->set_icon_name("curvz-join-bevel-symbolic");
        m_join_miter_btn->set_tooltip_text("Miter");
        m_join_round_btn->set_tooltip_text("Round");
        m_join_bevel_btn->set_tooltip_text("Bevel");
        m_join_round_btn->set_group(*m_join_miter_btn);
        m_join_bevel_btn->set_group(*m_join_miter_btn);
        switch (m_working.stroke.join) {
            case LineJoin::Miter: m_join_miter_btn->set_active(true); break;
            case LineJoin::Round: m_join_round_btn->set_active(true); break;
            case LineJoin::Bevel: m_join_bevel_btn->set_active(true); break;
        }
        m_join_miter_btn->signal_toggled().connect([this]() {
            if (m_syncing || !m_join_miter_btn->get_active()) return;
            m_working.stroke.join = LineJoin::Miter;
        });
        m_join_round_btn->signal_toggled().connect([this]() {
            if (m_syncing || !m_join_round_btn->get_active()) return;
            m_working.stroke.join = LineJoin::Round;
        });
        m_join_bevel_btn->signal_toggled().connect([this]() {
            if (m_syncing || !m_join_bevel_btn->get_active()) return;
            m_working.stroke.join = LineJoin::Bevel;
        });
        join_row->append(*m_join_miter_btn);
        join_row->append(*m_join_round_btn);
        join_row->append(*m_join_bevel_btn);
        grid->attach(*join_row, 1, row, 2, 1);
        ++row;
    }

    // S87 m1: miter limit, dash entry, dash offset, align dropdown all
    // removed from this dialog. Miter limit field is preserved on the
    // model (StrokeAppearance::miter_limit) and continues to render via
    // Cairo set_miter_limit; just no UI surface for editing it. Dash and
    // align were removed entirely from the data model — they had been
    // specced for a future phase but never reached the renderer.
}

// ── build_shadow_section ──────────────────────────────────────────────────
//
// S98. Drop-shadow as a first-class style appearance dimension. Mirrors the
// inspector's PropertiesPanel::build_shadow_section but flatter — the dialog
// has more horizontal room, so dx/dy share a row with the OFFSET label and
// blur sits on its own row. Sensitivity of every editable widget is slaved
// to the enable check so a "shadow off" style stays visible-but-greyed,
// letting a user iterate (re-enable to see the same config) — same idiom
// as the inspector and the Blend section's stroke-width slave.
//
// Colour rgb is written directly to m_working.shadow.color_* by the
// popover apply callback; alpha and opacity ride on Gtk::Adjustment so
// harvest_into_working can read the final values defensively. The shadow
// colour-picker uses with_alpha=false — alpha is the slider's job, two
// alpha controls in one popup would just confuse.
void StyleEditorDialog::build_shadow_section(Gtk::Box& root) {
    auto* lbl = Gtk::make_managed<Gtk::Label>("Shadow:");
    lbl->set_xalign(0.0f);
    lbl->add_css_class("dim-label");
    root.append(*lbl);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    grid->set_margin_top(4);
    root.append(*grid);

    int row = 0;

    // ── Enable check ──────────────────────────────────────────────────
    // Spans the grid; drives sensitive() of every other widget via the
    // apply_enabled lambda below. Default-off styles (built-in Default
    // / Outline Only / Fill Only, plus freshly-created user styles)
    // open with the section greyed; toggling enables fields.
    m_shadow_enable_chk = Gtk::make_managed<curvz::widgets::CheckButton>(
        "dlg_se_sen", "Enable shadow");
    curvz::utils::set_name(m_shadow_enable_chk, "dlg_se_sen", "style_editor_dialog_shadow_enable_check");
    m_shadow_enable_chk->set_active(m_working.shadow.enabled);
    grid->attach(*m_shadow_enable_chk, 0, row, 3, 1);
    ++row;

    // ── Offset (dx, dy) ───────────────────────────────────────────────
    // Y-down convention (positive dy = below) — matches SceneNode and
    // the inspector. SpinType::Distance gives signed-without-flip
    // semantics, which is what offsets need.
    auto* lbl_off = Gtk::make_managed<Gtk::Label>("Offset:");
    lbl_off->set_xalign(0.0f);
    grid->attach(*lbl_off, 0, row, 1, 1);

    auto* off_row = Gtk::make_managed<Gtk::Box>(
        Gtk::Orientation::HORIZONTAL, 4);

    m_shadow_dx_sp = Gtk::make_managed<CurvzSpinButton>(
        SpinType::Distance, m_canvas_model);
    curvz::utils::set_name(m_shadow_dx_sp, "dlg_se_sdx", "style_editor_dialog_shadow_dx_spn");
    m_shadow_dx_sp->with_value(m_working.shadow.dx)
                  ->with_tooltip("Horizontal offset (+right)");
    off_row->append(*m_shadow_dx_sp);
    if (auto* unit = m_shadow_dx_sp->get_unit_label()) off_row->append(*unit);

    m_shadow_dy_sp = Gtk::make_managed<CurvzSpinButton>(
        SpinType::Distance, m_canvas_model);
    curvz::utils::set_name(m_shadow_dy_sp, "dlg_se_sdy", "style_editor_dialog_shadow_dy_spn");
    m_shadow_dy_sp->with_value(m_working.shadow.dy)
                  ->with_tooltip("Vertical offset (+down)");
    off_row->append(*m_shadow_dy_sp);
    if (auto* unit = m_shadow_dy_sp->get_unit_label()) off_row->append(*unit);

    grid->attach(*off_row, 1, row, 2, 1);
    ++row;

    m_shadow_dx_sp->signal_internal_changed().connect([this](double v) {
        if (m_syncing) return;
        m_working.shadow.dx = v;
    });
    m_shadow_dy_sp->signal_internal_changed().connect([this](double v) {
        if (m_syncing) return;
        m_working.shadow.dy = v;
    });

    // ── Blur ──────────────────────────────────────────────────────────
    // Gaussian stddev in doc units. SpinType::Width is non-negative,
    // matching the renderer's clamp blur >= 0.
    auto* lbl_blur = Gtk::make_managed<Gtk::Label>("Blur:");
    lbl_blur->set_xalign(0.0f);
    grid->attach(*lbl_blur, 0, row, 1, 1);

    m_shadow_blur_sp = Gtk::make_managed<CurvzSpinButton>(
        SpinType::Width, m_canvas_model);
    curvz::utils::set_name(m_shadow_blur_sp, "dlg_se_sbl", "style_editor_dialog_shadow_blur_spn");
    m_shadow_blur_sp->with_value(m_working.shadow.blur)
                    ->with_tooltip("Blur radius (Gaussian stddev)");
    grid->attach(*m_shadow_blur_sp, 1, row, 1, 1);
    if (auto* unit = m_shadow_blur_sp->get_unit_label()) {
        grid->attach(*unit, 2, row, 1, 1);
    }
    ++row;

    m_shadow_blur_sp->signal_internal_changed().connect([this](double v) {
        if (m_syncing) return;
        m_working.shadow.blur = std::max(0.0, v);
    });

    // ── Colour swatch + alpha slider ──────────────────────────────────
    // Click-to-pick swatch; the slider next to it is the colour-side
    // alpha multiplier. The opacity slider below is the second
    // multiplier (final_a = color_a * opacity). Two sliders feels like
    // duplication but matches Affinity Designer's drop-shadow editor
    // and the inspector's idiom.
    auto* lbl_col = Gtk::make_managed<Gtk::Label>("Colour:");
    lbl_col->set_xalign(0.0f);
    grid->attach(*lbl_col, 0, row, 1, 1);

    auto* col_row = Gtk::make_managed<Gtk::Box>(
        Gtk::Orientation::HORIZONTAL, 6);

    m_shadow_swatch = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(m_shadow_swatch, "dlg_se_ssw", "style_editor_dialog_shadow_swatch_da");
    m_shadow_swatch->set_size_request(28, 22);
    m_shadow_swatch->set_valign(Gtk::Align::CENTER);
    m_shadow_swatch->set_can_target(true);
    // The draw lambda reads m_working.shadow.color_* at draw time so
    // the swatch live-updates after a colour pick. m_working outlives
    // the dialog by definition (it's a member); no capture lifetime
    // concern here.
    m_shadow_swatch->set_draw_func(
        [this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            // Background checker for transparent colours so an alpha
            // shadow against the panel background is visible.
            if (m_working.shadow.color_a < 0.999) {
                for (int yy = 0; yy < h; yy += 4)
                    for (int xx = 0; xx < w; xx += 4) {
                        bool dark = ((xx / 4) + (yy / 4)) % 2;
                        cr->set_source_rgb(
                            dark ? 0.7 : 0.85,
                            dark ? 0.7 : 0.85,
                            dark ? 0.7 : 0.85);
                        cr->rectangle(xx, yy, 4, 4);
                        cr->fill();
                    }
            }
            cr->set_source_rgba(m_working.shadow.color_r,
                                m_working.shadow.color_g,
                                m_working.shadow.color_b,
                                m_working.shadow.color_a);
            cr->rectangle(0, 0, w, h);
            cr->fill();
            // Hairline border so a near-white shadow on the panel
            // background isn't invisible.
            cr->set_source_rgba(0, 0, 0, 0.25);
            cr->set_line_width(1.0);
            cr->rectangle(0.5, 0.5, w - 1, h - 1);
            cr->stroke();
        });

    auto swatch_click = Gtk::GestureClick::create();
    swatch_click->set_button(1);
    swatch_click->signal_pressed().connect(
        [this](int, double, double) {
            if (m_syncing) return;
            color::Color initial(m_working.shadow.color_r,
                                 m_working.shadow.color_g,
                                 m_working.shadow.color_b,
                                 1.0);
            m_shadow_popover.open(
                *m_shadow_swatch, initial, /*with_alpha=*/false,
                [this](const color::Color& c) {
                    m_working.shadow.color_r = c.r;
                    m_working.shadow.color_g = c.g;
                    m_working.shadow.color_b = c.b;
                    if (m_shadow_swatch) m_shadow_swatch->queue_draw();
                });
        });
    m_shadow_swatch->add_controller(swatch_click);
    col_row->append(*m_shadow_swatch);

    // Colour alpha slider — 0..100 mapped to 0..1.
    m_shadow_color_a_adj = Gtk::Adjustment::create(
        m_working.shadow.color_a * 100.0, 0.0, 100.0, 1.0, 10.0);
    m_shadow_color_a_sl = Gtk::make_managed<curvz::widgets::Scale>(
        "dlg_se_sca", m_shadow_color_a_adj);
    curvz::utils::set_name(m_shadow_color_a_sl, "dlg_se_sca", "style_editor_dialog_shadow_color_alpha_slider");
    m_shadow_color_a_sl->set_draw_value(true);
    m_shadow_color_a_sl->set_value_pos(Gtk::PositionType::RIGHT);
    m_shadow_color_a_sl->set_digits(0);
    m_shadow_color_a_sl->set_hexpand(true);
    m_shadow_color_a_sl->set_tooltip_text(
        "Colour alpha (separate from Opacity below)");
    col_row->append(*m_shadow_color_a_sl);

    grid->attach(*col_row, 1, row, 2, 1);
    ++row;

    m_shadow_color_a_adj->signal_value_changed().connect([this]() {
        if (m_syncing) return;
        m_working.shadow.color_a = std::clamp(
            m_shadow_color_a_adj->get_value() / 100.0, 0.0, 1.0);
        if (m_shadow_swatch) m_shadow_swatch->queue_draw();
    });

    // ── Opacity ───────────────────────────────────────────────────────
    auto* lbl_op = Gtk::make_managed<Gtk::Label>("Opacity:");
    lbl_op->set_xalign(0.0f);
    grid->attach(*lbl_op, 0, row, 1, 1);

    m_shadow_opacity_adj = Gtk::Adjustment::create(
        m_working.shadow.opacity * 100.0, 0.0, 100.0, 1.0, 10.0);
    m_shadow_opacity_sl = Gtk::make_managed<curvz::widgets::Scale>(
        "dlg_se_sop", m_shadow_opacity_adj);
    curvz::utils::set_name(m_shadow_opacity_sl, "dlg_se_sop", "style_editor_dialog_shadow_opacity_slider");
    m_shadow_opacity_sl->set_draw_value(true);
    m_shadow_opacity_sl->set_value_pos(Gtk::PositionType::RIGHT);
    m_shadow_opacity_sl->set_digits(0);
    m_shadow_opacity_sl->set_hexpand(true);
    m_shadow_opacity_sl->set_tooltip_text(
        "Final shadow strength (multiplied with colour alpha)");
    grid->attach(*m_shadow_opacity_sl, 1, row, 2, 1);
    ++row;

    m_shadow_opacity_adj->signal_value_changed().connect([this]() {
        if (m_syncing) return;
        m_working.shadow.opacity = std::clamp(
            m_shadow_opacity_adj->get_value() / 100.0, 0.0, 1.0);
    });

    // ── Sensitivity slave ─────────────────────────────────────────────
    // Section stays visible-but-greyed when shadow is disabled — same
    // pattern as the inspector and the Blend section. Enable check is
    // intentionally always-active so the user can re-enable.
    auto apply_enabled = [this](bool on) {
        if (m_shadow_dx_sp)      m_shadow_dx_sp->set_sensitive(on);
        if (m_shadow_dy_sp)      m_shadow_dy_sp->set_sensitive(on);
        if (m_shadow_blur_sp)    m_shadow_blur_sp->set_sensitive(on);
        if (m_shadow_swatch)     m_shadow_swatch->set_sensitive(on);
        if (m_shadow_color_a_sl) m_shadow_color_a_sl->set_sensitive(on);
        if (m_shadow_opacity_sl) m_shadow_opacity_sl->set_sensitive(on);
    };
    apply_enabled(m_working.shadow.enabled);

    m_shadow_enable_chk->signal_toggled().connect(
        [this, apply_enabled]() {
            if (m_syncing) return;
            const bool on = m_shadow_enable_chk->get_active();
            m_working.shadow.enabled = on;
            apply_enabled(on);
        });
}

// ── build_button_row ──────────────────────────────────────────────────────
void StyleEditorDialog::build_button_row(Gtk::Box& root) {
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    btn_row->set_halign(Gtk::Align::END);
    btn_row->set_margin_top(4);

    // s201 m2 — Cancel and OK migrated to substrate Button. Cancel
    // promoted from a local to a member (m_btn_cancel) to give the
    // substrate ctor a stable home; OK already had a member slot from
    // the s199 m1 pilot, so just type-flip.
    m_btn_cancel = Gtk::make_managed<curvz::widgets::Button>(
        "dlg_se_cnc", "Cancel");
    curvz::utils::set_name(m_btn_cancel, "dlg_se_cnc", "style_editor_dialog_cancel_btn");

    m_btn_ok = Gtk::make_managed<curvz::widgets::Button>("dlg_se_ok", "OK");
    curvz::utils::set_name(m_btn_ok, "dlg_se_ok", "style_editor_dialog_ok_btn");
    m_btn_ok->add_css_class("suggested-action");
    m_btn_ok->set_receives_default(true);

    btn_row->append(*m_btn_cancel);
    btn_row->append(*m_btn_ok);
    root.append(*btn_row);

    m_btn_cancel->signal_clicked().connect(
        sigc::mem_fun(*this, &StyleEditorDialog::on_cancel));
    m_btn_ok->signal_clicked().connect(
        sigc::mem_fun(*this, &StyleEditorDialog::on_ok));
}

// ── wire_paint_editor ─────────────────────────────────────────────────────
//
// The dialog's PaintEditor wiring is much simpler than the inspector's:
// no selection, no broadcast, no coalescing. The model is a single Paint
// slot in m_working; we just translate signals into mutations.
//
// Break-on-override: any colour-change or type-change clears the
// associated binding id, mirroring the inspector's S83 m4h v2 rule.
// PaintEditor's annotation rebuilds via set_render_state, so the
// "(Vivid Red)" italic disappears immediately after the user edits.
void StyleEditorDialog::wire_paint_editor(PaintEditor& editor,
                                          color::Paint& target_paint,
                                          bool is_stroke) {
    std::string& binding_id =
        is_stroke ? m_stroke_binding_id : m_fill_binding_id;

    editor.signal_type_changed().connect(
        [this, &editor, &target_paint, &binding_id]
        (FillStyle::Type new_type) {
            if (m_syncing) return;
            // Break-on-override: any type-toggle drops the binding.
            binding_id.clear();
            switch (new_type) {
                case FillStyle::Type::None:
                    target_paint = color::None{};
                    break;
                case FillStyle::Type::CurrentColor:
                    target_paint = color::CurrentColor{};
                    break;
                case FillStyle::Type::Solid: {
                    // Preserve existing colour if we have one to
                    // preserve; otherwise default to black. S93 m3:
                    // when transitioning out of a Gradient back to
                    // Solid, take the first stop's colour rather than
                    // letting the catch-all default-init to black —
                    // small UX nicety mirroring the inspector's same-
                    // shaped handler.
                    color::Color c{};
                    std::visit(color::Overloaded{
                        [&](const color::Solid& s) { c = s.color; },
                        [&](const color::SwatchRef& r) { c = r.fallback; },
                        [&](const color::Gradient& g) {
                            if (!g.stops.empty()) c = g.stops.front().color;
                            // empty stops → c stays default-init (black)
                        },
                        [&](const auto&) { /* default-init c */ },
                    }, target_paint);
                    target_paint = color::Solid{c};
                    break;
                }
                case FillStyle::Type::LinearGradient:
                case FillStyle::Type::RadialGradient: {
                    // S93 m3: promote target_paint to a color::Gradient.
                    // Mirrors PropertiesPanel::add_fill_stroke_section's
                    // signal_type_changed handler — same default stops
                    // (black→white) and same bbox-fraction geometry
                    // (full horizontal spread for Linear; centre + half
                    // radius for Radial).
                    //
                    // If we're already on a Gradient (Linear↔Radial flip,
                    // not currently surfaced by PaintEditor's toggle but
                    // defensive), preserve the existing stops + geometry
                    // and only swap kind. Same shape as
                    // GradientDialog::sync_from_state's preserved-state
                    // handling.
                    color::Gradient g;
                    bool already_gradient = false;
                    if (const auto* existing =
                            std::get_if<color::Gradient>(&target_paint)) {
                        g = *existing;
                        already_gradient = true;
                    }
                    g.kind = (new_type == FillStyle::Type::RadialGradient)
                                 ? color::Gradient::Kind::Radial
                                 : color::Gradient::Kind::Linear;
                    if (!already_gradient) {
                        // Seed default 2-stop black→white linear span.
                        // Empty-stops handling lives in GradientDialog::
                        // show as well, but seeding here means the
                        // editor's preview ramp draws meaningfully
                        // before the user clicks Edit.
                        g.stops.clear();
                        g.stops.push_back(color::GradientStop{
                            0.0, color::Color{0.0, 0.0, 0.0, 1.0}});
                        g.stops.push_back(color::GradientStop{
                            1.0, color::Color{1.0, 1.0, 1.0, 1.0}});
                        g.g_x1 = 0.0; g.g_y1 = 0.5;
                        g.g_x2 = 1.0; g.g_y2 = 0.5;
                        g.g_r  = 0.5;
                    }
                    target_paint = std::move(g);
                    break;
                }
            }
            editor.set_render_state(compute_render_state(target_paint));
        });

    editor.signal_color_changed().connect(
        [this, &editor, &target_paint, &binding_id]
        (double r, double g, double b) {
            if (m_syncing) return;
            binding_id.clear();  // break-on-override
            color::Color c{};
            c.r = r; c.g = g; c.b = b; c.a = 1.0;
            target_paint = color::Solid{c};
            editor.set_render_state(compute_render_state(target_paint));
        });

    editor.signal_unbind_clicked().connect(
        [this, &editor, &target_paint, &binding_id]() {
            if (m_syncing) return;
            // Unbind: drop the binding id, preserve the current colour
            // as a Solid. PaintEditor's render state will redraw the
            // chip without the annotation.
            if (binding_id.empty()) return;  // nothing to do
            color::Color c{};
            std::visit(color::Overloaded{
                [&](const color::Solid& s) { c = s.color; },
                [&](const color::SwatchRef& r) { c = r.fallback; },
                [&](const auto&) { /* default */ },
            }, target_paint);
            target_paint = color::Solid{c};
            binding_id.clear();
            editor.set_render_state(compute_render_state(target_paint));
        });

    // S85: swatch picked from the embedded picker. Update the working
    // paint to a SwatchRef carrying the resolved colour as fallback,
    // and stash the id in the dialog's binding-id sidecar (m_*_
    // binding_id). harvest_into_working / on_ok rebuild the paint as
    // Paint{SwatchRef} on commit; keeping the working slot in sync
    // here means compute_render_state always sees the right state for
    // re-render between now and OK.
    editor.signal_swatch_picked().connect(
        [this, &editor, &target_paint, &binding_id]
        (color::SwatchId id) {
            if (m_syncing) return;
            if (id.empty()) return;
            // Resolve the swatch for its colour (fallback). Skip non-
            // solid kinds — Phase 4 only ships SolidSwatch, but the
            // variant lookup is forward-compatible.
            const color::Swatch* sw = m_sw_lib->find_swatch(id);
            if (!sw) return;  // dangling — silently ignore
            color::Color resolved{};
            if (const auto* solid = std::get_if<color::SolidSwatch>(sw)) {
                resolved = solid->color;
            } else {
                // Future gradient swatches: resolve to a representative
                // colour. For now, no-op the click on non-solid kinds.
                return;
            }
            target_paint = color::SwatchRef{id, resolved};
            binding_id   = id;
            editor.set_render_state(compute_render_state(target_paint));
        });

    // ── S93 m3 gradient edit ─────────────────────────────────────────
    //
    // PaintEditor → Edit gradient button → open the local
    // m_gradient_dialog. On Apply, the dialog returns a FillStyle which
    // we convert into a color::Gradient and write into target_paint.
    // Editing a gradient breaks any swatch binding — same break-on-
    // override rule as colour and type edits above.
    //
    // Mirrors MainWindow's wiring of the inspector's signal_request_
    // gradient_edit, but kept local: the style dialog hosts its own
    // GradientDialog instance so the modal stack is MainWindow →
    // StyleEditorDialog → GradientDialog without bouncing through a
    // signal up to MainWindow. s201 m1: the outer class is the window
    // itself now; pass *this as the gradient dialog's transient parent.
    editor.signal_gradient_edit_requested().connect(
        [this, &editor, &target_paint, &binding_id]
        (FillStyle current) {
            if (m_syncing) return;
            m_gradient_dialog.show(*this, current,
                [this, &editor, &target_paint, &binding_id]
                (FillStyle edited) {
                    // Break-on-override: a gradient edit drops any
                    // swatch binding. Mirrors signal_color_changed
                    // above and the inspector's apply_cb.
                    binding_id.clear();
                    // FillStyle → color::Gradient. Inlined rather than
                    // delegated to fill_to_paint_with_binding because
                    // we already cleared binding_id and want unconditional
                    // gradient promotion (the helper would degrade a
                    // solid result to color::Solid).
                    color::Gradient g;
                    g.kind = (edited.type == FillStyle::Type::RadialGradient)
                                 ? color::Gradient::Kind::Radial
                                 : color::Gradient::Kind::Linear;
                    g.stops.reserve(edited.stops.size());
                    for (const auto& s : edited.stops) {
                        g.stops.push_back(color::GradientStop{
                            s.offset, color::Color{s.r, s.g, s.b, s.a}});
                    }
                    g.g_x1 = edited.g_x1; g.g_y1 = edited.g_y1;
                    g.g_x2 = edited.g_x2; g.g_y2 = edited.g_y2;
                    g.g_r  = edited.g_r;
                    target_paint = std::move(g);
                    editor.set_render_state(compute_render_state(target_paint));
                });
        });
}

// ── compute_render_state ──────────────────────────────────────────────────
PaintEditor::RenderState
StyleEditorDialog::compute_render_state(const color::Paint& p) const {
    PaintEditor::RenderState s;
    s.paint     = paint_to_fill_for_editor(p);
    s.uniform   = true;        // single-style edit, no multi-select
    s.has_alpha = false;       // style paint follows inspector convention

    // Determine binding state. The dialog-tracked binding id is the
    // canonical source — m_*_binding_id is what gets reattached to
    // Paint at OK time.
    const std::string& binding_id =
        (&p == &m_working.stroke.paint) ? m_stroke_binding_id
                                        : m_fill_binding_id;

    if (!binding_id.empty()) {
        s.bound       = true;
        s.bound_mixed = false;
        if (const auto* sw = m_sw_lib->find_swatch(binding_id)) {
            const std::string& nm = color::swatch_header(*sw).name;
            s.bound_display_name = nm.empty()
                ? color::region_name(s.paint.r, s.paint.g, s.paint.b)
                : nm;
        } else {
            // Dangling — defensive; the library's delete cascade
            // normally prevents this from reaching us.
            s.bound_display_name = binding_id + "?";
        }
        s.bound_tooltip = (&p == &m_working.stroke.paint)
            ? "Unbind stroke from swatch (keep current colour)"
            : "Unbind fill from swatch (keep current colour)";
    } else {
        s.bound = false;
        if (s.paint.type == FillStyle::Type::Solid) {
            s.unbound_display_name =
                color::region_name(s.paint.r, s.paint.g, s.paint.b);
        }
    }

    // ── S85 swatch-pick fields ──────────────────────────────────────
    //
    // The dialog always has a library reference (constructor takes one);
    // never nullptr here. is_swatch_active mirrors bound — same logic
    // as the inspector: a binding-carrying paint sits on the Swatch
    // toggle. Toggling Swatch without picking is a transient widget-
    // local state that doesn't survive a re-render from compute_render_
    // state, which is fine — picking is the meaningful event.
    s.library          = m_sw_lib;
    s.binding_id       = binding_id;
    s.is_swatch_active = !binding_id.empty();

    // S93 m3: gradient editing enabled. The Paint variant grew a
    // Gradient arm in m2, so the dialog can now round-trip gradients
    // through OK without lossy degradation. PaintEditor's gradient row
    // (Edit button) becomes visible when the active paint is a
    // gradient; the type-toggle's Gradient button becomes sensitive.
    // wire_paint_editor connects the Edit button to a local
    // GradientDialog instance and the type toggle to a promote-to-
    // Gradient path that mirrors the inspector handler in
    // PropertiesPanel::add_fill_stroke_section.
    s.gradients_enabled = true;

    return s;
}

// ── harvest_into_working ──────────────────────────────────────────────────
//
// Defensive pre-OK pull of every entry's current text. Spinbuttons and
// dropdowns commit on every change so they're already in sync; entries
// commit on focus-leave, which OK-button-click DOES trigger naturally,
// but we read explicitly here as a belt-and-braces measure in case
// some host-supplied focus dance left an entry stale.
void StyleEditorDialog::harvest_into_working() {
    if (m_name_entry) {
        std::string typed = trim(m_name_entry->get_text());
        m_working.header.name = typed;
    }

    // Category: take the dropdown's current selection if it's not on
    // the sentinel; if it's on the sentinel, take the new-entry text.
    if (m_category_dd) {
        auto idx = m_category_dd->get_selected();
        if (idx != GTK_INVALID_LIST_POSITION &&
            idx < m_category_order.size()) {
            const std::string& choice = m_category_order[idx];
            if (choice == kCategorySentinelNew) {
                if (m_category_new_entry) {
                    std::string typed = trim(m_category_new_entry->get_text());
                    m_working.header.category = typed;  // empty → uncategorised
                }
            } else {
                m_working.header.category = choice;
            }
        }
    }

    // Spinbuttons — read their internal value (already kept in sync,
    // but cheap to re-pull). S87 m1: miter_limit, dash, dash_offset
    // removed from this dialog's UI; cap/join radio toggles already
    // wrote m_working.stroke.cap/join in their signal_toggled handlers.
    if (m_stroke_width_sp) m_working.stroke.width =
                               m_stroke_width_sp->get_internal_value();

    // S98: shadow read-back. enable / dx / dy / blur / alpha / opacity
    // are all kept in sync by their per-widget change handlers — but
    // belt-and-braces, same as the stroke width above. Colour rgb is
    // written directly by the popover apply callback; nothing to harvest.
    if (m_shadow_enable_chk)
        m_working.shadow.enabled = m_shadow_enable_chk->get_active();
    if (m_shadow_dx_sp)
        m_working.shadow.dx = m_shadow_dx_sp->get_internal_value();
    if (m_shadow_dy_sp)
        m_working.shadow.dy = m_shadow_dy_sp->get_internal_value();
    if (m_shadow_blur_sp)
        m_working.shadow.blur = std::max(0.0, m_shadow_blur_sp->get_internal_value());
    if (m_shadow_color_a_adj)
        m_working.shadow.color_a = std::clamp(
            m_shadow_color_a_adj->get_value() / 100.0, 0.0, 1.0);
    if (m_shadow_opacity_adj)
        m_working.shadow.opacity = std::clamp(
            m_shadow_opacity_adj->get_value() / 100.0, 0.0, 1.0);
}

// ── sync_from_working ─────────────────────────────────────────────────────
//
// s201 m1 — re-populate every widget value from the current m_working +
// m_mode + m_category_order. Runs on every show() after the first
// (build() handles the first show's initial values during widget
// construction). Under m_syncing=true so the signal handlers we
// connected in build() don't write the freshly-set values back into
// m_working as if they were user edits.
//
// Mirror of build()'s initial-value reads, kept in lockstep. If you
// add a new control to build(), add its read here too; reopening the
// dialog on a different style is what catches a missing entry.
void StyleEditorDialog::sync_from_working() {
    m_syncing = true;

    // ── Identity ───────────────────────────────────────────────────
    if (m_name_entry) {
        m_name_entry->set_text(m_working.header.name);
    }

    // Category dropdown: rebuild the StringList from the current
    // m_category_order (which show() refreshed from this call's
    // user_categories), then select the entry that matches
    // m_working.header.category. Same shape as build_identity_section's
    // initial population, just executed against the existing DropDown.
    if (m_category_dd) {
        auto string_list = Gtk::StringList::create({});
        int initial_idx = 0;
        for (std::size_t i = 0; i < m_category_order.size(); ++i) {
            const std::string& c = m_category_order[i];
            const std::string display =
                (c == kCategorySentinelNew) ? c :
                (c.empty())                 ? std::string(kCategoryUncategorised) :
                                              c;
            string_list->append(display);
            if (c == m_working.header.category &&
                c != kCategorySentinelNew) {
                initial_idx = static_cast<int>(i);
            }
        }
        m_category_dd->set_model(string_list);
        m_category_dd->set_selected(static_cast<guint>(initial_idx));
    }
    if (m_category_new_entry) {
        m_category_new_entry->set_text("");
        m_category_new_entry->set_visible(false);
    }

    // ── Fill / Stroke editor render state ─────────────────────────
    // compute_render_state reads m_working.fill / m_working.stroke.paint
    // plus the per-slot binding ids; set_render_state repaints the
    // editor's chip + annotation. PaintEditor itself is a long-lived
    // child of the dialog, surviving close/reopen alongside its parent.
    if (m_fill_editor) {
        m_fill_editor->set_render_state(compute_render_state(m_working.fill));
    }
    if (m_stroke_editor) {
        m_stroke_editor->set_render_state(
            compute_render_state(m_working.stroke.paint));
    }

    // ── Stroke ────────────────────────────────────────────────────
    if (m_stroke_width_sp) {
        m_stroke_width_sp->set_internal_value(m_working.stroke.width);
        m_stroke_width_sp->refresh_units();
    }

    // Cap row — set_group radio. Setting one active deactivates the
    // others. Same as build_stroke_section's initial-state switch.
    if (m_cap_butt_btn && m_cap_round_btn && m_cap_square_btn) {
        switch (m_working.stroke.cap) {
            case LineCap::Butt:   m_cap_butt_btn  ->set_active(true); break;
            case LineCap::Round:  m_cap_round_btn ->set_active(true); break;
            case LineCap::Square: m_cap_square_btn->set_active(true); break;
        }
    }

    // Join row — same radio shape.
    if (m_join_miter_btn && m_join_round_btn && m_join_bevel_btn) {
        switch (m_working.stroke.join) {
            case LineJoin::Miter: m_join_miter_btn->set_active(true); break;
            case LineJoin::Round: m_join_round_btn->set_active(true); break;
            case LineJoin::Bevel: m_join_bevel_btn->set_active(true); break;
        }
    }

    // ── Shadow (S98) ──────────────────────────────────────────────
    if (m_shadow_enable_chk) {
        m_shadow_enable_chk->set_active(m_working.shadow.enabled);
    }
    if (m_shadow_dx_sp) {
        m_shadow_dx_sp->set_internal_value(m_working.shadow.dx);
        m_shadow_dx_sp->refresh_units();
    }
    if (m_shadow_dy_sp) {
        m_shadow_dy_sp->set_internal_value(m_working.shadow.dy);
        m_shadow_dy_sp->refresh_units();
    }
    if (m_shadow_blur_sp) {
        m_shadow_blur_sp->set_internal_value(m_working.shadow.blur);
        m_shadow_blur_sp->refresh_units();
    }
    if (m_shadow_color_a_adj) {
        m_shadow_color_a_adj->set_value(m_working.shadow.color_a * 100.0);
    }
    if (m_shadow_opacity_adj) {
        m_shadow_opacity_adj->set_value(m_working.shadow.opacity * 100.0);
    }
    if (m_shadow_swatch) m_shadow_swatch->queue_draw();

    // Sensitivity-slave: build_shadow_section captures `apply_enabled`
    // by-value into the enable-checkbutton's toggled handler; that
    // handler only fires on actual state changes, so re-syncing the
    // enable check via set_active above may or may not trigger it.
    // Re-apply directly here so the section's sensitivity always
    // reflects the freshly-loaded m_working.shadow.enabled regardless
    // of whether the previous open left the chk at the same value.
    const bool shadow_on = m_working.shadow.enabled;
    if (m_shadow_dx_sp)      m_shadow_dx_sp->set_sensitive(shadow_on);
    if (m_shadow_dy_sp)      m_shadow_dy_sp->set_sensitive(shadow_on);
    if (m_shadow_blur_sp)    m_shadow_blur_sp->set_sensitive(shadow_on);
    if (m_shadow_swatch)     m_shadow_swatch->set_sensitive(shadow_on);
    if (m_shadow_color_a_sl) m_shadow_color_a_sl->set_sensitive(shadow_on);
    if (m_shadow_opacity_sl) m_shadow_opacity_sl->set_sensitive(shadow_on);

    m_syncing = false;
}

// ── on_ok ─────────────────────────────────────────────────────────────────
void StyleEditorDialog::on_ok() {
    harvest_into_working();

    // Reattach binding ids onto Paint. The fill / stroke paints in
    // m_working are pure (None/CurrentColor/Solid) at this point —
    // any mid-edit binding-clear has already happened in the
    // PaintEditor handlers. If the user never touched the paint, the
    // binding id is still set and we wrap the Paint as a SwatchRef
    // here to round-trip the binding back into the result Style.
    {
        FillStyle fs = paint_to_fill_for_editor(m_working.fill);
        m_working.fill = fill_to_paint_with_binding(fs, m_fill_binding_id);
    }
    {
        FillStyle fs = paint_to_fill_for_editor(m_working.stroke.paint);
        m_working.stroke.paint =
            fill_to_paint_with_binding(fs, m_stroke_binding_id);
    }

    // Mode::New / Mode::Duplicate clear the id so the library mints
    // a fresh stl_<uuid> at add time. Mode::Edit preserves it.
    if (m_mode != Mode::Edit) {
        m_working.header.id.clear();
    }

    // Hand off, then close. Fire the callback first so the result
    // value-copy reads from m_working while the dialog is still
    // intact; close() triggers hide-on-close (s201 m1) which clears
    // m_on_committed via signal_close_request. Calling close before
    // the callback would lose the commit path entirely. The cached
    // local cb pointer is what fires, isolating the call from the
    // member clear that the close handler does next.
    auto cb = m_on_committed;
    style::Style result = m_working;

    if (cb) cb(std::move(result));

    close();
}

// ── on_cancel ─────────────────────────────────────────────────────────────
void StyleEditorDialog::on_cancel() {
    LOG_DEBUG("StyleEditorDialog: cancel — discarding working buffer");
    // s201 m1 — close() now triggers hide-on-close. The signal_close_
    // request handler clears m_on_committed so a stale closure can't
    // fire from any unexpected path. Working buffer overwrites on next
    // show().
    close();
}

}  // namespace Curvz
