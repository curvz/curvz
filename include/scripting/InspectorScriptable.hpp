// scripting/InspectorScriptable.hpp ──────────────────────────────────────────
//
// s222 m2 — Scriptable for the inspector area's section open/close
// state. Wraps the existing MainWindow methods for collapse-all and
// focus-one (built in s202 m6 for the keyboard quick-jump float) and
// exposes them as script verbs so demo / setup scripts can prep the
// inspector visually before running a workload.
//
// ── Why this is a new Scriptable (not a verb on `pnl_styles`) ───────────────
//
// The inspector area isn't a single Gtk widget — it's a multi-section
// vertical scroller assembled from many panels (Layers, Library,
// Swatches, Styles, Themes, Documents, Preview, PropertiesPanel itself)
// plus PropertiesPanel's own internal collapsibles (Object / Document /
// Application groups and the per-selection sections inside them).
//
// Cross-section operations like "collapse everything" or "open just the
// Styles section" need to drive BOTH MainWindow's m_sec_apply registry
// (which manages the right-panel sections) AND PropertiesPanel's
// m_section_open map (which manages the inner sections). The existing
// MainWindow methods `collapse_all_inspector_sections()` and
// `focus_inspector_on()` already span both registries — see s202 m6's
// design block in MainWindow.hpp around line 220.
//
// Putting these verbs on `pnl_styles` would lie about scope (the verb
// affects every panel, not just styles). Putting them on PropertiesPanel
// would miss the right-panel sections. The right home is a Scriptable
// that wraps the inspector AREA, talking to MainWindow as the
// orchestrator that already spans both registries.
//
// ── Why this is a NEW pattern (not a row-bound model Scriptable) ────────────
//
// The four prior Scriptables (layers, guides, swatches, styles) wrap
// collections — sets of addressable items where each item has its own
// identity, lifecycle, and verb surface (rename, delete, etc.). The
// inspector isn't a collection in that sense: there's no per-section
// proxy to materialise. There's just a flat set of named sections, each
// of which can be opened or closed.
//
// So the addressing surface is flat — no `inspector.<id>` proxy — and
// the verbs all take the section title as an argument rather than
// being routed through a proxy. Closer to the singleton-panel
// Scriptables (PanelScriptable subclasses like StylesPanelScriptable)
// than to the model collections.
//
// ── Addressing ──────────────────────────────────────────────────────────────
//
//   inspector               — the Scriptable. ONE registry entry per
//                             app session; held as a member by
//                             MainWindow, registered for the lifetime
//                             of the window.
//
// No per-section proxy. `can_resolve` / `proxy_for` are unimplemented
// (inherit the base class's "not a collection" no-op shape).
//
// ── Verb surface ────────────────────────────────────────────────────────────
//
//   open "<title>"          — set the named section open. Other
//                             sections retain their current state.
//                             Title matches the human-readable
//                             section label as built — "Styles",
//                             "Swatches", "Layers", "Dimensions", etc.
//                             Unknown titles are silent no-ops.
//                             Sections that live inside PropertiesPanel
//                             (Dimensions, Node, Object/Application/
//                             Document groups, and their inner
//                             sections) are reached through the same
//                             plumbing — the MainWindow-side
//                             m_sec_apply path and the
//                             PropertiesPanel-side m_section_open
//                             path are both consulted.
//
//                             NOTE: for sections that live inside a
//                             parent group (e.g. "Styling" inside
//                             "Object"), open opens just the leaf —
//                             the parent group's collapse state is
//                             unchanged. If the parent is closed the
//                             leaf's flag is true but the user sees
//                             nothing. Use `focus` instead for the
//                             "make this section actually visible"
//                             demo intent — it cascades through
//                             parent groups (mirroring the
//                             focus_inspector_on behaviour).
//
//   collapse_all            — close every inspector section across
//                             both registries (MainWindow + Properties
//                             Panel). The persisted view state is
//                             updated; project-level fields like
//                             sec_styles_open get written; the next
//                             session load will restore the closed
//                             state (this matches the keyboard
//                             shortcut behaviour, which is a user-
//                             facing action with the same persistence
//                             semantics).
//
// ── Diagnostic surface (s244 m2) ───────────────────────────────────────────
//
// The inspector hosts the diagnostic verbs and queries that exercise
// m5c's two structural rules (path containment + RunContext gating).
// They live here rather than on a dedicated "diagnostics" Scriptable
// because inspector is already the meta-host (no per-instance proxies,
// flat addressable surface, single registry entry) and the surface
// introspects process-level state, not document state. Two verbs +
// nine queries, none of which mutate state.
//
// path_is_safe is exercised primarily through QUERIES (each with a
// hardcoded path) rather than a verb that takes a path arg. The
// reason is assertability: the DSL's `assert` form is
//
//   assert <object> <property> == <literal>
//
// — equality against a QUERY result only. Verb returns are observable
// in the trace but not assertable. The per-case query shape lets the
// smoke assert each path-safety outcome directly.
//
//   diagnose_universal                   (verb)
//                           — return String "ok" if dispatched.
//                             Universal mask (inherited default).
//                             The smoke's RunContext accept-path
//                             observable: confirms the dispatcher
//                             gating allows the call through when
//                             the verb's mask permits the caller
//                             context.
//
//   diagnose_test_runner_only            (verb)
//                           — would return String "ok" but is gated
//                             to ctx::TestRunner only. The smoke's
//                             RunContext refuse-path observable:
//                             from Scripter context (the only
//                             caller wired today), this verb's
//                             invoke never executes — the
//                             dispatcher returns a structured
//                             refusal error before reaching the
//                             body. The smoke verifies the refusal
//                             appears in the trace (no DSL-level
//                             assert; equality-only `assert` can't
//                             check error state, same negative-
//                             path convention as smoke 42's
//                             find_by_name miss case).
//
//   home_root                            (query)
//                           — String. The cached `$HOME` root from
//                             path_is_safe's init_roots(). Empty
//                             string if init failed.
//
//   tmp_root                             (query)
//                           — String. The cached `$TMPDIR` root from
//                             path_is_safe's init_roots(). Empty
//                             string if init failed.
//
//   home_root_initialised                (query)
//                           — Boolean. True iff home_root is non-
//                             empty. Assertable: `assert inspector
//                             home_root_initialised == true` is the
//                             smoke's init-verify shape.
//
//   tmp_root_initialised                 (query)
//                           — Boolean. Same shape, for tmp_root.
//
//   home_root_is_safe                    (query)
//                           — Boolean. path_is_safe(home_root). By
//                             construction the cached home_root IS
//                             $HOME's resolved root, so this is the
//                             acceptance case (returns true).
//
//   tmp_root_is_safe                     (query)
//                           — Boolean. path_is_safe(tmp_root). Same
//                             shape, acceptance case.
//
//   etc_is_safe                          (query)
//                           — Boolean. path_is_safe("/etc/passwd").
//                             /etc is universally outside $HOME and
//                             /tmp on Linux FHS, so this is the
//                             refusal case for the prefix check
//                             (returns false).
//
//   slash_is_safe                        (query)
//                           — Boolean. path_is_safe("/"). The
//                             filesystem root is shallower than
//                             either cached root by construction;
//                             refusal case.
//
//   empty_is_safe                        (query)
//                           — Boolean. path_is_safe(""). The
//                             degenerate empty-string case
//                             path_is_safe handles at the top of
//                             the predicate; refusal case.
//
// ── Out of scope for s222 m2 (the section-orchestration milestone) ────────
//
// **`focus` verb** — collapse_all + open in one shot, cascading through
// parent groups so the named section is actually visible. The
// MainWindow method `focus_inspector_on` already implements this with
// a parent-group lookup table; a verb wrapper would be a one-liner.
// Held out of m2 to keep the surface minimal (the two verbs above
// cover the "prep my demo" use case directly: collapse_all, then
// open the section the demo cares about). focus is the obvious next
// add once the use case demands it.
//
// **`close "<title>"`** — symmetric to open. The MainWindow doesn't
// expose a per-section close today (collapse_all is the only
// path); adding it would mean either a new `apply_section_state`
// method on MainWindow that resolves the section in either
// registry and writes false, or a Scriptable-internal duplicate of
// that resolution logic. Either is doable; held until the use case
// shows up.
//
// **Queries** — `section_titles`, `is_open "<title>"`, etc. Useful for
// scripts that introspect state; deferred because the demo-prep
// workflow doesn't need them. When a query is added, the verbs/
// queries split mirrors the model Scriptables.
//
// ── Lifetime ────────────────────────────────────────────────────────────────
//
// MainWindow* is captured as a raw pointer at construction; MainWindow
// outlives the Scriptable (the Scriptable is a member of MainWindow,
// destroyed in its dtor). No project getter needed — section state
// lives on MainWindow, not the project.

