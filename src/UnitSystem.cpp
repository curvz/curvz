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

namespace {

struct Parser {
    const char* start;
    const char* p;
    Unit        default_unit;
    bool        allow_units = true;

    void skip_ws() { while (*p && std::isspace((unsigned char)*p)) ++p; }

    // Read a unit suffix if present; return the unit. Throws if an
    // unknown letter run appears when allow_units is false, or if an
    // unrecognised two-letter suffix begins with [a-z].
    Unit read_unit() {
        skip_ws();
        if (!std::isalpha((unsigned char)*p)) return default_unit;
        // Collect the letter run
        const char* s = p;
        while (std::isalpha((unsigned char)*p)) ++p;
        std::string tok(s, p - s);
        if (!allow_units)
            throw std::runtime_error("Units not allowed here: '" + tok + "'.");
        if (tok == "in" || tok == "mm" || tok == "pt" || tok == "px")
            return UnitSystem::parse_unit(tok);
        throw std::runtime_error("Unknown unit '" + tok +
                                 "'. Use in, mm, pt, or px.");
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
        Unit u = read_unit();
        return UnitSystem::to_px(v, u);
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

bool UnitSystem::parse_expr(const std::string& input, Unit default_unit,
                             double& result_px) {
    std::string ignored;
    return parse_expr(input, default_unit, result_px, ignored, true);
}

bool UnitSystem::parse_expr(const std::string& input, Unit default_unit,
                             double& result_px, std::string& err_msg,
                             bool allow_units) {
    if (input.empty()) { err_msg = "Empty expression."; return false; }
    Parser parser{ input.c_str(), input.c_str(), default_unit, allow_units };
    try {
        double v = parser.expr();
        parser.skip_ws();
        if (*parser.p != '\0') {
            std::string ch(1, *parser.p);
            err_msg = "Unexpected '" + ch + "'.";
            return false;
        }
        result_px = v;
        err_msg.clear();
        return true;
    } catch (const std::exception& e) {
        err_msg = e.what();
        return false;
    }
}

} // namespace Curvz
