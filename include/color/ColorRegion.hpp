#pragma once
//
// ColorRegion.hpp — derive a human-readable region name from a colour.
//
// Purpose
//   The colour system needs a deterministic way to label any sRGB colour with
//   a short, glanceable name like "Vivid Red", "Dark Blue", "Light Brown",
//   or "Grey". This is used as a fallback in UI when a swatch or style has
//   no user-supplied name, and (next step) as the seed for auto-naming
//   newly-created swatches/styles so the user-facing rule "every swatch
//   and style has a non-empty name" can be enforced at creation time.
//
// Design
//   Pure function, no state. Takes sRGB doubles in [0, 1] (matching
//   Color.hpp's storage convention) or a Color directly. Returns a
//   capitalised string suitable for direct UI display.
//
//   Algorithm: convert to HSL, classify by lightness / saturation / hue
//   into one of the named buckets below, compose modifiers with the hue
//   family. Bucket boundaries are documented at the top of ColorRegion.cpp
//   so they can be tuned without spelunking through the implementation.
//
//   Bucket families:
//     - Greyscale (very low saturation): Black / Dark Grey / Grey /
//       Light Grey / White, by lightness band.
//     - Neutral / earth (low-moderate saturation in red/orange/yellow
//       hue range, low-mid lightness): Beige / Light Brown / Brown /
//       Dark Brown. Absorbs tan, taupe, olive-leaning browns.
//     - Chromatic (everything else): Red / Orange / Yellow / Green /
//       Cyan / Blue / Purple / Pink, optionally prefixed with
//       "Dark"/"Light" (lightness band) and/or "Pale"/"Vivid"
//       (saturation band).
//
//   Composition cap: at most 4 words. If a composition would exceed
//   4 words, the saturation modifier is dropped before the lightness
//   modifier (lightness conveys more visually).
//
// Future consumers
//   * Inspector swatch-name fallback when swatch_header.name is empty
//     (S83 m4h v2 — first consumer).
//   * Auto-name-at-birth invariant for SwatchLibrary::add_swatch and
//     StyleLibrary::add_style (deferred — separate slice).
//   * Default-swatch authoring: organise blank-project default swatches
//     by region category (deferred — M6 polish phase).
//

#include "color/Color.hpp"
#include <string>

namespace Curvz {
namespace color {

// Returns a region name for a colour. Always returns a non-empty,
// capitalised string suitable for direct UI display. Alpha is ignored.
//
// Examples:
//   region_name(1.00, 0.00, 0.00) = "Vivid Red"
//   region_name(0.00, 0.20, 0.40) = "Dark Blue"
//   region_name(0.66, 0.78, 0.91) = "Light Pale Blue"
//   region_name(0.55, 0.27, 0.07) = "Brown"
//   region_name(0.96, 0.96, 0.86) = "Beige"
//   region_name(0.50, 0.50, 0.50) = "Grey"
//   region_name(0.00, 0.00, 0.00) = "Black"
//   region_name(1.00, 1.00, 1.00) = "White"
std::string region_name(double r, double g, double b);

// Convenience overload taking a Color directly. Alpha ignored.
std::string region_name(const Color& c);

} // namespace color
} // namespace Curvz
