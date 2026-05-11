// Scriptable.hpp ─────────────────────────────────────────────────────────────
//
// The mixin every scriptable widget inherits. Mandates a name at
// construction (no default ctor), self-registers in the registry on
// construction, self-unregisters on destruction.
//
// invoke() is the write path — verbs that the user could trigger by
// interacting with the widget. The default handler bodies dispatch
// through the canonical GTK API so a script-driven `click` and a real
// user click run through the exact same code.
//
// query() is the read path — properties the user could observe. The
// implementation reads canonical GTK accessors (get_active, get_text,
// get_value, etc.). White-box reads, black-box writes.
//
// emit() is the outbound side of the bidirectional channel. The widget
// subscribes its canonical signal (signal_toggled, signal_changed, …)
// to its own emit() so real interactions broadcast events on the same
// channel script dispatches do. The registry routes the emitted event
// to any subscribers (the listener's recorder/logger).

#pragma once
#include "ScriptValue.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace scriptproto {

using ScriptArgs = std::vector<ScriptValue>;

class Scriptable {
public:
    Scriptable() = delete;
    explicit Scriptable(std::string_view name);
    virtual ~Scriptable();

    // No copy or move — registry holds raw pointers keyed by name.
    Scriptable(const Scriptable&)            = delete;
    Scriptable& operator=(const Scriptable&) = delete;
    Scriptable(Scriptable&&)                 = delete;
    Scriptable& operator=(Scriptable&&)      = delete;

    const std::string& scriptable_name() const { return m_name; }

    // Write path. Returns a ScriptValue holding any result (Null for
    // pure side-effect verbs). args are positional, lex'd from the
    // script line.
    virtual ScriptValue invoke(std::string_view verb,
                                const ScriptArgs& args) = 0;

    // Read path. Returns the value of the named property. Asking for an
    // unknown property returns Null and the listener flags it.
    virtual ScriptValue query(std::string_view property) const = 0;

    // List the verbs and queries the subclass supports. Used by
    // --list-script-objects / --describe in later milestones; the
    // sandbox calls it from the help command.
    virtual std::vector<std::string> verbs()      const = 0;
    virtual std::vector<std::string> properties() const = 0;

protected:
    // Subclasses call this from their canonical signal handler so real
    // user interactions emit on the same channel script dispatches do.
    // `event` is the verb-equivalent name (e.g. "toggled", "changed").
    void emit(std::string_view event, const ScriptValue& payload);

private:
    std::string m_name;
};

} // namespace scriptproto
