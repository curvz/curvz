// Button.cpp ─────────────────────────────────────────────────────────────────

#include "widgets/Button.hpp"

namespace scriptproto {

Button::Button(std::string_view name, const Glib::ustring& label)
    : ScriptableWidget<Gtk::Button>(name, label) {
    init_scriptable();
}

void Button::bind_canonical() {
    signal_clicked().connect([this]() {
        emit("clicked", ScriptValue::null());
    });
}

ScriptValue Button::invoke(std::string_view verb, const ScriptArgs&) {
    if (verb == "click") {
        activate();   // fires the activate signal → clicked
        return ScriptValue::null();
    }
    throw std::runtime_error("Button: unknown verb '" + std::string(verb) + "'");
}

ScriptValue Button::query(std::string_view property) const {
    if (property == "label") return ScriptValue::text(get_label());
    return ScriptValue::null();
}

std::vector<std::string> Button::verbs() const      { return { "click" }; }
std::vector<std::string> Button::properties() const { return { "label" }; }

} // namespace scriptproto
