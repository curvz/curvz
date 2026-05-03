#include "PaintEditor.hpp"

#include "color/SwatchLibrary.hpp"
#include "color/Swatch.hpp"
#include "color/Color.hpp"

#include <cairomm/context.h>
#include <glibmm/markup.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/stringlist.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Curvz {

// ── Anonymous helpers ────────────────────────────────────────────────────────
namespace {

// Reserved palette id that maps to the synthetic "All" pseudo-palette in
// the picker dropdown. Not a legal id produced by SwatchLibrary::add_
// palette (which generates UUID-style ids), so collision-safe.
constexpr const char* kAllPaletteId = "__all__";

// Chip metrics — match SwatchesPanel's CHIP_SIZE / CHIP_BORDER constants.
// We don't link the static constexpr from SwatchesPanel to keep this
// widget free of the SwatchesPanel header; values mirror the inspector
// pre-extraction chip the binding annotation lives next to.
constexpr int    CHIP_SIZE                  = 18;
constexpr double CHIP_CORNER_RADIUS         = 3.0;
constexpr double CHIP_BORDER_GREY           = 0.55;
constexpr double CHIP_BORDER_WIDTH          = 1.0;
constexpr double CHIP_BORDER_LUMA_THRESHOLD = 0.85;

// Diagonal-stripe pattern used when the host reports uniform=false. Lifted
// from PropertiesPanel.cpp's paint_mixed_swatch — duplicated here rather
// than promoted to a shared header because (a) it's small (~25 lines), (b)
// promoting it would mean leaking a Cairo dependency from a header that's
// otherwise GTK-widget-only. Keep the two copies in sync; the inspector's
// version is still used by the multi-select chip in the bind row above
// the colour widgets.
void paint_mixed_chip(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->rectangle(0, 0, w, h);
    cr->fill();
    cr->save();
    cr->set_source_rgb(0.55, 0.55, 0.55);
    cr->set_line_width(2.0);
    cr->rectangle(0, 0, w, h);
    cr->clip();
    const double span = (double)(w + h) * 1.5;
    const double step = 5.0;
    cr->translate(w * 0.5, h * 0.5);
    cr->rotate(M_PI / 4.0);
    cr->translate(-span * 0.5, -span * 0.5);
    for (double y = 0; y < span; y += step) {
        cr->move_to(0, y);
        cr->line_to(span, y);
    }
    cr->stroke();
    cr->restore();
    cr->set_source_rgba(0, 0, 0, 0.4);
    cr->set_line_width(1.0);
    cr->rectangle(0.5, 0.5, w - 1, h - 1);
    cr->stroke();
}

// Single-swatch chip draw — colour fill, optional active-binding ring.
// Mirrors SwatchesPanel::draw_chip's geometry (smaller line widths, since
// our chips are 18px rather than 24px). Active ring is drawn as a
// double-line (white inner + black outer) so it's visible on any chip
// background colour.
void paint_chip(const Cairo::RefPtr<Cairo::Context>& cr,
                int w, int h,
                const color::Color& c,
                bool is_active) {
    const double r = CHIP_CORNER_RADIUS;
    const double x = 0.5;
    const double y = 0.5;
    const double ww = static_cast<double>(w) - 1.0;
    const double hh = static_cast<double>(h) - 1.0;

    auto rounded_rect = [&](double x0, double y0,
                            double x1, double y1, double rad) {
        cr->move_to(x0 + rad, y0);
        cr->line_to(x1 - rad, y0);
        cr->arc(x1 - rad, y0 + rad, rad, -M_PI / 2, 0);
        cr->line_to(x1, y1 - rad);
        cr->arc(x1 - rad, y1 - rad, rad, 0, M_PI / 2);
        cr->line_to(x0 + rad, y1);
        cr->arc(x0 + rad, y1 - rad, rad, M_PI / 2, M_PI);
        cr->line_to(x0, y0 + rad);
        cr->arc(x0 + rad, y0 + rad, rad, M_PI, 3 * M_PI / 2);
        cr->close_path();
    };

    rounded_rect(x, y, x + ww, y + hh, r);
    cr->set_source_rgba(c.r, c.g, c.b, c.a);
    cr->fill_preserve();

    double luma = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    if (luma > CHIP_BORDER_LUMA_THRESHOLD || c.a < 0.95) {
        cr->set_source_rgba(CHIP_BORDER_GREY, CHIP_BORDER_GREY,
                            CHIP_BORDER_GREY, 1.0);
        cr->set_line_width(CHIP_BORDER_WIDTH);
        cr->stroke();
    } else {
        cr->begin_new_path();
    }

    if (is_active) {
        rounded_rect(x + 1.5, y + 1.5, x + ww - 1.5, y + hh - 1.5, r - 1);
        cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
        cr->set_line_width(1.2);
        cr->stroke();
        rounded_rect(x + 0.5, y + 0.5, x + ww - 0.5, y + hh - 0.5, r);
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
        cr->set_line_width(1.0);
        cr->stroke();
    }
}

// Resolve a swatch id to its display name + colour, picking out the
// SolidSwatch case. Returns false (no fields written) if the id is
// missing or refers to a non-solid swatch. Future gradient swatches
// would need their own visit case here.
bool resolve_solid(const color::SwatchLibrary& lib,
                   const color::SwatchId& id,
                   std::string& name_out,
                   color::Color& color_out) {
    const color::Swatch* sw = lib.find_swatch(id);
    if (!sw) return false;
    const auto* solid = std::get_if<color::SolidSwatch>(sw);
    if (!solid) return false;
    name_out  = solid->header.name;
    color_out = solid->color;
    return true;
}

} // anonymous namespace

