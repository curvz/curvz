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
//
// Unregistered substrate (s209 m1):
//
//   A second ctor takes `curvz::scripting::unregistered_t` instead of
//   a name. The resulting widget IS-A substrate type (same universal
//   verbs, same lifecycle) but skips the registry. The leaf shape is
//   uniform — same init_scriptable() call, same bind_canonical()
//   wiring; emit() short-circuits at the Scriptable layer for
//   unregistered instances. See Scriptable.hpp's tag-block comment
//   for the audit-driven rationale (Reading-C-blocked sites with
//   shared abbrevs across short-lived instantiations).
//
// Universal verbs (s191 m4):
//
//   Every wrapped widget gains `hide` / `show` write verbs and a
//   `visible` read query for free — the base implements them in terms
//   of GtkBase's set_visible/get_visible and intercepts them before
//   the leaf's invoke()/query() runs. Leaves implement invoke_leaf()
//   / query_leaf() (the new names for their dispatch tables); the
//   base's invoke()/query() handle universals first, then delegate.
//
//   Adding a universal verb later is one place to edit. Leaves can't
//   forget to support hide/show because they never see the call.
//   "Make the right thing the easy thing and the wrong thing
//   impossible" — the leaves can't override the universals' shape
//   even if they wanted to.

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

    // s209 m1 — unregistered tagged ctor.
    //
    // The widget IS-A substrate type (same template, same universal
    // verbs, same lifecycle) but is invisible to the script registry.
    // Used by short-lived constructions that would otherwise collide
    // on a shared abbrev when instantiated twice (per-show dialogs,
    // per-click popovers, per-loop helpers — see Scriptable.hpp's
    // tag-block comment).
    //
    // Leaves still call `init_scriptable()` as their final ctor line;
    // bind_canonical() still wires the canonical signal handler. The
    // handler's emit() call is a no-op (gated on m_registered in
    // Scriptable::emit), so the leaf shape stays uniform between
    // registered and unregistered instances. No leaf author needs to
    // know which side they're on. The dtor assert on m_initialised
    // continues to work identically.
    template <class... GtkArgs>
    explicit ScriptableWidget(unregistered_t, GtkArgs&&... args)
        : GtkBase(std::forward<GtkArgs>(args)...)
        , Scriptable(unregistered) {
        // bind_canonical() still deferred to init_scriptable() —
        // same vtable-readiness reason as the registered ctor.
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

    // ── Universal verbs (s191 m4) ───────────────────────────────────────
    //
    // hide / show / visible are wired here once for every widget that
    // inherits from this template. Leaves implement invoke_leaf() and
    // query_leaf(); the public invoke()/query() check the universals
    // first and only fall through if the verb wasn't a universal.
    //
    // verbs() and properties() concat the universal list with the leaf's
    // own list so `list` and introspection surface the full vocabulary.
    //
    // Note that hide/show speak `set_visible` directly — no signal
    // synchronizer. set_visible is a synchronous property write; there's
    // no thread-crossing "did the bool flip yet" gap to wait on. (visible
    // is just the same property's getter.)
    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override final {
        if (verb == "hide") {
            this->set_visible(false);
            return ScriptValue::null();
        }
        if (verb == "show") {
            this->set_visible(true);
            return ScriptValue::null();
        }
        return invoke_leaf(verb, args);
    }

    ScriptValue query(std::string_view property) const override final {
        if (property == "visible") return ScriptValue::boolean(this->get_visible());
        return query_leaf(property);
    }

    std::vector<std::string> verbs() const override final {
        auto v = leaf_verbs();
        v.push_back("hide");
        v.push_back("show");
        return v;
    }

    std::vector<std::string> properties() const override final {
        auto p = leaf_properties();
        p.push_back("visible");
        return p;
    }

protected:
    // Leaf-facing dispatch. invoke()/query()/verbs()/properties() above
    // are sealed (`final`) so leaves can't accidentally override the
    // universal-intercept layer. Each leaf overrides the *_leaf forms
    // with its own verb/property table.
    virtual ScriptValue invoke_leaf(std::string_view verb,
                                     const ScriptArgs& args) = 0;
    virtual ScriptValue query_leaf(std::string_view property) const = 0;
    virtual std::vector<std::string> leaf_verbs()      const = 0;
    virtual std::vector<std::string> leaf_properties() const = 0;

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
