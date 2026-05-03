#pragma once
#include "CurvzDocument.hpp"
#include <string>

// ── SvgOptimiser ──────────────────────────────────────────────────────────────
// Produces a clean, export-ready SVG from a CurvzDocument.
//
// Rules applied:
//   • GuideLayer and RefLayer nodes are skipped entirely (no output)
//   • Layers with visible == false are skipped
//   • Objects with visible == false are skipped
//   • All data-curvz-* private attributes are stripped
//   • Layer <g> wrappers are stripped (icons are flat — no layer structure needed)
//   • The SVG root carries viewBox and width/height from the document canvas
//   • The -symbolic suffix is NOT appended here — callers handle naming
//
// The returned string is a complete, standalone SVG document.

namespace Curvz {

std::string optimise_svg(const CurvzDocument& doc);

} // namespace Curvz
