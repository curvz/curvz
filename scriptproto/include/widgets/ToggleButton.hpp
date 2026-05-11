// ToggleButton.hpp ───────────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::ToggleButton.
//
// Verbs:
//   click               → toggles active, fires signal_clicked
//   set <bool>          → set_active(bool), fires signal_toggled
//
// Queries:
//   active              → Bool
//   label               → String (button label text, "" if icon-only)
//
// Emits:
//   toggled <bool>      on signal_toggled (real OR script-driven)

#pragma once
#include "ScriptableWidget.hpp"
#include <gtkmm/togglebutton.h>

namespace scriptproto {

class ToggleButton : public ScriptableWidget<Gtk::ToggleButton> {
public:
    ToggleButton(std::string_view name, const Glib::ustring& label = {});

    ScriptValue invoke(std::string_view verb,
                        const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace scriptproto
