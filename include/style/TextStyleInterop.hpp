#pragma once
//
// TextStyleInterop.hpp — the node <-> text-style bridge, sibling of StyleInterop
// (which bridges SceneNode and the graphic StyleLibrary). One function here: the
// per-paragraph BASELINE the fitter consults, under the paragraph's local spans.
//
// ── Option A (the resolve-timing decision, s340) ─────────────────────────────
//
// The fitter does NOT stamp resolved style values into the span bag and it does
// NOT hold the library. Instead, per paragraph, it asks for the resolved
// baseline (this function), then layers the paragraph's own local spans on top —
// design text_formatting_design.md sec.7's three-tier resolve:
//
//     box / cascade floor  ->  bound style (resolve up the parent chain)  ->  local spans
//     \_________________  resolve_paragraph_baseline  _________________/      \__ fitter __/
//
// Keeping style-derived values OUT of the span bag means the bag means exactly
// one thing (user direct formatting), and editing a style cascades for free:
// nothing was stamped, so the next fit re-asks and picks up the new resolved
// values. The cost Option A pays — the fitter needing a resolved baseline — is
// this one small, library-touching call, hoisted to the per-paragraph level
// (not per glyph/line-internal), and cheap (resolve() walks a short parent
// chain). A per-fit cache keyed by style id is the natural optimisation if the
// handful of distinct ids per box ever shows up in a profile; the API doesn't
// change.
//
// ── Decoupled on purpose ─────────────────────────────────────────────────────
//
// Takes the raw inputs (spans + byte + library + a box baseline) rather than a
// SceneNode&, so it composes the existing curvz::utils paragraph-span reader and
// the library resolve without pulling SceneNode/GTK into the style layer, and
// stays unit-testable. The fitter's caller builds the box baseline from the
// node's loose text scalars via box_baseline() at the layout-entry site.
//
// ── Scope (this milestone) — pure addition ───────────────────────────────────
// The seam only. No fitter call site yet (the layout functions in TextCursor
// read the node scalars directly until the wiring milestone points them here).
//

#include "style/TextStyle.hpp"
#include "style/TextStyleLibrary.hpp"
#include "curvz_utils.hpp"   // Curvz::AttrSpan, paragraph_attr_svalue_for_byte, kCurvzStyleAttr

#include <string>
#include <vector>

namespace Curvz {
namespace style {

// Build the box-level baseline — the concrete face of "no style assigned": a
// paragraph with no style binding falls back to the text box's loose scalars
// (which are themselves the box default / cascade floor). Indents and tabs have
// no box-level scalar (they are span-only), so they stay at the ResolvedTextStyle
// defaults (0 / empty); colour likewise stays at the floor here — the fitter
// continues to source glyph colour from the node fill / foreground spans until
// the wiring milestone folds colour through this baseline. leading_px == 0 means
// "derive from metrics" (the node's text_line_height == 0 convention passes
// straight through). align comes from the node's text_align string.
ResolvedTextStyle box_baseline(const std::string& family, double size,
                               bool bold, bool italic, double letter_spacing,
                               double leading_px, ParaAlign align);

// The per-paragraph baseline the fitter consults UNDER the local spans. Reads
// the kCurvzStyleAttr binding covering `para_start_byte`: empty (unbound) ->
// return `box`; otherwise -> the library's resolved style for that id (which
// itself bottoms at the hardcoded floor on an unknown / dangling id, so this is
// total). The caller then applies the paragraph's own align/leading/indent/tabs
// and per-run char spans on top of the returned baseline.
ResolvedTextStyle resolve_paragraph_baseline(const std::vector<Curvz::AttrSpan>& spans,
                                             unsigned para_start_byte,
                                             const TextStyleLibrary& lib,
                                             const ResolvedTextStyle& box);

} // namespace style
} // namespace Curvz
