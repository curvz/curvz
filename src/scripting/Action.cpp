// scripting/Action.cpp ───────────────────────────────────────────────────────
//
// s254 m2 — implementation of ActionGroupScriptable and the
// `add_scripted_action` helper.
//
// See `scripting/Action.hpp` for the design block, the helper-replaces-
// three-line-pattern motivation, and the per-verb context_mask posture.

#include "scripting/Action.hpp"
#include "CurvzLog.hpp"

#include <glibmm/variant.h>

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
        Kind::Stateless,
    };
}

void ActionGroupScriptable::register_bool_action(
        std::string_view verb,
        Glib::RefPtr<Gio::SimpleAction> action,
        std::string_view long_name,
        RunContextMask mask) {
    // s256 m2 — same overwrite semantics as register_action; the kind
    // tag is the only difference. invoke() routes on Entry::kind, and
    // query() exposes `<verb>_state` for StatefulBool entries.
    m_actions[std::string(verb)] = Entry{
        std::move(action),
        std::string(long_name),
        mask,
        Kind::StatefulBool,
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

    const Entry& entry = it->second;

    if (entry.kind == Kind::StatefulBool) {
        // s256 m2 — stateful bool routing. Two valid invocation shapes:
        //   - 0 args: activate() — flips current state.
        //   - 1 Bool arg: change_state(v) — sets explicitly.
        // Anything else refuses.
        if (args.empty()) {
            entry.action->activate();
            return ScriptValue::null();
        }
        if (args.size() == 1 && args[0].kind == ValueKind::Bool) {
            entry.action->change_state(
                Glib::Variant<bool>::create(args[0].b));
            return ScriptValue::null();
        }
        throw std::runtime_error(
            std::string(scriptable_name()) + " " + std::string(verb)
            + ": stateful bool verb takes 0 args (toggle) "
              "or 1 bool arg (set explicitly)");
    }

    // Stateless action. m1 first batch is all parameterless. When
    // parameterised verbs land (translate-dialog etc., per fork 3 of
    // the audit), this branch splits on the action's parameter type
    // and threads args through. For now, refuse any args at all —
    // silently dropping them would mask scripts that think they're
    // passing parameters when they aren't.
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
    entry.action->activate();

    return ScriptValue::null();
}

ScriptValue ActionGroupScriptable::query(std::string_view property) const {
    // s256 m2 — `<verb>_state` query for stateful bool actions. The
    // property name is the verb suffixed with `_state` (underscore-
    // separated to avoid colliding with hyphenated verb names like
    // `toggle-rulers`; the dispatch tokenizer is whitespace-split so
    // `toggle-rulers_state` is one token cleanly). If the verb prefix
    // refers to a StatefulBool entry, return its current state as a
    // Bool; otherwise null.
    constexpr std::string_view kStateSuffix = "_state";
    if (property.size() > kStateSuffix.size()
        && property.substr(property.size() - kStateSuffix.size())
               == kStateSuffix) {
        std::string verb(property.substr(
            0, property.size() - kStateSuffix.size()));
        auto it = m_actions.find(verb);
        if (it != m_actions.end()
            && it->second.kind == Kind::StatefulBool) {
            bool current = false;
            it->second.action->get_state(current);
            return ScriptValue::boolean(current);
        }
    }

    // No other properties surfaced today. Future per-action properties
    // (enabled state, parameterised verb introspection) land if a use
    // case names them.
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
    // s256 m2 — `<verb>_state` for every StatefulBool entry. Stateless
    // entries have no readable property today; future per-action
    // properties land here if a use case names them.
    std::vector<std::string> result;
    for (const auto& [name, entry] : m_actions) {
        if (entry.kind == Kind::StatefulBool) {
            result.push_back(name + "_state");
        }
    }
    return result;
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

// ──────────────────────────────────────────────────────────────────────────
// add_scripted_bool_action helper (s256 m2)
// ──────────────────────────────────────────────────────────────────────────

Glib::RefPtr<Gio::SimpleAction>
add_scripted_bool_action(Gio::ActionMap* action_map,
                         ActionGroupScriptable* group_scriptable,
                         std::string_view verb,
                         std::string_view long_name,
                         bool initial_state,
                         std::function<void(bool)> setter,
                         RunContextMask mask) {
    auto action = Gio::SimpleAction::create_bool(
        std::string(verb), initial_state);

    // Activate path — menu-click semantic. The activate signal for a
    // bool-stated action with no parameter type carries no value; we
    // read the current state and flip. The setter receives the FINAL
    // desired value and owns both side-effect and state-sync (see
    // Action.hpp header for the rationale).
    action->signal_activate().connect(
        [act = action, set = setter](const Glib::VariantBase&) {
            bool current = false;
            act->get_state(current);
            set(!current);
        });

    // change_state path — explicit-set semantic. The value is the
    // requested new state; pass it straight to the setter. If no
    // setter were connected to this signal, Gio's default behaviour
    // would call set_state() with the requested value AND skip the
    // side-effect — diverging the action's state from the world.
    // Connecting the setter here makes the wrapper the canonical
    // path for explicit-set regardless of who initiated it (script
    // via change_state(), keyboard binding via lookup_action +
    // change_state, etc.).
    action->signal_change_state().connect(
        [set = std::move(setter)](const Glib::VariantBase& v) {
            // The variant is the requested new state. Gio guarantees
            // the type matches the action's state type (Bool here);
            // if the caller passes a wrong-type variant, change_state
            // doesn't emit. Safe to cast.
            auto vb = Glib::VariantBase::cast_dynamic<Glib::Variant<bool>>(v);
            set(vb.get());
        });

    // Two-side registration. Same shape as add_scripted_action: the
    // action goes into Gio's action map (for menus / accels) AND
    // into the wrapper's Entry table (for script dispatch). The Entry
    // is tagged StatefulBool so invoke() routes through the activate-
    // vs-change_state branching at script call time.
    action_map->add_action(action);
    group_scriptable->register_bool_action(verb, action, long_name, mask);

    return action;
}

} // namespace curvz::scripting
