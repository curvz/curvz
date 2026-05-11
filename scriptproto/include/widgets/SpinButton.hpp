// SpinButton.hpp ─────────────────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::SpinButton.
//
// Verbs:
//   set <number>        → set_value, fires signal_value_changed
//   step <int>          → call spin(STEP_FORWARD/BACKWARD, |n|) per direction
//
// Queries:
//   value               → Double
//   min                 → Double
//   max                 → Double
//
// Emits:
//   value_changed <number>   on signal_value_changed

#pragma once
#include "ScriptableWidget.hpp"
#include <gtkmm/spinbutton.h>

namespace scriptproto {

class SpinButton : public ScriptableWidget<Gtk::SpinButton> {
public:
    SpinButton(std::string_view name,
                double min, double max, double step,
                int digits = 0);

    ScriptValue invoke(std::string_view verb,
                        const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace scriptproto
