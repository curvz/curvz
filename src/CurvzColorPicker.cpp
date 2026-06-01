//
// CurvzColorPicker.cpp — see header for architectural notes.
//

#include "CurvzColorPicker.hpp"
#include "CurvzEntry.hpp"
#include "ColorPickerRecents.hpp"
#include "curvz_utils.hpp"  // s121 m14: curvz::utils::set_name

#include <gtkmm/drawingarea.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/box.h>
#include <glibmm/main.h>
#include <gdk/gdkkeysyms.h>
#include <cairomm/cairomm.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Curvz {

namespace {

// Spectrum OKLCH ranges.
//   X axis: chroma 0 → C_MAX
//   Y axis: lightness 1 → 0 (top is lightest, matches CSS/photoshop habit)
//
// 0.37 is the nominal maximum in-gamut chroma for sRGB; going slightly past
// gives the "shoulder" clip zone that's visually useful as a reference.
constexpr double C_MAX = 0.37;

// s332 — gamut-relative chroma. The in-gamut sRGB region at a fixed hue is a
// wedge in the (C, L) plane, not a rectangle: dark and very light rows hold
// far less chroma than the mid-lights. Mapping the spectrum's X axis to
// absolute chroma 0..C_MAX therefore leaves a dead triangle (high chroma +
// low/high lightness) where every point clamps to one flat gamut-edge colour
// -- the "diagonal wall" that blocks selection. Instead we make X a FRACTION
// of the in-gamut chroma edge at the current row: the right edge always tracks
// the boundary, so the whole rectangle is live and selectable (the way every
// other picker behaves). max_chroma_at finds that edge by bisection up to the
// C_MAX ceiling; grey (c=0) is always in gamut, so the low end starts valid.
double max_chroma_at(double L, double h) {
  color::OKLCH probe;
  probe.l = L; probe.h = h; probe.a = 1.0;
  probe.c = C_MAX;
  if (color::oklch_in_gamut(probe)) return C_MAX;  // whole row fits
  double lo = 0.0, hi = C_MAX;
  for (int i = 0; i < 24; ++i) {
    double mid = 0.5 * (lo + hi);
    probe.c = mid;
    if (color::oklch_in_gamut(probe)) lo = mid; else hi = mid;
  }
  return lo;
}

// Hue strip preview colour. L and C are fixed; h sweeps 0..360.
// Chroma is at the sRGB gamut boundary (C_MAX) so each row lands on its
// most-saturated in-gamut representative — matches the bright strip
// users expect from Illustrator/Affinity/Figma. Lightness stays at 0.65
// so the strip doesn't wash out at pastel extremes.
constexpr double HUE_STRIP_L = 0.65;
constexpr double HUE_STRIP_C = 0.37;

// Checkerboard size (px) for alpha underlay and preview.
constexpr int CHECKER = 6;

// Paint a small checkerboard in the given rect. Used behind alpha-affected
// drawings so the user can see "transparent" at a glance.
void paint_checker(const Cairo::RefPtr<Cairo::Context>& cr,
                   double x, double y, double w, double h) {
    cr->save();
    cr->rectangle(x, y, w, h);
    cr->clip();
    for (int iy = 0; iy * CHECKER < h; ++iy) {
        for (int ix = 0; ix * CHECKER < w; ++ix) {
            bool dark = ((ix + iy) & 1);
            if (dark) cr->set_source_rgb(0.75, 0.75, 0.75);
            else      cr->set_source_rgb(0.95, 0.95, 0.95);
            cr->rectangle(x + ix * CHECKER, y + iy * CHECKER, CHECKER, CHECKER);
            cr->fill();
        }
    }
    cr->restore();
}

// Draw a crosshair ring at (px,py). Matches the "two concentric rings"
// convention used by most pickers so the cursor is visible over any
// colour background.
void paint_crosshair(const Cairo::RefPtr<Cairo::Context>& cr,
                     double px, double py, double r = 6.0) {
    cr->set_line_width(1.5);
    cr->set_source_rgb(0.0, 0.0, 0.0);
    cr->arc(px, py, r + 1.0, 0, 2 * M_PI);
    cr->stroke();
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->arc(px, py, r,       0, 2 * M_PI);
    cr->stroke();
}

} // anon namespace

// ─── construction ─────────────────────────────────────────────────────────