// ── ctor ─────────────────────────────────────────────────────────────────────

PaintEditor::PaintEditor(ColorPickerPopover& popover)
    : Gtk::Box(Gtk::Orientation::VERTICAL),
      m_popover(popover) {

    // ── Type toggle row ─────────────────────────────────────────────────────
    m_type_row.set_spacing(4);
    m_type_row.set_margin_start(2);
    m_type_row.set_homogeneous(false);
    m_type_row.set_halign(Gtk::Align::START);

    // Radio group: set_group ties them together so only one can be active.
    // The group anchor is m_btn_solid; we register the others against it.
    m_btn_none.set_group(m_btn_solid);
    m_btn_cc.set_group(m_btn_solid);
    m_btn_swatch.set_group(m_btn_solid);
    m_btn_gradient.set_group(m_btn_solid);

    for (auto* b : {&m_btn_solid, &m_btn_none, &m_btn_cc,
                    &m_btn_swatch, &m_btn_gradient})
        b->add_css_class("prop-type-btn");

    m_type_row.append(m_btn_solid);
    m_type_row.append(m_btn_none);
    m_type_row.append(m_btn_cc);
    m_type_row.append(m_btn_swatch);
    m_type_row.append(m_btn_gradient);
    append(m_type_row);

    // ── Colour row ──────────────────────────────────────────────────────────
    m_color_row.set_spacing(6);
    m_color_row.set_margin_start(2);
    m_color_row.set_margin_top(3);
    m_color_row.set_margin_bottom(2);

    m_swatch.set_size_request(28, 22);
    m_swatch.add_css_class("tb-swatch");
    m_swatch.set_valign(Gtk::Align::CENTER);

    m_hex_entry.set_max_length(7);
    m_hex_entry.set_width_chars(8);
    m_hex_entry.set_placeholder_text("#RRGGBB");
    m_hex_entry.add_css_class("tb-hex-entry");

    m_bind_lbl.add_css_class("dim-label");
    m_bind_lbl.set_xalign(0.0f);
    m_bind_lbl.set_hexpand(true);
    m_bind_lbl.set_ellipsize(Pango::EllipsizeMode::END);

    // U+00D7 MULTIPLICATION SIGN — matches the inspector's pre-extraction
    // glyph. Kept as a raw button rather than a stock icon to preserve
    // the existing visual exactly.
    m_unbind_btn.set_label("\xC3\x97");
    m_unbind_btn.add_css_class("prop-toggle");

    m_color_row.append(m_swatch);
    m_color_row.append(m_hex_entry);
    m_color_row.append(m_bind_lbl);
    m_color_row.append(m_unbind_btn);
    append(m_color_row);

    // ── Gradient row (S91) ──────────────────────────────────────────────────
    //
    // Mirror of the colour row in spirit: a small preview chip + an edit
    // affordance. Shown only when the active paint is a gradient and
    // gradients_enabled is set in RenderState. Edit clicks fan out via
    // signal_gradient_edit_requested for the host to handle.
    m_gradient_row.set_spacing(6);
    m_gradient_row.set_margin_start(2);
    m_gradient_row.set_margin_top(3);
    m_gradient_row.set_margin_bottom(2);

    // Ramp preview — wide so the user can read the colour transitions,
    // same height as the swatch chip for visual rhythm with the colour
    // row above.
    m_gradient_ramp.set_size_request(120, 22);
    m_gradient_ramp.set_hexpand(true);
    m_gradient_ramp.add_css_class("tb-swatch");
    m_gradient_ramp.set_valign(Gtk::Align::CENTER);

    m_btn_edit_gradient.set_tooltip_text("Edit gradient stops, type, and angle…");

    m_gradient_row.append(m_gradient_ramp);
    m_gradient_row.append(m_btn_edit_gradient);
    append(m_gradient_row);

    // ── Picker section (S85) ────────────────────────────────────────────────
    //
    // Two children, both managed/owned by m_picker_section:
    //   1. Palette dropdown (m_palette_dd) — populated by rebuild_palette_
    //      dropdown() per RenderState. Persistent widget; we just swap the
    //      StringList model.
    //   2. Chip flow (m_chip_flow) — torn down + rebuilt on every state
    //      change (active palette, active binding id). Cells are managed
    //      so the FlowBox auto-cleans on remove.
    //
    // The empty-state label sits inside the section too; visibility is
    // mutually exclusive with m_chip_flow.
    m_picker_section.set_spacing(4);
    m_picker_section.set_margin_start(2);
    m_picker_section.set_margin_top(4);
    m_picker_section.set_margin_bottom(2);

    m_picker_empty.set_text("No swatches in library");
    m_picker_empty.add_css_class("dim-label");
    m_picker_empty.set_xalign(0.0f);

    // The dropdown and chip-flow widgets are created lazily on the first
    // set_render_state call that has library != nullptr. This keeps the
    // initial widget tree minimal for hosts that never wire a library
    // (guide / grid / margin paths). They get appended into m_picker_
    // section on first use.

    append(m_picker_section);

    // ── Wire signals ────────────────────────────────────────────────────────
    //
    // Each handler bails out when m_syncing is true to avoid emitting on
    // programmatic state changes from set_render_state. The `active` check
    // on each toggle covers GtkRadioButton's "I've been deactivated"
    // signal — only the newly-activated button should emit.

    m_btn_solid.signal_toggled().connect([this]() {
        if (m_syncing || !m_btn_solid.get_active()) return;
        m_sig_type_changed.emit(FillStyle::Type::Solid);
    });
    m_btn_none.signal_toggled().connect([this]() {
        if (m_syncing || !m_btn_none.get_active()) return;
        m_sig_type_changed.emit(FillStyle::Type::None);
    });
    m_btn_cc.signal_toggled().connect([this]() {
        if (m_syncing || !m_btn_cc.get_active()) return;
        m_sig_type_changed.emit(FillStyle::Type::CurrentColor);
    });
    // S91 Gradient toggle. Unlike the Swatch toggle (which is a view-
    // only flip that defers binding until a chip click), Gradient IS a
    // real model type-change: the user clicked "I want this fill to be
    // a gradient", and the host needs to write that into the model and
    // seed defaults if there are no stops yet. We therefore fire
    // signal_type_changed(LinearGradient) — Linear is the entry default;
    // the user can switch to Radial inside the editor.
    //
    // The early-return on "already gradient" is symmetric with the
    // other toggles: a programmatic re-activation under m_syncing
    // doesn't need to re-emit. The host's signal_type_changed handler
    // is responsible for checking whether the seed is needed (paint
    // already has stops vs. a fresh promotion from Solid).
    m_btn_gradient.signal_toggled().connect([this]() {
        if (m_syncing || !m_btn_gradient.get_active()) return;
        // If we're already on a gradient (Linear ↔ Radial within the
        // editor), no signal needed — the active state is correct
        // already. Hosts re-call set_render_state after Apply.
        if (m_have_state && m_last.paint.is_gradient()) return;
        m_sig_type_changed.emit(FillStyle::Type::LinearGradient);
    });

    m_btn_edit_gradient.signal_clicked().connect([this]() {
        if (m_syncing || !m_have_state) return;
        // Fire even if the active paint isn't a gradient — the host's
        // GradientDialog::show promotes to defaults if not already a
        // gradient. In practice the Edit button is only visible while
        // paint.is_gradient(), so the defensive case is a programmatic
        // mistake somewhere upstream rather than a real user flow.
        m_sig_gradient_edit_requested.emit(m_last.paint);
    });
    // it reveals the picker section without bothering the host. We don't
    // fire signal_type_changed because the host's break-on-override
    // logic in that signal would clear any existing binding — exactly
    // wrong for "I want to look at swatches now."
    //
    // The host only learns about a real binding intent when the user
    // clicks a chip → signal_swatch_picked. Until then, toggling Swatch
    // and then back to Solid leaves the model untouched.
    //
    // Mechanism: update m_last in place to flip is_swatch_active, then
    // call apply_picker_section + apply_color_row + apply_binding_
    // annotation to refresh dependent UI. Skipped under m_syncing so
    // programmatic toggles (in apply_type_active) don't recurse.
    m_btn_swatch.signal_toggled().connect([this]() {
        if (m_syncing || !m_have_state) return;
        if (!m_btn_swatch.get_active()) return;  // group deactivation
        if (m_last.is_swatch_active) return;     // already there
        m_last.is_swatch_active = true;
        // Re-apply dependent UI under the syncing guard so internal
        // widget signals don't fire during the refresh.
        m_syncing = true;
        apply_color_row(m_last);
        apply_binding_annotation(m_last);
        apply_picker_section(m_last);
        m_syncing = false;
    });

    // Mirror handlers on Solid / None / CC: when one of those activates
    // and m_last.is_swatch_active was true, we need to flip the local
    // flag off so apply_picker_section hides the section on the next
    // render. (Without this, the picker would linger because the host's
    // signal_type_changed re-render uses RenderState produced by the
    // host — which might still carry is_swatch_active=true if the host
    // doesn't think to clear it.) Hosts SHOULD clear it themselves, but
    // we belt-and-brace here because the type buttons fire BEFORE the
    // host's signal handler even runs, and the picker section's
    // visibility would briefly look stale otherwise.
    auto clear_swatch_local = [this]() {
        if (m_syncing) return;
        m_last.is_swatch_active = false;
        // No need to re-apply — the host's signal_type_changed handler
        // will call set_render_state next, with is_swatch_active=false
        // (host responsibility). This is just to keep m_last consistent
        // for any apply_picker_section call that runs in between.
    };
    m_btn_solid.signal_toggled().connect([this, clear_swatch_local]() {
        if (m_btn_solid.get_active()) clear_swatch_local();
    });
    m_btn_none.signal_toggled().connect([this, clear_swatch_local]() {
        if (m_btn_none.get_active()) clear_swatch_local();
    });
    m_btn_cc.signal_toggled().connect([this, clear_swatch_local]() {
        if (m_btn_cc.get_active()) clear_swatch_local();
    });
    // Symmetric handler for Gradient — activating Gradient implicitly
    // means "not in Swatch view", so clear the local flag the same way
    // Solid/None/CC do. The host will push a fresh RenderState after
    // its signal_type_changed handler fires; this just keeps m_last
    // consistent for any apply_picker_section call that runs between.
    m_btn_gradient.signal_toggled().connect([this, clear_swatch_local]() {
        if (m_btn_gradient.get_active()) clear_swatch_local();
    });

    // Hex entry — fires on Return via CurvzEntry's on_commit hook.
    //
    // No-op guard: CurvzEntry fires commit on focus-leave as well as
    // Return. When an inspector refresh tears down the panel (e.g.
    // after an undo), the about-to-be-destroyed entry loses focus
    // and triggers commit. Without this guard, that commit would
    // emit signal_color_changed at the LAST APPLIED colour (the one
    // the user just undid), the host would re-apply it, and undo
    // would visibly do nothing. Skip the emit when the parsed
    // colour matches the last RenderState we received — that's the
    // signal that nothing changed since the last sync. Verified by
    // the s84 m4i diag1 log: spurious focus-leave commit fires ~107ms
    // after Ctrl+Z, with the post-edit RGB still in the entry text;
    // matching against m_last suppresses it.
    m_hex_entry.on_commit([this]() {
        if (m_syncing) return;
        double r = 0, g = 0, b = 0;
        if (parse_hex(m_hex_entry.get_text(), r, g, b)) {
            // Compare against last applied state at 8-bit hex precision —
            // matches fills_equal's quantisation in PropertiesPanel.cpp.
            if (m_have_state && m_last.uniform &&
                m_last.paint.type == FillStyle::Type::Solid) {
                auto q = [](double v) {
                    return (int)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
                };
                if (q(r) == q(m_last.paint.r) &&
                    q(g) == q(m_last.paint.g) &&
                    q(b) == q(m_last.paint.b)) {
                    return;
                }
            }
            m_sig_color_changed.emit(r, g, b);
        }
        // Bad input is silently ignored — same as the inspector's
        // pre-extraction behaviour. Set the entry text back from the
        // last good state so the user sees the value didn't take.
        else if (m_have_state && m_last.uniform &&
                 m_last.paint.type == FillStyle::Type::Solid) {
            // Re-render under the syncing guard so this re-set doesn't
            // fire on_commit again.
            m_syncing = true;
            m_hex_entry.set_text(format_hex(m_last.paint.r,
                                            m_last.paint.g,
                                            m_last.paint.b));
            m_syncing = false;
        }
    });

    // Swatch click → open the picker. The picker writes through every
    // tick of the drag via on_changed; we re-emit those as ColorChanged
    // signals to the host.
    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect([this](int, double, double) {
        if (!m_have_state) return;
        // Initial colour for the picker. When uniform=false the chip
        // shows stripes but the picker still needs an initial colour;
        // black is fine, the host will be re-told what to render once
        // the user picks something.
        color::Color initial(m_last.paint.r,
                             m_last.paint.g,
                             m_last.paint.b,
                             m_last.has_alpha ? m_last.paint.a : 1.0);
        bool with_alpha = m_last.has_alpha;
        m_popover.open(
            m_swatch, initial, with_alpha,
            // on_changed — picker drag tick or hex commit inside picker.
            [this](const color::Color& c) {
                if (m_syncing) return;
                m_sig_color_changed.emit(c.r, c.g, c.b);
            },
            // on_closed — committed flag forwarded to host. Most hosts
            // ignore this; SwatchesPanel uses it for create-on-Esc-
            // remove. The widget itself doesn't need to do anything
            // here — the picker has already self-reverted on Esc and
            // emitted the on_changed callback at the original colour.
            [this](bool committed) {
                m_sig_picker_closed.emit(committed);
            });
    });
    m_swatch.add_controller(click);

    // × unbind. Visibility is driven by RenderState; when hidden, the
    // click handler is unreachable so no guard needed.
    m_unbind_btn.signal_clicked().connect([this]() {
        if (m_syncing) return;
        m_sig_unbind_clicked.emit();
    });

    // Initial visibility — until the host pushes a state, the colour row
    // is hidden (no paint to show). Type buttons are visible but inert
    // because nothing is active yet. Picker section + gradient row hidden
    // too.
    m_color_row.set_visible(false);
    m_bind_lbl.set_visible(false);
    m_unbind_btn.set_visible(false);
    m_picker_section.set_visible(false);
    m_gradient_row.set_visible(false);
}

