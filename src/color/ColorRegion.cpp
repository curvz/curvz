//
// ColorRegion.cpp — bucket-lookup region naming for sRGB colours.
//
// See ColorRegion.hpp for purpose and consumer overview.
//
// Bucket boundaries (tunable — change here, don't sprinkle thresholds
// elsewhere). Validated against a 20-case test suite covering pure
// chromatic, light/dark variants, earth tones, and greyscale; see
// /tmp/region_test.cpp in the dev environment for the harness shape.
//
//   Saturation bands (S in [0,1] from RGB→HSL conversion):
//     S < 0.10                    → greyscale (no chromatic family)
//     0.10 <= S < 0.30            → "Pale" modifier (chromatic only,
//                                    AND only when no lightness modifier
//                                    applies — see suppression below)
//     0.30 <= S < 0.70            → no saturation modifier
//     S >= 0.70                   → "Vivid" modifier (suppressed if
//                                    "Dark" applies — see below)
//
//   Lightness bands (L in [0,1]):
//     L < 0.10                    → "Black" (greyscale)
//     0.10 <= L < 0.25            → "Dark Grey" (greyscale) /
//                                    "Dark" modifier (chromatic)
//     0.25 <= L < 0.75            → "Grey" / no lightness modifier
//     0.75 <= L < 0.90            → "Light Grey" / "Light" modifier
//     L >= 0.90                   → "White" / "Light" modifier
//
//   Modifier suppression rules:
//     * "Vivid" is suppressed when "Dark" applies. Dark colours read
//       as implicitly saturated; "Dark Vivid Blue" is awkward redundancy.
//     * "Pale" is suppressed when "Light" applies. Light colours read
//       as implicitly pale; "Light Pale Blue" reads stilted.
//     * Net effect: chromatic names are 1, 2, or 3 words. The 4-word
//       cap reserved in the API doc is for future hue families with
//       hyphenated names (e.g. "Yellow-Green").
//
//   Hue families (H in degrees, [0, 360)):
//     [345, 360) U [0, 15)   → Red
//     [15, 45)               → Orange
//     [45, 70)               → Yellow
//     [70, 165)              → Green
//     [165, 195)             → Cyan
//     [195, 250)             → Blue
//     [250, 295)             → Purple
//     [295, 345)             → Pink
//
//   Neutral / earth-tone override (the "browns"). Two paths into the
//   neutral bucket — the differentiator between "vivid orange" and
//   "saddle brown" is "warm-hue AND (desaturated OR very dark)":
//     Path A — desaturated warm at low/mid lightness:
//        hue in earth range AND 0.10 <= S < 0.55 AND L < 0.70
//     Path B — any warm at very low lightness (catches saturated
//        dark warms like saddle brown which read as brown despite
//        high S):
//        hue in earth range AND L < 0.40
//     Vivid mid-lightness warm (#FF8000, #FF0000) fails both paths
//     and lands in the chromatic bucket.
//   Once routed neutral, name by L band:
//        L <= 0.22         → "Dark Brown"
//        0.22 < L < 0.50   → "Brown"
//        0.50 <= L < 0.70  → "Light Brown"
//
//   Beige override (very light earth, low-mid saturation):
//     hue in earth range AND L >= 0.85 AND S < 0.65
//     → "Beige". Catches cream/eggshell/parchment without sweeping
//     in pale-yellow proper (which is more saturated).
//

#include "color/ColorRegion.hpp"
#include <algorithm>
#include <cmath>

