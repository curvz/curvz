// ThemeEditDialog.cpp — s183 m2 skeleton implementation.
//
// Identity section + button row. Body is a placeholder Gtk::Box that
// m3 (live thumbnail) and m4 (property controls) populate. m5 wires
// the Reset semantic. Lifetime self-management mirrors
// StyleEditorDialog: heap-allocated, signal_close_request defers
// deletion to signal_idle.

#include "ThemeEditDialog.hpp"

#include "CurvzEntry.hpp"
#include "CurvzLog.hpp"
#include "curvz_utils.hpp"

#include <giomm/liststore.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/notebook.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stringlist.h>
#include <glibmm/main.h>

#include <cctype>
#include <cmath>
#include <string>

namespace Curvz {
namespace {

// Local trim — same shape as StyleEditorDialog's. ASCII whitespace
// only; the panel's rename rules are ASCII-trim too, so a value that
// passes here passes there.
std::string trim_ws(const std::string& s) {
    std::size_t lead = 0;
    while (lead < s.size() &&
           std::isspace(static_cast<unsigned char>(s[lead]))) {
        ++lead;
    }
    std::size_t trail = s.size();
    while (trail > lead &&
           std::isspace(static_cast<unsigned char>(s[trail - 1]))) {
        --trail;
    }
    return s.substr(lead, trail - lead);
}

// s183 m3 — Mock-canvas dimensions for the thumbnail preview. The
// thumbnail draws this synthetic doc-space rect into screen space
// scaled to fit the DrawingArea, with workspace fill around the
// artboard. The dimensions matter only for grid spacing and margin
// values to look representative — pick a "common print-ish" mock
// (8×6 ratio, units arbitrary) so users can read the preview
// against their mental model of a typical artboard. Not exposed; the
// preview's purpose is theme appearance, not document geometry.
constexpr double kMockDocW = 800.0;
constexpr double kMockDocH = 600.0;

// Thumbnail screen size — drives DrawingArea size_request. Aspect
// matches kMockDocW:kMockDocH so the artboard scales without
// letterboxing.
constexpr int    kThumbnailW = 280;
constexpr int    kThumbnailH = 210;

} // namespace

// ── Construction ──────────────────────────────────────────────────────────
ThemeEditDialog::ThemeEditDialog(Gtk::Window& parent,
                                  theme::Theme initial,
                                  Motif initial_mode,
                                  CommittedFn on_committed)
    : m_working(initial)
    , m_initial(std::move(initial))
    , m_on_committed(std::move(on_committed))
    , m_preview_mode(initial_mode) {
    m_window = Gtk::make_managed<Gtk::Window>();
    m_window->set_title("Edit Theme");
    m_window->set_modal(true);
    // s183 m4 fix2: allow resize so user can drag to a usable size
    // on small screens. The notebook is the section that grows; the
    // ScrolledWindow inside build() bounds it on the shrink side.
    m_window->set_resizable(true);
    m_window->set_transient_for(parent);
    curvz::utils::apply_motif_class_from_parent(*m_window, parent);
    // m3: thumbnail is ~280px wide; identity grid + thumbnail width
    // sets the comfortable column. Height is natural — thumbnail +
    // controls determine it.
    m_window->set_default_size(420, -1);
    curvz::utils::set_name(m_window, "dlg_te", "theme_editor_dialog_window");

    build();

    // s183 m4 — attach colour-picker popovers to the dialog window.
    // attach() once per popover; open() per swatch click. The
    // signal_close_request handler below detaches before window
    // finalisation so GTK doesn't warn "Finalizing GtkWindow, but
    // it still has children left" on the popover children.
    m_motif_artboard_popover.attach(*m_window);
    m_motif_workspace_popover.attach(*m_window);
    m_motif_creation_popover.attach(*m_window);
    m_guides_popover.attach(*m_window);
    m_grid_popover.attach(*m_window);
    m_margin_popover.attach(*m_window);

    // Self-delete on window close. Same idiom as StyleEditorDialog —
    // the inner Gtk::Window is `make_managed` (lives until close); the
    // outer ThemeEditDialog object is heap-allocated by the wiring
    // lambda and we hook signal_close_request to delete `this` on the
    // next idle tick. Deferring via signal_idle avoids re-entering
    // GTK's own dispatch from inside the close-request handler.
    m_window->signal_close_request().connect(
        [this]() -> bool {
            // Detach popovers BEFORE window finalisation. See
            // StyleEditorDialog's same pattern.
            m_motif_artboard_popover.detach();
            m_motif_workspace_popover.detach();
            m_motif_creation_popover.detach();
            m_guides_popover.detach();
            m_grid_popover.detach();
            m_margin_popover.detach();
            Glib::signal_idle().connect_once([this]() { delete this; });
            return false;  // allow default close to proceed
        }, /*after=*/false);

    m_window->present();
}

ThemeEditDialog::~ThemeEditDialog() = default;

// ── build ─────────────────────────────────────────────────────────────────
void ThemeEditDialog::build() {
    m_syncing = true;

    // s183 m4 fix2: keep the dialog usable on small screens. Identity,
    // thumbnail, mode toggle, and the button row stay pinned. Only
    // the notebook scrolls.
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    root->set_margin(12);
    m_window->set_child(*root);

    // s183 m4 fix2 — seed the dialog's CanvasModel from the working
    // theme's display_unit so spinners initially display in the
    // theme's chosen unit.
    m_unit_model.display_unit = m_working.units.display_unit;

    build_identity_section(*root);
    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_body_section(*root);             // thumbnail + mode toggle (pinned)

    // Wrap the notebook in a ScrolledWindow. vexpand lets it absorb
    // any extra space; min content height bounds the scroller's
    // shrink, keeping at least a sensible amount of the notebook
    // visible. The button row below stays at the bottom of the
    // dialog and remains accessible regardless of internal scroll.
    m_props_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    curvz::utils::set_name(m_props_scroll, "dlg_te_scroll",
                           "theme_editor_dialog_props_scroll");
    m_props_scroll->set_policy(Gtk::PolicyType::NEVER,
                               Gtk::PolicyType::AUTOMATIC);
    m_props_scroll->set_vexpand(true);
    m_props_scroll->set_min_content_height(220);
    // s129 m9 lesson: set_propagate_natural_width(false) on
    // ScrolledWindow whose parent constrains width — otherwise the
    // notebook may push the dialog wider than intended. The dialog
    // itself doesn't constrain width tightly, so leave default
    // propagation; if width creeps we revisit.
    root->append(*m_props_scroll);

    auto* scroll_inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    m_props_scroll->set_child(*scroll_inner);
    build_properties_notebook(*scroll_inner);

    root->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
    build_button_row(*root);

    // s183 m4 prelude — initial highlight on whichever mode toggle
    // was set active during build_body_section.
    refresh_mode_toggle_highlight();

    m_syncing = false;
}

// ── build_identity_section ────────────────────────────────────────────────
//
// Just the name field for v1. Themes carry a header.category in the
// data model but no UI ever surfaced it (StylesPanel uses categories;
// ThemesPanel doesn't), so the dialog skips it too. If category UI
// arrives on the panel later, the same StyleEditorDialog idiom (a
// DropDown over user_categories with a "New category…" sentinel)
// drops in here unchanged.
void ThemeEditDialog::build_identity_section(Gtk::Box& root) {
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(8);
    root.append(*grid);

    auto* lbl_name = Gtk::make_managed<Gtk::Label>("Name:");
    lbl_name->set_xalign(0.0f);
    grid->attach(*lbl_name, 0, 0, 1, 1);

    m_name_entry = Gtk::make_managed<CurvzEntry>();
    curvz::utils::set_name(m_name_entry, "dlg_te_nm",
                           "theme_editor_dialog_name_entry");
    m_name_entry->set_text(m_working.header.name);
    m_name_entry->set_hexpand(true);
    grid->attach(*m_name_entry, 1, 0, 2, 1);

    // Idempotency guard mirrors StyleEditorDialog:
    //   - m_syncing skips during initial build.
    //   - Compare typed-vs-cached so destruction-time focus-leave
    //     fires don't churn the working buffer with the same value.
    m_name_entry->on_commit([this]() {
        if (m_syncing) return;
        std::string typed = trim_ws(m_name_entry->get_text());
        if (typed == m_working.header.name) return;
        m_working.header.name = typed;
    });
}

// ── build_body_section ────────────────────────────────────────────────────
//
// s183 m3 — live preview. Mode toggle row (Light / Dark) above a
// DrawingArea that renders a mock canvas using m_working's settings
// and m_preview_mode for the motif slot. m4 attaches per-sub-bundle
// property control sections below the thumbnail.
//
// The mode toggle pair are linked via set_group() so they behave as
// a radio. State change updates m_preview_mode and queue_draws the
// thumbnail. Critically, the toggle does NOT change the dialog's
// chrome motif — the dialog remains in the host app's actual mode.
// The toggle's only effect is which slot of m_working.motif the
// thumbnail reads from.
void ThemeEditDialog::build_body_section(Gtk::Box& root) {
    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    curvz::utils::set_name(body, "dlg_te_body", "theme_editor_dialog_body");
    body->set_halign(Gtk::Align::CENTER);
    root.append(*body);

    // ── Mode toggle row ─────────────────────────────────────────────
    auto* mode_row =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    mode_row->set_halign(Gtk::Align::CENTER);
    mode_row->add_css_class("linked");
    body->append(*mode_row);

    m_mode_light_btn = Gtk::make_managed<Gtk::ToggleButton>("Light");
    curvz::utils::set_name(m_mode_light_btn, "dlg_te_mode_l",
                           "theme_editor_dialog_mode_light_btn");
    m_mode_light_btn->set_tooltip_text(
        "Preview the theme's light-mode appearance");
    mode_row->append(*m_mode_light_btn);

    m_mode_dark_btn = Gtk::make_managed<Gtk::ToggleButton>("Dark");
    curvz::utils::set_name(m_mode_dark_btn, "dlg_te_mode_d",
                           "theme_editor_dialog_mode_dark_btn");
    m_mode_dark_btn->set_tooltip_text(
        "Preview the theme's dark-mode appearance");
    // Link the two toggles into a radio group so exactly one is on.
    m_mode_dark_btn->set_group(*m_mode_light_btn);
    mode_row->append(*m_mode_dark_btn);

    // Initial state — match m_preview_mode (set by ctor from
    // initial_mode arg). set_active(true) on one of the two toggles
    // forces the other to false via the group.
    if (m_preview_mode == Motif::Light) {
        m_mode_light_btn->set_active(true);
    } else {
        m_mode_dark_btn->set_active(true);
    }

    // Toggle handlers. Either toggle changing into the active state
    // is the canonical "user picked this mode" event. The inactive
    // toggle's signal also fires for the deactivation, but we only
    // act on the activation — guards against double-fires.
    m_mode_light_btn->signal_toggled().connect([this]() {
        if (m_syncing) return;
        if (!m_mode_light_btn->get_active()) return;  // ignore the off-edge
        if (m_preview_mode == Motif::Light) return;
        m_preview_mode = Motif::Light;
        refresh_mode_toggle_highlight();
        refresh_motif_swatches();
        queue_thumbnail_redraw();
    });
    m_mode_dark_btn->signal_toggled().connect([this]() {
        if (m_syncing) return;
        if (!m_mode_dark_btn->get_active()) return;
        if (m_preview_mode == Motif::Dark) return;
        m_preview_mode = Motif::Dark;
        refresh_mode_toggle_highlight();
        refresh_motif_swatches();
        queue_thumbnail_redraw();
    });

    // ── Thumbnail DrawingArea ───────────────────────────────────────
    m_thumbnail = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(m_thumbnail, "dlg_te_thumb",
                           "theme_editor_dialog_thumbnail");
    m_thumbnail->set_size_request(kThumbnailW, kThumbnailH);
    m_thumbnail->set_can_target(false);
    m_thumbnail->set_draw_func(
        [this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            draw_thumbnail(cr, w, h);
        });
    body->append(*m_thumbnail);
}

// ── queue_thumbnail_redraw ────────────────────────────────────────────────
//
// Cheap. Called after any working-buffer change. m3 only the mode
// toggle drives this; m4 will add a call from every property
// control's edit handler.
void ThemeEditDialog::queue_thumbnail_redraw() {
    if (m_thumbnail) m_thumbnail->queue_draw();
}

// ── draw_thumbnail ────────────────────────────────────────────────────────
//
// Renders the mock canvas. Layer order matches the real Canvas:
//   1. Workspace fill (entire DrawingArea)
//   2. Artboard fill (fitted into the area with margin around it)
//   3. Margin guide lines (if margins.enabled)
//   4. Grid lines or dots (if grid.enabled)
//   5. Sample guides (one V + one H, in guides.color)
//
// All draws are clipped to the thumbnail size. The mock-doc-space
// → screen transform is computed once per draw: scale-fit the mock
// rect with a pad on each side so workspace shows around the
// artboard. The padding's motif-blue is the workspace colour from
// the working theme's motif slot for the previewed mode.
void ThemeEditDialog::draw_thumbnail(const Cairo::RefPtr<Cairo::Context>& cr,
                                     int w, int h) const {
    // Pick the motif triple for the previewed mode. Reads from the
    // working buffer so live-edits in m4 reflect immediately.
    const auto& motif = m_working.motif;
    double ws_r, ws_g, ws_b, ab_r, ab_g, ab_b;
    if (m_preview_mode == Motif::Light) {
        ws_r = motif.light_workspace_r;
        ws_g = motif.light_workspace_g;
        ws_b = motif.light_workspace_b;
        ab_r = motif.light_artboard_r;
        ab_g = motif.light_artboard_g;
        ab_b = motif.light_artboard_b;
    } else {
        ws_r = motif.dark_workspace_r;
        ws_g = motif.dark_workspace_g;
        ws_b = motif.dark_workspace_b;
        ab_r = motif.dark_artboard_r;
        ab_g = motif.dark_artboard_g;
        ab_b = motif.dark_artboard_b;
    }

    // 1. Workspace fill.
    cr->set_source_rgba(ws_r, ws_g, ws_b, 1.0);
    cr->rectangle(0, 0, w, h);
    cr->fill();

    // ── Compute fit transform: mock doc → screen, with pad ──────────
    // Pad in screen pixels so the workspace ring is visible at any
    // thumbnail size. Aspect-fit with the smaller scale axis.
    constexpr double pad_px = 14.0;
    const double avail_w = w - 2 * pad_px;
    const double avail_h = h - 2 * pad_px;
    if (avail_w <= 0 || avail_h <= 0) return;
    const double sx = avail_w / kMockDocW;
    const double sy = avail_h / kMockDocH;
    const double scale = std::min(sx, sy);
    const double draw_w = kMockDocW * scale;
    const double draw_h = kMockDocH * scale;
    const double off_x = (w - draw_w) * 0.5;
    const double off_y = (h - draw_h) * 0.5;

    cr->save();
    cr->translate(off_x, off_y);
    cr->scale(scale, scale);

    // 2. Artboard fill.
    cr->set_source_rgba(ab_r, ab_g, ab_b, 1.0);
    cr->rectangle(0, 0, kMockDocW, kMockDocH);
    cr->fill();

    // 3. Margins (if enabled). Mirror Canvas::draw_margin_doc_space
    // line semantics — outline rect, column gap dividers, row gap
    // dividers. Alpha is bumped to ~3× as the real canvas does so
    // the lines stay legible at thumbnail scale.
    const auto& mg = m_working.margins;
    if (mg.enabled && mg.visible) {
        const double left   = mg.left;
        const double right  = mg.right;
        const double top    = mg.top;
        const double bottom = mg.bottom;
        const double ix = left;
        const double iy = top;
        const double iw = kMockDocW - left - right;
        const double ih = kMockDocH - top - bottom;
        if (iw > 0 && ih > 0) {
            const double a = std::min(mg.color_a * 3.0, 1.0);
            cr->set_source_rgba(mg.color_r, mg.color_g, mg.color_b, a);
            // line_width in mock-doc units; divide so it stays ~1px on
            // screen regardless of the scale.
            cr->set_line_width(1.0 / scale);

            // Margin border rect.
            cr->rectangle(ix, iy, iw, ih);
            cr->stroke();

            // Column dividers (gap-aware, same rule as the canvas).
            const int    cols = std::max(1, mg.columns);
            const double gap_x = mg.col_gap;
            if (cols > 1 && iw > 0) {
                const double col_w = (iw - gap_x * (cols - 1)) / cols;
                for (int c = 1; c < cols; ++c) {
                    double gx_left  = ix + c * col_w + (c - 1) * gap_x;
                    double gx_right = gx_left + gap_x;
                    cr->move_to(gx_left, iy);
                    cr->line_to(gx_left, iy + ih);
                    cr->stroke();
                    cr->move_to(gx_right, iy);
                    cr->line_to(gx_right, iy + ih);
                    cr->stroke();
                }
            }

            // Row dividers.
            const int    rows = std::max(1, mg.rows);
            const double gap_y = mg.row_gap;
            if (rows > 1 && ih > 0) {
                const double row_h = (ih - gap_y * (rows - 1)) / rows;
                for (int rrow = 1; rrow < rows; ++rrow) {
                    double gy_top    = iy + rrow * row_h + (rrow - 1) * gap_y;
                    double gy_bottom = gy_top + gap_y;
                    cr->move_to(ix, gy_top);
                    cr->line_to(ix + iw, gy_top);
                    cr->stroke();
                    cr->move_to(ix, gy_bottom);
                    cr->line_to(ix + iw, gy_bottom);
                    cr->stroke();
                }
            }
        }
    }

    // 4. Grid (if enabled). Mirrors Canvas::draw_grid_doc_space.
    const auto& gd = m_working.grid;
    if (gd.enabled && gd.visible && gd.spacing_x >= 0.5 && gd.spacing_y >= 0.5) {
        cr->set_source_rgba(gd.color_r, gd.color_g, gd.color_b, gd.color_a);
        cr->set_line_width(1.0 / scale);
        const double sxg = gd.spacing_x;
        const double syg = gd.spacing_y;
        const double oxg = gd.offset_x;
        const double oyg = gd.offset_y;

        if (gd.dots) {
            const double r = 1.5 / scale;
            for (double x = std::fmod(oxg, sxg); x <= kMockDocW; x += sxg) {
                for (double y = std::fmod(oyg, syg); y <= kMockDocH; y += syg) {
                    cr->arc(x, y, r, 0, 2 * M_PI);
                    cr->fill();
                }
            }
        } else {
            for (double x = std::fmod(oxg, sxg); x <= kMockDocW; x += sxg) {
                cr->move_to(x, 0);
                cr->line_to(x, kMockDocH);
                cr->stroke();
            }
            for (double y = std::fmod(oyg, syg); y <= kMockDocH; y += syg) {
                cr->move_to(0, y);
                cr->line_to(kMockDocW, y);
                cr->stroke();
            }
        }
    }

    // 5. Sample guides — one V + one H, drawn in guides.color when
    // guides.visible. Real guide positions are per-doc; the mock
    // shows two illustrative ones so the colour reads even when no
    // grid/margins are enabled. Positions chosen at 1/3 and 2/3 of
    // the artboard so they don't coincide with margin or grid lines
    // at default values.
    const auto& gu = m_working.guides;
    if (gu.visible) {
        cr->set_source_rgba(gu.color_r, gu.color_g, gu.color_b, 1.0);
        cr->set_line_width(1.0 / scale);
        const double vx = kMockDocW * (1.0 / 3.0);
        const double hy = kMockDocH * (2.0 / 3.0);
        cr->move_to(vx, 0);
        cr->line_to(vx, kMockDocH);
        cr->stroke();
        cr->move_to(0, hy);
        cr->line_to(kMockDocW, hy);
        cr->stroke();
    }

    cr->restore();
}

// ── build_button_row ──────────────────────────────────────────────────────
//
// Cancel | (spacer) | Reset | OK. Reset sits at the inboard end so
// it's reachable but not adjacent to OK (mis-click risk). Cancel
// goes far left as the standard GTK4 negative-action position; OK
// gets suggested-action and receives default focus.
void ThemeEditDialog::build_button_row(Gtk::Box& root) {
    auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    btn_row->set_margin_top(4);
    root.append(*btn_row);

    m_btn_cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    curvz::utils::set_name(m_btn_cancel, "dlg_te_cnc",
                           "theme_editor_dialog_cancel_btn");
    btn_row->append(*m_btn_cancel);

    // Spacer pushes Reset/OK to the right end, Cancel stays at the
    // left. Standard GTK4 dialog button-row layout.
    auto* spacer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    spacer->set_hexpand(true);
    btn_row->append(*spacer);

    m_btn_reset = Gtk::make_managed<Gtk::Button>("Reset");
    curvz::utils::set_name(m_btn_reset, "dlg_te_rst",
                           "theme_editor_dialog_reset_btn");
    m_btn_reset->set_tooltip_text(
        "Reset the currently-displayed mode's properties to defaults");
    btn_row->append(*m_btn_reset);

    m_btn_ok = Gtk::make_managed<Gtk::Button>("OK");
    curvz::utils::set_name(m_btn_ok, "dlg_te_ok",
                           "theme_editor_dialog_ok_btn");
    m_btn_ok->add_css_class("suggested-action");
    m_btn_ok->set_receives_default(true);
    btn_row->append(*m_btn_ok);

    m_btn_cancel->signal_clicked().connect(
        sigc::mem_fun(*this, &ThemeEditDialog::on_cancel));
    m_btn_reset->signal_clicked().connect(
        sigc::mem_fun(*this, &ThemeEditDialog::on_reset));
    m_btn_ok->signal_clicked().connect(
        sigc::mem_fun(*this, &ThemeEditDialog::on_ok));
}

// ── refresh_mode_toggle_highlight ─────────────────────────────────────────
//
// s183 m4 prelude — paint the active mode toggle with the OK-blue
// suggested-action class so the user can see at a glance which mode
// the thumbnail is currently previewing. Mirrors the standard GTK
// "this is the affirmative choice in this row" idiom we already use
// on the OK button.
void ThemeEditDialog::refresh_mode_toggle_highlight() {
    if (!m_mode_dark_btn || !m_mode_light_btn) return;
    auto set_hi = [](Gtk::ToggleButton* b, bool on) {
        if (!b) return;
        if (on) b->add_css_class("suggested-action");
        else    b->remove_css_class("suggested-action");
    };
    set_hi(m_mode_dark_btn,  m_preview_mode == Motif::Dark);
    set_hi(m_mode_light_btn, m_preview_mode == Motif::Light);
}

// ── refresh_motif_swatches ────────────────────────────────────────────────
//
// s183 m4 — Motif color swatches are mode-aware. When the user
// flips the thumbnail mode toggle, the three swatch DrawingAreas
// need to re-read whichever pair of motif fields is now active.
// The swatches' draw_func reads m_working.motif.*_{r,g,b} via the
// current m_preview_mode, so a simple queue_draw on each is enough
// — no working-buffer mutation here.
void ThemeEditDialog::refresh_motif_swatches() {
    if (m_swatch_motif_artboard)  m_swatch_motif_artboard->queue_draw();
    if (m_swatch_motif_workspace) m_swatch_motif_workspace->queue_draw();
    if (m_swatch_motif_creation)  m_swatch_motif_creation->queue_draw();
}

// ── build_properties_notebook ─────────────────────────────────────────────
//
// s183 m4 — Three-tab editor below the thumbnail. Tab choice grouped
// for compactness:
//   Tab 1 "Colors & Snap"  — Motif (mode-aware), Guides, Units, Snap.
//   Tab 2 "Grid"           — full grid sub-bundle.
//   Tab 3 "Margins"        — full margins sub-bundle.
//
// Each tab is a vertical Gtk::Box; each property is a labeled row.
// Snap, Grid and Margin are folded together with their related
// controls because grouping by sub-bundle reads cleanest in the
// data model and matches the inspector's section layout.
void ThemeEditDialog::build_properties_notebook(Gtk::Box& root) {
    auto* nb = Gtk::make_managed<Gtk::Notebook>();
    curvz::utils::set_name(nb, "dlg_te_nb", "theme_editor_dialog_notebook");
    nb->set_margin_top(8);
    root.append(*nb);

    auto* tab1 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    tab1->set_margin(10);
    build_tab_colors_snap(*tab1);
    nb->append_page(*tab1, "Colors & Snap");

    auto* tab2 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    tab2->set_margin(10);
    build_tab_grid(*tab2);
    nb->append_page(*tab2, "Grid");

    auto* tab3 = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    tab3->set_margin(10);
    build_tab_margins(*tab3);
    nb->append_page(*tab3, "Margins");
}

// ── small builders used across tabs ───────────────────────────────────────
namespace {

// Subsection header — bold label with a small top margin. Helps
// group related rows visually inside a tab. Escapes "&" to "&amp;"
// so user-supplied strings (and our own "Spacing & Offset") don't
// trip Pango's entity parser.
Gtk::Label* make_subhead(const char* text) {
    auto* lbl = Gtk::make_managed<Gtk::Label>();
    std::string escaped;
    escaped.reserve(std::char_traits<char>::length(text) + 8);
    for (const char* p = text; *p; ++p) {
        switch (*p) {
            case '&':  escaped += "&amp;";  break;
            case '<':  escaped += "&lt;";   break;
            case '>':  escaped += "&gt;";   break;
            default:   escaped += *p;       break;
        }
    }
    lbl->set_markup(std::string("<b>") + escaped + "</b>");
    lbl->set_xalign(0.0f);
    lbl->set_margin_top(4);
    return lbl;
}

// Build a clickable colour swatch. The draw_func captures `read_rgb`
// by value so each swatch reads its own theme slot at draw time. The
// gesture-click captures `on_click` to summon a popover.
//
// Returns the DrawingArea so the caller can stash a pointer for
// later queue_draw() calls (after the popover writes back).
template<typename ReadRGB, typename OnClick>
Gtk::DrawingArea* make_swatch(int w, int h,
                              ReadRGB read_rgb, OnClick on_click) {
    auto* da = Gtk::make_managed<Gtk::DrawingArea>();
    da->set_size_request(w, h);
    da->set_valign(Gtk::Align::CENTER);
    da->set_can_target(true);
    da->set_draw_func(
        [read_rgb](const Cairo::RefPtr<Cairo::Context>& cr, int ww, int hh) {
            double r, g, b;
            read_rgb(r, g, b);
            cr->set_source_rgba(r, g, b, 1.0);
            cr->rectangle(0, 0, ww, hh);
            cr->fill();
            // Hairline border so a near-white swatch isn't invisible
            // on a light dialog. Matches StyleEditorDialog's idiom.
            cr->set_source_rgba(0, 0, 0, 0.25);
            cr->set_line_width(1.0);
            cr->rectangle(0.5, 0.5, ww - 1, hh - 1);
            cr->stroke();
        });
    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect(
        [on_click](int, double, double) { on_click(); });
    da->add_controller(click);
    return da;
}

} // namespace

// ── build_tab_colors_snap ─────────────────────────────────────────────────
//
// Layout (top-to-bottom):
//   <b>Motif</b>         — 3 swatch rows (artboard / workspace / creation),
//                           mode-aware via m_preview_mode
//   <b>Guides</b>        — swatch + visible checkbox
//   <b>Units</b>         — display unit dropdown
//   <b>Snap</b>          — enabled master + 6 target checkboxes
void ThemeEditDialog::build_tab_colors_snap(Gtk::Box& page) {
    // ── Motif ─────────────────────────────────────────────────────
    page.append(*make_subhead("Motif"));

    auto motif_swatch_row = [this, &page](const char* label,
                                          Gtk::DrawingArea*& slot_ptr,
                                          ColorPickerPopover& popover,
                                          auto get_r, auto get_g, auto get_b,
                                          auto set_r, auto set_g, auto set_b) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>(label);
        lbl->set_xalign(0.0f);
        lbl->set_size_request(96, -1);
        row->append(*lbl);

        slot_ptr = make_swatch(
            36, 22,
            // read_rgb — mode-aware. Captures `this` so the draw-time
            // read sees the current m_preview_mode and the live
            // m_working.motif state.
            [this, get_r, get_g, get_b](double& r, double& gg, double& bb) {
                r  = get_r(m_working.motif, m_preview_mode);
                gg = get_g(m_working.motif, m_preview_mode);
                bb = get_b(m_working.motif, m_preview_mode);
            },
            // on_click — open the popover for this slot, wired to
            // write back into whichever mode slot is current.
            [this, slot_ptr_addr = &slot_ptr, &popover, get_r, get_g, get_b,
             set_r, set_g, set_b]() {
                if (m_syncing) return;
                color::Color initial(
                    get_r(m_working.motif, m_preview_mode),
                    get_g(m_working.motif, m_preview_mode),
                    get_b(m_working.motif, m_preview_mode),
                    1.0);
                popover.open(
                    **slot_ptr_addr, initial, /*with_alpha=*/false,
                    [this, slot_ptr_addr, set_r, set_g, set_b]
                    (const color::Color& c) {
                        set_r(m_working.motif, m_preview_mode, c.r);
                        set_g(m_working.motif, m_preview_mode, c.g);
                        set_b(m_working.motif, m_preview_mode, c.b);
                        if (*slot_ptr_addr) (*slot_ptr_addr)->queue_draw();
                        queue_thumbnail_redraw();
                    });
            });
        row->append(*slot_ptr);
        page.append(*row);
    };

