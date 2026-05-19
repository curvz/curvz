#include "UnitSystem.hpp"
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace Curvz {

// ── Conversion ────────────────────────────────────────────────────────────────

static constexpr double PX_PER_IN = 96.0;
static constexpr double PX_PER_MM = 96.0 / 25.4;
static constexpr double PX_PER_PT = 96.0 / 72.0;

double UnitSystem::from_px(double px, Unit u) {
    switch (u) {
        case Unit::In: return px / PX_PER_IN;
        case Unit::Mm: return px / PX_PER_MM;
        case Unit::Pt: return px / PX_PER_PT;
        default:       return px;
    }
}

double UnitSystem::to_px(double v, Unit u) {
    switch (u) {
        case Unit::In: return v * PX_PER_IN;
        case Unit::Mm: return v * PX_PER_MM;
        case Unit::Pt: return v * PX_PER_PT;
        default:       return v;
    }
}

const char* UnitSystem::label(Unit u) {
    switch (u) {
        case Unit::In: return "in";
        case Unit::Mm: return "mm";
        case Unit::Pt: return "pt";
        default:       return "px";
    }
}

Unit UnitSystem::parse_unit(const std::string& tok) {
    if (tok == "in" || tok == "\"") return Unit::In;
    if (tok == "mm")                return Unit::Mm;
    if (tok == "pt")                return Unit::Pt;
    if (tok == "px")                return Unit::Px;
    return Unit::Px;
}

// ── Expression parser ─────────────────────────────────────────────────────────
// Grammar (standard precedence):
//   expr   := term   { ('+'|'-') term   }
//   term   := unary  { ('*'|'/') unary  }
//   unary  := '-' unary | primary
//   primary:= number [unit_suffix] | '(' expr ')'
//
// s263 m5 — Parser is Domain-aware. The domain decides which suffixes
// are legal at primary() and how the resulting value is normalized:
// Length → px, Angle → degrees, Percentage / Dimensionless → raw.
// Mixing suffixes within a domain is fine (`5" + 3mm` in Length,
// `45 + 0.5rad` in Angle); a suffix from the wrong domain throws at
// the suffix-recognition step with a per-domain error message. The
// previous `allow_units` bool collapses into `Domain::Length` (true)
// and `Domain::Dimensionless` (false); the legacy entry points wrap
// the Domain-typed one for source-compat.

namespace {

// Angle conversion: 1 rad = 180/π degrees.
static constexpr double DEG_PER_RAD = 57.29577951308232;

struct Parser {
    const char* start;
    const char* p;
    Curvz::Domain   domain;
    Curvz::Unit     length_default_unit;  // only used when domain == Length

    void skip_ws() { while (*p && std::isspace((unsigned char)*p)) ++p; }

    // Consume an optional unit suffix and apply its conversion to `v`,
    // returning the domain-normalized scalar. Per-domain dispatch:
    //   Length        — `"` or letter-run in {in, mm, pt, px};
    //                   converts `v` to pixels.
    //   Angle         — letter-run in {deg, rad}; converts `v` to
    //                   degrees.
    //   Percentage    — optional `%` suffix; no conversion (the
    //                   stored value is the raw percentage).
    //   Dimensionless — no suffix legal; throws on any suffix found.
    //
    // Bare numbers (no suffix) default to: length_default_unit (Length),
    // degrees (Angle), raw (Percentage / Dimensionless).
    double read_and_apply_unit(double v) {
        using Curvz::Unit;
        using Curvz::Domain;
        skip_ws();

        // s263 m4 carry-forward — `"` as inch is only legal in Length.
        // Other domains reject the quote at the suffix-recognition step.
        if (*p == '"') {
            if (domain != Domain::Length)
                throw std::runtime_error(
                    "'\"' is a length suffix; not legal in this field.");
            ++p;
            return Curvz::UnitSystem::to_px(v, Unit::In);
        }

        // s263 m5 — `%` is legal only in Percentage; consumes but
        // does not convert (the stored value is the raw percentage).
        if (*p == '%') {
            if (domain != Domain::Percentage)
                throw std::runtime_error(
                    "'%' is a percentage suffix; not legal in this field.");
            ++p;
            return v;
        }

        // No alphabetic suffix? Apply the domain's bare-number default.
        if (!std::isalpha((unsigned char)*p)) {
            switch (domain) {
                case Domain::Length:
                    return Curvz::UnitSystem::to_px(v, length_default_unit);
                case Domain::Angle:
                case Domain::Percentage:
                case Domain::Dimensionless:
                    return v;
            }
            return v;
        }

        // Collect the letter run.
        const char* s = p;
        while (std::isalpha((unsigned char)*p)) ++p;
        std::string tok(s, p - s);

        // Per-domain legal sets.
        switch (domain) {
            case Domain::Length:
                if (tok == "in" || tok == "mm" || tok == "pt" || tok == "px")
                    return Curvz::UnitSystem::to_px(
                        v, Curvz::UnitSystem::parse_unit(tok));
                throw std::runtime_error("Unknown unit '" + tok +
                                          "'. Use in, mm, pt, or px.");
            case Domain::Angle:
                if (tok == "deg") return v;
                if (tok == "rad") return v * DEG_PER_RAD;
                throw std::runtime_error("Unknown unit '" + tok +
                                          "'. Use deg or rad.");
            case Domain::Percentage:
                throw std::runtime_error("Unknown unit '" + tok +
                                          "'. Use % or omit the suffix.");
            case Domain::Dimensionless:
                throw std::runtime_error("Units not allowed here: '" + tok +
                                          "'.");
        }
        return v;  // unreachable; switch is exhaustive
    }

    double primary() {
        skip_ws();
        if (*p == '\0')
            throw std::runtime_error("Missing value.");
        if (*p == '(') {
            ++p;
            double v = expr();
            skip_ws();
            if (*p != ')')
                throw std::runtime_error("Unclosed parenthesis.");
            ++p;
            return v;
        }
        if (*p == ')')
            throw std::runtime_error("Unexpected ')'.");
        // number
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) {
            std::string ch(1, *p);
            throw std::runtime_error("Not a number: '" + ch + "'.");
        }
        p = end;
        return read_and_apply_unit(v);
    }

    double unary() {
        skip_ws();
        if (*p == '-') { ++p; return -unary(); }
        if (*p == '+') { ++p; return  unary(); }
        return primary();
    }

    double term() {
        double v = unary();
        for (;;) {
            skip_ws();
            if (*p == '*') { ++p; v *= unary(); }
            else if (*p == '/') {
                ++p;
                double d = unary();
                if (std::fabs(d) < 1e-12)
                    throw std::runtime_error("Division by zero.");
                v /= d;
            }
            else break;
        }
        return v;
    }

    double expr() {
        double v = term();
        for (;;) {
            skip_ws();
            if      (*p == '+') { ++p; v += term(); }
            else if (*p == '-') { ++p; v -= term(); }
            else break;
        }
        return v;
    }
};

} // anonymous namespace

