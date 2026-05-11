// ScriptableRegistry.hpp ─────────────────────────────────────────────────────
//
// Process-wide singleton mapping scriptable names to their objects.
//
// Two responsibilities:
//   1. Address book — register_object / unregister_object / find by name.
//      Subscribers (the listener) look up an object by name to dispatch.
//   2. Event bus — subscribe a listener for the unified outbound stream.
//      Both real user interactions and script-driven invocations route
//      through emit() and reach every subscriber identically. This is
//      the bidirectional-channel insight in code.
//
// The registry is intentionally minimal. No threading, no async, no
// metadata indexing. m1's surface is enough; later milestones can
// extend it without breaking the early test scripts.

#pragma once
#include "ScriptValue.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scriptproto {

class Scriptable;

class ScriptableRegistry {
public:
    static ScriptableRegistry& instance();

    // Called from Scriptable's ctor/dtor. Throws on duplicate name —
    // the project's naming discipline forbids name collisions, so
    // hitting this is a programmer error worth crashing on.
    void register_object(const std::string& name, Scriptable* obj);
    void unregister_object(const std::string& name);

    // Lookup. Returns nullptr if not found; the listener emits a
    // structured "unknown object" error in that case.
    Scriptable* find(std::string_view name) const;

    // Names sorted alphabetically — for help / list / closest-match
    // diagnostics.
    std::vector<std::string> all_names() const;

    // Subscriber bus. Each subscribed callback receives every emit()
    // from every registered Scriptable. The listener subscribes to
    // produce its outbound trace; tests can subscribe to verify event
    // ordering matches the dispatch sequence.
    using Subscriber = std::function<void(const std::string& /*name*/,
                                          const std::string& /*event*/,
                                          const ScriptValue& /*payload*/)>;
    void subscribe(Subscriber sub);

    // Called by Scriptable::emit. Routes to every subscriber.
    void emit(const std::string& name,
              const std::string& event,
              const ScriptValue& payload);

private:
    ScriptableRegistry() = default;
    std::unordered_map<std::string, Scriptable*> m_objects;
    std::vector<Subscriber>                      m_subscribers;
};

} // namespace scriptproto
