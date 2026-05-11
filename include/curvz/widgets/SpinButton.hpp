// curvz/widgets/SpinButton.hpp ───────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::SpinButton. Lifted from scriptproto/
// during s187 m3.
//
// Note: Curvz has a larger custom subclass `Curvz::CurvzSpinButton`
// (unit-aware, with fluent construction). That class is NOT this one —
// see ARC.md for the open design fork on whether CurvzSpinButton should
// absorb the script substrate via inheritance from ScriptableWidget,
// or whether a parallel script wrapper is the right shape. This plain
// wrapper is available as a building block in the meantime.
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
//   value_changed <number>   on signal_value_changed (real OR script-driven)

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/spinbutton.h>

namespace curvz::widgets {

class SpinButton
    : public curvz::scripting::ScriptableWidget<Gtk::SpinButton> {
public:
    SpinButton(std::string_view name,
                double min, double max, double step,
                int digits = 0);

    // s189 m2 overload — inspector call sites build an external
    // Glib::RefPtr<Gtk::Adjustment> up front (sometimes to connect
    // signals on it directly, sometimes to share it across widgets,
    // sometimes to read get_value() in lambdas captured by other UI).
    // The wrapper accepts a pre-built Adjustment and routes it through
    // Gtk::SpinButton's adjustment-taking ctor via the base template's
    // perfect-forwarding. climb_rate is the canonical SpinButton
    // parameter — the same value GTK uses for set_increments(step,
    // step*10).
    SpinButton(std::string_view name,
                Glib::RefPtr<Gtk::Adjustment> adj,
                double climb_rate, int digits = 0);

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