CurvzColorPicker::CurvzColorPicker()
    : Gtk::Box(Gtk::Orientation::VERTICAL, 8) {
    curvz::utils::set_name(*this, "cp", "color_picker_root");
    set_margin(8);
    add_css_class("curvz-color-picker");

    build_spectrum_row();
    build_alpha_row();
    build_hex_row();
    build_recents_row();
}

// ─── public API ───────────────────────────────────────────────────────────

color::Color CurvzColorPicker::current() const {
    return m_current;
}

sigc::signal<void(color::Color)>& CurvzColorPicker::signal_changed() {
    return m_sig_changed;
}

sigc::signal<void(color::Color)>& CurvzColorPicker::signal_committed() {
    return m_sig_committed;
}

sigc::signal<void()>& CurvzColorPicker::signal_cancelled() {
    return m_sig_cancelled;
}

void CurvzColorPicker::set_initial(const color::Color& c) {
    m_original = c;
    m_current  = c;

    // Seed hue AND spectrum position from the incoming colour. Achromatic
    // inputs (h undefined) leave m_hue alone but still update the (nx,ny)
    // crosshair position — a plain grey lands on the left edge, lightness
    // row matching the grey value. Round-trip lossiness doesn't matter
    // here: the incoming colour is sRGB by contract, so to_oklch gives us
    // the canonical OKLCH position for it.
    auto oklch = color::to_oklch(c);
    if (oklch.c > 1e-6) m_hue = oklch.h;
    double cm = max_chroma_at(oklch.l, oklch.h);
    m_spectrum_nx = (cm > 1e-9) ? std::clamp(oklch.c / cm, 0.0, 1.0) : 0.0;
    m_spectrum_ny = std::clamp(1.0 - oklch.l, 0.0, 1.0);

    refresh_from_current();
}

void CurvzColorPicker::set_with_alpha(bool on) {
    if (m_with_alpha == on) return;
    m_with_alpha = on;
    if (m_alpha_row) m_alpha_row->set_visible(on);
    if (!on) {
        m_current.a = 1.0;
        refresh_from_current();
    }
}

void CurvzColorPicker::commit() {
    m_sig_committed.emit(m_current);
}

void CurvzColorPicker::cancel() {
    // S73: picker self-reverts. Restoring m_current to m_original and
    // emitting signal_changed *before* signal_cancelled gives the host's
    // on_changed callback a chance to write the original colour through
    // (e.g. to the live swatch) before the popover popsdown. If we
    // emitted signal_cancelled first, the popover would close, focus
    // would leave the hex entry, and the focus-loss-driven hex commit
    // chain would re-emit signal_changed at the dragged colour and
    // clobber the revert (this was the m11-and-earlier failure mode).
    m_current = m_original;
    sync_hue_from_current();
    refresh_from_current();
    m_sig_changed.emit(m_current);
    m_sig_cancelled.emit();
}

// ─── row construction ─────────────────────────────────────────────────────

