// curvz/widgets/SpinButton.cpp ───────────────────────────────────────────────

#include "curvz/widgets/SpinButton.hpp"
#include "UnitSystem.hpp"            // s219: dimensionless math parser
#include <gdk/gdkkeysyms.h>           // s219: GDK_KEY_* for keystroke filter
#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollerkey.h> // s219: capture-phase keystroke filter
#include <cmath>

namespace curvz::widgets {

using namespace curvz::scripting;

// ── s219 math wiring ────────────────────────────────────────────────────────
//
// The two SpinButton classes in Curvz used to split on a math axis:
// `Curvz::CurvzSpinButton` accepted math expressions (and unit suffixes),
// while `curvz::widgets::SpinButton` — the script-substrate wrapper — was
// math-blind. That meant the selection-section ROTATE field, the toolbar
// zoom percentage, the macro-editor numeric properties, and so on all
// silently rejected legitimate user input like "360/36" or "100*1.5".
//
// s219 lifts the math layer into this base. Every script-substrate spin
// now accepts dimensionless arithmetic. CurvzSpinButton keeps its
// unit-aware variant of the same machinery — units are its concern, not
// the substrate's. The two classes no longer differ on whether numeric
// input is "real" or "constrained".
//
// Three pieces:
//
//   1. set_numeric(false) — tells GtkText to stop pre-filtering keystrokes.
//   2. signal_input() — overrides Glib::strtod with UnitSystem::parse_expr
//      on the unitless branch.
//   3. CAPTURE-phase keystroke filter — swallows characters that aren't
//      digits, operators, or basic punctuation. Mirrors CurvzSpinButton's
//      filter without the unit-letter allowlist.

static bool widget_is_math_char(gunichar ch) {
    if (ch >= '0' && ch <= '9') return true;
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
        ch == '(' || ch == ')' || ch == '.' || ch == ',' ||
        ch == 'e' || ch == 'E' || ch == ' ' || ch == '\t')
        return true;
    return false;
}

void SpinButton::init_math() {
    set_numeric(false);

    // GTK input hook — same shape as CurvzSpinButton's. Returns 1 on
    // success (use new_value), -1 on parse failure (don't commit), 0
    // would fall back to strtod (which strips trailing non-numeric, NOT
    // what we want).
    signal_input().connect([this](double &new_value) -> int {
        std::string txt = get_text();
        auto first = txt.find_first_not_of(" \t");
        auto last  = txt.find_last_not_of(" \t");
        std::string t = (first == std::string::npos) ? std::string()
                        : txt.substr(first, last - first + 1);
        if (t.empty()) return -1;

        double v = 0.0;
        std::string err;
        if (!Curvz::UnitSystem::parse_expr(t, Curvz::Unit::Px, v, err,
                                            /*allow_units=*/false))
            return -1;

        // Honor the adjustment's digits — integer-style spins (digits=0)
        // should commit whole numbers when math produces a fractional
        // value. set_digits(0) on the adjustment already rounds for
        // display; rounding here keeps the committed value consistent.
        if (get_digits() == 0)
            v = std::round(v);

        double lo = get_adjustment()->get_lower();
        double hi = get_adjustment()->get_upper();
        if (v < lo || v > hi) return -1;

        new_value = v;
        return 1;
    }, false);

    // Capture-phase keystroke filter. Reject characters that can't appear
    // in a math expression before they reach the entry buffer.
    auto kc = Gtk::EventControllerKey::create();
    kc->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    kc->signal_key_pressed().connect(
        [](guint keyval, guint, Gdk::ModifierType mods) -> bool {
            if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete ||
                keyval == GDK_KEY_Left      || keyval == GDK_KEY_Right ||
                keyval == GDK_KEY_Home      || keyval == GDK_KEY_End ||
                keyval == GDK_KEY_Tab       || keyval == GDK_KEY_ISO_Left_Tab ||
                keyval == GDK_KEY_Return    || keyval == GDK_KEY_KP_Enter ||
                keyval == GDK_KEY_Escape)
                return false;
            if ((mods & (Gdk::ModifierType::CONTROL_MASK |
                         Gdk::ModifierType::ALT_MASK)) !=
                Gdk::ModifierType{})
                return false;
            gunichar ch = gdk_keyval_to_unicode(keyval);
            if (ch == 0) return false;
            if (!widget_is_math_char(ch))
                return true; // swallow
            return false;
        },
        false);
    add_controller(kc);
}

