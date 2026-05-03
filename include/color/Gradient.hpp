#pragma once
//
// Gradient.hpp — gradient paint data, in color:: namespace.
//
// S93 m1: color::Paint grows a Gradient arm. Existing gradient state lived
// only on Curvz::FillStyle (path-side); the Style System's color::Paint
// variant could only hold None/CurrentColor/Solid/SwatchRef and degraded
// gradients to "first stop's flat colour" via FillStyleInterop. That was
// the documented Phase-5-follow-on placeholder ("Paint doesn't model
// gradients yet"). This header is the canonical home of the new model.
//
// Why a sibling color::GradientStop type rather than reusing Curvz::
// GradientStop:
//   color/ is intentionally dependency-light — Paint.hpp only includes
//   Color.hpp. Reaching into SceneNode.hpp (where Curvz::GradientStop
//   lives) would pull the whole scene-graph header into every Paint
//   includer. The interop bridge in FillStyleInterop already exists for
//   exactly this kind of cross-namespace conversion; we add stop and
//   geometry conversions there.
//
// Coordinate convention:
//   Geometry is in **objectBoundingBox-fraction** space (0..1 of the
//   shape's bbox), matching SVG gradientUnits="objectBoundingBox" and
//   the existing Curvz::FillStyle convention. Renderers lerp into doc-
//   pixel space using the shape's actual bbox at draw time.
//
// LinearGradient: g_x1,g_y1 → g_x2,g_y2 are the gradient line endpoints.
// RadialGradient: g_x1,g_y1 are the focal point (fx,fy); g_x2,g_y2 are
// the outer-circle centre (cx,cy); g_r is the outer-circle radius (also
// a 0..1 fraction of the bbox's larger dimension).
//

#include "color/Color.hpp"

#include <vector>

namespace Curvz {
namespace color {

// One colour-stop on a gradient. Sibling of Curvz::GradientStop, but
// stores Color (rather than bare r/g/b/a) — matches the rest of color/.
struct GradientStop {
    double offset = 0.0;  // 0..1
    Color  color;

    GradientStop() = default;
    GradientStop(double o, const Color& c) : offset(o), color(c) {}
};

inline bool operator==(const GradientStop& a, const GradientStop& b) {
    // Channel equality is defined at 8-bit granularity by Color::operator==,
    // which is what we want here too — two stops at the same offset with
    // colours that round to the same wire bytes are equal.
    return a.offset == b.offset && a.color == b.color;
}
inline bool operator!=(const GradientStop& a, const GradientStop& b) {
    return !(a == b);
}

// Gradient paint. Linear or radial; stops + geometry. Does not include
// None/Solid/etc — those are siblings in the Paint variant. This is the
// "I am a gradient" leaf only.
struct Gradient {
    enum class Kind { Linear, Radial };
    Kind kind = Kind::Linear;

    std::vector<GradientStop> stops;

    // Geometry in bbox-fraction space (see file header).
    double g_x1 = 0.0, g_y1 = 0.5;  // linear: start;  radial: fx,fy
    double g_x2 = 1.0, g_y2 = 0.5;  // linear: end;    radial: cx,cy
    double g_r  = 0.5;               // radial only:    outer radius

    Gradient() = default;
};

inline bool operator==(const Gradient& a, const Gradient& b) {
    return a.kind == b.kind &&
           a.stops == b.stops &&
           a.g_x1 == b.g_x1 && a.g_y1 == b.g_y1 &&
           a.g_x2 == b.g_x2 && a.g_y2 == b.g_y2 &&
           a.g_r  == b.g_r;
}
inline bool operator!=(const Gradient& a, const Gradient& b) {
    return !(a == b);
}

} // namespace color
} // namespace Curvz
