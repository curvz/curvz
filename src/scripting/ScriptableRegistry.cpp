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

Scriptable::Scriptable(std::string_view name) : m_name(name) {
    ScriptableRegistry::instance().register_object(m_name, this);
}

Scriptable::~Scriptable() {
    ScriptableRegistry::instance().unregister_object(m_name);
}

void Scriptable::emit(std::string_view event, const ScriptValue& payload) {
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