void CurvzColorPicker::build_spectrum_row() {
    // Row: spectrum (expands) | 8px gap | hue strip (18px wide).
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_hexpand(true);

    m_spectrum_area = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(m_spectrum_area, "cp_spec", "color_picker_spectrum_da");
    m_spectrum_area->set_content_width(240);
    m_spectrum_area->set_content_height(180);
    m_spectrum_area->set_hexpand(true);
    m_spectrum_area->set_draw_func(
        sigc::mem_fun(*this, &CurvzColorPicker::draw_spectrum));

    // Click + drag on the spectrum.
    //
    // GestureClick handles tap-to-pick (press sets colour immediately).
    // GestureDrag handles drag tracking + commit. We record to recents
    // on drag_end because it fires reliably for both taps (zero-distance
    // drag) and real drags, whereas GestureClick::released can get
    // denied when the drag gesture claims the sequence.
    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect([this](int, double x, double y) {
        int w = m_spectrum_area->get_width();
        int h = m_spectrum_area->get_height();
        if (w <= 0 || h <= 0) return;
        set_from_spectrum(std::clamp(x / w, 0.0, 1.0),
                          std::clamp(y / h, 0.0, 1.0));
    });
    m_spectrum_area->add_controller(click);

    auto drag = Gtk::GestureDrag::create();
    drag->set_button(1);
    drag->signal_drag_update().connect([this, drag](double dx, double dy) {
        double sx = 0.0, sy = 0.0;
        drag->get_start_point(sx, sy);
        int w = m_spectrum_area->get_width();
        int h = m_spectrum_area->get_height();
        if (w <= 0 || h <= 0) return;
        double x = (sx + dx) / w;
        double y = (sy + dy) / h;
        set_from_spectrum(std::clamp(x, 0.0, 1.0),
                          std::clamp(y, 0.0, 1.0));
    });
    drag->signal_drag_end().connect([this](double, double) {
        record_recent();
    });
    m_spectrum_area->add_controller(drag);

    // Hue strip.
    m_hue_area = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(m_hue_area, "cp_hue", "color_picker_hue_da");
    m_hue_area->set_content_width(18);
    m_hue_area->set_content_height(180);
    m_hue_area->set_draw_func(
        sigc::mem_fun(*this, &CurvzColorPicker::draw_hue_strip));

    auto hue_click = Gtk::GestureClick::create();
    hue_click->set_button(1);
    hue_click->signal_pressed().connect([this](int, double, double y) {
        int h = m_hue_area->get_height();
        if (h <= 0) return;
        set_from_hue(std::clamp(y / h, 0.0, 1.0));
    });
    m_hue_area->add_controller(hue_click);

    auto hue_drag = Gtk::GestureDrag::create();
    hue_drag->set_button(1);
    hue_drag->signal_drag_update().connect([this, hue_drag](double, double dy) {
        double sx = 0.0, sy = 0.0;
        hue_drag->get_start_point(sx, sy);
        int h = m_hue_area->get_height();
        if (h <= 0) return;
        double y = (sy + dy) / h;
        set_from_hue(std::clamp(y, 0.0, 1.0));
    });
    hue_drag->signal_drag_end().connect([this](double, double) {
        record_recent();
    });
    m_hue_area->add_controller(hue_drag);

    row->append(*m_spectrum_area);
    row->append(*m_hue_area);
    append(*row);
}

void CurvzColorPicker::build_alpha_row() {
    m_alpha_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_alpha_row->set_hexpand(true);

    auto* lbl = Gtk::make_managed<Gtk::Label>("Alpha");
    lbl->set_xalign(0.0f);
    lbl->set_size_request(44, -1);

    m_alpha_area = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(m_alpha_area, "cp_alpha", "color_picker_alpha_da");
    m_alpha_area->set_content_height(18);
    m_alpha_area->set_hexpand(true);
    m_alpha_area->set_draw_func(
        sigc::mem_fun(*this, &CurvzColorPicker::draw_alpha_slider));

    auto click = Gtk::GestureClick::create();
    click->set_button(1);
    click->signal_pressed().connect([this](int, double x, double) {
        int w = m_alpha_area->get_width();
        if (w <= 0) return;
        set_from_alpha(std::clamp(x / w, 0.0, 1.0));
    });
    m_alpha_area->add_controller(click);

    auto drag = Gtk::GestureDrag::create();
    drag->set_button(1);
    drag->signal_drag_update().connect([this, drag](double dx, double) {
        double sx = 0.0, sy = 0.0;
        drag->get_start_point(sx, sy);
        int w = m_alpha_area->get_width();
        if (w <= 0) return;
        double x = (sx + dx) / w;
        set_from_alpha(std::clamp(x, 0.0, 1.0));
    });
    drag->signal_drag_end().connect([this](double, double) {
        record_recent();
    });
    m_alpha_area->add_controller(drag);

    m_alpha_row->append(*lbl);
    m_alpha_row->append(*m_alpha_area);
    append(*m_alpha_row);
}

