// scripting/Action.hpp ───────────────────────────────────────────────────────
//
// s254 m2 — first move on Tier 2 (action wrappers). Wraps the Gio action
// surface as a Scriptable so scripts can address the actions catalogued in
// s254 m1's `tier2_action_audit.md`. One `ActionGroupScriptable` per action
// group (e.g. `win`, `styles`, `swatches`); each holds the wrapped actions
// in that group as verbs.
//
// ── The mapping ──────────────────────────────────────────────────────────────
//
// Gio's action surface uses `<group>.<name>` paths — `win.undo`,
// `styles.create-empty`, `recents.open`. The natural Scriptable shape is
// one Scriptable per group: the registry holds entries named `win`,
// `styles`, etc., and `invoke("undo", {})` on the `win` Scriptable
// dispatches to the wrapped `win.undo` action.
//
// **Why one-per-group, not one-per-action.** The registry refuses two
// live Scriptables under the same name; one-per-action would be 49 wrap-
// now Scriptables across the `win` group alone, each registered under
// names like `win.undo` (which works mechanically — registry uses
// `std::string_view` for names, dotted names lookup fine) but the
// dispatcher's verb-on-Scriptable mental model breaks down. A script
// line `win.undo` parses naturally as `<scriptable>.<verb>` =
// `win` Scriptable, `undo` verb — the same surface shape every other
// Scriptable uses (`proj save`, `objects group`, `styles.<style-iid>
// rename "new"`). One-per-group preserves that uniformity. The
// LEDGER's s253 Tier 2 row names this explicitly: "action-group-as-
// Scriptable mapping is 1:1 with existing registry."
//
// **Already-reachable and unsafe entries don't appear in the verb
// surface.** The audit's bucket B (already-reachable) entries are not
// wrapped — the canonical script surface for those outcomes is the
// existing Scriptable (`proj save`, `objects group`, etc.). Bucket C
// (unsafe / launcher / dead) entries are also not wrapped — they're
// decided-not-deferred. Both buckets stay in the Gio action surface
// for the GUI to invoke; the wrapper just doesn't expose them. An
// unknown-verb invocation on this Scriptable falls through to the
// default refusal at the invoke chain's end.
//
// ── The wrapping ─────────────────────────────────────────────────────────────
//
// Each wrapped action is held by reference (`Glib::RefPtr<Gio::SimpleAction>`)
// alongside its long-name annotation (for `widget_names_sync` parity with
// the substrate widgets, see Tier 1's pattern) and its RunContext mask.
//
// `invoke(verb, args)` looks up the verb in the action map; on hit, calls
// `m_action->activate()` — the SAME path the GUI takes when the user
// clicks the menu item. Script-driven and user-driven invocations run
// through identical callback chains. This is the "White-box reads,
// black-box writes" property from `Scriptable.hpp`.
//
// `query(property)` is currently a stub returning Null for everything;
// future per-action properties (enabled state, current-state for bool
// actions) can be added as `<verb>_enabled` / `<verb>_state` if a use
// case names them. Today's surface is invoke-only.
//
// `verbs()` returns the keys of the action map — the actual wrap-now
// subset. `properties()` returns empty.
//
// `context_mask(verb)` returns the per-verb mask stored alongside each
// wrapped action; unknown verbs fall through to `ctx::all_three`, matching
// the Scriptable base contract (the invoke body itself refuses unknown
// verbs).
//
// ── The helper ───────────────────────────────────────────────────────────────
//
// `add_scripted_action` is the per-site helper that replaces the existing
// 3-line `Gio::SimpleAction::create + signal_activate + add_action`
// pattern. Sites that were:
//
//   auto act_undo = Gio::SimpleAction::create("undo");
//   act_undo->signal_activate().connect(
//       [this](const Glib::VariantBase &) { on_undo(); });
//   add_action(act_undo);
//
// become:
//
//   add_scripted_action(this, m_action_group_scriptable.get(),
//                       "undo", "act_undo",
//                       [this]{ on_undo(); });
//
// The two strings parallel the substrate-widget pattern (abbrev +
// long-name) per fork 9's default in `tier2_action_audit.md`. The
// first argument is the `Gio::ActionMap*` to wire the action into —
// `this` from a MainWindow method works because Gtk::ApplicationWindow
// inherits Gio::ActionMap. Panel sites pass `m_actions.get()`.
//
// **Lifetime.** ActionGroupScriptable is owned by MainWindow as a
// `unique_ptr` member, constructed BEFORE the action declarations in
// the zones file so the helper has a live group_scriptable pointer to
// register actions against. Same lifetime shape as ProjScriptable and
// ExportScriptable.
//
// **m1 first batch (s254 m2):** 5 wrap-now actions in the `win` group
// — `undo`, `redo`, `select-all`, `deselect-all`, `zoom-fit`. All
// parameterless; all under the default `Scripter | TestRunner` mask
// (matches recent m5b convention — wrap-now actions are script-
// addressable from both the in-app DSL playground and the future CLI
// test harness, but excluded from Macro per the same posture as the
// proj surface). Cap on 49 wrap-now opens; remaining 44 land across
// subsequent m3-m6 sessions by menu domain.

