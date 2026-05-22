// widgets/SpinButton.hpp ───────────────────────────────────────────────
//
// Scriptable wrapper around CurvzSpinButton. Lifted from scriptproto/
// during s187 m3.
//
// Note: Curvz has a larger custom subclass `Curvz::CurvzSpinButton`
// (unit-aware, with fluent construction). That class is NOT this one,
// but as of s219 the two no longer differ on math input — both accept
// arithmetic expressions in their entry buffer. The remaining open
// design fork from s187 (whether CurvzSpinButton should inherit from
// ScriptableWidget for script-addressability) is independent of the
// math layer and tracked separately in ARC.md.
//
// Verbs:
//   set <number>        → set_value, fires signal_value_changed.
//                         Numeric back door; bypasses the parser.
//   parse <string>      → route a string through the field's text-entry
//                         parse path (same path a typing user takes).
//                         Returns true on commit, false on refusal. The
//                         substrate's default implementation routes
//                         through the dimensionless parser (matches the
//                         widget's signal_input behaviour). Subclasses
//                         like CurvzSpinButton override the
//                         `try_parse_and_commit` hook to route through
//                         their richer (domain-aware) parser.
//                         Added s263 m6 so the conversion-calc parser
//                         has a script-addressable surface — see
//                         smoke 65 (units parser regression suite) for
//                         the design rationale.
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

class SpinButton : public curvz::scripting::ScriptableWidget<Gtk::SpinButton> {
public:
  SpinButton(std::string_view name, double min, double max, double step,
             int digits = 0);

  // s189 m2 overload — inspector call sites build an external
  // Glib::RefPtr<Gtk::Adjustment> up front (sometimes to connect
  // signals on it directly, sometimes to share it across widgets,
  // sometimes to read get_value() in lambdas captured by other UI).
  // The wrapper accepts a pre-built Adjustment and routes it through
  // CurvzSpinButton's adjustment-taking ctor via the base template's
  // perfect-forwarding. climb_rate is the canonical SpinButton
  // parameter — the same value GTK uses for set_increments(step,
  // step*10).
  SpinButton(std::string_view name, Glib::RefPtr<Gtk::Adjustment> adj,
             double climb_rate, int digits = 0);

  // s211 m2 — unregistered substrate SpinButton (Adjustment-taking
  // form). The widget IS-A substrate SpinButton (same universal
  // verbs, same CurvzSpinButton surface, same lifecycle), but skips
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
             Glib::RefPtr<Gtk::Adjustment> adj, double climb_rate,
             int digits = 0);

  curvz::scripting::ScriptValue
  invoke_leaf(std::string_view verb,
              const curvz::scripting::ScriptArgs &args) override;
  curvz::scripting::ScriptValue
  query_leaf(std::string_view property) const override;
  std::vector<std::string> leaf_verbs() const override;
  std::vector<std::string> leaf_properties() const override;

  // s263 m6 — virtual hook for the `parse` substrate verb. Routes
  // a string through the widget's text-entry parse path. The
  // substrate's default implementation calls the dimensionless
  // parser (same shape as signal_input's parse_expr call with
  // allow_units=false / Domain::Dimensionless), clamps to the
  // adjustment's range, and commits via set_value() on success.
  // Returns true on commit, false on refusal — the caller can
  // assert against the post-call value() query for a refusal-
  // didn't-mutate signal.
  //
  // Open design fork (s187, banked): CurvzSpinButton currently
  // inherits from CurvzSpinButton directly, NOT from this substrate
  // widget. That means CurvzSpinButton instances aren't script-
  // addressable today; the rich Domain-aware parser from s263 m4-m5
  // is reachable only through the user's text-entry path. When the
  // s187 fork resolves (CurvzSpinButton inherits from this widget),
  // CurvzSpinButton will override `try_parse_and_commit` to route
  // through its richer (domain-aware) parser — same script-
  // addressable verb name, polymorphic behaviour, parser surface
  // unified across substrate and rich-widget call sites. Until then,
  // the substrate hook is dimensionless-only; the rich parser is
  // exercised through the GUI text entry but not from scripts.
  virtual bool try_parse_and_commit(const std::string &text);

protected:
  void bind_canonical() override;

private:
  // s219 — wire signal_input parser + CAPTURE-phase keystroke filter.
  // Called from each constructor after init_scriptable(). Pulls in
  // UnitSystem::parse_expr for dimensionless math expressions.
  void init_math();
};

} // namespace curvz::widgets