void CurvzColorPicker::build_hex_row() {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    row->set_hexpand(true);

    auto* lbl = Gtk::make_managed<Gtk::Label>("Hex");
    lbl->set_xalign(0.0f);
    lbl->set_size_request(44, -1);

    m_hex_entry = Gtk::make_managed<CurvzEntry>();
    curvz::utils::set_name(m_hex_entry, "cp_hex", "color_picker_hex_entry");
    m_hex_entry->set_hexpand(true);
    m_hex_entry->set_max_length(9); // "#RRGGBBAA"
    m_hex_entry->on_commit([this]() {
        // Re-entrancy guard: refresh_from_current() sets the entry text,
        // which (on focus-leave) would re-fire this callback. The m_loading
        // flag short-circuits that. Per handoff S64 gotcha, reset is
        // deferred via signal_idle so async signal delivery finishes
        // before we clear the flag.
        if (m_loading) return;
        commit_hex_entry();
    });

    // Escape-cancel on the hex entry. CurvzEntry's built-in Escape handling
    // only clears its own cancelled flag; we layer a capture-phase key
    // controller on the whole picker to catch Escape anywhere.
    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Escape) { cancel(); return true; }
            return false;
        }, false);
    add_controller(kc);

    // Return / KP_Enter → commit-and-dismiss.
    //
    // BUBBLE phase (not CAPTURE) so child widgets get first crack at
    // the key. That matters for the hex entry: CurvzEntry's own CAPTURE
    // handler for Return fires first, runs commit_hex_entry() which
    // emits signal_committed. The popover host dismisses on that. Our
    // BUBBLE handler only fires when focus is elsewhere (spectrum,
    // hue strip, alpha slider, recents) — in those cases the child
    // didn't consume Return, so it bubbles up and we commit+dismiss.
    //
    // Space was a candidate too but Space-in-text-entry is a printable
    // character the user expects to type; adding Space-dismiss here
    // felt off even though it was technically filtered by BUBBLE. Stick
    // with Return only — matches dialog-OK convention.
    auto kc_commit = Gtk::EventControllerKey::create();
    kc_commit->set_propagation_phase(Gtk::PropagationPhase::BUBBLE);
    kc_commit->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Return ||
                keyval == GDK_KEY_KP_Enter) {
                commit();
                return true;
            }
            return false;
        }, false);
    add_controller(kc_commit);

    // Preview swatch: 40x24.
    m_preview_area = Gtk::make_managed<Gtk::DrawingArea>();
    curvz::utils::set_name(m_preview_area, "cp_pv", "color_picker_preview_da");
    m_preview_area->set_content_width(40);
    m_preview_area->set_content_height(24);
    m_preview_area->set_draw_func(
        sigc::mem_fun(*this, &CurvzColorPicker::draw_preview));

    row->append(*lbl);
    row->append(*m_hex_entry);
    row->append(*m_preview_area);
    append(*row);
}

// ─── drawing ──────────────────────────────────────────────────────────────

void CurvzColorPicker::draw_spectrum(const Cairo::RefPtr<Cairo::Context>& cr,
                                     int w, int h) {
    if (w <= 0 || h <= 0) return;

    // Sample the OKLCH (C, L) grid at fixed current hue. We draw at 3px
    // cells for performance — the gradient is smooth enough that per-pixel
    // would just burn CPU. A 240x180 spectrum at 3px cells = 4,800 fills
    // per redraw, well within Cairo's comfort zone.
    const int step = 3;
    for (int y = 0; y < h; y += step) {
        double ny = static_cast<double>(y) / h;
        double Lr = 1.0 - ny;
        double cmax = max_chroma_at(Lr, m_hue);  // gamut edge for this row
        for (int x = 0; x < w; x += step) {
            double nx = static_cast<double>(x) / w;
            color::OKLCH ok;
            ok.c = nx * cmax;
            ok.l = Lr;
            ok.h = m_hue;
            ok.a = 1.0;
            auto rgb = color::from_oklch(ok);
            cr->set_source_rgb(rgb.r, rgb.g, rgb.b);
            cr->rectangle(x, y, step, step);
            cr->fill();
        }
    }

    // Crosshair at the user's stored spectrum position. We deliberately
    // do NOT recompute from m_current — that round-trip is lossy when the
    // target OKLCH is outside the sRGB gamut (from_oklch clamps, and
    // to_oklch(clamped) moves the point). Drawing from m_spectrum_nx/ny
    // keeps the crosshair exactly under the cursor.
    paint_crosshair(cr, m_spectrum_nx * w, m_spectrum_ny * h);
}

