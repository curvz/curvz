// curvz/widgets/ToggleButton.cpp ─────────────────────────────────────────────

#include "curvz/widgets/ToggleButton.hpp"

namespace curvz::widgets {

using namespace curvz::scripting;

ToggleButton::ToggleButton(std::string_view name, const Glib::ustring& label)
    : ScriptableWidget<Gtk::ToggleButton>(name, label) {
    // One-line discipline: every leaf ctor ends with this. The base
    // can't call bind_canonical() from its own ctor (pure-virtual
    // dispatch through a partially-constructed object is UB).
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

ScriptValue ToggleButton::invoke_leaf(std::string_view verb,
                                  const ScriptArgs& args) {
    if (verb == "click") {
        // GTK is threaded — activate() routes through the gesture
        // controller on a context that isn't this stack frame's. The
        // synchronizer waits on signal_toggled to confirm the work
        // landed before returning to the listener. See the base's
        // wait_for_signal() doc for the full contract.
        wait_for_signal(
            [this](sigc::slot<void()> slot) {
                return signal_toggled().connect(std::move(slot));
            },
            [this]() { activate(); });
        return ScriptValue::null();
    }
    if (verb == "set") {
        if (args.size() != 1 || args[0].kind != ValueKind::Bool)
            throw std::runtime_error("set expects one bool argument");
        // set_active() is synchronous in the calling thread — the
        // property mutation and signal_toggled emission happen inline.
        // No synchronizer needed.
        set_active(args[0].b);
        return ScriptValue::null();
    }
    throw std::runtime_error(
        "ToggleButton: unknown verb '" + std::string(verb) + "'");
}

ScriptValue ToggleButton::query_leaf(std::string_view property) const {
    if (property == "active") return ScriptValue::boolean(get_active());
    if (property == "label")  return ScriptValue::text(get_label());
    return ScriptValue::null();
}

std::vector<std::string> ToggleButton::leaf_verbs() const {
    return { "click", "set" };
}

std::vector<std::string> ToggleButton::leaf_properties() const {
    return { "active", "label" };
}

} // namespace curvz::widgets
