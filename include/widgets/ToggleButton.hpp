// widgets/ToggleButton.hpp ─────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::ToggleButton. First widget type lifted
// into Curvz proper during s186 m2. The Node-tool button was the only
// production-side site that used it through s187; s188 m1 widened it
// to every tool button on the main toolbar via the funnel migration.
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
//
// Note on coexistence with curvz::utils::set_name:
//   The wrapper's scriptable name (e.g. "tb_nod") lives in the script
//   registry — a parallel addressing scheme. The GTK widget-name set by
//   curvz::utils::set_name(*this, "tb_nod", ...) is the same string,
//   though they live in separate stores. s188 m1's toolbar funnel
//   ensures both names always agree by construction: the funnel
//   constructs the wrapper with abbrev and calls set_name with the
//   same abbrev — no drift possible.

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/togglebutton.h>

namespace curvz::widgets {

class ToggleButton
    : public curvz::scripting::ScriptableWidget<Gtk::ToggleButton> {
public:
  ToggleButton(std::string_view name, const Glib::ustring &label = {});

  // s209 m2 — unregistered substrate ToggleButton. The widget IS-A
  // substrate ToggleButton (same universal verbs, same Gtk::ToggleButton
  // surface, same lifecycle), but skips the script registry — its
  // `toggled` emission is silent on the outbound channel and it
  // can't be addressed by abbrev. Use this at call sites where
  // multiple instances would otherwise collide on a shared abbrev
  // (ContextBar's `add_toggle`, per-loop helper-multipliers, …).
  // Mirrors the Button tagged ctor added in s209 m1.
  ToggleButton(curvz::scripting::unregistered_t,
               const Glib::ustring &label = {});

  curvz::scripting::ScriptValue
  invoke_leaf(std::string_view verb,
              const curvz::scripting::ScriptArgs &args) override;
  curvz::scripting::ScriptValue
  query_leaf(std::string_view property) const override;
  std::vector<std::string> leaf_verbs() const override;
  std::vector<std::string> leaf_properties() const override;

protected:
  void bind_canonical() override;
};

} // namespace curvz::widgets