// ── Domain-aware entry point (s263 m5) ────────────────────────────────────────
bool UnitSystem::parse_expr(const std::string& input, Domain domain,
                             Unit length_default_unit, double& result,
                             std::string& err_msg) {
    if (input.empty()) { err_msg = "Empty expression."; return false; }
    Parser parser{ input.c_str(), input.c_str(), domain, length_default_unit };
    try {
        double v = parser.expr();
        parser.skip_ws();
        if (*parser.p != '\0') {
            std::string ch(1, *parser.p);
            err_msg = "Unexpected '" + ch + "'.";
            return false;
        }
        result = v;
        err_msg.clear();
        return true;
    } catch (const std::exception& e) {
        err_msg = e.what();
        return false;
    }
}

// ── Legacy entry points (kept for source-compat) ──────────────────────────────
//
// allow_units=true  → Domain::Length, result in pixels.
// allow_units=false → Domain::Dimensionless, result is the raw value.
// Both forms preserve the pre-s263-m5 caller contract.
bool UnitSystem::parse_expr(const std::string& input, Unit default_unit,
                             double& result_px) {
    std::string ignored;
    return parse_expr(input, Domain::Length, default_unit, result_px, ignored);
}

bool UnitSystem::parse_expr(const std::string& input, Unit default_unit,
                             double& result_px, std::string& err_msg,
                             bool allow_units) {
    Domain d = allow_units ? Domain::Length : Domain::Dimensionless;
    return parse_expr(input, d, default_unit, result_px, err_msg);
}

} // namespace Curvz
