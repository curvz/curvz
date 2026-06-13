#pragma once
// ── GlyphOutline — the shared "glyphs in, outline SceneNodes out" unit ───────
// s350 m1 (docs/text_on_path_v2.md save-presentation arc). The per-glyph
// FreeType contour extraction (FTOutlineCtx + decompose callbacks) and the
// pattern-text glyph walk used to live file-static in Canvas_ops.cpp /
// Canvas_draw.cpp, reachable only from a Canvas instance. SvgWriter has no
// Canvas, so the "always-complete SVG" ruling (every save emits rendered text
// as real glyph outlines, regenerated each save, discarded by the parser) had
// no way to produce those outlines at write time.
//
// This unit is that seam: one place that turns laid-out text — box flow or
// pattern (text-on-path) — into glyph-outline SceneNodes. Both the convert
// verb (Canvas_ops::text_to_paths_op / build_mgr_outline_group) and the
// SvgWriter presentation emit consume it, so the canvas, the converted paths,
// and the saved file are exact by construction off ONE outliner.
//
// Geometry comes through the same pumps the renderer uses: compute_text_layout
// for the baselines, pattern_walk_path + BezierPath arc parameterization for
// the on-curve placement. Nothing is recomputed here that the fitter already
// produced — wrap, margins, per-run styles, justify, letter-spacing, flip and
// the v2 ascender-attachment convention are all already baked into the
// PangoLayout advances / positions this unit reads.

#include "SceneNode.hpp"   // SceneNode, PathData, FillStyle, StrokeStyle
#include "math/Vec2.hpp"   // Vec2 (PatternGlyph::pos) — SceneNode.hpp doesn't pull it
#include <pango/pango.h>   // PangoGlyphInfo / PangoFont (PatternGlyph fields)
#include <functional>
#include <memory>
#include <vector>

namespace Curvz {

struct BaselineLayout;
struct TextLayout;
namespace style { class TextStyleLibrary; }

// ── PatternGlyph ─────────────────────────────────────────────────────────────
// One placed glyph yielded by pattern_glyph_walk. The renderer inks it; the
// outliner FreeType-decomposes it. Hoisted out of Canvas (was Canvas::Pattern-
// Glyph) so the free walk and both consumers share one definition; Canvas.hpp
// keeps `using PatternGlyph = Curvz::PatternGlyph;` for source compatibility.
//
// Frame contract (matches show_glyph_string): the renderer's frame is
// translate(pos), rotate(angle), pen at (-adv/2, pen_y). show_glyph_string
// applies the glyph geometry x/y offsets on top of the pen, so an OUTLINE
// consumer must add geometry.x_offset / y_offset itself:
//   local = (fx + x_off - adv/2,  pen_y + y_off - fy)   for FT point (fx, fy)
// then rotate by angle about pos.
struct PatternGlyph {
    PangoGlyphInfo *info = nullptr;  // glyph id + geometry (advance, offsets)
    PangoFont      *font = nullptr;  // run font (resolved family/size/weight)
    Vec2            pos;             // doc point on the walk path at glyph centre
    double          angle  = 0.0;    // tangent rotation, flip's pi applied
    double          adv_px = 0.0;    // advance incl. layout-baked letter spacing
    double          pen_y  = 0.0;    // perpendicular pen drop in the rotated
                                     // frame: bl.y + attach perp (flip->ascent) - rise
    bool            has_fg = false;  // a foreground span covers this run
    double fg_r = 0, fg_g = 0, fg_b = 0, fg_a = 1;  // span colour if has_fg
};

// ── The pattern-text glyph walk (free) ───────────────────────────────────────
// Walks `text_obj` along its guide path, yielding one PatternGlyph per placed
// glyph. `lib` is the project's text-style library (may be null). Returns false
// when nothing can place (no pattern baselines / degenerate walk) so callers
// may fall back. Was Canvas::pattern_glyph_walk; Canvas keeps a thin wrapper
// that passes project()->text_styles.
bool pattern_glyph_walk(const SceneNode &text_obj, const SceneNode &guide,
                        const style::TextStyleLibrary *lib,
                        const std::function<void(const PatternGlyph &)> &fn);

// ── Box-flow text outline ────────────────────────────────────────────────────
// Lay `text` into `boundary` (starting at byte_start in the flow), then append
// one outline SceneNode per glyph to `out` — a bare Path for single-contour
// glyphs, a Compound for glyphs with 2+ contours (counters fill even-odd).
// `fill`/`stroke` ride every node (per-run foreground-span colour for box text
// stays a deferred follow-up — same as the emit_baseline_glyph_nodes it
// replaces). `fallback_px` seeds FT sizing when the resolved run pattern
// reports no pixel size. Inits/tears down its own FT_Library. No-op when the
// layout is empty. Faithful to the original: convert-to-path output is
// unchanged.
void outline_box_text(const SceneNode *boundary, const SceneNode *text,
                      size_t byte_start, const style::TextStyleLibrary *lib,
                      const FillStyle &fill, const StrokeStyle &stroke,
                      double fallback_px,
                      std::vector<std::unique_ptr<SceneNode>> &out);

// ── Pattern (text-on-path) outline ───────────────────────────────────────────
// Walk `text_obj` along `guide` and return colour-bucketed outline Compounds:
// one Compound for the uniform-colour case, one Compound per foreground-span
// colour for the styled case (each Compound paints even-odd with its OWN fill;
// child fills inert -- S58g). Fills/stroke/opacity are seeded from text_obj;
// the caller names the nodes and decides the wrapper (the convert verb wraps
// multi-bucket in a Group; SvgWriter wraps everything in a compat <g>). Empty
// vector when nothing places. Inits/tears down its own FT_Library.
std::vector<std::unique_ptr<SceneNode>>
outline_pattern_text(const SceneNode &text_obj, const SceneNode &guide,
                     const style::TextStyleLibrary *lib);

} // namespace Curvz