// ── set_render_state ─────────────────────────────────────────────────────────

void PaintEditor::set_render_state(const RenderState& s) {
    m_last = s;
    m_have_state = true;

    // Programmatic updates run inside m_syncing so widget signal handlers
    // bail. Pattern is the same as PropertiesPanel's m_loading flag — see
    // header comment.
    m_syncing = true;

    // Type toggle active state. Set all four; only the matching one
    // stays active because they share a radio group. For the mixed case
    // (uniform=false) we leave all four deactivated — the inspector's
    // pre-extraction behaviour was that mixed paint shows no toggle as
    // active so the user can still click any of them to snap the
    // selection.
    if (s.uniform) {
        apply_type_active(s.paint.type, s.is_swatch_active);
    } else {
        // Deactivate all five. Setting a button inactive when it's the
        // current group anchor leaves the group with nothing active —
        // GTK4 allows this for ToggleButton groups. Verified by the
        // inspector's pre-extraction flow which did exactly this.
        m_btn_solid.set_active(false);
        m_btn_none.set_active(false);
        m_btn_cc.set_active(false);
        m_btn_swatch.set_active(false);
        m_btn_gradient.set_active(false);
    }

    // Swatch toggle is sensitive only when host has wired a library.
    // Done outside the active-state block because library presence is
    // independent of which type is active.
    m_btn_swatch.set_sensitive(s.library != nullptr);

    // S91: Gradient toggle is sensitive only when the host opts in via
    // gradients_enabled. Both the inspector (PropertiesPanel) and the
    // Style Manager (StyleEditorDialog, post-S93 m3) opt in.
    m_btn_gradient.set_sensitive(s.gradients_enabled);

    apply_color_row(s);
    apply_binding_annotation(s);
    apply_picker_section(s);
    apply_gradient_row(s);

    m_syncing = false;
}

