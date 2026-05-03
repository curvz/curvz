//
// Color.cpp — implementation of hex round-trip.
//
// Parsing is deliberately strict: a leading '#' is required, and only the
// four accepted lengths (3, 4, 6, 8 hex digits) are honored. Anything else
// returns std::nullopt. Don't be clever about recovery — callers need to
// know when input is malformed.
//

#include "color/Color.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Curvz {
namespace color {

namespace {

// Parse a single hex char into its 0–15 value. Returns -1 on non-hex input.
inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Parse two consecutive hex chars as an 8-bit value.
// Returns -1 if either char is not hex.
inline int hex_pair(char hi, char lo) {
    int h = hex_nibble(hi);
    int l = hex_nibble(lo);
    if (h < 0 || l < 0) return -1;
    return (h << 4) | l;
}

} // anonymous namespace

std::optional<Color> from_hex(const std::string& s) {
    // Must start with '#'; strip it.
    if (s.empty() || s[0] != '#') return std::nullopt;

    const std::size_t n = s.size() - 1;
    const char* p = s.data() + 1;

    // 3 or 4 char form: expand each nibble by doubling (R -> RR).
    // This is the standard CSS short-hex rule; "#F0A" == "#FF00AA".
    if (n == 3 || n == 4) {
        int vals[4] = { 0, 0, 0, 15 };  // default alpha nibble = F
        for (std::size_t i = 0; i < n; ++i) {
            int v = hex_nibble(p[i]);
            if (v < 0) return std::nullopt;
            vals[i] = v;
        }
        Color c;
        c.r = channel_from_u8(static_cast<std::uint8_t>((vals[0] << 4) | vals[0]));
        c.g = channel_from_u8(static_cast<std::uint8_t>((vals[1] << 4) | vals[1]));
        c.b = channel_from_u8(static_cast<std::uint8_t>((vals[2] << 4) | vals[2]));
        c.a = channel_from_u8(static_cast<std::uint8_t>((vals[3] << 4) | vals[3]));
        return c;
    }

    // 6 or 8 char form: two hex digits per channel.
    if (n == 6 || n == 8) {
        int r = hex_pair(p[0], p[1]);
        int g = hex_pair(p[2], p[3]);
        int b = hex_pair(p[4], p[5]);
        if (r < 0 || g < 0 || b < 0) return std::nullopt;

        int a = 255;
        if (n == 8) {
            a = hex_pair(p[6], p[7]);
            if (a < 0) return std::nullopt;
        }

        Color c;
        c.r = channel_from_u8(static_cast<std::uint8_t>(r));
        c.g = channel_from_u8(static_cast<std::uint8_t>(g));
        c.b = channel_from_u8(static_cast<std::uint8_t>(b));
        c.a = channel_from_u8(static_cast<std::uint8_t>(a));
        return c;
    }

    return std::nullopt;
}

std::string to_hex(const Color& c) {
    std::uint8_t r = channel_to_u8(c.r);
    std::uint8_t g = channel_to_u8(c.g);
    std::uint8_t b = channel_to_u8(c.b);
    std::uint8_t a = channel_to_u8(c.a);

    char buf[10];  // "#RRGGBBAA\0" = 10 bytes
    if (a == 255) {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
    } else {
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", r, g, b, a);
    }
    return std::string(buf);
}

// =========================================================================
// HSL conversions
// =========================================================================
//
// Standard CSS HSL algorithm. The usual reference is the CSS Color Module
// Level 3 spec; the math is also in any graphics textbook. Hue is degrees,
// sat/light are [0, 1]. For achromatic input (max == min) we zero out hue
// and saturation — the hue is mathematically undefined there, and picking
// a canonical representative keeps round-tripping stable.
// =========================================================================

HSL to_hsl(const Color& c) {
    HSL out;
    out.a = c.a;

    const double mx = std::max({ c.r, c.g, c.b });
    const double mn = std::min({ c.r, c.g, c.b });
    const double d  = mx - mn;

    out.l = (mx + mn) * 0.5;

    if (d < 1e-12) {
        // Achromatic: hue and saturation are undefined. Canonicalize.
        out.h = 0.0;
        out.s = 0.0;
        return out;
    }

    // Saturation: the standard HSL formula, branching on lightness.
    out.s = (out.l > 0.5)
        ? d / (2.0 - mx - mn)
        : d / (mx + mn);

    // Hue, based on which channel is the max.
    double h;
    if (mx == c.r) {
        h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    } else if (mx == c.g) {
        h = (c.b - c.r) / d + 2.0;
    } else {
        h = (c.r - c.g) / d + 4.0;
    }
    out.h = h * 60.0;  // convert 0..6 to degrees

    // Normalize h to [0, 360). std::fmod can return negative; guard against it.
    if (out.h < 0.0)   out.h += 360.0;
    if (out.h >= 360.0) out.h -= 360.0;

    return out;
}

namespace {
    // HSL helper: map a hue-adjusted position to an RGB channel value.
    // Standard CSS formulation.
    inline double hue_to_rgb(double p, double q, double t) {
        if (t < 0.0) t += 1.0;
        if (t > 1.0) t -= 1.0;
        if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
        if (t < 1.0 / 2.0) return q;
        if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
        return p;
    }
}

Color from_hsl(const HSL& hsl) {
    Color out;
    out.a = hsl.a;

    if (hsl.s < 1e-12) {
        // Achromatic: all three channels equal lightness.
        out.r = out.g = out.b = hsl.l;
        return out;
    }

    const double l = hsl.l;
    const double q = (l < 0.5) ? l * (1.0 + hsl.s) : l + hsl.s - l * hsl.s;
    const double p = 2.0 * l - q;
    const double h = hsl.h / 360.0;  // back to [0, 1] for the helper

    out.r = hue_to_rgb(p, q, h + 1.0 / 3.0);
    out.g = hue_to_rgb(p, q, h);
    out.b = hue_to_rgb(p, q, h - 1.0 / 3.0);
    return out;
}

// =========================================================================
// OKLCH conversions
// =========================================================================
//
// Pipeline:
//   sRGB (gamma-encoded)  <->  linear sRGB  <->  LMS  <->  Lab (oklab)  <->  LCh
//
// Coefficients from Björn Ottosson's reference post, "A perceptual color
// space for image processing" (2020). The Lab variant is specifically
// Ottosson's "oklab"; LCh is just polar form. See
// https://bottosson.github.io/posts/oklab/ for the derivation.
//
// The three cube-root / cube operations in the LMS step are the nonlinearity
// that makes the space perceptually uniform.
// =========================================================================

namespace {

// sRGB companding: piecewise, matches IEC 61966-2-1 exactly (no approximation).
inline double srgb_to_linear(double c) {
    if (c <= 0.04045) return c / 12.92;
    return std::pow((c + 0.055) / 1.055, 2.4);
}

inline double linear_to_srgb(double c) {
    if (c <= 0.0031308) return 12.92 * c;
    return 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

inline double clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

} // anonymous namespace

OKLCH to_oklch(const Color& c) {
    OKLCH out;
    out.a = c.a;

    // sRGB -> linear
    const double lr = srgb_to_linear(c.r);
    const double lg = srgb_to_linear(c.g);
    const double lb = srgb_to_linear(c.b);

    // linear RGB -> LMS (Ottosson's M1 matrix)
    const double l_ = 0.4122214708 * lr + 0.5363325363 * lg + 0.0514459929 * lb;
    const double m_ = 0.2119034982 * lr + 0.6806995451 * lg + 0.1073969566 * lb;
    const double s_ = 0.0883024619 * lr + 0.2817188376 * lg + 0.6299787005 * lb;

    // nonlinearity: cube root
    const double l = std::cbrt(l_);
    const double m = std::cbrt(m_);
    const double s = std::cbrt(s_);

    // LMS -> oklab (Ottosson's M2 matrix)
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double a_lab = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double b_lab = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;

    // oklab -> OKLCH (polar)
    out.l = L;
    out.c = std::sqrt(a_lab * a_lab + b_lab * b_lab);

    if (out.c < 1e-7) {
        // Achromatic in Lab space: hue is undefined. Canonicalize to 0.
        // Threshold is generous (1e-7) to absorb numerical noise from the
        // cbrt / matrix pipeline — grey sRGB through to_oklch yields
        // C ~ 2e-8 on this hardware, well above the tightest possible float
        // epsilon but well below any perceptible chroma (JND ~ 2e-3).
        out.h = 0.0;
    } else {
        double h = std::atan2(b_lab, a_lab) * 180.0 / M_PI;
        if (h < 0.0) h += 360.0;
        out.h = h;
    }

    return out;
}

Color from_oklch(const OKLCH& lch) {
    Color out;
    out.a = lch.a;

    // OKLCH -> oklab
    const double h_rad = lch.h * M_PI / 180.0;
    const double a_lab = lch.c * std::cos(h_rad);
    const double b_lab = lch.c * std::sin(h_rad);
    const double L     = lch.l;

    // oklab -> LMS (inverse of M2)
    const double l = L + 0.3963377774 * a_lab + 0.2158037573 * b_lab;
    const double m = L - 0.1055613458 * a_lab - 0.0638541728 * b_lab;
    const double s = L - 0.0894841775 * a_lab - 1.2914855480 * b_lab;

    // inverse nonlinearity: cube
    const double l_ = l * l * l;
    const double m_ = m * m * m;
    const double s_ = s * s * s;

    // LMS -> linear RGB (inverse of M1)
    const double lr =  4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_;
    const double lg = -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_;
    const double lb = -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_;

    // linear -> sRGB, then clamp to gamut.
    // See header note: OKLCH can represent out-of-gamut colors. We clamp.
    // Chroma-reduction gamut mapping is a Phase 7 concern.
    out.r = clamp01(linear_to_srgb(lr));
    out.g = clamp01(linear_to_srgb(lg));
    out.b = clamp01(linear_to_srgb(lb));
    return out;
}

} // namespace color
} // namespace Curvz
