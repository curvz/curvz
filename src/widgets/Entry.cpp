// widgets/Entry.cpp ────────────────────────────────────────────────────

#include "widgets/Entry.hpp"

namespace curvz::widgets {

using namespace curvz::scripting;

// Gtk::Entry's default ctor takes no args; the base template handles
// the zero-arg case cleanly via perfect forwarding of an empty pack.
Entry::Entry(std::string_view name) : ScriptableWidget<Gtk::Entry>(name) {
  init_scriptable();
}

// s211 m1 — unregistered substrate Entry.
//
// Forwards the tag to the template's parallel ctor (which routes it
// to Scriptable's unregistered ctor — empty name, m_registered=false).
// Still calls init_scriptable(); bind_canonical() still wires
// signal_changed() to emit(); emit() short-circuits at the Scriptable
// layer for unregistered instances. The leaf body is uniform with
// the registered ctor by design — no special-casing here, only the
// base-list initialisation differs. Mirrors Button.cpp's s209 m1 work
// and ToggleButton.cpp's s209 m2 work.
Entry::Entry(unregistered_t) : ScriptableWidget<Gtk::Entry>(unregistered) {
  init_scriptable();
}

void Entry::bind_canonical() {
  signal_changed().connect(
      [this]() { emit("changed", ScriptValue::text(get_text())); });
}

ScriptValue Entry::invoke_leaf(std::string_view verb, const ScriptArgs &args) {
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
  throw std::runtime_error("Entry: unknown verb '" + std::string(verb) + "'");
}

ScriptValue Entry::query_leaf(std::string_view property) const {
  if (property == "text")
    return ScriptValue::text(get_text());
  return ScriptValue::null();
}

std::vector<std::string> Entry::leaf_verbs() const { return {"set", "clear"}; }
std::vector<std::string> Entry::leaf_properties() const { return {"text"}; }

} // namespace curvz::widgets