void PaintEditor::apply_type_active(FillStyle::Type t, bool is_swatch) {
    // is_swatch overrides t — when the host says the active type is
    // Swatch, the underlying FillStyle::Type might still be Solid (the
    // editor projects SwatchRef→Solid for chip rendering), but the
    // toggle should sit on Swatch.
    if (is_swatch) {
        m_btn_solid.set_active(false);
        m_btn_none.set_active(false);
        m_btn_cc.set_active(false);
        m_btn_swatch.set_active(true);
        m_btn_gradient.set_active(false);
        return;
    }
    // S91: Linear and Radial both light up the same Gradient toggle —
    // the dialog handles linear↔radial within itself. The toggle row is
    // about *kind of paint*, not gradient sub-type.
    const bool is_grad = (t == FillStyle::Type::LinearGradient ||
                          t == FillStyle::Type::RadialGradient);
    m_btn_solid.set_active(t == FillStyle::Type::Solid);
    m_btn_none.set_active(t == FillStyle::Type::None);
    m_btn_cc.set_active(t == FillStyle::Type::CurrentColor);
    m_btn_swatch.set_active(false);
    m_btn_gradient.set_active(is_grad);
}

void PaintEditor::apply_color_row(const RenderState& s) {
    // Colour row is visible whenever paint *might* be Solid: uniform Solid
    // (definitely show), uniform Swatch-active (show — colour preview is
    // what the binding resolves to), or non-uniform (show so the user can
    // snap to Solid via swatch click). Hidden on uniform-non-Solid-and-
    // non-Swatch (None / CurrentColor / Gradient).
    //
    // Gradient note (S91): when the active paint is a gradient, the
    // gradient row owns the chip-area real estate; the colour row stays
    // hidden. Switching back to Solid (via toggle) reveals the colour
    // row again.
    bool show_row;
    if (!s.uniform) {
        show_row = true;
    } else if (s.is_swatch_active) {
        show_row = true;
    } else {
        show_row = (s.paint.type == FillStyle::Type::Solid);
    }
    m_color_row.set_visible(show_row);
    if (!show_row) return;

    if (!s.uniform) {
        m_hex_entry.set_text("");
        m_hex_entry.set_placeholder_text("mixed");
        m_swatch.set_draw_func(
            [](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                paint_mixed_chip(cr, w, h);
            });
    } else {
        m_hex_entry.set_placeholder_text("#RRGGBB");
        m_hex_entry.set_text(format_hex(s.paint.r, s.paint.g, s.paint.b));
        // Capture by value for the draw closure — the lambda outlives
        // this call. Using FillStyle by value (just 5 doubles) avoids
        // any risk of dangling refs.
        FillStyle pc = s.paint;
        m_swatch.set_draw_func(
            [pc](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                cr->set_source_rgb(pc.r, pc.g, pc.b);
                cr->rectangle(0, 0, w, h);
                cr->fill();
                cr->set_source_rgba(0, 0, 0, 0.4);
                cr->set_line_width(1.0);
                cr->rectangle(0.5, 0.5, w - 1, h - 1);
                cr->stroke();
            });
    }
    m_swatch.queue_draw();
}