SpinButton::SpinButton(std::string_view name,
                       double min, double max, double step,
                       int digits)
    : ScriptableWidget<Gtk::SpinButton>(name) {
    set_range(min, max);
    set_increments(step, step * 10.0);
    set_digits(digits);
    init_scriptable();
    init_math();
}

// s189 m2 — pre-built Adjustment overload. The Adjustment-taking
// Gtk::SpinButton ctor is `(adj, climb_rate, digits)`; we route those
// three args through the template base's perfect-forwarding. Inspector
// call sites that connect adj->signal_value_changed() or share the
// Adjustment across widgets keep that external handle alive — same
// Adjustment, now wrapped by a scriptable SpinButton.
SpinButton::SpinButton(std::string_view name,
                       Glib::RefPtr<Gtk::Adjustment> adj,
                       double climb_rate, int digits)
    : ScriptableWidget<Gtk::SpinButton>(name, adj, climb_rate, digits) {
    init_scriptable();
    init_math();
}

// s211 m2 — unregistered substrate SpinButton (Adjustment-taking form).
//
// Forwards the tag to the template's parallel ctor (which routes it
// to Scriptable's unregistered ctor — empty name, m_registered=false);
// the trailing (adj, climb_rate, digits) goes to Gtk::SpinButton's
// adjustment-taking ctor via perfect-forwarding. Still calls
// init_scriptable(); bind_canonical() still wires
// signal_value_changed() to emit(); emit() short-circuits at the
// Scriptable layer for unregistered instances. Mirrors Button.cpp's
// s209 m1 work, ToggleButton.cpp's s209 m2 work, and Entry.cpp's
// s211 m1 work.
SpinButton::SpinButton(unregistered_t,
                       Glib::RefPtr<Gtk::Adjustment> adj,
                       double climb_rate, int digits)
    : ScriptableWidget<Gtk::SpinButton>(unregistered, adj, climb_rate, digits) {
    init_scriptable();
    init_math();
}

void SpinButton::bind_canonical() {
    signal_value_changed().connect([this]() {
        emit("value_changed", ScriptValue::real(get_value()));
    });
}

ScriptValue SpinButton::invoke_leaf(std::string_view verb, const ScriptArgs& args) {
    if (verb == "set") {
        if (args.size() != 1)
            throw std::runtime_error("SpinButton.set expects one numeric arg");
        double v = 0.0;
        if      (args[0].kind == ValueKind::Double) v = args[0].d;
        else if (args[0].kind == ValueKind::Int)    v = static_cast<double>(args[0].i);
        else throw std::runtime_error("SpinButton.set expects a number");
        // set_value() is synchronous — signal_value_changed fires inline.
        set_value(v);
        return ScriptValue::null();
    }
    if (verb == "step") {
        if (args.size() != 1 || args[0].kind != ValueKind::Int)
            throw std::runtime_error("SpinButton.step expects one int arg");
        long long n = args[0].i;
        auto dir = (n >= 0) ? Gtk::SpinType::STEP_FORWARD
                            : Gtk::SpinType::STEP_BACKWARD;
        for (long long i = 0; i < std::llabs(n); ++i) spin(dir, 1.0);
        return ScriptValue::null();
    }
    throw std::runtime_error(
        "SpinButton: unknown verb '" + std::string(verb) + "'");
}

ScriptValue SpinButton::query_leaf(std::string_view property) const {
    if (property == "value") return ScriptValue::real(get_value());
    if (property == "min")   return ScriptValue::real(get_adjustment()->get_lower());
    if (property == "max")   return ScriptValue::real(get_adjustment()->get_upper());
    return ScriptValue::null();
}

std::vector<std::string> SpinButton::leaf_verbs() const {
    return { "set", "step" };
}
std::vector<std::string> SpinButton::leaf_properties() const {
    return { "value", "min", "max" };
}

} // namespace curvz::widgets