    // Closures that pick the right mode-aware field. Six of each so
    // we can compose them into the rows above.
    auto art_r = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_artboard_r : m.dark_artboard_r;
    };
    auto art_g = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_artboard_g : m.dark_artboard_g;
    };
    auto art_b = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_artboard_b : m.dark_artboard_b;
    };
    auto ws_r = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_workspace_r : m.dark_workspace_r;
    };
    auto ws_g = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_workspace_g : m.dark_workspace_g;
    };
    auto ws_b = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_workspace_b : m.dark_workspace_b;
    };
    auto cr_r = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_creation_r : m.dark_creation_r;
    };
    auto cr_g = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_creation_g : m.dark_creation_g;
    };
    auto cr_b = [](const theme::MotifSettings& m, Motif mo) -> double {
        return mo == Motif::Light ? m.light_creation_b : m.dark_creation_b;
    };

    auto art_set_r = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_artboard_r = v;
        else                    m.dark_artboard_r  = v;
    };
    auto art_set_g = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_artboard_g = v;
        else                    m.dark_artboard_g  = v;
    };
    auto art_set_b = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_artboard_b = v;
        else                    m.dark_artboard_b  = v;
    };
    auto ws_set_r = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_workspace_r = v;
        else                    m.dark_workspace_r  = v;
    };
    auto ws_set_g = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_workspace_g = v;
        else                    m.dark_workspace_g  = v;
    };
    auto ws_set_b = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_workspace_b = v;
        else                    m.dark_workspace_b  = v;
    };
    auto cr_set_r = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_creation_r = v;
        else                    m.dark_creation_r  = v;
    };
    auto cr_set_g = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_creation_g = v;
        else                    m.dark_creation_g  = v;
    };
    auto cr_set_b = [](theme::MotifSettings& m, Motif mo, double v) {
        if (mo == Motif::Light) m.light_creation_b = v;
        else                    m.dark_creation_b  = v;
    };

    motif_swatch_row("Artboard",  m_swatch_motif_artboard,
                     m_motif_artboard_popover,
                     art_r, art_g, art_b, art_set_r, art_set_g, art_set_b);
    motif_swatch_row("Workspace", m_swatch_motif_workspace,
                     m_motif_workspace_popover,
                     ws_r, ws_g, ws_b, ws_set_r, ws_set_g, ws_set_b);
    motif_swatch_row("Creation",  m_swatch_motif_creation,
                     m_motif_creation_popover,
                     cr_r, cr_g, cr_b, cr_set_r, cr_set_g, cr_set_b);

    // ── Guides ────────────────────────────────────────────────────
    page.append(*make_subhead("Guides"));
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>("Color");
        lbl->set_xalign(0.0f);
        lbl->set_size_request(96, -1);
        row->append(*lbl);

        m_swatch_guides = make_swatch(
            36, 22,
            [this](double& r, double& g, double& b) {
                r = m_working.guides.color_r;
                g = m_working.guides.color_g;
                b = m_working.guides.color_b;
            },
            [this]() {
                if (m_syncing) return;
                color::Color initial(m_working.guides.color_r,
                                     m_working.guides.color_g,
                                     m_working.guides.color_b, 1.0);
                m_guides_popover.open(
                    *m_swatch_guides, initial, /*with_alpha=*/false,
                    [this](const color::Color& c) {
                        m_working.guides.color_r = c.r;
                        m_working.guides.color_g = c.g;
                        m_working.guides.color_b = c.b;
                        if (m_swatch_guides) m_swatch_guides->queue_draw();
                        queue_thumbnail_redraw();
                    });
            });
        row->append(*m_swatch_guides);

        m_guides_visible_chk = Gtk::make_managed<Gtk::CheckButton>("Visible");
        m_guides_visible_chk->set_active(m_working.guides.visible);
        m_guides_visible_chk->signal_toggled().connect([this]() {
            if (m_syncing) return;
            m_working.guides.visible = m_guides_visible_chk->get_active();
            queue_thumbnail_redraw();
        });
        row->append(*m_guides_visible_chk);

        page.append(*row);
    }

    // ── Units ─────────────────────────────────────────────────────
    page.append(*make_subhead("Units"));
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>("Display unit");
        lbl->set_xalign(0.0f);
        lbl->set_size_request(96, -1);
        row->append(*lbl);

        auto sl = Gtk::StringList::create({"Pixels", "Inches",
                                           "Millimeters", "Points"});
        m_units_dd = Gtk::make_managed<Gtk::DropDown>(sl);
        m_units_dd->set_hexpand(true);
        // Index matches enum order: Px=0, In=1, Mm=2, Pt=3.
        m_units_dd->set_selected(
            static_cast<guint>(m_working.units.display_unit));
        m_units_dd->property_selected().signal_changed().connect([this]() {
            if (m_syncing) return;
            const auto idx = m_units_dd->get_selected();
            if (idx == GTK_INVALID_LIST_POSITION) return;
            m_working.units.display_unit = static_cast<Unit>(idx);
            // s183 m4 fix3 — propagate to every dimensional spinner.
            // CurvzSpinButton reads from m_unit_model.display_unit;
            // updating it then calling refresh_units() on each
            // spinner re-displays the same stored internal-px value
            // in the new unit. Internal values are unchanged; only
            // the displayed text reformats.
            m_unit_model.display_unit = m_working.units.display_unit;
            auto refresh = [](CurvzSpinButton* sp) {
                if (sp) sp->refresh_units();
            };
            refresh(m_grid_spacing_x);
            refresh(m_grid_spacing_y);
            refresh(m_grid_offset_x);
            refresh(m_grid_offset_y);
            refresh(m_margin_top);
            refresh(m_margin_bottom);
            refresh(m_margin_left);
            refresh(m_margin_right);
            refresh(m_margin_col_gap);
            refresh(m_margin_row_gap);
            // No thumbnail dependency on display_unit (the thumbnail
            // is purely visual), so skip the redraw.
        });
        row->append(*m_units_dd);
        page.append(*row);
    }

    // ── Snap ──────────────────────────────────────────────────────
    page.append(*make_subhead("Snap"));
    {
        m_snap_enabled_chk = Gtk::make_managed<Gtk::CheckButton>("Enable snapping");
        m_snap_enabled_chk->set_margin_start(8);
        m_snap_enabled_chk->set_active(m_working.snap.enabled);
        m_snap_enabled_chk->signal_toggled().connect([this]() {
            if (m_syncing) return;
            m_working.snap.enabled = m_snap_enabled_chk->get_active();
            // No thumbnail effect for snap settings; they're behavior,
            // not appearance.
        });
        page.append(*m_snap_enabled_chk);

        // 6 target checkboxes in a 3x2 grid for compactness.
        auto* grid = Gtk::make_managed<Gtk::Grid>();
        grid->set_row_spacing(2);
        grid->set_column_spacing(16);
        grid->set_margin_start(24);
        grid->set_margin_top(2);

        auto add_chk = [&](Gtk::CheckButton*& slot, const char* label,
                           bool initial, auto setter,
                           int col, int row) {
            slot = Gtk::make_managed<Gtk::CheckButton>(label);
            slot->set_active(initial);
            slot->signal_toggled().connect(
                [this, slot, setter]() {
                    if (m_syncing) return;
                    setter(m_working.snap, slot->get_active());
                });
            grid->attach(*slot, col, row, 1, 1);
        };

        add_chk(m_snap_guides_chk,  "Guides",
                m_working.snap.snap_guides,
                [](auto& s, bool v) { s.snap_guides = v; },  0, 0);
        add_chk(m_snap_grid_chk,    "Grid",
                m_working.snap.snap_grid,
                [](auto& s, bool v) { s.snap_grid = v; },    1, 0);
        add_chk(m_snap_margins_chk, "Margins",
                m_working.snap.snap_margins,
                [](auto& s, bool v) { s.snap_margins = v; }, 2, 0);
        add_chk(m_snap_nodes_chk,   "Nodes",
                m_working.snap.snap_nodes,
                [](auto& s, bool v) { s.snap_nodes = v; },   0, 1);
        add_chk(m_snap_edges_chk,   "Edges",
                m_working.snap.snap_edges,
                [](auto& s, bool v) { s.snap_edges = v; },   1, 1);
        add_chk(m_snap_centers_chk, "Centers",
                m_working.snap.snap_centers,
                [](auto& s, bool v) { s.snap_centers = v; }, 2, 1);

        page.append(*grid);
    }
}