// ── Gradient row (S91) ───────────────────────────────────────────────────────

void PaintEditor::apply_gradient_row(const RenderState& s) {
    // Visibility gate: row shows iff the host opted into gradient
    // editing AND the active paint is a gradient AND the selection is
    // uniform. Mixed selections fall through the colour-row path (the
    // user clicks Gradient on the toggle row to snap everyone to a
    // gradient — that fires signal_type_changed).
    const bool show = s.gradients_enabled
                   && s.uniform
                   && s.paint.is_gradient();
    m_gradient_row.set_visible(show);
    if (!show) return;

    // Capture stops by value for the draw closure — same lifetime
    // pattern as the swatch chip's per-paint capture above. The vector
    // copy is small (each stop is 5 doubles + an offset) and the closure
    // outlives this call until the next set_render_state.
    std::vector<GradientStop> stops = s.paint.stops;
    // Sort defensively so reversed lists still render in canonical
    // order. Mirrors GradientDialog::draw_track's sort on read.
    std::sort(stops.begin(), stops.end(),
              [](const GradientStop& a, const GradientStop& b) {
                  return a.offset < b.offset;
              });

    m_gradient_ramp.set_draw_func(
        [stops](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            // Empty-stops fallback — paint a flat mid-grey so the user
            // sees *something* rather than a transparent strip. In
            // practice the host seeds defaults before we get here, but
            // a 0-stop gradient is a legal intermediate state during
            // type-change → seeding.
            if (stops.empty()) {
                cr->set_source_rgb(0.5, 0.5, 0.5);
                cr->rectangle(0, 0, w, h);
                cr->fill();
            } else if (stops.size() == 1) {
                // Single stop: flat fill at that colour. Cairo's
                // LinearGradient with one stop renders transparent on
                // some backends, so handle it explicitly.
                const auto& s0 = stops.front();
                cr->set_source_rgba(s0.r, s0.g, s0.b, s0.a);
                cr->rectangle(0, 0, w, h);
                cr->fill();
            } else {
                auto pat = Cairo::LinearGradient::create(0, 0, w, 0);
                for (const auto& st : stops) {
                    pat->add_color_stop_rgba(
                        std::clamp(st.offset, 0.0, 1.0),
                        st.r, st.g, st.b, st.a);
                }
                cr->set_source(pat);
                cr->rectangle(0, 0, w, h);
                cr->fill();
            }
            // Faint outline so the ramp reads as a UI element, not
            // bleed. Same idiom as the chip swatch above.
            cr->set_source_rgba(0, 0, 0, 0.4);
            cr->set_line_width(1.0);
            cr->rectangle(0.5, 0.5, w - 1, h - 1);
            cr->stroke();
        });
    m_gradient_ramp.queue_draw();
}

