// curvz/widgets/CheckButton.cpp ──────────────────────────────────────────────

#include "curvz/widgets/CheckButton.hpp"

namespace curvz::widgets {

using namespace curvz::scripting;

CheckButton::CheckButton(std::string_view name, const Glib::ustring& label)
    : ScriptableWidget<Gtk::CheckButton>(name, label) {
    init_scriptable();
}

void CheckButton::bind_canonical() {
    signal_toggled().connect([this]() {
        emit("toggled", ScriptValue::boolean(get_active()));
    });
}

ScriptValue CheckButton::invoke(std::string_view verb, const ScriptArgs& args) {
    if (verb == "click") {
        // Gtk::CheckButton derives from Widget, not Button. The
        // canonical "click equivalent" is toggling active. set_active()
        // is synchronous — signal_toggled fires inline. No synchronizer
        // needed.
        set_active(!get_active());
        return ScriptValue::null();
    }
    if (verb == "set") {
        if (args.size() != 1 || args[0].kind != ValueKind::Bool)
            throw std::runtime_error("CheckButton.set expects one bool arg");
        set_active(args[0].b);
        return ScriptValue::null();
    }
    throw std::runtime_error(
        "CheckButton: unknown verb '" + std::string(verb) + "'");
}

ScriptValue CheckButton::query(std::string_view property) const {
    if (property == "active") return ScriptValue::boolean(get_active());
    if (property == "label")  return ScriptValue::text(get_label());
    return ScriptValue::null();
}

std::vector<std::string> CheckButton::verbs() const {
    return { "click", "set" };
}
std::vector<std::string> CheckButton::properties() const {
    return { "active", "label" };
}

} // namespace curvz::widgets