void CurvzColorPicker::draw_hue_strip(const Cairo::RefPtr<Cairo::Context>& cr,
                                      int w, int h) {
    if (w <= 0 || h <= 0) return;

    // One row per 2px of height; hue = top→0, bottom→360 is the convention
    // most users expect from pickers, matching Affinity / Figma.
    const int step = 2;
    for (int y = 0; y < h; y += step) {
        double ny = static_cast<double>(y) / h;
        color::OKLCH ok;
        ok.l = HUE_STRIP_L;
        ok.c = HUE_STRIP_C;
        ok.h = ny * 360.0;
        ok.a = 1.0;
        auto rgb = color::from_oklch(ok);
        cr->set_source_rgb(rgb.r, rgb.g, rgb.b);
        cr->rectangle(0, y, w, step);
        cr->fill();
    }

    // Hue cursor: a horizontal line across the strip.
    double cy = (m_hue / 360.0) * h;
    cr->set_line_width(1.5);
    cr->set_source_rgb(0.0, 0.0, 0.0);
    cr->move_to(0, cy + 0.5); cr->line_to(w, cy + 0.5);
    cr->stroke();
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->set_line_width(0.8);
    cr->move_to(0, cy + 0.5); cr->line_to(w, cy + 0.5);
    cr->stroke();
}

void CurvzColorPicker::draw_alpha_slider(const Cairo::RefPtr<Cairo::Context>& cr,
                                         int w, int h) {
    if (w <= 0 || h <= 0) return;

    // Checkerboard underlay.
    paint_checker(cr, 0, 0, w, h);

    // Gradient from transparent→opaque over current rgb.
    auto grad = Cairo::LinearGradient::create(0, 0, w, 0);
    grad->add_color_stop_rgba(0.0, m_current.r, m_current.g, m_current.b, 0.0);
    grad->add_color_stop_rgba(1.0, m_current.r, m_current.g, m_current.b, 1.0);
    cr->set_source(grad);
    cr->rectangle(0, 0, w, h);
    cr->fill();

    // Thumb marker.
    double cx = m_current.a * w;
    cr->set_line_width(1.5);
    cr->set_source_rgb(0.0, 0.0, 0.0);
    cr->rectangle(cx - 2.0, 0.0, 4.0, h);
    cr->stroke();
    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->set_line_width(0.8);
    cr->rectangle(cx - 2.0, 0.0, 4.0, h);
    cr->stroke();
}

void CurvzColorPicker::draw_preview(const Cairo::RefPtr<Cairo::Context>& cr,
                                    int w, int h) {
    if (w <= 0 || h <= 0) return;

    // Checker underlay so alpha is visible.
    paint_checker(cr, 0, 0, w, h);

    // Left half: original. Right half: current. Shared diagonal line
    // between them so the comparison is obvious even when only alpha
    // changed.
    cr->save();
    cr->rectangle(0, 0, w / 2.0, h);
    cr->clip();
    cr->set_source_rgba(m_original.r, m_original.g, m_original.b, m_original.a);
    cr->rectangle(0, 0, w, h);
    cr->fill();
    cr->restore();

    cr->save();
    cr->rectangle(w / 2.0, 0, w / 2.0, h);
    cr->clip();
    cr->set_source_rgba(m_current.r, m_current.g, m_current.b, m_current.a);
    cr->rectangle(0, 0, w, h);
    cr->fill();
    cr->restore();

    // Hairline separator between the halves.
    cr->set_line_width(1.0);
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.4);
    cr->move_to(w / 2.0 + 0.5, 0);
    cr->line_to(w / 2.0 + 0.5, h);
    cr->stroke();

    // Outer border.
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.3);
    cr->rectangle(0.5, 0.5, w - 1.0, h - 1.0);
    cr->stroke();
}

// ─── input → state ────────────────────────────────────────────────────────

void CurvzColorPicker::set_from_spectrum(double nx, double ny) {
    // Record the user's click position directly. The crosshair will draw
    // at exactly this (nx, ny) regardless of any gamut clamping the
    // sRGB conversion below applies. This is the fix for "crosshair
    // drifts away from the cursor" seen on saturated reds etc.
    m_spectrum_nx = nx;
    m_spectrum_ny = ny;

    color::OKLCH ok;
    ok.l = 1.0 - ny;
    ok.c = nx * max_chroma_at(ok.l, m_hue);  // X = fraction of the gamut edge
    ok.h = m_hue;
    ok.a = m_current.a;

    auto rgb = color::from_oklch(ok);
    rgb.a = m_current.a;

    m_current = rgb;
    refresh_from_current();
    m_sig_changed.emit(m_current);
}

