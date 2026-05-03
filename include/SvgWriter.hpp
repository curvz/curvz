#pragma once
#include "CurvzDocument.hpp"
#include <string>

namespace Curvz {

// Writes a clean, minimal SVG from a CurvzDocument.
// Returns the SVG string, or empty string on failure.
std::string write_svg(const CurvzDocument& doc);

// Write SVG directly to a file path. Returns true on success.
bool write_svg_file(const CurvzDocument& doc, const std::string& path);

// ── S104 m1 — Export Documents metadata ─────────────────────────────────────
// Same output as write_svg_file, plus two custom attributes on the root
// <svg> element that record the user's chosen export sizing intent:
//
//   data-curvz-export-units = "px" | "mm" | "in" | "pt"
//   data-curvz-export-size  = "<numeric value in those units>"
//   data-curvz-export-fit   = "width" | "height"
//
// These let downstream tooling (and Curvz itself, on re-import) recover
// the units the document was authored at, even though SVG itself only
// carries px-equivalents in viewBox/width/height. Standard SVG renderers
// ignore unknown data-* attributes — this is purely metadata.
//
// Used by Project → Export Documents… SVG path. Not used by the icon-
// theme exporter (icons don't have a meaningful "20mm" intent).
bool write_svg_file_with_export_meta(const CurvzDocument& doc,
                                     const std::string& path,
                                     const std::string& units,
                                     double size_value,
                                     const std::string& fit_side);

} // namespace Curvz