namespace Curvz {
namespace color {

namespace {

// sRGB → HSL conversion. Standard formula; H in degrees [0, 360),
// S and L in [0, 1]. Greyscale (R==G==B) returns H=0, S=0, L=mid.
struct Hsl { double h; double s; double l; };

Hsl rgb_to_hsl(double r, double g, double b) {
    r = std::clamp(r, 0.0, 1.0);
    g = std::clamp(g, 0.0, 1.0);
    b = std::clamp(b, 0.0, 1.0);
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double l  = (mx + mn) * 0.5;
    double h = 0.0;
    double s = 0.0;
    const double d = mx - mn;
    if (d > 1e-9) {
        s = (l > 0.5) ? (d / (2.0 - mx - mn)) : (d / (mx + mn));
        if      (mx == r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
        else if (mx == g) h = (b - r) / d + 2.0;
        else              h = (r - g) / d + 4.0;
        h *= 60.0;
    }
    return { h, s, l };
}

// Hue family classifier. Returns the chromatic family name for a hue
// in degrees. Boundaries match the table at the top of this file.
const char* hue_family(double h) {
    // Wrap into [0, 360)
    if (h < 0.0)   h += 360.0;
    if (h >= 360.0) h -= 360.0;
    if (h >= 345.0 || h < 15.0)  return "Red";
    if (h < 45.0)                return "Orange";
    if (h < 70.0)                return "Yellow";
    if (h < 165.0)               return "Green";
    if (h < 195.0)               return "Cyan";
    if (h < 250.0)               return "Blue";
    if (h < 295.0)               return "Purple";
    return "Pink";  // [295, 345)
}

// Returns true if the (H, S, L) triple lands in the neutral / earth-tone
// region (the "browns"). The differentiator vs vivid warm colours is
// "warm-hue AND (desaturated-low-mid OR very dark)":
//   * Desaturated warm at any reasonable lightness → brown family
//     (#D2B48C tan, #8B7355 burlywood-shade)
//   * Dark warm regardless of saturation → brown family
//     (#8B4513 saddle brown is at S=0.76 — saturated, but dark enough
//     to read brown rather than orange)
// Vivid mid-lightness warm (#FF8000 orange, #FF0000 red) lands in
// chromatic — both branches reject it.
bool is_neutral_earth(double h, double s, double l) {
    // Hue in red/orange/yellow range — wrap-aware for red.
    const bool earth_hue =
        (h >= 345.0) || (h < 70.0);
    if (!earth_hue) return false;
    // Path A: desaturated warm at low/mid lightness.
    if (s >= 0.10 && s < 0.55 && l < 0.70) return true;
    // Path B: any warm at very low lightness — saturated dark warms
    // (saddle brown, mahogany, chocolate) read as browns despite high S.
    if (l < 0.40) return true;
    return false;
}

} // anonymous namespace

std::string region_name(double r, double g, double b) {
    const Hsl hsl = rgb_to_hsl(r, g, b);
    const double h = hsl.h;
    const double s = hsl.s;
    const double l = hsl.l;

    // ── Greyscale ─────────────────────────────────────────────────
    // Very low saturation — no chromatic family applies; name by L
    // band alone.
    if (s < 0.10) {
        if (l < 0.10) return "Black";
        if (l < 0.25) return "Dark Grey";
        if (l < 0.75) return "Grey";
        if (l < 0.90) return "Light Grey";
        return "White";
    }

    // ── Beige (very light earth tones) ────────────────────────────
    // Cream / eggshell / parchment land here. Treated as its own
    // single-word name rather than composing as "Light Pale Yellow".
    // The S<0.65 cutoff catches #F5F5DC (s≈0.56) without sweeping
    // in pale-yellow proper.
    {
        const bool earth_hue = (h >= 345.0) || (h < 70.0);
        if (earth_hue && l >= 0.85 && s < 0.65) {
            return "Beige";
        }
    }

    // ── Neutral / brown bucket ────────────────────────────────────
    // Earth-hue + moderate-low saturation + sub-mid lightness. Names
    // are self-contained (no further modifiers). The "Dark Brown"
    // band uses <= rather than < so that #4D3319 (l=0.20 exactly)
    // lands as Dark Brown rather than slipping into plain Brown.
    if (is_neutral_earth(h, s, l)) {
        if (l <= 0.22)  return "Dark Brown";
        if (l < 0.50)   return "Brown";
        return "Light Brown";  // 0.50 <= l < 0.70
    }

    // ── Chromatic ─────────────────────────────────────────────────
    // Compose: [Light/Dark] [Pale/Vivid] <Hue>. Cap at 4 words by
    // dropping saturation if both modifiers would apply alongside
    // a multi-word family — but our families are all single words,
    // so this only ever produces 1, 2, or 3 words. The 4-word cap
    // is reserved for future hue families (e.g. "Yellow-Green").
    const char* family = hue_family(h);

    // Lightness modifier
    const char* light_mod = nullptr;
    if      (l < 0.25)        light_mod = "Dark";
    else if (l >= 0.75)       light_mod = "Light";

    // Saturation modifier
    // "Vivid" is suppressed when "Dark" applies — dark colours are
    // implicitly saturated; "Dark Vivid Blue" reads as awkward
    // redundancy. Same for "Light Pale" — light colours are usually
    // pale, and the double-modifier reads stilted.
    const char* sat_mod = nullptr;
    if      (s < 0.30 && !light_mod) sat_mod = "Pale";
    else if (s >= 0.70 && !light_mod) sat_mod = "Vivid";

    std::string out;
    if (light_mod) { out += light_mod; out += ' '; }
    if (sat_mod)   { out += sat_mod;   out += ' '; }
    out += family;
    return out;
}

std::string region_name(const Color& c) {
    return region_name(c.r, c.g, c.b);
}

} // namespace color
} // namespace Curvz