void CurvzColorPicker::set_from_hue(double ny) {
    m_hue = std::clamp(ny, 0.0, 1.0) * 360.0;

    // Rebuild current colour using the user's stored spectrum position,
    // NOT to_oklch(m_current). Going through m_current would re-derive
    // (C, L) from the gamut-clamped sRGB, which doesn't match where the
    // cursor is drawn. This way hue-strip drags keep the crosshair in
    // place and only swap the hue, exactly like Affinity.
    color::OKLCH ok;
    ok.l = 1.0 - m_spectrum_ny;
    ok.c = m_spectrum_nx * max_chroma_at(ok.l, m_hue);  // re-edge for new hue
    ok.h = m_hue;
    ok.a = m_current.a;
    auto rgb = color::from_oklch(ok);
    rgb.a = m_current.a;

    m_current = rgb;
    refresh_from_current();
    m_sig_changed.emit(m_current);
}

void CurvzColorPicker::set_from_alpha(double nx) {
    if (!m_with_alpha) return;
    m_current.a = std::clamp(nx, 0.0, 1.0);
    refresh_from_current();
    m_sig_changed.emit(m_current);
}

// ─── hex entry ────────────────────────────────────────────────────────────

void CurvzColorPicker::commit_hex_entry() {
    if (!m_hex_entry) return;
    std::string txt = m_hex_entry->get_text();

    auto parsed = color::from_hex(txt);
    if (!parsed) {
        // Invalid input: silently revert the entry to the current colour's
        // hex. No error banner — user sees the text snap back.
        m_loading = true;
        m_hex_entry->set_text(color::to_hex(m_current));
        Glib::signal_idle().connect_once([this] { m_loading = false; });
        return;
    }

    // If parsed hex has no alpha (#RRGGBB), keep the picker's current alpha.
    // from_hex defaults missing alpha to 1.0; the user's intent with a 6-digit
    // hex is "this colour, don't change my alpha". Honour that.
    if (txt.size() == 4 || txt.size() == 7) {
        parsed->a = m_current.a;
    }

    m_current = *parsed;
    sync_hue_from_current();
    refresh_from_current();
    m_sig_changed.emit(m_current);
    // Record before committed (popover hosts tear down on committed;
    // see pick_recent for the same rule).
    record_recent();
    m_sig_committed.emit(m_current);
}

void CurvzColorPicker::sync_hue_from_current() {
    auto ok = color::to_oklch(m_current);
    // Only update m_hue if the new colour has meaningful chroma; otherwise
    // preserve the user's last hue so the spectrum doesn't reset to red
    // every time they pass through greyscale.
    if (ok.c > 1e-6) m_hue = ok.h;

    // Also sync the spectrum (nx, ny) position so the crosshair jumps
    // to the new colour. This path runs when a colour arrives from
    // outside the spectrum — hex entry, pick_recent — where the user
    // didn't click in the spectrum, so the OKLCH round-trip is the
    // best position we have. Users who then drag in the spectrum
    // overwrite this via set_from_spectrum.
    double cm = max_chroma_at(ok.l, ok.h);
    m_spectrum_nx = (cm > 1e-9) ? std::clamp(ok.c / cm, 0.0, 1.0) : 0.0;
    m_spectrum_ny = std::clamp(1.0 - ok.l, 0.0, 1.0);
}

// ─── refresh ──────────────────────────────────────────────────────────────

void CurvzColorPicker::refresh_from_current() {
    // Update hex entry (guarded against re-entry).
    if (m_hex_entry) {
        m_loading = true;
        m_hex_entry->set_text(color::to_hex(m_current));
        // Per S64 handoff: defer m_loading reset via signal_idle so async
        // signal delivery from set_text finishes before we allow commit
        // callbacks to run again.
        Glib::signal_idle().connect_once([this] { m_loading = false; });
    }

    // Redraw every area; each depends on m_current or m_hue.
    if (m_spectrum_area) m_spectrum_area->queue_draw();
    if (m_hue_area)      m_hue_area->queue_draw();
    if (m_alpha_area)    m_alpha_area->queue_draw();
    if (m_preview_area)  m_preview_area->queue_draw();
    for (auto* slot : m_recent_slots) {
        if (slot) slot->queue_draw();
    }
}

