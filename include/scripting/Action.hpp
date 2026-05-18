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
// proj surface). Cap on 48 wrap-now opens (post s256 m1 audit
// correction — was 49 before; `win.clear` was a phantom listing);
// remaining 43 land across subsequent m3-m10 sessions by menu domain.
// s256 m2 closes the sweep to 46/48 with two open forks remaining.

// ── Stateful bool actions (s256 m2) ──────────────────────────────────────────
//
// View-toggle actions (`win.toggle-rulers`, `win.toggle-outline`) are
// `Gio::SimpleAction::create_bool` rather than `::create` — they carry a
// boolean state that the GUI surfaces as a menu-item checkmark. The
// stateful shape needs a sibling helper, `add_scripted_bool_action`, which
// builds the action with `create_bool`, wires BOTH `signal_activate` (for
// menu-click flip semantics) and `signal_change_state` (for explicit-set
// semantics), and registers the same Entry shape so the wrapper-side
// invoke dispatches uniformly.
//
// **Script-side surface (per audit fork 10 default).** One verb name,
// two invocation shapes:
//
//   win toggle-rulers          → activate() — flips current state
//   win toggle-rulers true     → change_state(true) — sets explicitly
//   win toggle-rulers false    → change_state(false) — sets explicitly
//
// The dispatcher detects arg count and bool-arg shape inside invoke().
// Zero args → activate(); one Bool arg → change_state(); any other
// arg shape → refuse.
//
// **Wrapper readback (s256 m2 default, audit fork 10 left this open).**
// The wrapper surfaces a `<verb>_state` query property — for the rulers
// verb, that's `toggle-rulers_state`. Returns `ScriptValue::boolean` of
// the action's current state. Hyphens in property names tokenize fine
// because the dispatch tokenizer is whitespace-split (see
// `ScriptListener.cpp` token splitter). The smoke's 4-phase shape (read
// initial / flip / read flipped / flip back) needs this readback to
// produce PASS observables; absent it, the smoke degrades to the same
// invoke-only conservative shape as m1 first-batch and ships 0 PASS.
//
// **Activate-vs-change_state wiring.** Gio's default for create_bool
// actions with NULL parameter type is "if no activate handler is
// connected, default-toggle via change_state." The legacy sites
// (`toggle-rulers`, `toggle-outline`) both connect activate handlers,
// so the default-toggle path is NOT in play; the helper preserves the
// same posture (connect activate explicitly) for parity. Two consequences:
//   1. The activate handler reads current state, computes `!current`,
//      and calls the setter with the inverted value. Same shape as the
//      legacy toggle-rulers handler did inline.
//   2. The change_state handler extracts the bool from the variant and
//      calls the setter with that value. No default-set path; the
//      handler IS the path.
// Both routes hand the setter the FINAL desired value. The setter is
// responsible for applying the side-effect AND for syncing the action's
// own state (`set_state(v)`) — sites that already have a signal-driven
// state-sync wire (e.g. `signal_outline_mode_changed` re-syncs the
// outline action via `lookup_action(...)->set_state(...)`) don't need
// to set_state inside the setter; sites without (rulers) do it inline.
// This matches the legacy posture verbatim.
//
// **Refusal-vs-set-on-fail.** The setter may refuse the change (e.g.
// `toggle-outline` refuses an outline→preview transition at extreme
// zoom because the drop-shadow Cairo buffer would crash the app). The
// helper does NOT pre-emptively set the action state; the setter
// owns both the side-effect AND the state-sync, so a setter that
// returns early without firing the canvas signal also leaves the
// action state at its current value. The diverged-state problem from
// a setter-that-refuses doesn't bite if the setter is the single source
// of truth for both axes.

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

    // s256 m2 — stateful bool sibling of register_action. Same shape
    // as register_action but tags the Entry as StatefulBool so invoke()
    // routes through the activate/change_state branching, and query()
    // surfaces `<verb>_state` as a Bool property. Called by
    // `add_scripted_bool_action` after the create_bool action has been
    // built and wired.
    void register_bool_action(std::string_view verb,
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
    // Two action shapes are catalogued today:
    //   - Stateless: parameterless action, invoke routes to activate()
    //     with no args, refuses any args.
    //   - StatefulBool: bool-stated action, invoke routes to activate()
    //     on zero-arg (flip current) or change_state(v) on one Bool
    //     arg (set explicitly). Query("<verb>_state") returns the
    //     current bool state.
    // Future kinds (parametrised int / string actions, per audit fork
    // 3 for `translate-dialog`) land as additional enumerators with
    // matching invoke branches.
    enum class Kind { Stateless, StatefulBool };

    struct Entry {
        Glib::RefPtr<Gio::SimpleAction> action;
        std::string long_name;
        RunContextMask mask;
        Kind kind = Kind::Stateless;
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

// s256 m2 — stateful bool variant. Builds a `Gio::SimpleAction::create_bool`
// with the given initial state, connects BOTH `signal_activate` (flip)
// and `signal_change_state` (set explicitly), and registers under the
// StatefulBool entry kind. Both signals route to `setter(desired_value)`:
//
//   - activate path: setter(!current_state)
//   - change_state path: setter(requested_value_from_variant)
//
// The setter owns the side-effect AND the action-state sync — it must
// call `action->set_state(Glib::Variant<bool>::create(v))` itself (or
// have a separate signal-driven wire that does so, like the outline-
// mode signal does today for `toggle-outline`). The helper does NOT
// touch state automatically; a setter that refuses (e.g. safety gate)
// must leave state untouched, and a setter that succeeds must update
// state. This matches the legacy posture inside the original Gio
// activate handlers verbatim.
//
// Script-side surface:
//   - `win toggle-rulers`         → activate(), flips current
//   - `win toggle-rulers true`    → change_state(true), sets explicitly
//   - `win toggle-rulers false`   → change_state(false), sets explicitly
//   - query `toggle-rulers_state` → ScriptValue::Bool of current state
//
// Returns the SimpleAction RefPtr like `add_scripted_action`; callers
// that need to hold the reference (rare for view toggles — state-sync
// goes via lookup_action by name today) can.
Glib::RefPtr<Gio::SimpleAction>
add_scripted_bool_action(Gio::ActionMap* action_map,
                         ActionGroupScriptable* group_scriptable,
                         std::string_view verb,
                         std::string_view long_name,
                         bool initial_state,
                         std::function<void(bool)> setter,
                         RunContextMask mask =
                             ctx::Scripter | ctx::TestRunner);

} // namespace curvz::scripting
