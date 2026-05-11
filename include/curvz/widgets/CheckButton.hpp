// curvz/widgets/CheckButton.hpp ──────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::CheckButton. Lifted from scriptproto/
// during s187 m3.
//
// Verbs:
//   click               → toggles active (via set_active), fires signal_toggled
//   set <bool>          → set_active(b)
//
// Queries:
//   active              → Bool
//   label               → String
//
// Emits:
//   toggled <bool>      on signal_toggled (real OR script-driven)

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/checkbutton.h>

namespace curvz::widgets {

class CheckButton
    : public curvz::scripting::ScriptableWidget<Gtk::CheckButton> {
public:
    CheckButton(std::string_view name, const Glib::ustring& label = {});

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