#pragma once
#include "scripting/Scriptable.hpp"
#include "scripting/ScriptValue.hpp"
#include "scripting/RunContext.hpp"

#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/refptr.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace curvz::scripting {

class ActionGroupScriptable : public Scriptable {
public:
    // Registers as `name` (typically "win", "styles", "swatches"). The
    // host is non-owning; the Scriptable lives as a member of the
    // widget that owns the action group, so the host pointer is valid
    // for the Scriptable's entire lifetime.
    explicit ActionGroupScriptable(std::string_view name);
    ~ActionGroupScriptable() override = default;

    // Add an already-constructed SimpleAction to the wrapper's verb
    // surface. Called by `add_scripted_action`; the helper builds the
    // action with its callback, adds it to the host's action group,
    // and then registers the wrapper-side mapping here.
    //
    // `verb` is the action name (e.g. "undo") — same string Gio uses
    // for the in-group lookup. `long_name` is the abbrev-parallel
    // annotation (e.g. "act_undo") for widget_names_sync parity.
    // `mask` is the RunContext mask; defaults to Scripter | TestRunner
    // per the wrap-now default posture (see header for the rationale).
    void register_action(std::string_view verb,
                         Glib::RefPtr<Gio::SimpleAction> action,
                         std::string_view long_name,
                         RunContextMask mask =
                             ctx::Scripter | ctx::TestRunner);

    // Scriptable contract.
    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;
    RunContextMask context_mask(std::string_view verb) const override;

private:
    struct Entry {
        Glib::RefPtr<Gio::SimpleAction> action;
        std::string long_name;
        RunContextMask mask;
    };
    std::unordered_map<std::string, Entry> m_actions;
};

// Helper: build + wire + register in one call. Replaces the three-
// line Gio::SimpleAction::create / signal_activate / add_action
// pattern at every wrap-now site.
//
// `action_map` is the Gio::ActionMap the action wires into. Most win.*
// sites pass `this` from a MainWindow method (Gtk::ApplicationWindow
// inherits Gio::ActionMap). Panel sites pass `m_actions.get()` — the
// SimpleActionGroup the panel inserts under a dotted prefix.
// `group_scriptable` is the ActionGroupScriptable the wrapper-side
// mapping registers against.
//
// `callback` is the parameterless body the action runs on activation
// — same lambda content that used to go into `signal_activate.connect`.
// The helper handles the signature adaptation (the underlying signal
// hands the lambda a `const Glib::VariantBase&` which the helper
// swallows).
//
// Returns the SimpleAction RefPtr so callers that need to hold the
// reference (for set_enabled toggles, etc.) can. Most sites discard
// it.
//
// Mask defaults to `ctx::Scripter | ctx::TestRunner` per the wrap-now
// posture; sites with stricter requirements pass it explicitly.
Glib::RefPtr<Gio::SimpleAction>
add_scripted_action(Gio::ActionMap* action_map,
                    ActionGroupScriptable* group_scriptable,
                    std::string_view verb,
                    std::string_view long_name,
                    std::function<void()> callback,
                    RunContextMask mask =
                        ctx::Scripter | ctx::TestRunner);

} // namespace curvz::scripting
