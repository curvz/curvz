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

    // s211 m2 — unregistered substrate SpinButton (Adjustment-taking
    // form). The widget IS-A substrate SpinButton (same universal
    // verbs, same Gtk::SpinButton surface, same lifecycle), but skips
    // the script registry — its `value_changed` emission is silent on
    // the outbound channel and it can't be addressed by abbrev. Use
    // this at call sites where multiple instances would otherwise
    // collide on a shared abbrev (MacroEditorWindow's per-property
    // `add_spin` lambda, PropertiesPanel's per-row spin helpers if
    // they ever land here, …). Mirrors the Button ctor added in
    // s209 m1, the ToggleButton ctor added in s209 m2, and the Entry
    // ctor added in s211 m1. The (min, max, step, digits) form
    // doesn't get a tagged ctor yet — first need is the Adjustment
    // form, and the leaf shape is uniform via perfect forwarding.
    SpinButton(curvz::scripting::unregistered_t,
               Glib::RefPtr<Gtk::Adjustment> adj,
               double climb_rate, int digits = 0);

    curvz::scripting::ScriptValue invoke_leaf(
            std::string_view verb,
            const curvz::scripting::ScriptArgs& args) override;
    curvz::scripting::ScriptValue query_leaf(
            std::string_view property) const override;
    std::vector<std::string> leaf_verbs()      const override;
    std::vector<std::string> leaf_properties() const override;

protected:
    void bind_canonical() override;
};

} // namespace curvz::widgets
