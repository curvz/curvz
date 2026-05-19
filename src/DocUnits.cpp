#include "DocUnits.hpp"

#include "CoordSpace.hpp"
#include "UnitSystem.hpp"

#include <algorithm>

namespace Curvz {
namespace DocUnits {

// ── s265 m2: intent-aware unit pump ──────────────────────────────────────
//
// The user's coordinate system is whatever they typed into the Dimensions
// "Size" field. For a 16-inch poster, "1" on the ruler means one inch,
// not one doc-pixel — even though the working plane is 1000 doc-px on the
// short side. This is the seam where doc-pixels become user-units, so the
// rule lives HERE and every caller (rulers, status bar, inspector, layer
// panel, sidecar exporter, spin buttons) inherits it for free.
//
// Resolution order, in priority:
//
//   1. intended_w/h/unit set         The Size+Units the user typed.
//                                    Scale doc-px to that span directly:
//                                    user_value = doc_value
//                                               * (intended_w / canvas_w)
//                                    Result is in intended_unit (px / in /
//                                    mm / pt) — no further from_px.
//
//   2. Physical mode, intent unset   Legacy Curvz pre-s264 Physical mode:
//                                    phys_width/height in inches drive
//                                    the scale. Kept for back-compat
//                                    with files that pre-date intent.
//
//   3. RatioQuality, intent unset    Pre-s264 Ratio mode reported in
//                                    "% of short axis." Kept for back-
//                                    compat. Once Ratio mode retires this
//                                    branch dies with it.
//
//   4. Pixel, intent unset           1:1 with doc-px, then converted via
//                                    UnitSystem::from_px (the 96-dpi
//                                    SVG-default assumption). Old files
//                                    in any unit other than px hit this.
//
// Single point of truth: changing the rule here changes the rule
// everywhere. Per the canon principle: "find the seam where a concept
// lives, build a small abstraction at that seam."
//
// Helper for the intent branch. Picks the right unit out of the
// intended_unit string. Empty / unrecognised → px.
namespace {
inline Unit intended_unit_to_Unit(const std::string& s) {
    if (s == "in") return Unit::In;
    if (s == "mm") return Unit::Mm;
    if (s == "pt") return Unit::Pt;
    return Unit::Px;
}
inline bool has_intent(const CanvasModel* cm) {
    return cm && cm->intended_w > 0.0 && cm->intended_h > 0.0;
}
} // namespace

double doc_to_display_x(double doc_x, const CanvasModel* cm,
                        double ruler_ox) {
    // X doesn't flip; subtract ruler origin in user space.
    double user_x = doc_x - ruler_ox;
    if (!cm)
        return user_x;

    // ── Branch 1: intent set. The user typed a Size; report in that span.
    if (has_intent(cm)) {
        double cw = (double)cm->canvas_width();
        double scale = (cw > 0.0) ? (cm->intended_w / cw) : 1.0;
        return user_x * scale;
    }

    int q = std::max(1, cm->quality);
    double scaled;
    if (cm->display_mode == DisplayMode::RatioQuality) {
        scaled = user_x / q * 100.0;
    } else if (cm->display_mode == DisplayMode::Physical) {
        double sp = std::min(cm->phys_width, cm->phys_height);
        scaled = user_x / q * sp;
        return scaled; // already in physical units
    } else {
        scaled = user_x; // Pixel: 1:1
    }
    return UnitSystem::from_px(scaled, cm->display_unit);
}

double doc_to_display_y(double doc_y, const CanvasModel* cm,
                        double ruler_oy) {
    double ch = cm ? (double)cm->canvas_height() : 1.0;
    CoordSpace cs{ch};
    double user_y = cs.to_user_y(doc_y) - ruler_oy;
    if (!cm)
        return user_y;

    // ── Branch 1: intent set. The user typed a Size; report in that span.
    if (has_intent(cm)) {
        double scale = (ch > 0.0) ? (cm->intended_h / ch) : 1.0;
        return user_y * scale;
    }

    int q = std::max(1, cm->quality);
    double scaled;
    if (cm->display_mode == DisplayMode::RatioQuality) {
        scaled = user_y / q * 100.0;
    } else if (cm->display_mode == DisplayMode::Physical) {
        double sp = std::min(cm->phys_width, cm->phys_height);
        scaled = user_y / q * sp;
        return scaled;
    } else {
        scaled = user_y;
    }
    return UnitSystem::from_px(scaled, cm->display_unit);
}

double display_to_doc_x(double disp, const CanvasModel* cm,
                        double ruler_ox) {
    double user_x = disp;
    if (cm) {
        // ── Inverse Branch 1: intent set. Map user-units back to doc-px.
        if (has_intent(cm)) {
            double cw = (double)cm->canvas_width();
            double scale = (cm->intended_w > 0.0) ? (cw / cm->intended_w)
                                                  : 1.0;
            return disp * scale + ruler_ox;
        }
        int q = std::max(1, cm->quality);
        if (cm->display_mode == DisplayMode::RatioQuality) {
            user_x = disp / 100.0 * q;
        } else if (cm->display_mode == DisplayMode::Physical) {
            double sp = std::min(cm->phys_width, cm->phys_height);
            user_x = (sp > 0) ? disp / sp * q : disp;
        } else {
            user_x = UnitSystem::to_px(disp, cm->display_unit);
        }
    }
    return user_x + ruler_ox;
}

double display_to_doc_y(double disp, const CanvasModel* cm,
                        double ruler_oy) {
    double user_y = disp;
    if (cm) {
        // ── Inverse Branch 1: intent set. Map user-units back to doc-px.
        if (has_intent(cm)) {
            double ch_i = (double)cm->canvas_height();
            double scale = (cm->intended_h > 0.0) ? (ch_i / cm->intended_h)
                                                  : 1.0;
            user_y = disp * scale;
        } else {
            int q = std::max(1, cm->quality);
            if (cm->display_mode == DisplayMode::RatioQuality) {
                user_y = disp / 100.0 * q;
            } else if (cm->display_mode == DisplayMode::Physical) {
                double sp = std::min(cm->phys_width, cm->phys_height);
                user_y = (sp > 0) ? disp / sp * q : disp;
            } else {
                user_y = UnitSystem::to_px(disp, cm->display_unit);
            }
        }
    }
    user_y += ruler_oy;
    double ch = cm ? (double)cm->canvas_height() : 1.0;
    CoordSpace cs{ch};
    return cs.to_doc_y(user_y);
}

} // namespace DocUnits
} // namespace Curvz
