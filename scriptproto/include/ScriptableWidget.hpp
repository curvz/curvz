// ScriptableWidget.hpp ───────────────────────────────────────────────────────
//
// Templated mixin that pairs any Gtk::Widget subclass with Scriptable.
//
// Centralises three pieces of discipline that were repeated in every
// hand-rolled wrapper in v1:
//
//   1. Named construction. Every leaf MUST supply a name; the base ctor
//      validates it (non-empty, no whitespace, not a reserved keyword)
//      before passing it to Scriptable. A leaf can't bypass validation
//      because it can't reach the registry path except through us.
//
//   2. Canonical signal binding. Each gtkmm widget has a different
//      canonical signal (signal_toggled, signal_changed,
//      signal_value_changed, property_selected). The base can't know
//      which to bind, so it declares bind_canonical() as a pure
//      virtual the leaf MUST override. Forgetting it is a compile-time
//      error (the class can't be instantiated).
//
//   3. Construction-argument forwarding. The leaf hands us the name +
//      whatever the GtkBase ctor needs; perfect-forwarding routes the
//      rest into GtkBase. Common case handled in one line; widgets
//      with awkward setup (DropDown's StringList model, Scale's
//      range/increments) configure in their own ctor body after the
//      base has constructed.
//
// Construction order subtlety, surfaced for future readers:
//
//   GtkBase ctor runs BEFORE Scriptable ctor (declaration order on the
//   base-class list). That means GtkBase is fully constructed and the
//   widget is alive by the time we hand m_name to the registry.
//
//   Conversely, ~GtkBase runs AFTER ~Scriptable. So when the registry
//   unregister fires (in ~Scriptable), the widget is still a valid
//   GtkBase. Subscribers walking the registry mid-teardown see a live
//   widget at unregister-time.
//
// init_scriptable() — leaf ctor discipline:
//
//   Pure virtuals can't be called from a base ctor (UB: the derived
//   vtable isn't in place yet). bind_canonical() is therefore deferred:
//   every leaf calls init_scriptable() as the last line of its own
//   ctor, which dispatches through the now-complete vtable to the
//   leaf's bind_canonical() override.
//
//   Forgetting init_scriptable() in a leaf means the canonical signal
//   is never wired and the wrapper is silently dead on the outbound
//   channel. To make this loud rather than silent, ~Scriptable asserts
//   m_initialised in debug builds — a leaf author who forgets sees an
//   assertion at destruction time during testing.
//
//   bind_canonical() being pure-virtual catches the other failure mode
//   at compile time: a leaf that doesn't override it can't be
//   instantiated, which means the template can't ship a leaf with no
//   emit-side wiring.

#pragma once
#include "Scriptable.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace scriptproto {

// Reserved keywords from the DSL — names that would collide with the
// listener's built-in verbs. Leaves cannot register under these names.
inline bool is_reserved_script_name(std::string_view name) {
    return name == "list"      || name == "subscribe" ||
           name == "sleep"     || name == "quit"      ||
           name == "get"       || name == "assert";
}

// Validate and pass through. Throws on invalid names so construction
// fails loudly at the call site. Returns std::string (not string_view)
// because we hand it to Scriptable which copies into m_name — using
// string_view here would imply a non-owning return which the lifetime
// of the temporary doesn't actually provide for downstream users.
inline std::string validated_script_name(std::string_view name) {
    if (name.empty())
        throw std::runtime_error("ScriptableWidget: name must be non-empty");
    for (char c : name) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '"')
            throw std::runtime_error(
                "ScriptableWidget: name '" + std::string(name) +
                "' contains whitespace or quote");
    }
    if (is_reserved_script_name(name))
        throw std::runtime_error(
            "ScriptableWidget: name '" + std::string(name) +
            "' shadows a DSL keyword");
    return std::string(name);
}

template <class GtkBase>
class ScriptableWidget : public GtkBase, public Scriptable {
public:
    // Perfect-forwarding ctor. Name is validated first, then routed to
    // Scriptable; remaining args route to GtkBase. Leaves that need
    // setter-style configuration (Scale's range, DropDown's model)
    // do that in their own ctor body after this returns.
    template <class... GtkArgs>
    explicit ScriptableWidget(std::string_view name, GtkArgs&&... args)
        : GtkBase(std::forward<GtkArgs>(args)...)
        , Scriptable(validated_script_name(name)) {
        // bind_canonical() is deferred to init_scriptable() because we
        // can't dispatch through the vtable from a base-class ctor.
        // See header notes above.
    }

    ~ScriptableWidget() override {
        // Loud failure for a leaf that forgot init_scriptable(). Debug-
        // only — release builds don't pay the runtime cost. This catches
        // the discipline violation at destruction time during testing,
        // which is when the registry would otherwise show a silent
        // dead-outbound-channel wrapper.
        assert(m_initialised &&
               "ScriptableWidget: leaf forgot init_scriptable() in its ctor");
    }

protected:
    // Leaf calls this from its ctor's last line. Subsequent calls are
    // no-ops — guarded so re-call from a sub-leaf (if anyone ever
    // extends a leaf) doesn't double-bind.
    void init_scriptable() {
        if (m_initialised) return;
        m_initialised = true;
        bind_canonical();
    }

    // Pure virtual the leaf MUST override. Connects the widget's
    // canonical signal (signal_toggled / signal_changed / etc.) to a
    // lambda that calls this->emit(event_name, payload). Compile-time
    // failure if a leaf forgets to override.
    virtual void bind_canonical() = 0;

private:
    bool m_initialised = false;
};

} // namespace scriptproto