// ─── recents ──────────────────────────────────────────────────────────────

void CurvzColorPicker::build_recents_row() {
    // Row of 12 fixed-size 18×18 swatches, 4px spacing. Each is its own
    // DrawingArea so we can render dashed-empty-state per slot without
    // juggling a single large area's layout.
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    row->set_hexpand(true);
    row->add_css_class("curvz-color-picker-recents");

    for (std::size_t i = 0; i < m_recent_slots.size(); ++i) {
        auto* slot = Gtk::make_managed<Gtk::DrawingArea>();
        slot->set_content_width(18);
        slot->set_content_height(18);
        // Per-slot draw captures `i` so each DrawingArea pulls its own
        // entry from the snapshot at paint time. Snapshot is cheap (copy
        // of 12 Color structs) so we don't cache across slots.
        slot->set_draw_func(
            [this, i](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
                draw_recent_slot(cr, w, h, i);
            });

        auto click = Gtk::GestureClick::create();
        click->set_button(1);
        click->signal_released().connect(
            [this, i](int, double, double) { pick_recent(i); });
        slot->add_controller(click);

        m_recent_slots[i] = slot;
        row->append(*slot);
    }

    append(*row);
}

void CurvzColorPicker::draw_recent_slot(
    const Cairo::RefPtr<Cairo::Context>& cr, int w, int h,
    std::size_t slot_index) {
    if (w <= 0 || h <= 0) return;

    auto snap = ColorPickerRecents::get().snapshot();

    if (slot_index >= snap.size()) {
        // Empty slot: dashed rounded rectangle, subtle. The affordance
        // stays discoverable without screaming at the user.
        //
        // No dash-reset at the end — we return immediately after stroke,
        // and the next draw cycle gets a fresh Cairo context. If this
        // function ever grows more drawing past this block, reset dash
        // explicitly with cr->set_dash(std::vector<double>{}, 0.0).
        std::vector<double> dashes{2.0, 2.0};
        cr->set_dash(dashes, 0.0);
        cr->set_line_width(1.0);
        cr->set_source_rgba(0.0, 0.0, 0.0, 0.25);
        cr->rectangle(0.5, 0.5, w - 1.0, h - 1.0);
        cr->stroke();
        return;
    }

    const auto& c = snap[slot_index];

    // Checker underlay so alpha-transparent recents are legible.
    // Smaller checker for a smaller slot — 3px instead of 6.
    cr->save();
    cr->rectangle(0, 0, w, h);
    cr->clip();
    const int small_checker = 3;
    for (int iy = 0; iy * small_checker < h; ++iy) {
        for (int ix = 0; ix * small_checker < w; ++ix) {
            bool dark = ((ix + iy) & 1);
            if (dark) cr->set_source_rgb(0.75, 0.75, 0.75);
            else      cr->set_source_rgb(0.95, 0.95, 0.95);
            cr->rectangle(ix * small_checker, iy * small_checker,
                          small_checker, small_checker);
            cr->fill();
        }
    }
    cr->restore();

    // Colour fill.
    cr->set_source_rgba(c.r, c.g, c.b, c.a);
    cr->rectangle(0, 0, w, h);
    cr->fill();

    // Hairline border.
    cr->set_line_width(1.0);
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.3);
    cr->rectangle(0.5, 0.5, w - 1.0, h - 1.0);
    cr->stroke();
}

void CurvzColorPicker::pick_recent(std::size_t slot_index) {
    auto snap = ColorPickerRecents::get().snapshot();
    if (slot_index >= snap.size()) return;         // empty slot

    m_current = snap[slot_index];
    sync_hue_from_current();
    refresh_from_current();
    m_sig_changed.emit(m_current);
    // Record BEFORE emitting signal_committed. Committed listeners may
    // tear down the popover (and with it, this picker's widget subtree);
    // touching m_recent_slots inside record_recent() afterwards would
    // be a use-after-free. Move-to-front dedup in ColorPickerRecents
    // makes the re-record safe idempotent.
    record_recent();
    m_sig_committed.emit(m_current);
}

void CurvzColorPicker::record_recent() {
    ColorPickerRecents::get().add(m_current);
    // Redraw the strip so the new entry appears immediately.
    for (auto* slot : m_recent_slots) {
        if (slot) slot->queue_draw();
    }
}

} // namespace Curvz
