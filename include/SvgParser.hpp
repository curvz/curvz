#pragma once
#include "CurvzDocument.hpp"
#include <string>
#include <memory>

namespace Curvz {

// Parse a minimal SVG file into a CurvzDocument.
// Only handles the subset we write: rect, ellipse, path with basic style attrs.
// Returns nullptr on failure.
//
// fail_reason (optional, s360): if non-null and the parse fails (file
// unreadable, or a catastrophic parse exception), it receives a one-line
// human-readable reason for the interactive import path to surface in a
// dialog. Left untouched on success. Non-interactive callers (template /
// icon-scan / project load) pass nullptr and are unaffected.
std::unique_ptr<CurvzDocument> parse_svg_file(const std::string& path,
                                              std::string* fail_reason = nullptr);

// Parse from an SVG string.
std::unique_ptr<CurvzDocument> parse_svg(const std::string& svg);

} // namespace Curvz
