#pragma once
#include <string>

namespace Curvz {

// ── Unit enum ─────────────────────────────────────────────────────────────────
enum class Unit { Px, In, Mm, Pt };

// ── Domain enum (s263 m5) ─────────────────────────────────────────────────────
//
// A spin field declares which family of values it accepts; the parser
// uses this to decide which unit suffixes are legal and to refuse
// cross-domain mixing structurally (a length suffix in an angle field
// is unparseable, not just semantically wrong). The keystroke filter
// uses the same enum to decide which letter keys reach the entry
// buffer.
//
//   Length         — distance / coordinate / size fields. Accepts
//                    `in`, `"`, `mm`, `pt`, `px`. Bare numbers
//                    default to the field's display unit.
//                    Returned value is normalized to pixels for the
//                    caller to convert back through `from_px()`.
//   Angle          — rotation fields. Accepts `deg`, `rad`. Bare
//                    numbers default to degrees. Returned value is
//                    normalized to degrees (matching the codebase's
//                    pervasive `angle_deg` API surface).
//   Percentage     — 0..100 fields. Accepts `%`. Bare numbers and
//                    `%`-suffixed numbers are treated the same;
//                    the suffix is a no-op acknowledgment that
//                    matches the field label, not a /100 conversion.
//   Dimensionless  — counts, integers, anything without a unit
//                    family. Rejects all suffixes (current
//                    `allow_units=false` behaviour).
enum class Domain { Length, Angle, Percentage, Dimensionless };

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

    // ── Parse expression — Domain-aware path (s263 m5) ────────────────────
    //
    // The Domain-typed signature is the preferred entry point. The
    // parser uses the Domain to decide which suffixes are legal and
    // to compose error messages naming the per-domain legal set.
    //
    // `default_unit` is only meaningful for Domain::Length (the unit
    // to apply to bare numbers; ignored for Angle / Percentage /
    // Dimensionless where the bare-number default is implicit in the
    // domain).
    //
    // For Domain::Length, `result` is normalized to pixels (same as
    // the legacy signature). For Domain::Angle, `result` is degrees.
    // For Domain::Percentage and Dimensionless, `result` is the raw
    // entered value with the suffix stripped if present.
    static bool parse_expr(const std::string& expr, Domain domain,
                           Unit length_default_unit, double& result,
                           std::string& err_msg);

    // ── Legacy signatures (kept for source-compat) ────────────────────────
    //
    // Forward to the Domain-typed path: allow_units=true maps to
    // Domain::Length; allow_units=false maps to Domain::Dimensionless.
    // Call sites can migrate incrementally — the Domain-typed entry
    // point is the source of truth; these are thin wrappers.
    //
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
