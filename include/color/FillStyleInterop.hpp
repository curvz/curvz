#pragma once
//
// FillStyleInterop.hpp — bridge between the new color::Paint variant and the
// legacy FillStyle tagged struct.
//
// Phase 1 / M3. During the migration (Phases 1–9), Paint and FillStyle coexist.
// New code paths use Paint; existing renderers continue to read FillStyle
// until each is migrated (one per milestone, starting Phase 2).
//
// This bridge is deliberately one-way-safe: converting a FillStyle to Paint
// is lossless for all three FillStyle states (None, CurrentColor, Solid).
// Converting a Paint back to FillStyle is lossless for those three, and
// resolves SwatchRef via the supplied SwatchLibrary (or falls back to the
// ref's cached colour if no library is available or the id is dead).
//
// Phase 5 M1 update: SwatchRef now resolves through resolve_paint() to a
// concrete colour instead of degrading to CurrentColor. The library-aware
// overload is the preferred form for callers in the color_system; the
// library-less overload remains for renderers that don't have the project
// in scope.
//
// This header is NOT pure — it pulls SceneNode.hpp for FillStyle, which
// transitively pulls glib (for the UUID generator). Callers who need just
// the Paint atoms should include Paint.hpp instead. That separation keeps
// the math / generator layer free of GTK dependencies.
//

#include "color/Paint.hpp"
#include "SceneNode.hpp"  // FillStyle

namespace Curvz {
namespace color {

class SwatchLibrary;  // forward decl for the library-aware overload below

// FillStyle -> Paint. Lossless for all three FillStyle states.
Paint to_paint(const FillStyle& fs);

// Paint -> FillStyle.
//
// Two overloads:
//
//   * to_fillstyle(p)        — no library. Lossless for None / CurrentColor
//                              / Solid. SwatchRef falls back to its cached
//                              colour as a Solid. Suitable for renderers
//                              that don't have the library in scope.
//
//   * to_fillstyle(p, lib)   — library-aware. Looks up SwatchRef ids
//                              against the supplied library, returning a
//                              Solid with the swatch's current colour.
//                              Falls back to the cached colour if the id
//                              is unknown. This is the preferred form for
//                              any caller that touches the color_system.
//
// Both forms are stable across swatch-kind additions: future gradient
// swatches that can't be represented as a single Solid still resolve to a
// fallback colour (to be chosen by the gradient-aware code in that phase).
FillStyle to_fillstyle(const Paint& p);
FillStyle to_fillstyle(const Paint& p, const SwatchLibrary& lib);

} // namespace color
} // namespace Curvz
