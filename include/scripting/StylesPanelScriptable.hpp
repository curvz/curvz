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
//     Pre-condition: the parent inspector section must be expanded.
//     Popovers position relative to their button's on-screen
//     allocation; a collapsed section gives the kebab no allocation
//     and GTK falls back to a default location (upper-right of the
//     app window). Call pnl_styles expand_section first if the
//     section might be collapsed — m4's verb.
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
// Verbs (m4 — narrow section state):
//
//   pnl_styles expand_section
//     Opens THIS panel's parent inspector section if it's currently
//     collapsed. No-op if already open. Narrow flip — affects only
//     the "Styles" section, not its parent group or any siblings.
//
//     Implementation: invokes the apply-state closure MainWindow
//     handed the panel via set_section_state. That closure drives
//     the full cascade — body visibility, arrow glyph, project
//     field update, schedule_save — same path the user-driven
//     header-click takes.
//
//     For tests and special-case narration. Most narrated scripts
//     want focus_section (m5) instead — see below.
//
//   pnl_styles collapse_section
//     Symmetric narrow flip. Closes THIS section. No-op if already
//     closed. Same caveat as expand_section: doesn't touch the
//     parent group or siblings.
//
// Verbs (m5 — composite focus):
//
//   pnl_styles focus_section
//     "Show me the Styles section." Collapses every registered
//     inspector section, then walks the ancestor chain open
//     (outermost first). For Styles today: collapses everything,
//     opens "Content", opens "Styles". The result is an inspector
//     quiet except for the Styles panel — the right starting state
//     for a narrated walkthrough that's about to point at it.
//
//     Why a composite and not just "expand my parent + expand me":
//     the kebab popover positions relative to its button's on-
//     screen allocation. A collapsed parent group hides every
//     child section's allocation regardless of the child's own
//     open flag, so expanding only the leaf section leaves the
//     kebab unrendered and popup() falls back to a default
//     location. The chain walk handles the entire ancestor path
//     — Styles isn't special, every panel sits in a group, so the
//     verb is named for the user-visible outcome ("focus on this
//     panel") not the mechanical detail ("open ancestors").
//
//     The "collapse every other section" pass is intentional. A
//     presenter clearing the slide before pointing at the new
//     figure; narrated scripts should land on a quiet inspector
//     before drawing attention to a specific panel.
//
//   The canonical narrated prologue with m5:
//
//     pnl_styles focus_section
//     sleep 400
//     pnl_styles highlight_self
//     sleep 700
//     pnl_styles show_kebab
//     sleep 400
//     pnl_styles highlight_menuitem "create-empty"
//     sleep 600
//     pnl_styles hide_kebab
//     pnl_styles new_style
//
//   Focus the panel (quiet inspector + open the chain), pulse it
//   so the viewer registers where to look, open the kebab over its
//   now-visible button, highlight the menu item, close, fire.
//   focus_section is orthogonal to highlight_self and show_kebab —
//   the script writer sequences the beats. There's no auto-collapse
//   at the end of the script because narration often wants the
//   section to stay open while the dialog interaction plays out.
//
// Queries:
//
//   active_category       — current active category name (string).
//   active_is_app_tier    — true if the active category is app-tier.
//   kebab_visible         — true if the kebab popover is open. (m2)
//   section_open          — true if the parent inspector section is
//                           expanded. (m4) Reads m_section_open_flag
//                           which MainWindow keeps current as the user
//                           toggles the section header.
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
