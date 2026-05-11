// curvz/widgets/SpinButton.cpp ───────────────────────────────────────────────

#include "curvz/widgets/SpinButton.hpp"
#include <gtkmm/adjustment.h>
#include <cmath>

namespace curvz::widgets {

using namespace curvz::scripting;

SpinButton::SpinButton(std::string_view name,
                       double min, double max, double step,
                       int digits)
    : ScriptableWidget<Gtk::SpinButton>(name) {
    set_range(min, max);
    set_increments(step, step * 10.0);
    set_digits(digits);
    init_scriptable();
}

void SpinButton::bind_canonical() {
    signal_value_changed().connect([this]() {
        emit("value_changed", ScriptValue::real(get_value()));
    });
}

ScriptValue SpinButton::invoke(std::string_view verb, const ScriptArgs& args) {
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

ScriptValue SpinButton::query(std::string_view property) const {
    if (property == "value") return ScriptValue::real(get_value());
    if (property == "min")   return ScriptValue::real(get_adjustment()->get_lower());
    if (property == "max")   return ScriptValue::real(get_adjustment()->get_upper());
    return ScriptValue::null();
}

std::vector<std::string> SpinButton::verbs() const {
    return { "set", "step" };
}
std::vector<std::string> SpinButton::properties() const {
    return { "value", "min", "max" };
}

} // namespace curvz::widgets