// ── build_tab_grid ────────────────────────────────────────────────────────
void ThemeEditDialog::build_tab_grid(Gtk::Box& page) {
    // Enable + visible row.
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        m_grid_enabled_chk = Gtk::make_managed<Gtk::CheckButton>("Enabled");
        m_grid_enabled_chk->set_active(m_working.grid.enabled);
        m_grid_enabled_chk->signal_toggled().connect([this]() {
            if (m_syncing) return;
            m_working.grid.enabled = m_grid_enabled_chk->get_active();
            queue_thumbnail_redraw();
        });
        row->append(*m_grid_enabled_chk);

        m_grid_visible_chk = Gtk::make_managed<Gtk::CheckButton>("Visible");
        m_grid_visible_chk->set_active(m_working.grid.visible);
        m_grid_visible_chk->signal_toggled().connect([this]() {
            if (m_syncing) return;
            m_working.grid.visible = m_grid_visible_chk->get_active();
            queue_thumbnail_redraw();
        });
        row->append(*m_grid_visible_chk);

        m_grid_dots_chk = Gtk::make_managed<Gtk::CheckButton>("Dots (not lines)");
        m_grid_dots_chk->set_active(m_working.grid.dots);
        m_grid_dots_chk->signal_toggled().connect([this]() {
            if (m_syncing) return;
            m_working.grid.dots = m_grid_dots_chk->get_active();
            queue_thumbnail_redraw();
        });
        row->append(*m_grid_dots_chk);
        page.append(*row);
    }

    // s183 m4 fix1: pair X/Y horizontally — two rows of two spinners.
    // s183 m4 fix2: use CurvzSpinButton so values reflow when the
    // units dropdown changes. Spacing is positive (Width); offset
    // can be negative (Distance).
    {
        page.append(*make_subhead("Spacing & Offset"));

        // Helper to add a label / X-spin / Y-spin row. The spin
        // factories pass SpinType::Width (positive-only) for spacing
        // and SpinType::Distance for offset.
        auto add_xy_row = [this](Gtk::Box& container, const char* label,
                                 SpinType type,
                                 double initial_x, double initial_y,
                                 CurvzSpinButton*& slot_x,
                                 CurvzSpinButton*& slot_y,
                                 auto setter_x, auto setter_y) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            row->set_margin_start(8);
            auto* lbl = Gtk::make_managed<Gtk::Label>(label);
            lbl->set_xalign(0.0f);
            lbl->set_size_request(72, -1);
            row->append(*lbl);

            slot_x = Gtk::make_managed<CurvzSpinButton>(type, &m_unit_model);
            slot_x->set_internal_value(initial_x);
            slot_x->set_hexpand(true);
            slot_x->signal_internal_changed().connect(
                [this, setter_x](double v) {
                    if (m_syncing) return;
                    setter_x(m_working.grid, v);
                    queue_thumbnail_redraw();
                });
            row->append(*slot_x);
            if (auto* ul = slot_x->get_unit_label()) row->append(*ul);

            slot_y = Gtk::make_managed<CurvzSpinButton>(type, &m_unit_model);
            slot_y->set_internal_value(initial_y);
            slot_y->set_hexpand(true);
            slot_y->signal_internal_changed().connect(
                [this, setter_y](double v) {
                    if (m_syncing) return;
                    setter_y(m_working.grid, v);
                    queue_thumbnail_redraw();
                });
            row->append(*slot_y);
            if (auto* ul = slot_y->get_unit_label()) row->append(*ul);

            container.append(*row);
        };

        add_xy_row(page, "Spacing", SpinType::Width,
                   m_working.grid.spacing_x, m_working.grid.spacing_y,
                   m_grid_spacing_x, m_grid_spacing_y,
                   [](auto& gd, double v) { gd.spacing_x = v; },
                   [](auto& gd, double v) { gd.spacing_y = v; });
        add_xy_row(page, "Offset", SpinType::Distance,
                   m_working.grid.offset_x, m_working.grid.offset_y,
                   m_grid_offset_x, m_grid_offset_y,
                   [](auto& gd, double v) { gd.offset_x = v; },
                   [](auto& gd, double v) { gd.offset_y = v; });
    }

    // Color + alpha.
    {
        page.append(*make_subhead("Color"));
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);

        m_swatch_grid = make_swatch(
            36, 22,
            [this](double& r, double& g, double& b) {
                r = m_working.grid.color_r;
                g = m_working.grid.color_g;
                b = m_working.grid.color_b;
            },
            [this]() {
                if (m_syncing) return;
                color::Color initial(m_working.grid.color_r,
                                     m_working.grid.color_g,
                                     m_working.grid.color_b, 1.0);
                m_grid_popover.open(
                    *m_swatch_grid, initial, /*with_alpha=*/false,
                    [this](const color::Color& c) {
                        m_working.grid.color_r = c.r;
                        m_working.grid.color_g = c.g;
                        m_working.grid.color_b = c.b;
                        if (m_swatch_grid) m_swatch_grid->queue_draw();
                        queue_thumbnail_redraw();
                    });
            });
        row->append(*m_swatch_grid);

        auto* lbl_a = Gtk::make_managed<Gtk::Label>("Alpha %");
        row->append(*lbl_a);
        auto adj = Gtk::Adjustment::create(
            m_working.grid.color_a * 100.0, 0.0, 100.0, 1.0, 10.0);
        m_grid_alpha_pct = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
        m_grid_alpha_pct->signal_value_changed().connect([this]() {
            if (m_syncing) return;
            m_working.grid.color_a = m_grid_alpha_pct->get_value() / 100.0;
            queue_thumbnail_redraw();
        });
        row->append(*m_grid_alpha_pct);
        page.append(*row);
    }
}

