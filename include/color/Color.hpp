#pragma once
//
// Color.hpp — atomic color type for Curvz's color system.
//
// Design notes (see ARCHITECTURE.md "Color System"):
//
//   * Storage is sRGB floats in [0, 1]. This is the canonical internal form.
//     OKLCH and HSL live as conversion targets in the generators, not as
//     primary storage. Cairo + SVG both speak sRGB natively — converting at
//     every draw / serialize call is a cost we don't want.
//
//   * Alpha is a channel of Color, not a separate "opacity" field on the
//     object holding it. A "color with 50% alpha" is one concept, not two.
//     Object-level opacity remains available for compound paint (fill+stroke
//     together) but is not the path for simple transparent fills.
//
//   * Equality is at 8-bit hex granularity. Float tolerance is insufficient:
//     a color entered via Gdk::RGBA picker and a color parsed from hex will
//     not compare equal bit-for-bit even when they represent the same visible
//     result. Rounding to 0–255 int before comparing matches how the SVG /
//     hex world thinks of "the same color".
//
//   * This header is pure — no GTK, no Cairo, no glibmm. Gdk::RGBA interop
//     lives in a separate section at the bottom, guarded so the file can be
//     included from the math layer without pulling GTK.
//
// Phase 1 / M1. No callers yet. Interop with existing FillStyle lands in M3.
//

#include <cstdint>
#include <string>
#include <optional>

namespace Curvz {
namespace color {

struct Color {
    double r = 0.0;   // sRGB, 0–1
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;

    constexpr Color() = default;
    constexpr Color(double r, double g, double b, double a = 1.0)
        : r(r), g(g), b(b), a(a) {}

    // Named constructors for the common atoms. Kept out of the default
    // constructor list so `Color{}` stays transparent-black (the C++ default
    // for uninitialized doubles would give us that anyway, but spelling it out
    // makes intent clear).
    static constexpr Color black()       { return { 0.0, 0.0, 0.0, 1.0 }; }
    static constexpr Color white()       { return { 1.0, 1.0, 1.0, 1.0 }; }
    static constexpr Color transparent() { return { 0.0, 0.0, 0.0, 0.0 }; }
};

// --- Channel rounding ------------------------------------------------------
//
// Clamp a double channel into [0, 1] and round to an 8-bit integer. Used by
// equality, hex serialization, and any code that needs to cross the
// float ↔ wire boundary.
inline std::uint8_t channel_to_u8(double c) {
    if (c <= 0.0) return 0;
    if (c >= 1.0) return 255;
    // Round-to-nearest; +0.5 is fine because c is already in [0,1].
    return static_cast<std::uint8_t>(c * 255.0 + 0.5);
}

inline double channel_from_u8(std::uint8_t c) {
    return static_cast<double>(c) / 255.0;
}

// --- Equality at 8-bit granularity ----------------------------------------
//
// See the header comment on why this is the right equality for Color.
// Two Colors compare equal iff their RGBA channels round to the same
// 0–255 integers.

inline bool operator==(const Color& lhs, const Color& rhs) {
    return channel_to_u8(lhs.r) == channel_to_u8(rhs.r)
        && channel_to_u8(lhs.g) == channel_to_u8(rhs.g)
        && channel_to_u8(lhs.b) == channel_to_u8(rhs.b)
        && channel_to_u8(lhs.a) == channel_to_u8(rhs.a);
}

inline bool operator!=(const Color& lhs, const Color& rhs) {
    return !(lhs == rhs);
}

// --- Hex round-trip --------------------------------------------------------
//
// Accepted input formats:
//    "#RGB"       — 4 bit per channel, alpha = 1
//    "#RGBA"      — 4 bit per channel, including alpha
//    "#RRGGBB"    — 8 bit per channel, alpha = 1
//    "#RRGGBBAA"  — 8 bit per channel, including alpha
//
// The leading "#" is required (no bare "RRGGBB" — that catches typos that
// happen to look numeric). Whitespace is not trimmed; caller's responsibility.
//
// Returns std::nullopt on any malformed input. Does NOT throw.
std::optional<Color> from_hex(const std::string& s);

// Always emits "#RRGGBB" (alpha = 1.0) or "#RRGGBBAA" (alpha < 1.0).
// Uses lowercase hex for consistency with CSS convention.
std::string to_hex(const Color& c);

// --- HSL conversions -------------------------------------------------------
//
// Standard CSS/SVG HSL:
//   h ∈ [0, 360)  degrees, hue
//   s ∈ [0, 1]    saturation
//   l ∈ [0, 1]    lightness
//   a ∈ [0, 1]    alpha, passed through unchanged
//
// Round-trip is lossy: HSL is a non-injective mapping from RGB (achromatic
// colors have undefined hue; saturation is indeterminate at l=0 and l=1),
// so HSL(from_hsl(…to_hsl(c)…)) != c in general for achromatic c. We
// normalize to h=0, s=0 for achromatic inputs, so at least the result is
// stable under repeated round-tripping.

struct HSL {
    double h = 0.0;   // degrees, [0, 360)
    double s = 0.0;   // [0, 1]
    double l = 0.0;   // [0, 1]
    double a = 1.0;   // [0, 1]
};

HSL   to_hsl(const Color& c);
Color from_hsl(const HSL& hsl);

// --- OKLCH conversions -----------------------------------------------------
//
// OKLCH (Björn Ottosson, 2020) is a perceptually uniform polar color space.
// We use it for generators (ramps, harmonies) where RGB-interpolated output
// looks muddy in the mid-range. Internal storage remains sRGB; OKLCH is
// strictly a conversion target.
//
//   L ∈ [0, 1]         perceptual lightness
//   C ∈ [0, ~0.4]      chroma (distance from grey); in-gamut colors rarely
//                      exceed 0.37, but the math doesn't enforce a cap
//   h ∈ [0, 360)       hue in degrees
//   a ∈ [0, 1]         alpha, passed through unchanged
//
// Gamut: OKLCH is a larger space than sRGB, so round-tripping an arbitrary
// OKLCH value through from_oklch() may require clamping. This implementation
// clamps each sRGB channel to [0, 1] on return. Chroma-reducing gamut
// mapping is a generator-level concern (Phase 7), not the atom's job.

struct OKLCH {
    double l = 0.0;   // [0, 1]
    double c = 0.0;   // [0, ~0.4]
    double h = 0.0;   // degrees, [0, 360)
    double a = 1.0;   // [0, 1]
};

OKLCH to_oklch(const Color& c);
Color from_oklch(const OKLCH& oklch);

// True when the OKLCH triple lies inside the sRGB gamut -- i.e. from_oklch
// would return it without clamping any channel. Lets callers (a colour picker,
// say) find the in-gamut chroma edge at a given lightness+hue and present a
// fully-selectable spectrum, instead of a rectangle whose far corners all
// clamp to one flat dead colour.
bool oklch_in_gamut(const OKLCH& oklch);

} // namespace color
} // namespace Curvz
