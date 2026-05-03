#pragma once
#include <string>

namespace Curvz {

// ── Unit enum ─────────────────────────────────────────────────────────────────
enum class Unit { Px, In, Mm, Pt };

// ── Conversion constants (all relative to 96 px/inch) ─────────────────────────
//   1 in  = 96 px
//   1 mm  = 96 / 25.4  ≈ 3.779528 px
//   1 pt  = 96 / 72    ≈ 1.333333 px

struct UnitSystem {
    // Convert a value in px to display unit
    static double from_px(double px, Unit u);

    // Convert a value in display unit to px
    static double to_px(double v, Unit u);

    // Short label shown in UI  ("px", "in", "mm", "pt")
    static const char* label(Unit u);

    // Parse a unit suffix token — returns Unit::Px if unrecognised
    static Unit parse_unit(const std::string& tok);

    // Parse a full expression such as "4.25in + 16mm * 2" and return
    // the result in pixels.  Bare numbers are treated as `default_unit`.
    // Returns false if the expression is invalid (result unchanged).
    static bool parse_expr(const std::string& expr, Unit default_unit,
                           double& result_px);

    // Same, but on failure fills `err_msg` with a user-readable explanation.
    // Letter-set restricts which unit suffix letters are legal:
    // pass "" to accept in/mm/pt/px (default), pass "" and set allow_units=false
    // to reject all unit letters (for Angle / Percentage / Integer fields).
    static bool parse_expr(const std::string& expr, Unit default_unit,
                           double& result_px, std::string& err_msg,
                           bool allow_units = true);
};

} // namespace Curvz