void PaintEditor::apply_binding_annotation(const RenderState& s) {
    // The annotation has dual identity (matches inspector pre-extraction):
    //
    //   * BOUND   → bold-italic name (or "<multiple>") with × visible.
    //   * UNBOUND → plain-italic region-name fallback with × hidden.
    //
    // Mixed-binding selections render BOUND styling because they ARE
    // bound, just inconsistently — the × is visible so the user can
    // unbind everyone in one click.
    //
    // Hidden entirely when:
    //   - paint type is not Solid (None / CurrentColor have no colour
    //     to annotate)
    //   - bound is false AND unbound_display_name is empty (nothing
    //     useful to show)
    //   - the colour row itself is hidden

    if (!m_color_row.get_visible()) {
        m_bind_lbl.set_visible(false);
        m_unbind_btn.set_visible(false);
        return;
    }

    // Mixed-paint selections show "mixed" in the hex entry and stripes
    // on the chip; layering a binding annotation on top would be noisy.
    // Hide both annotation widgets in that case. Matches the inspector's
    // pre-extraction `show_annotation = uniform_now` gate.
    if (!s.uniform) {
        m_bind_lbl.set_visible(false);
        m_unbind_btn.set_visible(false);
        return;
    }

    m_unbind_btn.set_visible(s.bound);
    // Unbind tooltip set per-call so multi-instance editors (fill +
    // stroke) carry slot-specific text from the host.
    m_unbind_btn.set_tooltip_text(
        s.bound_tooltip.empty() ? std::string("Unbind from swatch")
                                : s.bound_tooltip);

    bool has_text = false;
    std::string display;
    std::string tooltip;
    bool bold_italic = false;

    if (s.bound) {
        if (s.bound_mixed) {
            display = "<multiple>";
            tooltip = "Selected nodes are bound to multiple swatches";
        } else {
            display = s.bound_display_name;
            tooltip = s.bound_tooltip;
        }
        bold_italic = true;
        has_text = !display.empty();
    } else if (s.paint.type == FillStyle::Type::Solid || s.is_swatch_active) {
        // Region-name fallback shows under both Solid and Swatch-active
        // (when no binding yet) — same useful signal in both cases.
        display = s.unbound_display_name;
        tooltip = "Derived colour-region name (this colour is not "
                  "bound to a swatch)";
        bold_italic = false;
        has_text = !display.empty();
    }

    m_bind_lbl.set_visible(has_text);
    if (!has_text) return;

    // Pango markup needs angle brackets / ampersands escaped — the
    // display string can contain "<multiple>".
    std::string esc = Glib::Markup::escape_text(display);
    std::string markup = bold_italic
        ? std::string("<b><i>(") + esc + ")</i></b>"
        : std::string("<i>(")    + esc + ")</i>";
    m_bind_lbl.set_markup(markup);
    m_bind_lbl.set_tooltip_text(tooltip);
}

