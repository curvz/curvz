// scripting/Scriptable.hpp ───────────────────────────────────────────────────
//
// The mixin every scriptable widget inherits via ScriptableWidget<GtkBase>.
// Lifted from scriptproto/ during s186 m2 — namespace changed from
// scriptproto:: to curvz::scripting:: on the way in, contract otherwise
// unchanged from the sandbox v2.
//
// Mandates a name at construction (no default ctor), self-registers in the
// registry on construction, self-unregisters on destruction.
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
#include "scripting/RunContext.hpp"
#include "scripting/ScriptValue.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace curvz::scripting {

using ScriptArgs = std::vector<ScriptValue>;

// s209 m1 — opt-out tag for substrate widgets that don't need (or
// can't have) a registry entry.
//
// The registry refuses two live Scriptables under the same name. That's
// the right invariant for long-lived hosts (one PaintEditor, one
// ColorPickerPopover, one Toolbar) where each widget is genuinely
// addressable. It's the wrong invariant for short-lived constructions
// that share an abbrev across instances:
//
//   - per-show heap-allocated self-deleting dialogs;
//   - per-click popovers built inside controller lambdas;
//   - per-loop helper-multipliers (`add_btn` called N times per tool
//     change in ContextBar, four-button popovers in Ruler's unit list,
//     per-tile entries in DocumentGallery, …).
//
// For these, the widget is structurally NOT script-addressable at the
// per-instance level. An "About dialog Close button" doesn't need its
// own script handle; a per-tile Entry in a per-doc loop is addressed
// at the document level, not the entry level. Forcing them through
// the registry collides on construction; refusing them substrate at
// all leaves the type inconsistent with everything around them.
//
// `unregistered_t{}` is the tagged-ctor opt-out. A widget constructed
// with the tag IS-A substrate type (uniform with every registered
// substrate widget — same template, same `set_visible`/`show`/`hide`
// surface, same lifecycle), but skips the registration step. The dtor,
// `force_unregister()`, and `emit()` all bail when `m_registered`
// is false, so the registry never sees the instance and the canonical
// signal handler's emit() call is a no-op.
//
// Naming: empty `m_name`. An unregistered Scriptable can't be looked
// up, can't be emit-targeted, can't collide with anything. Anyone
// reading the registry sees only the registered surface.
//
// See LEDGER.md "On Reading C scope (s208 m5)" for the audit that
// justified this; see CANON's "Lifetime shapes" section for the
// long-lived/short-lived distinction the tag formalises.
struct unregistered_t { explicit unregistered_t() = default; };
inline constexpr unregistered_t unregistered{};

class Scriptable {
public:
    Scriptable() = delete;
    explicit Scriptable(std::string_view name);
    explicit Scriptable(unregistered_t);
    virtual ~Scriptable();

    // No copy or move — registry holds raw pointers keyed by name.
    Scriptable(const Scriptable&)            = delete;
    Scriptable& operator=(const Scriptable&) = delete;
    Scriptable(Scriptable&&)                 = delete;
    Scriptable& operator=(Scriptable&&)      = delete;

    const std::string& scriptable_name() const { return m_name; }

    // s191 m7 — early-unregister escape hatch for inspector rebuild.
    //
    // The substrate registry refuses two live Scriptables under the
    // same name. The inspector's rebuild pattern destroys and
    // reconstructs the entire section's widget tree; GTK4 destroys
    // widgets at idle priority, so under a self-rebuild click handler
    // the new widgets construct (and try to register) before the old
    // ones finish destroying. Result: registry collision, crash.
    //
    // force_unregister() removes our entry from the registry *now*,
    // synchronously, before the widget's actual destruction. The
    // dtor's unregister is idempotent (registry::erase on missing
    // name is a no-op), so calling this twice — or letting the dtor
    // run after a force-unregister — is safe.
    //
    // Called from PropertiesPanel::do_clear via a subtree walk before
    // m_inner.remove(*c). That's the structural fix for the s191 m6
    // registry-collision crash — replaces the brittle signal_timeout
    // workaround.
    //
    // s209 m1: no-op for unregistered Scriptables (m_registered=false).
    // The force_unregister_subtree pump walks any container that may
    // hold substrate widgets; unregistered children must coexist with
    // registered siblings without contributing to the registry.
    void force_unregister();