#pragma once
#include "scripting/Scriptable.hpp"
#include "scripting/ScriptValue.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Curvz {
class MainWindow;
}

namespace curvz::scripting {

class InspectorScriptable : public Scriptable {
public:
    // Registers as "inspector" via the Scriptable base ctor.
    // `main_window` is non-owning; the Scriptable is held as a member
    // of MainWindow and destroyed in MainWindow's dtor, so the pointer
    // is valid for the Scriptable's entire lifetime. nullptr would
    // be a bug (we can't function without it); we don't accept it as
    // a defensive option — caller must pass a real pointer.
    explicit InspectorScriptable(Curvz::MainWindow* main_window);
    ~InspectorScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

    // s244 m2 — RunContext mask declarations. Per-verb mask for the
    // diagnostic verbs the inspector hosts for m5c's smoke. The base
    // class's default returns ctx::all_three for every verb; we
    // override here ONLY to mark `diagnose_test_runner_only` as
    // TestRunner-only so the smoke can observe the refusal path.
    // Every other verb on this Scriptable (including the existing
    // `open` / `collapse_all`, and the new diagnose_universal /
    // diagnose_path_safety verbs) inherits the all-three default.
    //
    // See CANON's "RunContext gates the verb surface" entry for the
    // declaration discipline (declared once at the verb, enforced
    // once at the dispatcher) and RunContext.hpp for the mask values.
    RunContextMask context_mask(std::string_view verb) const override;

private:
    Curvz::MainWindow* m_main_window;
};

} // namespace curvz::scripting
