#pragma once
#include "math/Vec2.hpp"
#include <cmath>

// ── CoordSpace — coordinate system translator ─────────────────────────────────
//
// Curvz stores all geometry in Cairo/SVG native space:
//   origin = top-left of artboard
//   Y increases downward
//
// Users work in design space:
//   origin = bottom-left of artboard
//   Y increases upward
//
// This struct is the ONLY place in the codebase where the Y-flip is performed.
// Every value crossing the internal↔user boundary must go through it.
//
// RULE: the expression (canvas_height - y) must appear nowhere except here.
//       If you see it elsewhere it is a bug.
//
// Usage:
//   CoordSpace cs{m_doc->canvas_height()};
//
//   // Display a stored node coordinate in the status bar / inspector:
//   double display_y = cs.to_user_y(node.y);
//
//   // Accept a value typed by the user into an inspector field:
//   node.y = cs.to_doc_y(user_input_y);
//
// X is unchanged — only Y flips.
// Angles flip sign: doc_angle = -user_angle (because Y axis is inverted).
// Deltas (dx, dy) flip Y sign the same way as absolute coordinates.
//
// ─────────────────────────────────────────────────────────────────────────────

namespace Curvz {

struct CoordSpace {
    double canvas_h = 1.0;  // canvas_height() from CurvzDocument — must be > 0

    explicit CoordSpace(double canvas_height) : canvas_h(canvas_height) {}

    // ── Scalar Y ─────────────────────────────────────────────────────────────

    // Internal (Y-down) → user display (Y-up)
    double to_user_y(double doc_y)  const { return canvas_h - doc_y; }

    // User input (Y-up) → internal storage (Y-down)
    double to_doc_y(double user_y)  const { return canvas_h - user_y; }

    // X is a pass-through — provided for symmetry so call sites are uniform
    double to_user_x(double doc_x)  const { return doc_x; }
    double to_doc_x(double user_x)  const { return user_x; }

    // ── Point ─────────────────────────────────────────────────────────────────

    Vec2 to_user(double doc_x, double doc_y)  const { return {doc_x, canvas_h - doc_y}; }
    Vec2 to_user(Vec2 doc)                    const { return {doc.x, canvas_h - doc.y}; }

    Vec2 to_doc(double user_x, double user_y) const { return {user_x, canvas_h - user_y}; }
    Vec2 to_doc(Vec2 user)                    const { return {user.x, canvas_h - user.y}; }

    // ── Delta (relative movement) ─────────────────────────────────────────────
    // A delta in Y-down space has the opposite sign in Y-up space.
    // Use these when translating drag deltas or offset values, not absolute positions.

    double to_user_dy(double doc_dy)  const { return -doc_dy; }
    double to_doc_dy(double user_dy)  const { return -user_dy; }

    Vec2 to_user_delta(double ddx, double ddy) const { return {ddx, -ddy}; }
    Vec2 to_doc_delta(double udx, double udy)  const { return {udx, -udy}; }

    // ── Angle ─────────────────────────────────────────────────────────────────
    // In Y-down space: 0° = right, angles increase clockwise.
    // In Y-up  space: 0° = right, angles increase counter-clockwise (standard math).
    // Flip: user_angle = -doc_angle  (in radians or degrees — same formula)

    double to_user_angle_deg(double doc_deg)  const { return -doc_deg; }
    double to_doc_angle_deg(double user_deg)  const { return -user_deg; }

    double to_user_angle_rad(double doc_rad)  const { return -doc_rad; }
    double to_doc_angle_rad(double user_rad)  const { return -user_rad; }

    // ── Width / Height ────────────────────────────────────────────────────────
    // Widths and heights are always positive magnitudes — no flip needed.
    // Provided as explicit no-ops so call sites are self-documenting.

    double to_user_w(double doc_w) const { return doc_w; }
    double to_user_h(double doc_h) const { return doc_h; }
    double to_doc_w(double user_w) const { return user_w; }
    double to_doc_h(double user_h) const { return user_h; }
};

} // namespace Curvz
