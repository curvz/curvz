// scripting/ScriptableRegistry.cpp ───────────────────────────────────────────
//
// Singleton + the small piece of Scriptable that ties to the registry
// (ctor/dtor and emit). Kept together so the registration discipline
// lives in one place.
//
// Lifted from scriptproto/ during s186 m2 — namespace renamed,
// otherwise identical.

#include "scripting/Scriptable.hpp"
#include "scripting/ScriptableRegistry.hpp"

#include <algorithm>
#include <stdexcept>

namespace curvz::scripting {

// ── Scriptable ───────────────────────────────────────────────────────────────

Scriptable::Scriptable(std::string_view name)
    : m_name(name), m_registered(true) {
    ScriptableRegistry::instance().register_object(m_name, this);
}

// s209 m1 — tagged ctor for substrate widgets that opt out of
// registration. See Scriptable.hpp's tag-block comment for the
// audit-driven rationale (Reading-C-blocked sites: per-show dialogs,
// per-click popovers, per-loop helper-multipliers).
//
// m_name stays empty; an unregistered Scriptable is unreachable from
// `find`, can never collide with a registered name, and emit() is a
// no-op so the canonical signal handler runs harmlessly.
Scriptable::Scriptable(unregistered_t)
    : m_name(), m_registered(false) {
    // No registry touch by design.
}

Scriptable::~Scriptable() {
    if (m_registered) {
        ScriptableRegistry::instance().unregister_object(m_name);
    }
}

// s191 m7 — synchronous unregister for the subtree-clear path.
// See Scriptable.hpp's comment block for the lifecycle rationale.
// Idempotent: erase-on-missing is a no-op, so the dtor's
// unregister_object after force_unregister is safe.
//
// s209 m1 — also a no-op for unregistered instances. The
// force_unregister_subtree pump (curvz_utils) walks any container
// that may hold substrate widgets; unregistered children must
// pass through without touching the registry.
void Scriptable::force_unregister() {
    if (m_registered) {
        ScriptableRegistry::instance().unregister_object(m_name);
    }
}

void Scriptable::emit(std::string_view event, const ScriptValue& payload) {
    // s209 m1: unregistered instances are silent on the outbound
    // channel. Leaves still wire their canonical signal handler the
    // same way — keeping the leaf shape uniform — but the resulting
    // emit() call short-circuits here. No subscriber can be expecting
    // events from an unregistered (and therefore unaddressable)
    // instance, so dropping the event is the correct behaviour.
    if (!m_registered) return;
    ScriptableRegistry::instance().emit(m_name,
                                        std::string(event),
                                        payload);
}

// ── ScriptableRegistry ───────────────────────────────────────────────────────

ScriptableRegistry& ScriptableRegistry::instance() {
    static ScriptableRegistry r;
    return r;
}

void ScriptableRegistry::register_object(const std::string& name,
                                          Scriptable* obj) {
    auto [it, inserted] = m_objects.emplace(name, obj);
    if (!inserted) {
        throw std::runtime_error(
            "curvz::scripting: duplicate Scriptable name '" + name + "'");
    }
}

void ScriptableRegistry::unregister_object(const std::string& name) {
    m_objects.erase(name);
}

Scriptable* ScriptableRegistry::find(std::string_view name) const {
    auto it = m_objects.find(std::string(name));
    return it == m_objects.end() ? nullptr : it->second;
}

std::vector<std::string> ScriptableRegistry::all_names() const {
    std::vector<std::string> names;
    names.reserve(m_objects.size());
    for (const auto& [k, _] : m_objects) names.push_back(k);
    std::sort(names.begin(), names.end());
    return names;
}

void ScriptableRegistry::subscribe(Subscriber sub) {
    m_subscribers.push_back(std::move(sub));
}

void ScriptableRegistry::emit(const std::string& name,
                               const std::string& event,
                               const ScriptValue& payload) {
    for (auto& sub : m_subscribers) sub(name, event, payload);
}

} // namespace curvz::scripting
