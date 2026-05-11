// curvz/widgets/Scale.cpp ────────────────────────────────────────────────────

#include "curvz/widgets/Scale.hpp"
#include <gtkmm/adjustment.h>

namespace curvz::widgets {

using namespace curvz::scripting;

Scale::Scale(std::string_view name, double min, double max, double step)
    : ScriptableWidget<Gtk::Scale>(name, Gtk::Orientation::HORIZONTAL) {
    set_range(min, max);
    set_increments(step, step * 10.0);
    set_draw_value(true);
    init_scriptable();
}

// s189 m2 — pre-built Adjustment overload. Routes (adj, orientation)
// through the base template's perfect-forwarding to Gtk::Scale's
// (Adjustment, Orientation) ctor. Range / step come from the
// Adjustment; orientation defaults to horizontal. draw_value is left
// to the caller — slider visual style (value-pos, draw_value,
// set_digits) is set at the call site after construction. (Contrast
// the primitive-form ctor above, which set_draw_value(true) was the
// matching expectation: with a pre-built Adjustment the call site
// already chose its display style.)
Scale::Scale(std::string_view name,
             Glib::RefPtr<Gtk::Adjustment> adj,
             Gtk::Orientation orientation)
    : ScriptableWidget<Gtk::Scale>(name, adj, orientation) {
    init_scriptable();
}

void Scale::bind_canonical() {
    // Gtk::Range (Scale's base) emits signal_value_changed when its
    // value moves — regardless of whether the move came from user drag
    // or set_value(). One signal, both paths.
    signal_value_changed().connect([this]() {
        emit("value_changed", ScriptValue::real(get_value()));
    });
}

ScriptValue Scale::invoke_leaf(std::string_view verb, const ScriptArgs& args) {
    if (verb == "set") {
        if (args.size() != 1)
            throw std::runtime_error("Scale.set expects one numeric arg");
        double v = 0.0;
        if      (args[0].kind == ValueKind::Double) v = args[0].d;
        else if (args[0].kind == ValueKind::Int)    v = static_cast<double>(args[0].i);
        else throw std::runtime_error("Scale.set expects a number");
        // set_value() is synchronous — signal_value_changed fires inline.
        set_value(v);
        return ScriptValue::null();
    }
    throw std::runtime_error(
        "Scale: unknown verb '" + std::string(verb) + "'");
}

ScriptValue Scale::query_leaf(std::string_view property) const {
    if (property == "value") return ScriptValue::real(get_value());
    if (property == "min")   return ScriptValue::real(get_adjustment()->get_lower());
    if (property == "max")   return ScriptValue::real(get_adjustment()->get_upper());
    return ScriptValue::null();
}

std::vector<std::string> Scale::leaf_verbs() const      { return { "set" }; }
std::vector<std::string> Scale::leaf_properties() const { return { "value", "min", "max" }; }

} // namespace curvz::widgets
