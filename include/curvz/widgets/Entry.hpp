// curvz/widgets/Entry.hpp ────────────────────────────────────────────────────
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

class Entry
    : public curvz::scripting::ScriptableWidget<Gtk::Entry> {
public:
    explicit Entry(std::string_view name);

    curvz::scripting::ScriptValue invoke(
            std::string_view verb,
            const curvz::scripting::ScriptArgs& args) override;
    curvz::scripting::ScriptValue query(
            std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace curvz::widgets
