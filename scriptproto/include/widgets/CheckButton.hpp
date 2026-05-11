// CheckButton.hpp ────────────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::CheckButton.
//
// Verbs:
//   click               → fire signal_clicked (toggles)
//   set <bool>          → set_active(b)
//
// Queries:
//   active              → Bool
//   label               → String

#pragma once
#include "ScriptableWidget.hpp"
#include <gtkmm/checkbutton.h>

namespace scriptproto {

class CheckButton : public ScriptableWidget<Gtk::CheckButton> {
public:
    CheckButton(std::string_view name, const Glib::ustring& label = {});

    ScriptValue invoke(std::string_view verb,
                        const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace scriptproto
