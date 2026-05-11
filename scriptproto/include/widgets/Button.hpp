// Button.hpp ─────────────────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::Button (non-toggling).
//
// Verbs:
//   click               → emits signal_clicked
//
// Queries:
//   label               → String
//
// Emits:
//   clicked             on signal_clicked

#pragma once
#include "ScriptableWidget.hpp"
#include <gtkmm/button.h>

namespace scriptproto {

class Button : public ScriptableWidget<Gtk::Button> {
public:
    Button(std::string_view name, const Glib::ustring& label = {});

    ScriptValue invoke(std::string_view verb,
                        const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace scriptproto
