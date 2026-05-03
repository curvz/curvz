#pragma once
#include "Vec2.hpp"
#include <utility>
#include <vector>

namespace Curvz {

// A single cubic bezier segment: p0 (anchor) → p1 (handle-out) → p2 (handle-in) → p3 (anchor)
struct CubicSegment {
    Vec2 p0, p1, p2, p3;

    // Point on curve at parameter t ∈ [0,1] — De Casteljau
    Vec2 at(double t) const;

    // Tangent (first derivative) at t — not normalised
    Vec2 tangent(double t) const;

    // Normal (perpendicular to tangent) at t — not normalised
    Vec2 normal(double t) const;

    // Nearest t on this segment to point pt (Newton-Raphson, ~10 iters)
    double nearest_t(Vec2 pt) const;

    // Distance from pt to nearest point on segment
    double dist_to(Vec2 pt) const;

    // Split into two segments at t
    std::pair<CubicSegment, CubicSegment> split(double t) const;

    // Approximate arc length via Gaussian quadrature
    double length(int subdivisions = 16) const;

    // Axis-aligned bounding box
    void bbox(Vec2& out_min, Vec2& out_max) const;

    // Find curve-curve intersection t values (recursive subdivision)
    // Returns pairs of (t_this, t_other)
    std::vector<std::pair<double,double>> intersect(
        const CubicSegment& other,
        double epsilon = 1e-5,
        int    max_depth = 16) const;
};

} // namespace Curvz
