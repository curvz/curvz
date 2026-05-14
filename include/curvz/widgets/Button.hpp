// curvz/widgets/Button.hpp ───────────────────────────────────────────────────
//
// Scriptable wrapper around Gtk::Button (non-toggling). Lifted from
// scriptproto/ during s187 m3.
//
// Verbs:
//   click               → activate(), fires signal_clicked
//
// Queries:
//   label               → String (button label text, "" if icon-only)
//
// Emits:
//   clicked             on signal_clicked (real OR script-driven)
//
// Note on coexistence with curvz::utils::set_name:
//   The wrapper's scriptable name (the abbrev — e.g. "tb_mb" for the
//   macro button) lives in the script registry. The GTK widget-name set
//   by curvz::utils::set_name(*this, "tb_mb", "main_toolbar_macro_btn")
//   is the same one as before and still applies. Both naming systems
//   are independent and coexist; the scriptable name is canonically
//   the abbrev so the existing widget_names.db serves as the long-name
//   resolver for human-readable script addressing.

#pragma once
#include "scripting/ScriptableWidget.hpp"
#include <gtkmm/button.h>

namespace curvz::widgets {

class Button
    : public curvz::scripting::ScriptableWidget<Gtk::Button> {
public:
    Button(std::string_view name, const Glib::ustring& label = {});

    // s209 m1 — unregistered substrate Button. The widget IS-A
    // substrate Button (same universal verbs, same Gtk::Button
    // surface, same lifecycle), but skips the script registry — its
    // `clicked` emission is silent on the outbound channel and it
    // can't be addressed by abbrev. Use this at call sites where
    // multiple instances would otherwise collide on a shared abbrev
    // (ContextBar's `add_btn`, Ruler's per-unit popover buttons,
    // about-dialog Close buttons, …).
    Button(curvz::scripting::unregistered_t,
           const Glib::ustring& label = {});

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
