// curvz/widgets/ToggleButton.hpp ─────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::ToggleButton. First widget type lifted
// into Curvz proper during s186 m2. The Node-tool button is the only
// production-side site that uses it for m2; m3 widens the migration
// across the rest of the Toolbar's toggle buttons.
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
//   The wrapper's scriptable name (e.g. "tool.node") lives in the
//   script registry — a parallel addressing scheme. The GTK widget-
//   name set by curvz::utils::set_name(*this, "tb_nod", ...) is the
//   same one as before and still applies. Both naming systems are
//   independent and coexist.

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/togglebutton.h>

namespace curvz::widgets {

class ToggleButton
    : public curvz::scripting::ScriptableWidget<Gtk::ToggleButton> {
public:
    ToggleButton(std::string_view name, const Glib::ustring& label = {});

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
