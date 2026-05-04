#pragma once
#include "CurvzDocument.hpp"
#include <string>
#include <vector>

// ── PngExporter ───────────────────────────────────────────────────────────────
// Renders a CurvzDocument to PNG at one or more sizes using Cairo.
// Rendering is done directly from the document model (no librsvg dependency).
//
// currentColor is rendered as black (0,0,0) — the freedesktop convention for
// symbolic icon PNGs. GTK recolours the SVG variant at runtime; the PNG
// variants are for fallback / system-wide installation only.
//
// Standard icon sizes:
//   16, 24, 32, 48, 64, 128, 256
//
// For non-square documents the image is scaled to fit within the target square
// with the aspect ratio preserved (letterboxed with transparency).

namespace Curvz {

// Standard sizes exported for a complete icon theme
static constexpr int k_png_sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
static constexpr int k_png_size_count = 7;

// Render the document to a PNG file at the given pixel size.
// Square output, aspect-preserved with letterboxed transparency.
// Returns true on success.
bool export_png(const CurvzDocument& doc, const std::string& path, int size_px);

// Render to an in-memory PNG blob. Returns empty vector on failure.
std::vector<unsigned char> render_png(const CurvzDocument& doc, int size_px);

// ── S104 m1 — non-square aspect-preserving export ─────────────────────────────
//
// Render the document to a PNG file at the given pixel dimensions. The
// document is scaled to FILL the surface — the canvas's aspect ratio is
// preserved by construction (caller is expected to compute height from
// width × doc-aspect or vice-versa). No letterboxing.
//
// Used by Project → Export Documents… where the user fixes one dimension
// (width or height) and the other is derived from the document's aspect
// ratio. The square fit-into-square model used by export_png is wrong
// for general document export — that one targets the icon-theme PNG
// fallback case where every output must be a fixed square.
//
// Returns true on success.
bool export_png_sized(const CurvzDocument& doc,
                      const std::string& path,
                      int width_px,
                      int height_px);

// ── Template thumbnail (S63 M2) ───────────────────────────────────────────────
// Renders an annotated thumbnail for a template bundle: the canvas outline
// plus proportional grid (gray dots at intersections) and margin (solid red
// rectangle), with the template name centered and the canvas dimensions
// below. Used by TemplateLibrary::save() so every bundle — seed or user-
// authored — gets a self-describing thumbnail readable at 128×128.
//
// grid_divisions / margin_ratio / grid_offset_ratio come straight from the
// template's metadata. Zero values disable the corresponding annotation, so
// user templates saved without proportional rules get a plain canvas + name
// + dimensions thumbnail (still better than a blank square).
//
// name_label is the human name rendered centered on the thumb (typically
// meta.name). dimensions_label is rendered smaller below (typically the spec
// string like "1080 × 1920" or "8.5 × 11 in"). Empty strings skip the label.
//
// workspace_r/g/b and artboard_r/g/b are the project's motif-resolved colours
// for the variant being rendered — the bundle stores one PNG per motif so
// the dialog can pick the matching one at display time. These map directly
// to CurvzProject::workspace_r() / artboard_r() for the chosen motif.
bool export_template_thumbnail(const CurvzDocument& doc,
                               const std::string& path,
                               int size_px,
                               const std::string& name_label,
                               const std::string& dimensions_label,
                               int grid_divisions,
                               double margin_ratio,
                               double grid_offset_ratio,
                               double workspace_r, double workspace_g, double workspace_b,
                               double artboard_r,  double artboard_g,  double artboard_b);

} // namespace Curvz
