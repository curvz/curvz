#pragma once
#include "CurvzDocument.hpp"
#include <string>
#include <memory>

namespace Curvz {

// Parse a minimal SVG file into a CurvzDocument.
// Only handles the subset we write: rect, ellipse, path with basic style attrs.
// Returns nullptr on failure.
std::unique_ptr<CurvzDocument> parse_svg_file(const std::string& path);

// Parse from an SVG string.
std::unique_ptr<CurvzDocument> parse_svg(const std::string& svg);

} // namespace Curvz
