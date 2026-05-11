// scripting/ScriptValue.hpp ──────────────────────────────────────────────────
//
// The currency that crosses the script/widget seam. A widget's invoke()
// argument list is a vector of these; a widget's query() return is one of
// these. The DSL parser produces these from script tokens; the assertion
// engine compares these for equality.
//
// Five types cover the property intake variety GTK ships:
//   - Bool       (ToggleButton.active, CheckButton.active)
//   - Int        (DropDown.selected — zero-based index)
//   - Double     (SpinButton.value, Scale.value)
//   - String     (Entry.text, DropDown.text-of-selected)
//   - Null       (return value of click/clicked which has no readable result)
//
// Comparison semantics are tight: int and double do NOT cross-compare
// (the DSL forces the author to be explicit — `value == 0.75` not
// `value == 0`). String comparison is exact. Bool literal tokens are
// `true` / `false`.
//
// The value type is intentionally NOT a std::variant. A small tagged
// union keeps the header free of <variant> and gives stable layout for
// the few places that switch on `kind`.
//
// Lifted from scriptproto/ during s186 m2 — namespace renamed,
// otherwise unchanged.

#pragma once
#include <string>
#include <sstream>

namespace curvz::scripting {

enum class ValueKind { Null, Bool, Int, Double, String };

struct ScriptValue {
    ValueKind   kind = ValueKind::Null;
    bool        b    = false;
    long long   i    = 0;
    double      d    = 0.0;
    std::string s;

    static ScriptValue null()                       { return {}; }
    static ScriptValue boolean(bool v)              { ScriptValue r; r.kind = ValueKind::Bool;   r.b = v; return r; }
    static ScriptValue integer(long long v)         { ScriptValue r; r.kind = ValueKind::Int;    r.i = v; return r; }
    static ScriptValue real(double v)               { ScriptValue r; r.kind = ValueKind::Double; r.d = v; return r; }
    static ScriptValue text(std::string v)          { ScriptValue r; r.kind = ValueKind::String; r.s = std::move(v); return r; }

    // Strict equality. No int/double coercion. Different-kind compares
    // always false — the DSL should never produce a mixed-kind compare
    // because the literal lexer is unambiguous (3 → Int, 3.0 → Double).
    bool equals(const ScriptValue& o) const {
        if (kind != o.kind) return false;
        switch (kind) {
            case ValueKind::Null:   return true;
            case ValueKind::Bool:   return b == o.b;
            case ValueKind::Int:    return i == o.i;
            case ValueKind::Double: return d == o.d;
            case ValueKind::String: return s == o.s;
        }
        return false;
    }

    // Render for log/output. Matches DSL literal form so a captured
    // value can be pasted back into a script.
    std::string repr() const {
        std::ostringstream os;
        switch (kind) {
            case ValueKind::Null:   os << "null"; break;
            case ValueKind::Bool:   os << (b ? "true" : "false"); break;
            case ValueKind::Int:    os << i; break;
            case ValueKind::Double: os << d; break;
            case ValueKind::String: os << '"' << s << '"'; break;
        }
        return os.str();
    }
};

} // namespace curvz::scripting
