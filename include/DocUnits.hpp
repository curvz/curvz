#pragma once
//
// DocUnits — canonical document → display coordinate conversion.
//
// The user-facing coordinate system the ruler shows, the inspector
// reports, and the sidecar exporter writes is the same coordinate
// system. This file is the seam where that conversion lives so every
// caller goes through the same pump.
//
// Inputs:
//   - A doc-space coordinate in raw pixels (Y-down, origin at canvas
//     top-left — Cairo/SVG native).
//   - The doc's CanvasModel (carries display_mode, display_unit,
//     quality, phys_width/height — everything the conversion depends on).
//   - The ruler origin offset in user space (subtract from user-space
//     coord to position relative to the ruler's chosen zero).
//
// Outputs:
//   - The numeric value the user would read on the ruler / inspector
//     for that coordinate, in the doc's current display unit.
//
// Branches on display_mode:
//   - Pixel:        scaled by UnitSystem::from_px(value, display_unit)
//   - Physical:     scaled by quality / phys_short — already in
//                   physical units (no further from_px), since the
//                   doc's px-per-physical-unit is determined by
//                   quality / phys_short
//   - RatioQuality: scaled to percentage (0..100) of the short axis
//
// Y handling: the Y pump applies CoordSpace's Y-flip (Y-down ->
// Y-up) before subtracting the ruler origin, since user space is
// Y-up with origin at the artboard's bottom-left.
//
// Inverse pumps (display_to_doc_*) are provided for inspector
// commit paths.
//
// History: this conversion lived as static file-local helpers in
// PropertiesPanel.cpp until s177. Lifted to a shared pump when
// RefptExporter needed the same conversion at the export boundary,
// per the canon principle "find the seam where a concept lives,
// build a small abstraction at that seam, compose callers."
//

#include "CurvzDocument.hpp"

namespace Curvz {
namespace DocUnits {

// Forward conversion — doc-space (Y-down, origin at canvas TL, raw
// pixels) to display value (display unit, ruler-origin-translated).
//
// `cm` may be null; if so, the X function passes user_x through
// untouched and the Y function falls back to a 1-unit canvas height
// (this matches the behaviour of the inspector's static helpers
// pre-lift, preserved for safety).
double doc_to_display_x(double doc_x, const CanvasModel* cm,
                        double ruler_ox);
double doc_to_display_y(double doc_y, const CanvasModel* cm,
                        double ruler_oy);

// Inverse — display value (display unit, ruler-origin-translated)
// back to doc-space (Y-down, origin at canvas TL, raw pixels).
double display_to_doc_x(double disp, const CanvasModel* cm,
                        double ruler_ox);
double display_to_doc_y(double disp, const CanvasModel* cm,
                        double ruler_oy);

} // namespace DocUnits
} // namespace Curvz
