// curvz/widgets/Button.cpp ───────────────────────────────────────────────────

#include "curvz/widgets/Button.hpp"

namespace curvz::widgets {

using namespace curvz::scripting;

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
        // activate() routes through GTK's gesture path — same thread-
        // crossing issue as ToggleButton::click. Synchronizer waits
        // on signal_clicked to confirm the work landed before
        // returning. See ScriptableWidget::wait_for_signal docs.
        wait_for_signal(
            [this](sigc::slot<void()> slot) {
                return signal_clicked().connect(std::move(slot));
            },
            [this]() { activate(); });
        return ScriptValue::null();
    }
    throw std::runtime_error(
        "Button: unknown verb '" + std::string(verb) + "'");
}

ScriptValue Button::query(std::string_view property) const {
    if (property == "label") return ScriptValue::text(get_label());
    return ScriptValue::null();
}

std::vector<std::string> Button::verbs() const      { return { "click" }; }
std::vector<std::string> Button::properties() const { return { "label" }; }

} // namespace curvz::widgets
