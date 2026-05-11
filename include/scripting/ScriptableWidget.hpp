// scripting/ScriptableWidget.hpp ─────────────────────────────────────────────
//
// Templated mixin that pairs any Gtk::Widget subclass with Scriptable.
// The load-bearing piece of the Script-Driven Curvz substrate.
//
// Centralises four pieces of discipline that would otherwise be
// repeated in every hand-rolled wrapper:
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
//   4. Synchronizer for thread-crossing write verbs. GTK is threaded;
//      Widget::activate() and similar event-dispatching writes queue
//      work onto a context that's not the listener's calling thread.
//      The state flip and signal emission happen there, not in the
//      caller's stack frame. A time-based wait is guessing across a
//      thread boundary; the honest answer is signal-bound. The base
//      provides `wait_for_signal()` so every leaf invoke that needs
//      synchronisation does the same right thing the same way.
//      See "The synchronizer" section below for the contract.
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
//   channel. To make this loud rather than silent, ~ScriptableWidget
//   asserts m_initialised in debug builds — a leaf author who forgets
//   sees an assertion at destruction time during testing.
//
//   bind_canonical() being pure-virtual catches the other failure mode
//   at compile time: a leaf that doesn't override it can't be
//   instantiated, which means the template can't ship a leaf with no
//   emit-side wiring.

#pragma once
#include "scripting/Scriptable.hpp"
#include "CurvzLog.hpp"

#include <cassert>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <glibmm/main.h>
#include <sigc++/connection.h>

namespace curvz::scripting {

// Reserved keywords from the DSL — names that would collide with the
// listener's built-in verbs. Leaves cannot register under these names.
inline bool is_reserved_script_name(std::string_view name) {
    return name == "list"      || name == "subscribe" ||
           name == "sleep"     || name == "quit"      ||
           name == "get"       || name == "assert";
}

// Validate and pass through. Throws on invalid names so construction
// fails loudly at the call site.
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

    // ── The synchronizer ─────────────────────────────────────────────────
    //
    // GTK is threaded. Widget::activate(), property mutators that route
    // through GTK's event/gesture dispatch, and other write paths queue
    // work onto a context that is not the listener's calling thread.
    // The state flip and the corresponding signal emission (signal_toggled,
    // signal_changed, …) happen there, not in the caller's stack frame.
    //
    // A time-based wait — 5ms, 50ms, whatever — is guessing across a
    // thread boundary. The honest answer is signal-bound: subscribe to
    // the canonical "done" signal BEFORE dispatching the write, then
    // pump the local main context until the signal fires or a hard
    // ceiling expires.
    //
    // Usage from a leaf's invoke():
    //
    //     wait_for_signal(
    //         [this](sigc::slot<void()> slot) {
    //             return signal_toggled().connect(std::move(slot));
    //         },
    //         [this]() { activate(); }
    //     );
    //
    // Two callables:
    //   subscribe  — given a slot, return the sigc::connection from
    //                connecting that slot to whichever signal the leaf
    //                cares about. The base disconnects after the pump.
    //   dispatch   — call the underlying write (activate(), set_text(),
    //                set_value(), …) that ultimately fires the signal.
    //
    // Ceiling: defaults to 500ms — empirical conservative bound (the
    // cold path measured during s186 m2 close-out was ~235ms; 500 gives
    // headroom). If the signal hasn't fired within the ceiling, log a
    // warning and return — that signals a real bug, not slow dispatch.
    // m3 calibration can refine ceilings per wait class.
    //
    // Real-user interactions bypass this entirely: a user-click triggers
    // GTK's gesture path directly, not invoke(). The synchronizer is
    // invisible to the user-facing path.
    //
    // Return value: true if the signal fired before the ceiling, false
    // if the ceiling expired. Most leaves can ignore the return; the
    // contract is "we waited as long as we could." A leaf can inspect
    // the return if it wants to escalate (e.g. mark the verb as having
    // hit the cold-path wake-retry case).
    using SubscribeFn = std::function<sigc::connection(sigc::slot<void()>)>;
    using DispatchFn  = std::function<void()>;

    bool wait_for_signal(SubscribeFn subscribe,
                         DispatchFn  dispatch,
                         std::chrono::milliseconds ceiling
                             = std::chrono::milliseconds(500)) {
        bool done = false;
        sigc::connection conn = subscribe(
            sigc::slot<void()>([&done]() { done = true; }));

        dispatch();

        auto ctx = Glib::MainContext::get_default();
        auto start = std::chrono::steady_clock::now();
        while (!done) {
            // iteration(false): non-blocking. Process one pending event
            // if any, otherwise return immediately. Loop until signal
            // lands or ceiling hit.
            ctx->iteration(false);
            if (std::chrono::steady_clock::now() - start >= ceiling) {
                LOG_WARN("ScriptableWidget[{}] wait_for_signal: ceiling "
                         "({}ms) expired before signal fired",
                         scriptable_name(), ceiling.count());
                break;
            }
        }

        conn.disconnect();
        return done;
    }

private:
    bool m_initialised = false;
};

} // namespace curvz::scripting
