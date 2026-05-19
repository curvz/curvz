// widgets/Entry.hpp ────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::Entry. Lifted from scriptproto/ during
// s187 m3.
//
// Verbs:
//   set "<text>"        → set_text, fires signal_changed
//   clear               → equivalent to set ""
//
// Queries:
//   text                → String
//
// Emits:
//   changed <text>      on signal_changed (real OR script-driven)

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/entry.h>

namespace curvz::widgets {

class Entry : public curvz::scripting::ScriptableWidget<Gtk::Entry> {
public:
  explicit Entry(std::string_view name);

  // s211 m1 — unregistered substrate Entry. The widget IS-A
  // substrate Entry (same universal verbs, same Gtk::Entry
  // surface, same lifecycle), but skips the script registry — its
  // `changed` emission is silent on the outbound channel and it
  // can't be addressed by abbrev. Use this at call sites where
  // multiple instances would otherwise collide on a shared abbrev
  // (DocumentGallery's per-tile rename Entry, MacroEditorWindow's
  // per-property add_entry lambda, SwatchesPanel's prompt_text
  // transient builders, …). Mirrors the Button tagged ctor added
  // in s209 m1 and the ToggleButton tagged ctor added in s209 m2.
  Entry(curvz::scripting::unregistered_t);

  curvz::scripting::ScriptValue
  invoke_leaf(std::string_view verb,
              const curvz::scripting::ScriptArgs &args) override;
  curvz::scripting::ScriptValue
  query_leaf(std::string_view property) const override;
  std::vector<std::string> leaf_verbs() const override;
  std::vector<std::string> leaf_properties() const override;

protected:
  void bind_canonical() override;
};

} // namespace curvz::widgets
