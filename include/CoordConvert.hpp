#pragma once
#include "CoordSpace.hpp"
#include "CurvzDocument.hpp"  // CanvasModel, DisplayMode
#include "UnitSystem.hpp"     // Unit enum

// ── CoordConvert — doc-space <-> display-space, the canonical seam ───────────
//
// Curvz stores all geometry in doc-space pixels: Y-down, origin top-left of
// canvas, no ruler offset, units always px. The user sees a different space:
// Y-up, origin set by the ruler (defaults to bottom-left), units chosen by
// AppPreferences (px / in / mm / pt). The full conversion is a stack of three
// orthogonal transforms:
//
//   doc (Y-down, top-left, px)
//     |   CoordSpace Y-flip
//     v
//   user-px (Y-up, top-left, px)
//     |   ruler-origin offset
//     v
//   ruler-px (Y-up, ruler-origin, px)
//     |   px_to_unit / unit_to_px (DPI-aware)
//     v
//   display (Y-up, ruler-origin, model->display_unit)
//
// CoordSpace handles step 1 — and only step 1. This header composes all three
// into the four position helpers that any UI surface needing user-facing
// coordinates should use. Both CurvzSpinButton (PositionX / PositionY) and
// non-spinner consumers (the right-click guide dialog, etc.) call these
// helpers; the math has exactly one home.
//
// Angle and delta helpers are not duplicated here — CoordSpace already has
// to_user_angle_deg / to_doc_angle_deg / to_user_delta / to_doc_delta and
// those compose without any need for ruler offset or unit scaling.
//
// ─────────────────────────────────────────────────────────────────────────────

namespace Curvz {

// ── DPI-aware unit conversion ────────────────────────────────────────────────
// In Physical mode the document carries an explicit DPI that overrides the
// CSS-standard 96. Pixel / Ratio modes always use 96. The two helpers below
// are the canonical px <-> unit gateway.

inline double effective_dpi(const CanvasModel* m) {
    if (!m) return 96.0;
    if (m->display_mode == DisplayMode::Physical && m->dpi > 0)
        return (double)m->dpi;
    return 96.0;
}

// doc-px -> display unit
inline double px_to_unit(double internal, Unit u, const CanvasModel* m) {
    double dpi = effective_dpi(m);
    if (dpi <= 0.0) dpi = 96.0;
    switch (u) {
    case Unit::Px: return internal;
    case Unit::In: return internal / dpi;
    case Unit::Mm: return internal / dpi * 25.4;
    case Unit::Pt: return internal / dpi * 72.0;
    }
    return internal;
}

// display unit -> doc-px
inline double unit_to_px(double display, Unit u, const CanvasModel* m) {
    double dpi = effective_dpi(m);
    if (dpi <= 0.0) dpi = 96.0;
    switch (u) {
    case Unit::Px: return display;
    case Unit::In: return display * dpi;
    case Unit::Mm: return display / 25.4 * dpi;
    case Unit::Pt: return display / 72.0 * dpi;
    }
    return display;
}

// ── Position conversions (full stack) ─────────────────────────────────────────
//
// doc_x is a stored X coordinate (doc-px). ruler_ox is the user-space ruler
// origin in px (typically from m_doc->ruler_origin_x). The model carries
// display_unit and DPI context.
//
// X has no Y-flip; the stack is ruler-offset + unit conversion.
// Y adds the CoordSpace flip on top.

inline double doc_to_display_x(double doc_x,
                               const CanvasModel* m,
                               double ruler_ox) {
    double user_x = doc_x - ruler_ox;
    return px_to_unit(user_x, m ? m->display_unit : Unit::Px, m);
}

inline double display_to_doc_x(double display_x,
                               const CanvasModel* m,
                               double ruler_ox) {
    double user_x = unit_to_px(display_x, m ? m->display_unit : Unit::Px, m);
    return user_x + ruler_ox;
}

inline double doc_to_display_y(double doc_y,
                               const CanvasModel* m,
                               double ruler_oy) {
    if (!m) return doc_y;
    CoordSpace cs{(double)m->canvas_height()};
    double user_y = cs.to_user_y(doc_y) - ruler_oy;
    return px_to_unit(user_y, m->display_unit, m);
}

inline double display_to_doc_y(double display_y,
                               const CanvasModel* m,
                               double ruler_oy) {
    if (!m) return display_y;
    double user_y = unit_to_px(display_y, m->display_unit, m);
    user_y += ruler_oy;
    CoordSpace cs{(double)m->canvas_height()};
    return cs.to_doc_y(user_y);
}

} // namespace Curvz