// ── build_tab_margins ─────────────────────────────────────────────────────
void ThemeEditDialog::build_tab_margins(Gtk::Box& page) {
    // Enable + visible row.
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        m_margin_enabled_chk = Gtk::make_managed<Gtk::CheckButton>("Enabled");
        m_margin_enabled_chk->set_active(m_working.margins.enabled);
        m_margin_enabled_chk->signal_toggled().connect([this]() {
            if (m_syncing) return;
            m_working.margins.enabled = m_margin_enabled_chk->get_active();
            queue_thumbnail_redraw();
        });
        row->append(*m_margin_enabled_chk);

        m_margin_visible_chk = Gtk::make_managed<Gtk::CheckButton>("Visible");
        m_margin_visible_chk->set_active(m_working.margins.visible);
        m_margin_visible_chk->signal_toggled().connect([this]() {
            if (m_syncing) return;
            m_working.margins.visible = m_margin_visible_chk->get_active();
            queue_thumbnail_redraw();
        });
        row->append(*m_margin_visible_chk);
        page.append(*row);
    }

    // s183 m4 fix1+2: pair Top/Bottom and Left/Right horizontally.
    // Dimensional fields (sides, gaps) use CurvzSpinButton so they
    // refresh when the units dropdown changes; counts stay plain
    // Gtk::SpinButton (integers, no unit).

    // Helper: append a row "Label: [dimSpinA][unit] [dimSpinB][unit]".
    // Used by sides (top/bottom, left/right).
    auto add_dim_pair_row = [this](Gtk::Box& container, const char* label,
                                   SpinType type,
                                   double init_a, double init_b,
                                   CurvzSpinButton*& slot_a,
                                   CurvzSpinButton*& slot_b,
                                   auto setter_a, auto setter_b,
                                   const char* sublabel_a,
                                   const char* sublabel_b) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>(label);
        lbl->set_xalign(0.0f);
        lbl->set_size_request(72, -1);
        row->append(*lbl);

        // sub_a "T" / "L"
        if (sublabel_a && *sublabel_a) {
            auto* sla = Gtk::make_managed<Gtk::Label>(sublabel_a);
            sla->add_css_class("dim-label");
            row->append(*sla);
        }
        slot_a = Gtk::make_managed<CurvzSpinButton>(type, &m_unit_model);
        slot_a->set_internal_value(init_a);
        slot_a->set_hexpand(true);
        slot_a->signal_internal_changed().connect(
            [this, setter_a](double v) {
                if (m_syncing) return;
                setter_a(m_working.margins, v);
                queue_thumbnail_redraw();
            });
        row->append(*slot_a);
        if (auto* ul = slot_a->get_unit_label()) row->append(*ul);

        // sub_b "B" / "R"
        if (sublabel_b && *sublabel_b) {
            auto* slb = Gtk::make_managed<Gtk::Label>(sublabel_b);
            slb->add_css_class("dim-label");
            row->append(*slb);
        }
        slot_b = Gtk::make_managed<CurvzSpinButton>(type, &m_unit_model);
        slot_b->set_internal_value(init_b);
        slot_b->set_hexpand(true);
        slot_b->signal_internal_changed().connect(
            [this, setter_b](double v) {
                if (m_syncing) return;
                setter_b(m_working.margins, v);
                queue_thumbnail_redraw();
            });
        row->append(*slot_b);
        if (auto* ul = slot_b->get_unit_label()) row->append(*ul);

        container.append(*row);
    };

    // Single-distance helper for the column/row gaps.
    auto add_dim_row = [this](Gtk::Box& container, const char* label,
                              SpinType type, double initial,
                              CurvzSpinButton*& slot, auto setter) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>(label);
        lbl->set_xalign(0.0f);
        lbl->set_size_request(96, -1);
        row->append(*lbl);

        slot = Gtk::make_managed<CurvzSpinButton>(type, &m_unit_model);
        slot->set_internal_value(initial);
        slot->set_hexpand(true);
        slot->signal_internal_changed().connect(
            [this, setter](double v) {
                if (m_syncing) return;
                setter(m_working.margins, v);
                queue_thumbnail_redraw();
            });
        row->append(*slot);
        if (auto* ul = slot->get_unit_label()) row->append(*ul);
        container.append(*row);
    };

    // Integer count helper (plain Gtk::SpinButton; no unit).
    auto add_count_row = [this](Gtk::Box& container, const char* label,
                                int initial,
                                Gtk::SpinButton*& slot, auto setter) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);
        auto* lbl = Gtk::make_managed<Gtk::Label>(label);
        lbl->set_xalign(0.0f);
        lbl->set_size_request(96, -1);
        row->append(*lbl);

        auto adj = Gtk::Adjustment::create(initial, 1, 999, 1.0, 10.0);
        slot = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
        slot->set_hexpand(true);
        slot->signal_value_changed().connect([this, slot, setter]() {
            if (m_syncing) return;
            setter(m_working.margins, static_cast<int>(slot->get_value()));
            queue_thumbnail_redraw();
        });
        row->append(*slot);
        container.append(*row);
    };

    // Sides — Top/Bottom on one row, Left/Right on another.
    page.append(*make_subhead("Sides"));
    add_dim_pair_row(page, "Vertical", SpinType::Distance,
                     m_working.margins.top, m_working.margins.bottom,
                     m_margin_top, m_margin_bottom,
                     [](auto& m, double v) { m.top = v; },
                     [](auto& m, double v) { m.bottom = v; },
                     "T", "B");
    add_dim_pair_row(page, "Horizontal", SpinType::Distance,
                     m_working.margins.left, m_working.margins.right,
                     m_margin_left, m_margin_right,
                     [](auto& m, double v) { m.left = v; },
                     [](auto& m, double v) { m.right = v; },
                     "L", "R");

    // Columns.
    page.append(*make_subhead("Columns"));
    add_count_row(page, "Count", m_working.margins.columns, m_margin_cols,
                  [](auto& m, int v) { m.columns = v; });
    add_dim_row(page, "Gap", SpinType::Width, m_working.margins.col_gap,
                m_margin_col_gap,
                [](auto& m, double v) { m.col_gap = v; });

    // Rows.
    page.append(*make_subhead("Rows"));
    add_count_row(page, "Count", m_working.margins.rows, m_margin_rows,
                  [](auto& m, int v) { m.rows = v; });
    add_dim_row(page, "Gap", SpinType::Width, m_working.margins.row_gap,
                m_margin_row_gap,
                [](auto& m, double v) { m.row_gap = v; });

    // Color + alpha.
    page.append(*make_subhead("Color"));
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);

        m_swatch_margin = make_swatch(
            36, 22,
            [this](double& r, double& g, double& b) {
                r = m_working.margins.color_r;
                g = m_working.margins.color_g;
                b = m_working.margins.color_b;
            },
            [this]() {
                if (m_syncing) return;
                color::Color initial(m_working.margins.color_r,
                                     m_working.margins.color_g,
                                     m_working.margins.color_b, 1.0);
                m_margin_popover.open(
                    *m_swatch_margin, initial, /*with_alpha=*/false,
                    [this](const color::Color& c) {
                        m_working.margins.color_r = c.r;
                        m_working.margins.color_g = c.g;
                        m_working.margins.color_b = c.b;
                        if (m_swatch_margin) m_swatch_margin->queue_draw();
                        queue_thumbnail_redraw();
                    });
            });
        row->append(*m_swatch_margin);

        auto* lbl_a = Gtk::make_managed<Gtk::Label>("Alpha %");
        row->append(*lbl_a);
        auto adj = Gtk::Adjustment::create(
            m_working.margins.color_a * 100.0, 0.0, 100.0, 1.0, 10.0);
        m_margin_alpha_pct = Gtk::make_managed<Gtk::SpinButton>(adj, 1.0, 0);
        m_margin_alpha_pct->signal_value_changed().connect([this]() {
            if (m_syncing) return;
            m_working.margins.color_a = m_margin_alpha_pct->get_value() / 100.0;
            queue_thumbnail_redraw();
        });
        row->append(*m_margin_alpha_pct);
        page.append(*row);
    }
}