// ── Picker section (S85) ─────────────────────────────────────────────────────

void PaintEditor::apply_picker_section(const RenderState& s) {
    // Visibility gate: section shows iff the Swatch toggle is the active
    // type AND a library was wired. (Without a library the toggle is
    // greyed out and unreachable; defensive on both checks anyway.)
    bool show = s.uniform && s.is_swatch_active && (s.library != nullptr);
    m_picker_section.set_visible(show);
    if (!show) return;

    const auto& lib = *s.library;

    // Lazy-create the dropdown + chip flow on first show. Both live
    // permanently in m_picker_section once created (the chip flow is
    // emptied + repopulated, never destroyed).
    if (!m_palette_dd) {
        // Use empty StringList; rebuild_palette_dropdown will swap a
        // populated one in below.
        auto sl = Gtk::StringList::create({});
        m_palette_dd = Gtk::make_managed<Gtk::DropDown>(sl);
        m_palette_dd->set_hexpand(true);
        m_palette_dd->add_css_class("flat");
        m_palette_dd->add_css_class("caption");
        m_picker_section.append(*m_palette_dd);
    }
    if (!m_chip_flow) {
        m_chip_flow = Gtk::make_managed<Gtk::FlowBox>();
        m_chip_flow->set_orientation(Gtk::Orientation::HORIZONTAL);
        m_chip_flow->set_homogeneous(true);
        m_chip_flow->set_min_children_per_line(1);
        m_chip_flow->set_max_children_per_line(16);
        m_chip_flow->set_row_spacing(4);
        m_chip_flow->set_column_spacing(4);
        m_chip_flow->set_selection_mode(Gtk::SelectionMode::NONE);
        m_picker_section.append(*m_chip_flow);
        m_picker_section.append(m_picker_empty);
    }

    // Library completely empty? Show empty-state and hide chooser.
    if (lib.empty()) {
        m_palette_dd->set_visible(false);
        m_chip_flow->set_visible(false);
        m_picker_empty.set_visible(true);
        return;
    }

    m_palette_dd->set_visible(true);
    m_picker_empty.set_visible(false);

    // Rebuild the dropdown's StringList; m_picker_palette_ids tracks the
    // dropdown's order. Selection-changed handler is reconnected after
    // rebuild because we disconnect-then-rebuild to avoid spurious fires.
    rebuild_palette_dropdown(lib, s.binding_id);

    // Resolve the currently-selected palette → list of swatch ids the
    // chip grid should render. Sentinel "__all__" means walk every
    // swatch in the library. Empty palette → empty grid.
    std::vector<color::SwatchId> grid_ids;
    if (m_picker_palette_ids.empty()) {
        // No palettes defined — fall back to "All".
        grid_ids = lib.all_swatch_ids();
    } else {
        guint sel_idx = m_palette_dd->get_selected();
        if (sel_idx == GTK_INVALID_LIST_POSITION ||
            sel_idx >= m_picker_palette_ids.size()) {
            sel_idx = 0;
        }
        const color::PaletteId& pid = m_picker_palette_ids[sel_idx];
        if (pid == kAllPaletteId) {
            grid_ids = lib.all_swatch_ids();
        } else if (const color::Palette* p = lib.find_palette(pid)) {
            grid_ids = p->swatches;
        }
    }

    rebuild_chip_grid(lib, grid_ids, s.binding_id);
    m_chip_flow->set_visible(true);
}