    // Write path. Returns a ScriptValue holding any result (Null for
    // pure side-effect verbs). args are positional, lex'd from the
    // script line.
    virtual ScriptValue invoke(std::string_view verb,
                                const ScriptArgs& args) = 0;

    // Read path. Returns the value of the named property. Asking for an
    // unknown property returns Null and the listener flags it.
    virtual ScriptValue query(std::string_view property) const = 0;

    // List the verbs and queries the subclass supports. Used by
    // help / introspection (`list` from the Scripter window for now,
    // --list-script-objects / --describe in later milestones).
    virtual std::vector<std::string> verbs()      const = 0;
    virtual std::vector<std::string> properties() const = 0;

    // ── Router hooks (s216 m1) ───────────────────────────────────────────
    //
    // Collection-as-router with transient per-instance proxies. See
    // CANON's "Row-bound model Scriptables" entry.
    //
    // The default implementations make these a no-op for every existing
    // widget Scriptable — only collection Scriptables (LayersScriptable
    // and the future guides / swatches / styles / themes / objects
    // collections) override. The listener's `find_object` calls
    // `can_resolve(key)` to ask whether this collection owns the iid
    // after the prefix dot, and `proxy_for(key)` to materialise a
    // transient per-instance Scriptable for the dispatch.
    //
    // Proxies are owned for the duration of a single listener dispatch
    // by the `ResolvedObject` wrapper find_object returns. Their
    // invoke/query bodies resolve the iid through the project's iid
    // index on every call — the iid index is the truth, the registry
    // entry is incidental. A deleted iid stops resolving; an undeleted
    // iid (undo) resolves again.

    // Does this Scriptable resolve `key` to something it owns right
    // now? Default false — non-collection Scriptables don't route.
    virtual bool can_resolve(std::string_view /*key*/) const {
        return false;
    }

    // Materialise a transient Scriptable for `key`. Returns nullptr if
    // can_resolve(key) would return false; callers should check first.
    // The returned proxy is owned by the listener for the duration of
    // the single-line dispatch and destroyed when the wrapper goes
    // out of scope.
    virtual std::unique_ptr<Scriptable>
        proxy_for(std::string_view /*key*/) {
        return nullptr;
    }

    // ── RunContext gating (s244 m2) ──────────────────────────────────────
    //
    // Each verb on this Scriptable declares which caller contexts may
    // invoke it. Default is `ctx::all_three` — every existing verb on
    // every existing Scriptable stays callable from every existing
    // caller (backward-compat under CANON's "default is widest
    // reasonable" rule). Sensitive verbs override this method to
    // declare a narrower mask; the dispatcher checks the caller's
    // context against the returned mask before calling invoke().
    //
    // Reads (`query`) are not gated — see CANON's RunContext entry:
    // "Every model query. Reads can't corrupt." The dispatcher only
    // calls this method on the invoke path.
    //
    // The verb name is passed in so a single Scriptable with mixed
    // verb sensitivities can declare them per-verb without splitting
    // into multiple Scriptables. Unknown verb names (e.g. typos)
    // SHOULD fall through to the default — the invoke() method itself
    // is responsible for noticing the verb is unknown.
    //
    // See scripting/RunContext.hpp for the enum, the mask type, and
    // the helper constants (ctx::TestRunner, ctx::Macro, ctx::Scripter,
    // ctx::all_three).
    virtual RunContextMask context_mask(std::string_view /*verb*/) const {
        return ctx::all_three;
    }

protected:
    // Subclasses call this from their canonical signal handler so real
    // user interactions emit on the same channel script dispatches do.
    // `event` is the verb-equivalent name (e.g. "toggled", "changed").
    void emit(std::string_view event, const ScriptValue& payload);

private:
    std::string m_name;
    // s209 m1: false for instances constructed via `unregistered_t`.
    // Gates ctor's registration, dtor's unregister, force_unregister(),
    // and emit() — an unregistered Scriptable is invisible to the
    // registry and silent on the outbound channel.
    bool m_registered;
};

} // namespace curvz::scripting