// ── on_ok ─────────────────────────────────────────────────────────────────
//
// Flush any pending entry-edit by reading the entry one more time
// (focus may not have left yet at click time, so the on_commit hasn't
// fired and m_working.header.name could be stale). Then fire the
// callback and close. The host's on_committed pushes the
// UpdateThemeCommand.
void ThemeEditDialog::on_ok() {
    // Final flush of the name entry. The CurvzEntry on_commit fires
    // on focus-leave, but a click on OK doesn't always cause the
    // entry's focus controller to leave before the click handler runs
    // (depends on event ordering). Reading directly here closes the
    // gap.
    if (m_name_entry) {
        std::string typed = trim_ws(m_name_entry->get_text());
        m_working.header.name = typed;
    }

    if (m_on_committed) {
        m_on_committed(m_working);
    }
    if (m_window) m_window->close();
}

// ── on_cancel ─────────────────────────────────────────────────────────────
//
// Discard m_working entirely. The host never hears about it. Close.
void ThemeEditDialog::on_cancel() {
    LOG_DEBUG("ThemeEditDialog: cancel — discarding working buffer");
    if (m_window) m_window->close();
}

// ── on_reset ──────────────────────────────────────────────────────────────
//
// s184 m1 — Mode-scoped reset to factory defaults.
//
// Scope: ONLY the motif sub-bundle's currently-displayed pair. If the
// thumbnail is showing Dark, the dark_* triple resets; if Light, the
// light_* triple resets. The off-mode pair is untouched. Other
// sub-bundles (units, guides, grid, margins, snap) are mode-
// independent and outside the scope of a "reset what I'm looking at"
// gesture — the user can edit those directly, or Duplicate-and-edit
// the theme if they want a full clean slate.
//
// Source of truth: MotifSettings's default-member-initialisers (see
// theme/Theme.hpp). Default-constructing a temporary and copying the
// active mode's three triples gives us the factory values without
// duplicating constants.
//
// Persistence: this only mutates m_working. The user can still Cancel
// to discard, or OK to commit via UpdateThemeCommand. The live
// thumbnail and the three motif swatches repaint immediately so the
// reset state is visible.
void ThemeEditDialog::on_reset() {
    LOG_DEBUG("ThemeEditDialog: reset motif pair for mode {}",
              m_preview_mode == Motif::Light ? "Light" : "Dark");

    const theme::MotifSettings factory;
    if (m_preview_mode == Motif::Light) {
        m_working.motif.light_artboard_r  = factory.light_artboard_r;
        m_working.motif.light_artboard_g  = factory.light_artboard_g;
        m_working.motif.light_artboard_b  = factory.light_artboard_b;
        m_working.motif.light_workspace_r = factory.light_workspace_r;
        m_working.motif.light_workspace_g = factory.light_workspace_g;
        m_working.motif.light_workspace_b = factory.light_workspace_b;
        m_working.motif.light_creation_r  = factory.light_creation_r;
        m_working.motif.light_creation_g  = factory.light_creation_g;
        m_working.motif.light_creation_b  = factory.light_creation_b;
    } else {
        m_working.motif.dark_artboard_r  = factory.dark_artboard_r;
        m_working.motif.dark_artboard_g  = factory.dark_artboard_g;
        m_working.motif.dark_artboard_b  = factory.dark_artboard_b;
        m_working.motif.dark_workspace_r = factory.dark_workspace_r;
        m_working.motif.dark_workspace_g = factory.dark_workspace_g;
        m_working.motif.dark_workspace_b = factory.dark_workspace_b;
        m_working.motif.dark_creation_r  = factory.dark_creation_r;
        m_working.motif.dark_creation_g  = factory.dark_creation_g;
        m_working.motif.dark_creation_b  = factory.dark_creation_b;
    }

    refresh_motif_swatches();
    if (m_thumbnail) m_thumbnail->queue_draw();
}

} // namespace Curvz
