// scripting/PalettesScriptable.hpp ───────────────────────────────────────────
//
// s243 m2 — seventh row-bound model Scriptable, closing the s243 arc.
// Wraps the project's `SwatchLibrary` palette pool as a collection
// Scriptable under abbrev `palettes`, with transient `palettes.<iid>`
// proxies for per-instance operations via the s216 m1 router hooks
// (Scriptable::can_resolve / proxy_for).
//
// Sibling of SwatchesScriptable — both wrap the same SwatchLibrary,
// just opposite ends of it (swatches and palettes are stored in
// parallel maps on the library). The two Scriptables compose: a script
// can mint a swatch via `swatches new`, mint a palette via `palettes
// new`, then... well, that's exactly what's deferred — per-palette
// swatch ops (`palettes.<iid> add_swatch <swatch_iid>`,
// `remove_swatch`, `reorder_swatch`) are out of scope for m2. See
// "Out of scope for s243 m2" below.
//
// ── How palettes differ from swatches at the Scriptable level ─────────────
//
// Palettes and swatches share the storage model (two-tier defaults +
// custom, string-id identity, project-scoped lifetime, library-level
// signals for fan-out) so the Scriptable shape is near-identical to
// SwatchesScriptable. Three substantive differences:
//
// 1. Names are mandatory. The panel rejects empty names on new /
//    rename (a palette without a name is meaningless — there's no
//    auto-derived fallback the way swatches get region names). The
//    Scriptable mirrors that: empty `name` argument on `palettes new`
//    returns "" (no command pushed); empty argument on
//    `palettes.<iid> rename` returns Null.
//
// 2. There's an `active_palette` concept on the library — exactly one
//    palette is "the current one" for the swatches panel grid. The
//    Scriptable exposes this as `active_id` (collection query) and
//    `activate` (collection + proxy verb). Activation is a transient
//    working-state mutation, NOT undoable — same posture as the
//    panel's dropdown click, and matches AddPaletteCommand's
//    `set_make_active` flag, which exists precisely because activation
//    rides alongside command-driven CRUD without being a command of
//    its own.
//
// 3. Palettes contain swatches, which means a proxy can answer
//    queries about its contents: `swatch_count` (membership size)
//    and `swatches` (comma-separated swatch ids in palette order).
//    These are read-only in m2; per-palette swatch ops (`add_swatch`,
//    `remove_swatch`, `reorder_swatch`) are deferred.
//
// ── Addressing ────────────────────────────────────────────────────────────
//
//   palettes              — the collection Scriptable. Set verbs and
//                           set queries. ONE registry entry per app
//                           session; held as a member by MainWindow,
//                           registered for the lifetime of the window.
//
//   palettes.<iid>        — per-instance proxy, materialised on demand
//                           by the collection. Reads / writes the
//                           specific palette's name / activation /
//                           contents through the project's
//                           SwatchLibrary. NOT registered — the
//                           registry only knows about `palettes`. A
//                           `list` from the Scripter shows `palettes`
//                           but never shows `palettes.<iid>` entries.
//
// ── Lifetime and the project pointer ──────────────────────────────────────
//
// Same shape as SwatchesScriptable: a project getter
// (`std::function<CurvzProject*()>`) resolved fresh on every verb call,
// not a captured raw pointer. MainWindow's m_project unique_ptr gets
// reset and reassigned at runtime (close-project, load-project); the
// getter survives that, the Scriptable does not need re-registration.
//
// MainWindow constructs the PalettesScriptable once (after m_project
// is initialised) and the registry holds the entry for the window's
// lifetime.
//
// ── Commands and undo ─────────────────────────────────────────────────────
//
// Every mutating CRUD verb pushes a command from the s243 m1 palette-
// CRUD quintet (AddPaletteCommand / RemovePaletteCommand /
// RenamePaletteCommand / PaletteMembershipCommand — the membership
// one is unused in m2 since per-palette swatch ops are out of scope).
//
//   new                  → AddPaletteCommand (standard ctor) with
//                          set_make_active(true) — mirrors panel UX
//                          contract that new palettes become active.
//   delete               → RemovePaletteCommand (the ctor snapshots
//                          palette value + active state pre-remove for
//                          full round-trip undo)
//   rename (both forms)  → RenamePaletteCommand (old + new name)
//   duplicate            → call library->duplicate_palette directly,
//                          then wrap in AddPaletteCommand::already_added
//                          with set_make_active(true) — exactly the
//                          panel's pattern, preserves
//                          duplicate_palette as single source of truth
//                          for "what's in a duplicate".
//
// The push pattern is `cmd->execute(); m_history->push(std::move(cmd))`
// — execute-then-push, matching SwatchesScriptable's shape (and for
// the same reason: the palette commands carry their mutation in their
// own execute() body, not as a diff applied at the call site).
//
// `activate` is NOT a command — set_active_palette is transient
// working state, deliberately outside undo. Same posture as the
// panel's dropdown click. The state DOES persist (it lives in
// project.json's editor_state block); it just doesn't ride the undo
// stack.
//
// The `m_history` pointer is captured at construction and outlives
// the Scriptable (CommandHistory is a value member of MainWindow).
// Same non-owning lifetime story as the other library-collection
// Scriptables.
//
// ── Defaults-tier guard ───────────────────────────────────────────────────
//
// SwatchLibrary maintains TWO tiers for palettes — defaults (shipped,
// read-only) and custom (per-project, mutable). Every mutating verb
// refuses on a defaults-tier id, returning Null without pushing a
// command. The UI's escape hatch is `duplicate` — the user duplicates
// a default to get a custom-tier copy they can edit. The Scriptable
// mirrors that affordance.
//
// `activate` is the one mutating verb that DOES accept defaults ids —
// activation is a per-project pointer at any palette in either tier,
// not a mutation of the palette itself.
//
// Read queries work on both tiers: `count`, `all_ids`, `find_by_name`,
// proxy `name` / `is_default` / `swatch_count` / `swatches` / `iid`
// all resolve through SwatchLibrary's find_palette (custom-first,
// then defaults).
//
// Custom-vs-default partition is exposed through two parallel set
// queries: `custom_ids` and `default_ids`. Same shape as
// SwatchesScriptable.
//
// ── Verb / query surface ──────────────────────────────────────────────────
//
// Verb names come from the panel's own UI vocabulary (kebab menu +
// chip context menu): `new`, `delete`, `duplicate`, `rename`. The
// `activate` verb is new — palettes have an activation concept that
// swatches don't (the dropdown's selected row), and scripting it
// needs a verb. The proxy `swatches` query is new for the same reason
// — palettes own a swatch-id list as part of their value type.
//
// COLLECTION VERBS (on `palettes`):
//   new "<name>"                         — create a new custom-tier
//                                          palette with the given
//                                          name (REQUIRED — empty
//                                          returns "" without
//                                          pushing). Returns the new
//                                          id. The new palette becomes
//                                          active (matches panel UX).
//                                          Pushes AddPaletteCommand
//                                          with set_make_active(true).
//   delete "<iid>"                       — remove a custom-tier
//                                          palette. Refuses on
//                                          defaults-tier ids (returns
//                                          Null without pushing).
//                                          Pushes RemovePaletteCommand,
//                                          which snapshots palette
//                                          value AND active-state
//                                          pre-remove for full
//                                          round-trip undo. If the
//                                          deleted palette was active,
//                                          active is cleared (panel
//                                          falls back to first
//                                          palette on next refresh).
//   duplicate "<iid>" ["<new_name>"]     — duplicate a palette from
//                                          either tier into the custom
//                                          tier. Calls
//                                          library->duplicate_palette
//                                          directly (single source of
//                                          truth for "what's in a
//                                          duplicate" — name fallback,
//                                          cross-tier swatch refs,
//                                          builtin flag clearing),
//                                          then wraps the result in
//                                          AddPaletteCommand::already_added
//                                          with set_make_active(true).
//                                          new_name optional; if
//                                          empty/missing, library
//                                          appends " copy" to the
//                                          source name. Returns the
//                                          new id, or "" on failure.
//   rename "<iid>" "<name>"              — rename a custom-tier
//                                          palette. Refuses on
//                                          defaults-tier ids and on
//                                          empty name. Pushes
//                                          RenamePaletteCommand
//                                          (old + new). Skip-no-op
//                                          guard: rename to current
//                                          name doesn't push.
//   activate "<iid>"                     — set the library's active
//                                          palette. Accepts ids from
//                                          either tier (activation
//                                          isn't a mutation of the
//                                          palette itself). NOT
//                                          undoable. Empty / unknown
//                                          id returns Null without
//                                          touching active.
//   find_by_name "<name>"                — id of the first palette
//                                          (any tier) whose name
//                                          exactly matches. Returns
//                                          "" on miss. Same first-hit
//                                          contract as
//                                          SwatchesScriptable::find_by_name.
//
// COLLECTION QUERIES (on `palettes`):
//   count          — int          total palettes in both tiers.
//   all_ids        — string       comma-separated PaletteIds across
//                                  both tiers (defaults first, then
//                                  custom). Same future-foreach-grammar
//                                  shape as `swatches all_ids`.
//   custom_ids     — string       comma-separated PaletteIds in the
//                                  custom tier only. Convenience for
//                                  iterating the editable palettes
//                                  without tripping defaults-tier
//                                  refusal on every mutating verb.
//   default_ids    — string       comma-separated PaletteIds in the
//                                  defaults tier only.
//   active_id      — string       the currently-active palette id, or
//                                  "" if no active palette is set.
//                                  May reference either tier.
//
// ELEMENT VERBS (on `palettes.<iid>`):
//   rename "<name>"                      — write palette.name. Refuses
//                                          on defaults-tier (returns
//                                          Null) and on empty name.
//                                          Pushes RenamePaletteCommand.
//   activate                             — set this palette as active.
//                                          NOT undoable. Works on
//                                          either tier. No args.
//
// ELEMENT QUERIES (on `palettes.<iid>`):
//   name           — string       palette.name.
//   is_default     — bool         true iff the palette lives in the
//                                  defaults tier. Useful for scripts
//                                  that want to gate their own
//                                  mutations on whether the target is
//                                  editable.
//   is_active      — bool         true iff this palette is the
//                                  library's active palette. Cheap
//                                  alternative to `get palettes
//                                  active_id` and string-comparing.
//   swatch_count   — int          number of swatches in this palette's
//                                  `swatches` vector. Membership
//                                  size, not the global swatch pool
//                                  size.
//   swatches       — string       comma-separated SwatchIds in palette
//                                  order. The swatches themselves may
//                                  live in either tier — palette
//                                  membership is cross-tier-friendly
//                                  (a custom palette can reference
//                                  default swatches and vice versa).
//                                  Empty string for an empty palette.
//   iid            — string       palette id (the addressing key).
//                                  Named `iid` for shape-symmetry
//                                  with LayerProxy / GuideProxy /
//                                  SwatchProxy — same "the
//                                  per-instance key for this proxy"
//                                  semantics.
//
// ── Out of scope for s243 m2 ──────────────────────────────────────────────
//
// **Per-palette swatch ops** — `add_swatch`, `remove_swatch`,
// `reorder_swatch` are NOT exposed by this Scriptable in m2. The
// underlying machinery is in place (PaletteMembershipCommand's
// apply_() handles all four membership transitions: pure add, pure
// remove, move, no-op; only the `already_moved` factory ships in s243
// m1). When per-palette swatch ops are wanted, add `already_added` /
// `already_removed` factories to PaletteMembershipCommand and the
// three proxy verbs alongside. Deferred from m2 to keep the smoke
// count in the 18-22 range — adding three mutating verbs with their
// round-trip phases would push past 25.
//
// **Gradient / path-blend palettes** — palettes are pure swatch-id
// vectors today; no special-cased variants. If future palette types
// land (e.g., gradient palettes that own their stops), they'll
// extend the proxy surface.
//
// ── On active-project scope ───────────────────────────────────────────────
//
// Same shape as SwatchesScriptable: all verbs operate on
// `project->swatches` (the library hangs off the project, NOT off
// any specific document). Palettes are project-scoped state; the
// "active document" doesn't enter into palette lookup.

