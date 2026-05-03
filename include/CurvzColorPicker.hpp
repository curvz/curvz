#pragma once
//
// CurvzColorPicker — Phase 2 replacement for Gtk::ColorDialog.
//
// A self-contained Gtk::Box that the caller can drop into a popover, a
// dialog window, or an inspector section. The widget owns no position
// policy; it just emits signal_changed() live and signal_committed() on
// Enter / explicit OK.
//
// Layout (top to bottom):
//   ┌─────────────────────────────┬───┐
//   │ Chroma / Lightness spectrum │ H │    (Cairo-drawn DrawingAreas.)
//   │ (pick C,L at current hue)   │ u │     H strip is 18px wide.
//   │                             │ e │
//   │                             │   │
//   ├─────────────────────────────┴───┤
//   │ Alpha slider (full width)        │   Only shown when with_alpha=true.
//   ├─────────────────────────────┬───┤
//   │ Hex input                   │▓▓▓│   Live preview swatch on the right
//   └─────────────────────────────┴───┘   (new over old, diagonal split).
//
// Colour math:
//   * Hue is an OKLCH h-degree (0–360).
//   * Spectrum samples a 2D grid in OKLCH (C on X, L on Y at current h)
//     and clamps each sample back into sRGB in from_oklch(). Samples that
//     fell outside the sRGB gamut are clamped at sRGB edge; this produces
//     the visible "shoulder" past ~0.37 chroma. That's fine — it matches
//     how Affinity and Figma present the same panel.
//   * Storage is still sRGB (color::Color). OKLCH values are recomputed
//     from current sRGB on every round-trip, so there's no hidden drift.
//
// Phase 2 / M1. No call sites wired yet.
//

#include "color/Color.hpp"

#include <gtkmm/box.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/label.h>
#include <glibmm/refptr.h>
#include <sigc++/signal.h>

#include <array>
#include <cstddef>

namespace Curvz {

class CurvzEntry; // fwd decl; defined in CurvzEntry.hpp

class CurvzColorPicker : public Gtk::Box {
public:
    CurvzColorPicker();

    // Set the picker's starting colour. Also records it as the "original"
    // half of the preview swatch.
    void set_initial(const color::Color& c);

    // Whether the alpha slider is visible and editable. Default: true.
    // When false, alpha is pinned to 1.0.
    void set_with_alpha(bool on);

    // Current colour, reflecting whatever the user has done since
    // set_initial().
    color::Color current() const;

    // signal_changed fires on every interactive edit (drag, slider move,
    // hex commit). Useful for live previews.
    sigc::signal<void(color::Color)>& signal_changed();

    // signal_committed fires when the user explicitly accepts the value
    // (Enter in the hex field, or later an OK button on a hosting dialog
    // that calls commit() manually). Callers using this as a popover can
    // just treat signal_changed as commit and ignore this.
    sigc::signal<void(color::Color)>& signal_committed();

    // signal_cancelled fires on Escape in the hex field. Hosting containers
    // that want Escape to close themselves should listen for this.
    sigc::signal<void()>& signal_cancelled();

    // Explicit commit / cancel; a hosting dialog's OK / Cancel buttons
    // can call these to route through the same signals as keyboard input.
    void commit();
    void cancel();

private:
    // UI construction. Each of these builds a single row or area and
    // attaches it to *this. Called once from the ctor.
    void build_spectrum_row();
    void build_alpha_row();
    void build_hex_row();
    void build_recents_row();

    // Drawing callbacks for the three Cairo areas.
    void draw_spectrum(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_hue_strip(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_alpha_slider(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_preview(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_recent_slot(const Cairo::RefPtr<Cairo::Context>& cr,
                          int w, int h, std::size_t slot_index);

    // Input handlers. set_from_spectrum / set_from_hue / set_from_alpha
    // take normalised [0,1] coordinates; coordinate-space conversions
    // happen in the gesture callbacks that wire to these.
    void set_from_spectrum(double nx, double ny);
    void set_from_hue(double ny);
    void set_from_alpha(double nx);

    // Apply a recent slot's colour as the current value. No-op if the
    // slot is empty.
    void pick_recent(std::size_t slot_index);

    // Push m_current onto the global recents history. Called from the
    // drag-end / click-release handlers on the three input areas, and
    // from committed hex entries.
    void record_recent();

    // Parse the hex entry and, if valid, apply. Invalid input is reverted
    // to the current colour on commit (silent restore).
    void commit_hex_entry();

    // Pull m_current into the hex entry + all DrawingAreas. Called from
    // set_initial() and after any interactive edit. The m_loading guard
    // prevents the entry's commit signal from looping back into us.
    void refresh_from_current();

    // Ensure m_hue is consistent with m_current when the user changed
    // m_current via hex or alpha (not via the hue strip). Without this
    // the hue cursor would drift away from the actual colour.
    void sync_hue_from_current();

    // --- state ----------------------------------------------------------

    color::Color m_original {color::Color::black()};
    color::Color m_current  {color::Color::black()};

    // Hue is tracked separately because achromatic colours (C=0) don't
    // have a meaningful hue; we preserve the user's last-selected hue
    // across forays into greyscale so the spectrum doesn't collapse.
    double m_hue = 0.0; // degrees, [0, 360)

    // Spectrum cursor position, stored directly rather than derived from
    // m_current. The derivation (sRGB -> OKLCH -> normalised) is lossy
    // when the requested OKLCH fell outside the sRGB gamut: from_oklch()
    // clamps, and the round-trip no longer matches the user's click.
    // Storing nx/ny keeps the crosshair under the cursor.
    double m_spectrum_nx = 0.0;  // 0..1, chroma axis
    double m_spectrum_ny = 1.0;  // 0..1, lightness-from-top axis (1 = bottom)

    bool m_with_alpha = true;

    // Re-entrancy guard on hex entry commit.
    bool m_loading = false;

    // --- widgets --------------------------------------------------------

    Gtk::DrawingArea* m_spectrum_area = nullptr;
    Gtk::DrawingArea* m_hue_area      = nullptr;
    Gtk::DrawingArea* m_alpha_area    = nullptr;
    Gtk::DrawingArea* m_preview_area  = nullptr;
    Gtk::Box*         m_alpha_row     = nullptr; // hide/show wholesale
    CurvzEntry*       m_hex_entry     = nullptr;

    // Recents strip. Twelve DrawingAreas, one per slot. Populated with
    // whatever ColorPickerRecents::snapshot() yields at draw time.
    std::array<Gtk::DrawingArea*, 12> m_recent_slots {};

    // --- signals --------------------------------------------------------

    sigc::signal<void(color::Color)> m_sig_changed;
    sigc::signal<void(color::Color)> m_sig_committed;
    sigc::signal<void()>             m_sig_cancelled;
};

} // namespace Curvz
