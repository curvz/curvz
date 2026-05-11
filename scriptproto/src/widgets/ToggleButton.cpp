// ToggleButton.cpp ───────────────────────────────────────────────────────────

#include "widgets/ToggleButton.hpp"

namespace scriptproto {

ToggleButton::ToggleButton(std::string_view name, const Glib::ustring& label)
    : ScriptableWidget<Gtk::ToggleButton>(name, label) {
    // One-line discipline: every leaf ctor ends with this. The base
    // can't call bind_canonical() from its own ctor (pure-virtual
    // dispatch through a partially-constructed object is UB), so the
    // leaf surfaces the call site.
    init_scriptable();
}

void ToggleButton::bind_canonical() {
    // signal_toggled fires whether the toggle was driven by real input
    // or by set_active() — so script-driven and user-driven paths
    // converge here. Exactly the bidirectional-channel design point.
    signal_toggled().connect([this]() {
        emit("toggled", ScriptValue::boolean(get_active()));
    });
}

ScriptValue ToggleButton::invoke(std::string_view verb, const ScriptArgs& args) {
    if (verb == "click") {
        // Canonical activate path — same as a user pressing the button.
        // Widget::activate() emits activate, which for buttons routes
        // through clicked, which for ToggleButton flips active and
        // emits signal_toggled.
        activate();
        return ScriptValue::null();
    }
    if (verb == "set") {
        if (args.size() != 1 || args[0].kind != ValueKind::Bool)
            throw std::runtime_error("set expects one bool argument");
        set_active(args[0].b);
        return ScriptValue::null();
    }
    throw std::runtime_error("ToggleButton: unknown verb '" + std::string(verb) + "'");
}

ScriptValue ToggleButton::query(std::string_view property) const {
    if (property == "active") return ScriptValue::boolean(get_active());
    if (property == "label")  return ScriptValue::text(get_label());
    return ScriptValue::null();
}

std::vector<std::string> ToggleButton::verbs() const {
    return { "click", "set" };
}

std::vector<std::string> ToggleButton::properties() const {
    return { "active", "label" };
}

} // namespace scriptproto