#pragma once
#include "scripting/Scriptable.hpp"
#include "scripting/ScriptValue.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Curvz {
struct CurvzProject;
class  CommandHistory;
}

namespace curvz::scripting {

class PalettesScriptable : public Scriptable {
public:
    using ProjectGetter = std::function<Curvz::CurvzProject*()>;

    // Registers as "palettes" via the Scriptable base ctor. The project
    // getter is called fresh on every verb invocation — never cached —
    // so MainWindow can swap its m_project unique_ptr freely without
    // invalidating us. The getter is allowed to return nullptr; verbs
    // degrade gracefully (count returns 0, all_ids returns "", etc.).
    //
    // `history` is non-owning and may be nullptr (test harness path).
    // Lives at MainWindow scope alongside the project unique_ptr; the
    // CommandHistory object itself has a stable address (it's a value
    // member, not a pointer that gets swapped), so storing a raw
    // pointer is safe for the PalettesScriptable's lifetime. Mirrors
    // SwatchesScriptable's history contract — dereferenced on every
    // mutating CRUD verb.
    PalettesScriptable(ProjectGetter get_project,
                       Curvz::CommandHistory* history);
    ~PalettesScriptable() override = default;

    ScriptValue invoke(std::string_view verb,
                       const ScriptArgs& args) override;
    ScriptValue query(std::string_view property) const override;
    std::vector<std::string> verbs()      const override;
    std::vector<std::string> properties() const override;

    // Router hooks — see CANON's "Row-bound model Scriptables".
    bool can_resolve(std::string_view key) const override;
    std::unique_ptr<Scriptable> proxy_for(std::string_view key) override;

private:
    ProjectGetter          m_get_project;
    Curvz::CommandHistory* m_history;   // non-owning; outlives us
};

} // namespace curvz::scripting
