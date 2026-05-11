// curvz/widgets/Entry.cpp ────────────────────────────────────────────────────

#include "curvz/widgets/Entry.hpp"

namespace curvz::widgets {

using namespace curvz::scripting;

// Gtk::Entry's default ctor takes no args; the base template handles
// the zero-arg case cleanly via perfect forwarding of an empty pack.
Entry::Entry(std::string_view name)
    : ScriptableWidget<Gtk::Entry>(name) {
    init_scriptable();
}

void Entry::bind_canonical() {
    signal_changed().connect([this]() {
        emit("changed", ScriptValue::text(get_text()));
    });
}

ScriptValue Entry::invoke(std::string_view verb, const ScriptArgs& args) {
    if (verb == "set") {
        if (args.size() != 1 || args[0].kind != ValueKind::String)
            throw std::runtime_error("Entry.set expects one string arg");
        // set_text() is synchronous — property mutation and
        // signal_changed emission happen inline on the calling thread.
        // No synchronizer needed (unlike activate()-based verbs).
        set_text(args[0].s);
        return ScriptValue::null();
    }
    if (verb == "clear") {
        set_text("");
        return ScriptValue::null();
    }
    throw std::runtime_error(
        "Entry: unknown verb '" + std::string(verb) + "'");
}

ScriptValue Entry::query(std::string_view property) const {
    if (property == "text") return ScriptValue::text(get_text());
    return ScriptValue::null();
}

std::vector<std::string> Entry::verbs() const {
    return { "set", "clear" };
}
std::vector<std::string> Entry::properties() const {
    return { "text" };
}

} // namespace curvz::widgets
