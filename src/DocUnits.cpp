#include "DocUnits.hpp"

#include "CoordSpace.hpp"
#include "UnitSystem.hpp"

#include <algorithm>

namespace Curvz {
namespace DocUnits {

double doc_to_display_x(double doc_x, const CanvasModel* cm,
                        double ruler_ox) {
    // X doesn't flip; subtract ruler origin in user space.
    double user_x = doc_x - ruler_ox;
    if (!cm)
        return user_x;
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
    user_y += ruler_oy;
    double ch = cm ? (double)cm->canvas_height() : 1.0;
    CoordSpace cs{ch};
    return cs.to_doc_y(user_y);
}

} // namespace DocUnits
} // namespace Curvz
