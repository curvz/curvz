// Entry.hpp ──────────────────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::Entry.
//
// Verbs:
//   set "<text>"        → set_text, fires signal_changed
//   clear               → equivalent to set ""
//
// Queries:
//   text                → String
//
// Emits:
//   changed <text>      on signal_changed

#pragma once
#include "ScriptableWidget.hpp"
#include <gtkmm/entry.h>

namespace scriptproto {

class Entry : public ScriptableWidget<Gtk::Entry> {
public:
    explicit Entry(std::string_view name);

    ScriptValue invoke(std::string_view verb,
                        const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace scriptproto
