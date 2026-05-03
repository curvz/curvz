#pragma once
#include <cmath>

namespace Curvz {

struct Vec2 {
    double x = 0, y = 0;

    Vec2() = default;
    Vec2(double x, double y) : x(x), y(y) {}

    Vec2  operator+(const Vec2& o) const { return {x+o.x, y+o.y}; }
    Vec2  operator-(const Vec2& o) const { return {x-o.x, y-o.y}; }
    Vec2  operator*(double t)      const { return {x*t,   y*t};   }
    Vec2  operator/(double t)      const { return {x/t,   y/t};   }
    Vec2& operator+=(const Vec2& o)      { x+=o.x; y+=o.y; return *this; }
    Vec2& operator-=(const Vec2& o)      { x-=o.x; y-=o.y; return *this; }

    double dot(const Vec2& o)   const { return x*o.x + y*o.y; }
    double cross(const Vec2& o) const { return x*o.y - y*o.x; }
    double length()             const { return std::sqrt(x*x + y*y); }
    double length_sq()          const { return x*x + y*y; }
    double dist(const Vec2& o)  const { return (*this - o).length(); }
    double dist_sq(const Vec2& o) const { return (*this - o).length_sq(); }

    Vec2 normalised() const {
        double len = length();
        return len > 1e-12 ? Vec2{x/len, y/len} : Vec2{0,0};
    }

    Vec2 perp() const { return {-y, x}; } // 90° CCW
};

inline Vec2 lerp(Vec2 a, Vec2 b, double t) {
    return a + (b - a) * t;
}

} // namespace Curvz
