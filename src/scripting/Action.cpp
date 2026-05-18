// scripting/Action.cpp ───────────────────────────────────────────────────────
//
// s254 m2 — implementation of ActionGroupScriptable and the
// `add_scripted_action` helper.
//
// See `scripting/Action.hpp` for the design block, the helper-replaces-
// three-line-pattern motivation, and the per-verb context_mask posture.

#include "scripting/Action.hpp"
#include "CurvzLog.hpp"

#include <stdexcept>

namespace curvz::scripting {

ActionGroupScriptable::ActionGroupScriptable(std::string_view name)
    : Scriptable(name) {
    // Registry registration happens in the Scriptable base ctor.
    // MainWindow holds us as a member; the registry entry lives for
    // the window's lifetime. Future panel-side instances will live
    // as members of their respective panels.
}

void ActionGroupScriptable::register_action(
        std::string_view verb,
        Glib::RefPtr<Gio::SimpleAction> action,
        std::string_view long_name,
        RunContextMask mask) {
    // No collision check — if two register_action calls land on the
    // same verb name within a group, the second overwrites the first.
    // That matches Gio's add_action semantics (calling add_action
    // twice with the same name silently replaces the first), so the
    // wrapper-side map stays consistent with the action-map side.
    m_actions[std::string(verb)] = Entry{
        std::move(action),
        std::string(long_name),
        mask,
    };
}

ScriptValue ActionGroupScriptable::invoke(std::string_view verb,
                                          const ScriptArgs& args) {
    LOG_INFO("DIAG ActionGroupScriptable::invoke group='{}' verb='{}' "
             "args.size={}",
             scriptable_name(), std::string(verb), args.size());

    auto it = m_actions.find(std::string(verb));
    if (it == m_actions.end()) {
        // Verb not in the wrap-now subset OR a typo. The error message
        // is shaped the same way the existing Scriptables shape theirs
        // — "<scriptable> <verb>: <reason>" — so script error lines
        // are consistently parseable.
        throw std::runtime_error(
            std::string(scriptable_name()) + " " + std::string(verb)
            + ": unknown verb (not in wrap-now subset; see "
              "tier2_action_audit.md)");
    }

    // m1 first batch is all parameterless. When parameterised verbs
    // land (step-repeat etc., per fork 5 of the audit), this branch
    // splits on the action's parameter type (`get_parameter_type()`
    // returns null for parameterless actions, the VariantType for
    // parameterised ones) and threads args through. For now, refuse
    // any args at all — silently dropping them would mask scripts
    // that think they're passing parameters when they aren't.
    if (!args.empty()) {
        throw std::runtime_error(
            std::string(scriptable_name()) + " " + std::string(verb)
            + ": this verb takes no arguments");
    }

    // Activate the underlying Gio action. Goes through the SAME
    // callback chain a menu click does — the wrapper is transparent
    // at the dispatch layer. "White-box reads, black-box writes" per
    // Scriptable.hpp's contract block: scripts and users invoke the
    // same code path.
    it->second.action->activate();

    return ScriptValue::null();
}

ScriptValue ActionGroupScriptable::query(std::string_view /*property*/) const {
    // No properties surfaced today. Future per-action properties
    // (enabled state, current-state for bool actions) land if a use
    // case names them; the audit's m2 doesn't require them yet.
    return ScriptValue::null();
}

std::vector<std::string> ActionGroupScriptable::verbs() const {
    std::vector<std::string> result;
    result.reserve(m_actions.size());
    for (const auto& [name, _] : m_actions) {
        result.push_back(name);
    }
    return result;
}

std::vector<std::string> ActionGroupScriptable::properties() const {
    // See query() — no properties today.
    return {};
}

RunContextMask
ActionGroupScriptable::context_mask(std::string_view verb) const {
    auto it = m_actions.find(std::string(verb));
    if (it == m_actions.end()) {
        // Unknown verb: fall through to ctx::all_three per the
        // Scriptable base contract. The invoke() body refuses the
        // unknown verb directly; the dispatcher's mask check is
        // separate from the invoke-time existence check.
        return ctx::all_three;
    }
    return it->second.mask;
}

// ──────────────────────────────────────────────────────────────────────────
// add_scripted_action helper
// ──────────────────────────────────────────────────────────────────────────

Glib::RefPtr<Gio::SimpleAction>
add_scripted_action(Gio::ActionMap* action_map,
                    ActionGroupScriptable* group_scriptable,
                    std::string_view verb,
                    std::string_view long_name,
                    std::function<void()> callback,
                    RunContextMask mask) {
    auto action = Gio::SimpleAction::create(std::string(verb));

    // Adapt the no-arg callback to the signal's signature. The signal
    // hands a const VariantBase& which is empty for parameterless
    // actions — the helper swallows it so callers write `[]{ ... }`
    // instead of `[](const Glib::VariantBase&){ ... }`.
    action->signal_activate().connect(
        [cb = std::move(callback)](const Glib::VariantBase&) { cb(); });

    // Two-side registration: Gio's action-map (so menus / accels /
    // GTK dispatch find it) AND the ActionGroupScriptable (so the
    // script dispatcher finds it via the registry). Both views
    // reference the same SimpleAction object via the RefPtr; the
    // single-source-of-truth lives in the SimpleAction itself.
    action_map->add_action(action);
    group_scriptable->register_action(verb, action, long_name, mask);

    return action;
}

} // namespace curvz::scripting
