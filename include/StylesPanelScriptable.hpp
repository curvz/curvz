// scripting/StylesPanelScriptable.hpp ────────────────────────────────────────
//
// s202 m1 — first concrete PanelScriptable. Wraps StylesPanel and
// exposes its user-outcome verbs to the script under abbrev
// `pnl_styles`. The panel constructs an instance of this in its ctor
// (behind CURVZ_DIAGNOSTIC) and holds it as a member; registration
// and unregistration follow panel lifetime automatically.
//
// Verbs (m1, post-fix):
//
//   pnl_styles new_style
//     Invokes the panel's "+ New style" outcome — the same one the
//     kebab menu's "New style" item produces. Implementation calls
//     StylesPanel::action_create_empty() through the friend
//     declaration. Returns null on success.
//
//   pnl_styles expand_category "<name>"
//     Switches the panel's active category to <name>. Walks the
//     panel's m_category_order vector to resolve the name AND its
//     tier (app vs user), so callers don't need to know which tier
//     a category belongs to. On a name miss, no-op — the panel stays
//     where it was. Use the `has_category` query to verify presence
//     before relying on the switch.
//
//     Why not use StylesPanel::set_active_category directly:
//     set_active_category is the "restore-saved-state" entry point;
//     if its (name, tier) pair isn't found it falls back to index 0
//     of the category list (typically "Built-in" on a fresh project),
//     overwriting m_active_category. That fallback is correct for
//     stale-project-state recovery but wrong for a scripted switch
//     where the script's intent is "go there if it exists." The
//     Scriptable does the lookup itself and only invokes the setter
//     on a hit.
//
//   pnl_styles highlight_self [ms]
//     Adds the .demo-highlight CSS class to the panel's root widget
//     and removes it `ms` milliseconds later (default 600). Used as
//     the "look here next" pre-step in narrated scripts. Optional
//     int arg; non-int or negative is treated as the default.
//
// Verbs (m2 — kebab + menuitem demonstration):
//
//   pnl_styles show_kebab
//     Pops up the kebab MenuButton's popover programmatically. Same
//     popover the user sees when they click the kebab. Idempotent:
//     opens-if-closed, no-op if already open. The popover's children
//     (the model buttons for "New style", "New style from selection",
//     etc.) construct after popup() returns; highlight_menuitem
//     handles that timing internally so the script doesn't have to.
//
//   pnl_styles highlight_menuitem "<action-suffix>"
//     Adds the .demo-highlight CSS class to the kebab popover's model
//     button whose action-name matches "styles.<action-suffix>". The
//     argument is the action suffix (e.g. "create-empty"), not the
//     menu label — labels are subject to wording / localisation; the
//     action name is the stable identifier. Auto-removes on the same
//     600ms timeout as highlight_self.
//
//     Lifetime defense: the popover destroys its model-button children
//     on popdown. If a script calls hide_kebab before the 600ms
//     timeout fires, the helper's lambda would dereference a
//     destroyed widget. hide_kebab proactively cancels any in-flight
//     highlight to close that window.
//
//     If the popover isn't open when the verb fires, the script
//     should call show_kebab first; without an open popover there's
//     no model button to highlight (no-op, no error — the timeout
//     just never schedules).
//
//   pnl_styles hide_kebab
//     Closes the kebab popover. Idempotent. Cancels any in-flight
//     highlight on a model button first (those widgets are about to
//     be destroyed).
//
//   The narrated kebab dance composes these into the visual prologue:
//
//     pnl_styles show_kebab
//     sleep 400
//     pnl_styles highlight_menuitem "create-empty"
//     sleep 600
//     pnl_styles hide_kebab
//     pnl_styles new_style
//
//   Show the popover, let it settle, highlight the target item, hold
//   the highlight long enough to be read, close, fire. new_style is
//   kept orthogonal to the kebab verbs (rather than auto-closing the
//   kebab) so the script-writer controls the beats explicitly.
//
// Queries (m1, post-fix):
//
//   active_category       — current active category name (string).
//   active_is_app_tier    — true if the active category is app-tier.
//
//   No has_category query: expand_category is idempotent (no-op on
//   miss), so a test verifies presence indirectly via the post-
//   condition "after expand_category X, active_category == X". On a
//   miss the assert fails, which is the right test outcome. An
//   explicit has_category would need an arg-bearing query() (today
//   it's name-only); deferred until a second arg-bearing use case
//   surfaces.
//
// Verbs explicitly NOT shipping:
//
//   collapse_all — the handoff proposed this as symmetric to
//     expand_category, but StylesPanel always shows exactly one
//     category (the dropdown chooser idiom). There is no "nothing
//     selected" state to collapse TO; mapping it to "switch to
//     (uncategorised)" works on a project with uncategorised user
//     styles but silently does nothing on a fresh project where no
//     user tier exists. Rather than ship a verb whose meaning depends
//     on project state, drop it. Closing a narrated chapter can
//     simply expand the next category — the visible state change
//     reads cleanly without an explicit close.
//
// Universal: hide/show/visible from ScriptableWidget DON'T apply here
// — PanelScriptable inherits Scriptable directly, not the templated
// widget mixin. If a future need surfaces ("hide the whole panel"),
// add the verb on PanelScriptable's base class.
//
// Why a separate header from the panel itself:
//
//   Diagnostic-only code shouldn't bloat the panel's production
//   header. Keeping the Scriptable subclass in its own file means
//   production builds compile only the panel without ever pulling in
//   scripting/Scriptable.hpp or its includes. The cost is one tiny
//   header pair; the benefit is a clean production surface.

#pragma once
#include "scripting/PanelScriptable.hpp"
#include "scripting/ScriptValue.hpp"

#include <string>
#include <vector>

namespace Curvz {
class StylesPanel;  // fwd — implementation includes the full header
}

namespace curvz::scripting {

class StylesPanelScriptable : public PanelScriptable {
public:
    // Non-owning. The panel owns this Scriptable as a member, so the
    // panel outlives the Scriptable by construction. Registers under
    // abbrev "pnl_styles" via the base ctor.
    explicit StylesPanelScriptable(Curvz::StylesPanel* panel);
    ~StylesPanelScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

private:
    Curvz::StylesPanel* m_panel;  // non-owning back-pointer
};

} // namespace curvz::scripting