void PaintEditor::rebuild_palette_dropdown(const color::SwatchLibrary& lib,
                                            const std::string& binding_id) {
    if (!m_palette_dd) return;

    // Disconnect prior signal so the rebuild + programmatic re-selection
    // below don't fire user-flow handlers. Reconnected at the end of
    // this method.
    m_palette_dd_conn.disconnect();

    auto pids = lib.all_palette_ids();
    auto sl = Gtk::StringList::create({});

    // Build (id, name) order. We always offer "All" as the first row;
    // it's a synthetic palette that lists every swatch, and it's the
    // only place a swatch outside any palette can be discovered.
    m_picker_palette_ids.clear();
    m_picker_palette_ids.reserve(pids.size() + 1);

    m_picker_palette_ids.push_back(kAllPaletteId);
    sl->append("All");

    for (const auto& pid : pids) {
        const color::Palette* p = lib.find_palette(pid);
        sl->append(p && !p->name.empty() ? p->name : pid);
        m_picker_palette_ids.push_back(pid);
    }

    m_palette_dd->set_model(sl);

    // Pick which palette to land on:
    //   1. The palette containing binding_id, if any.
    //   2. Else the library's active_palette(), if it exists.
    //   3. Else row 0 ("All").
    guint chosen_idx = 0;
    if (!binding_id.empty()) {
        for (std::size_t i = 1; i < m_picker_palette_ids.size(); ++i) {
            const color::Palette* p =
                lib.find_palette(m_picker_palette_ids[i]);
            if (!p) continue;
            const auto& sws = p->swatches;
            if (std::find(sws.begin(), sws.end(), binding_id) != sws.end()) {
                chosen_idx = static_cast<guint>(i);
                break;
            }
        }
    }
    if (chosen_idx == 0) {
        const auto& active = lib.active_palette();
        if (!active.empty()) {
            for (std::size_t i = 1; i < m_picker_palette_ids.size(); ++i) {
                if (m_picker_palette_ids[i] == active) {
                    chosen_idx = static_cast<guint>(i);
                    break;
                }
            }
        }
    }

    // Programmatic selection — m_syncing is already true from the
    // outer set_render_state call, so nothing fires anyway, but we
    // also defensively skip the user handler via the disconnect
    // above.
    m_palette_dd->set_selected(chosen_idx);

    // Reconnect. Selection-changed re-runs apply_picker_section to
    // rebuild the chip grid for the newly chosen palette. The palette
    // change itself doesn't propagate to the host — the picker is a
    // local navigation surface, the binding id only changes when the
    // user actually clicks a chip.
    m_palette_dd_conn = m_palette_dd->property_selected().signal_changed()
        .connect([this]() {
            if (m_syncing) return;
            if (!m_have_state || !m_last.library) return;
            // Rebuild the chip grid for the new palette without a full
            // set_render_state cycle — that would re-disconnect /
            // reconnect this signal under us.
            const auto& lib = *m_last.library;
            std::vector<color::SwatchId> grid_ids;
            guint sel_idx = m_palette_dd->get_selected();
            if (sel_idx != GTK_INVALID_LIST_POSITION &&
                sel_idx < m_picker_palette_ids.size()) {
                const auto& pid = m_picker_palette_ids[sel_idx];
                if (pid == kAllPaletteId) {
                    grid_ids = lib.all_swatch_ids();
                } else if (const color::Palette* p =
                              lib.find_palette(pid)) {
                    grid_ids = p->swatches;
                }
            }
            rebuild_chip_grid(lib, grid_ids, m_last.binding_id);
        });
}

void PaintEditor::rebuild_chip_grid(const color::SwatchLibrary& lib,
                                     const std::vector<color::SwatchId>& ids,
                                     const std::string& active_binding_id) {
    if (!m_chip_flow) return;

    // Tear down current children. FlowBox's remove takes a child widget;
    // get_first_child + iterate is the GTK4 idiom.
    while (auto* child = m_chip_flow->get_first_child()) {
        m_chip_flow->remove(*child);
    }

    for (const auto& id : ids) {
        std::string name;
        color::Color c;
        if (!resolve_solid(lib, id, name, c)) continue; // skip non-solid
                                                         // / dangling

        const bool is_active = (!active_binding_id.empty() &&
                                active_binding_id == id);

        auto* area = Gtk::make_managed<Gtk::DrawingArea>();
        area->set_content_width(CHIP_SIZE);
        area->set_content_height(CHIP_SIZE);
        // Capture colour by value so the draw closure is independent of
        // any later library mutation. is_active is fine to capture by
        // value too — the chip is rebuilt whenever active_binding_id
        // changes.
        area->set_draw_func(
            [c, is_active](const Cairo::RefPtr<Cairo::Context>& cr,
                           int w, int h) {
                paint_chip(cr, w, h, c, is_active);
            });
        area->add_css_class("swatch-chip");
        area->set_cursor(Gdk::Cursor::create("pointer"));

        // Tooltip: "name #rrggbb" when name available, else "#rrggbb".
        std::string hex = color::to_hex(c);
        std::string tip = name.empty() ? hex : (name + " " + hex);
        area->set_tooltip_text(tip);

        // Left-click → emit signal_swatch_picked. No right-click (no
        // context menu in the picker; that lives in SwatchesPanel).
        // Use signal_released to match SwatchesPanel — pressed-style
        // gestures sometimes get swallowed by sequence-claim conflicts.
        auto gesture = Gtk::GestureClick::create();
        gesture->set_button(1);
        gesture->signal_released().connect(
            [this, id](int, double, double) {
                if (m_syncing) return;
                m_sig_swatch_picked.emit(id);
            });
        area->add_controller(gesture);

        m_chip_flow->append(*area);
    }
}

// ── Hex helpers ──────────────────────────────────────────────────────────────

bool PaintEditor::parse_hex(const std::string& hex,
                            double& r, double& g, double& b) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() != 6) return false;
    try {
        r = std::stoi(s.substr(0, 2), nullptr, 16) / 255.0;
        g = std::stoi(s.substr(2, 2), nullptr, 16) / 255.0;
        b = std::stoi(s.substr(4, 2), nullptr, 16) / 255.0;
        return true;
    } catch (...) {
        return false;
    }
}

std::string PaintEditor::format_hex(double r, double g, double b) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                  (int)std::round(std::clamp(r, 0.0, 1.0) * 255),
                  (int)std::round(std::clamp(g, 0.0, 1.0) * 255),
                  (int)std::round(std::clamp(b, 0.0, 1.0) * 255));
    return buf;
}

} // namespace Curvz
