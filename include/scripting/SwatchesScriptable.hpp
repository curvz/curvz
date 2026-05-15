// scripting/SwatchesScriptable.hpp ────────────────────────────────────────────
//
// s221 m1 — third row-bound model Scriptable, following LayersScriptable
// (s216 m1) and GuidesScriptable (s218 m1) but adapted for a LIBRARY
// collection rather than a SceneNode collection. Wraps the active
// project's `SwatchLibrary` as a collection Scriptable under abbrev
// `swatches`, materialising transient `swatches.<id>` proxies for per-
// instance operations via the s216 m1 router hooks
// (Scriptable::can_resolve / proxy_for).
//
// ── How swatches differ from layers / guides ───────────────────────────────
//
// Layers and guides are SceneNodes — they live in a doc's scene tree
// and are addressed by `internal_id` (a UUID minted at construction).
// `find_by_iid(project, iid)` walks every document's scene tree to
// resolve them.
//
// Swatches are NOT SceneNodes. They live in `CurvzProject::swatches`
// (a `SwatchLibrary`) under a `SwatchId` (also a std::string, but
// minted by the library's own generator, distinct from the SceneNode
// UUID generator). Lookup is project-scoped, not document-scoped:
// `proj->swatches.find_swatch(id)` is the direct call, no walk needed.
//
// The router hook contract is unchanged — `can_resolve(key)` returns
// true iff the key addresses a live swatch, `proxy_for(key)` returns a
// transient proxy that resolves the SwatchId on every verb call. The
// IMPLEMENTATION of those hooks just goes through the library rather
// than through `curvz::utils::find_by_iid`.
//
// ── Addressing ──────────────────────────────────────────────────────────────
//
//   swatches                — the collection Scriptable. Set verbs and
//                             set queries. ONE registry entry per app
//                             session; held as a member by MainWindow,
//                             registered for the lifetime of the window.
//
//   swatches.<id>           — per-instance proxy, materialised on demand
//                             by the collection. Reads / writes the
//                             specific swatch's name / colour through
//                             the project's SwatchLibrary. NOT
//                             registered — the registry only knows about
//                             `swatches`. A `list` from the Scripter
//                             shows `swatches` but never shows
//                             `swatches.<id>` entries (same pilot
//                             success condition as layers/guides).
//
// ── Lifetime and the project pointer ────────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable: a project getter
// (`std::function<CurvzProject*()>`) resolved fresh on every verb call,
// not a captured raw pointer. MainWindow's m_project unique_ptr gets
// reset and reassigned at runtime (close-project, load-project); the
// getter survives that, the Scriptable does not need re-registration.
//
// MainWindow constructs the SwatchesScriptable once (after m_project is
// initialised) and the registry holds the entry for the window's
// lifetime.
//
// ── Commands and undo ───────────────────────────────────────────────────────
//
// Unlike GuidesScriptable (which is direct-write only, because guides
// aren't undoable in the inspector either), every mutating swatch verb
// pushes a `CommandHistory` command. The s220 m1a sweep made all four
// CRUD operations undoable from the panel; this Scriptable rides on
// that exact same plumbing:
//
//   new / duplicate    → AddSwatchCommand (standard ctor — scripted
//                        adds don't go through the popover's
//                        already_added factory)
//   delete             → RemoveSwatchCommand
//   rename             → EditSwatchCommand (colour unchanged, name
//                        diffed)
//   color (on proxy)   → EditSwatchCommand (colour diffed, name
//                        unchanged)
//
// The push pattern is `cmd->execute(); m_history->push(std::move(cmd))`,
// matching StylesPanel::action_create_empty's on_committed callback
// (which is the closest in-tree precedent for "execute-then-push" where
// the command's execute() IS the mutation, as opposed to LayersScriptable's
// "mutate-then-push" where execute()/undo() only handle redo/undo
// because the initial mutation happens at the call site). The shape
// difference is intrinsic to the command kind — AddSwatchCommand /
// RemoveSwatchCommand / EditSwatchCommand all carry the mutation in
// their execute() body; the LayerField commands only carry the diff.
//
// The `m_history` pointer is captured at construction and outlives the
// Scriptable (CommandHistory is a value member of MainWindow). Same
// non-owning lifetime story as the layers/guides scriptables.
//
// ── Defaults-tier guard ─────────────────────────────────────────────────────
//
// SwatchLibrary maintains TWO tiers — defaults (shipped, read-only at
// the library's behaviour level) and custom (per-project, fully mutable).
// Every mutating verb must refuse on a defaults-tier id, returning
// Null without pushing a command. The UI's escape hatch is
// `duplicate` — the user duplicates a default to get a custom-tier
// copy they can edit. The Scriptable mirrors that same affordance.
//
// Read queries work on both tiers: `count`, `all_ids`, `find_by_name`,
// proxy `name`/`color`/`is_default`/`iid` all resolve through
// SwatchLibrary's find_swatch (custom-first, then defaults).
//
// Custom-vs-default partition is exposed through two parallel set
// queries: `custom_ids` and `default_ids`. Scripts that want to iterate
// only the editable swatches use `custom_ids`; scripts that want a full
// picture use `all_ids`.
//
// ── Verb / query surface ────────────────────────────────────────────────────
//
// Verb names come from the panel's own UI vocabulary (kebab menu +
// chip context menu): `new`, `delete`, `duplicate`, `rename`. The proxy
// surface (`rename`, `color`) mirrors the open-edit popover's two
// editable fields (the inline name entry + the colour picker). Same
// "verbs come from the application's UI vocabulary, not API conventions"
// rule established for LayersScriptable in s217 m2.
//
// COLLECTION VERBS (on `swatches`):
//   new                                  — create a new custom-tier
//                                          solid swatch. Optional args:
//                                          name (string) and hex
//                                          (string, "#rrggbb" or
//                                          "#rrggbbaa"). Defaults to
//                                          empty name and pure black.
//                                          Returns the new id, or "" on
//                                          failure (library minted an
//                                          empty id, which signals a
//                                          collision — defensive only,
//                                          shouldn't happen).
//   delete "<id>"                        — remove a custom-tier swatch.
//                                          Refuses on defaults-tier ids
//                                          (returns Null without
//                                          pushing). Pushes
//                                          RemoveSwatchCommand which
//                                          snapshots palette membership
//                                          for round-trip undo.
//   duplicate "<id>"                     — duplicate a swatch from
//                                          either tier into the custom
//                                          tier. The duplicate has a
//                                          fresh id; name gets " copy"
//                                          appended (empty name stays
//                                          empty — same shape as the
//                                          panel's ctx-duplicate
//                                          handler). Returns the new
//                                          id, or "" on failure.
//   rename "<id>" "<name>"               — rename a custom-tier swatch.
//                                          Refuses on defaults-tier ids.
//                                          Pushes EditSwatchCommand
//                                          with colour unchanged
//                                          (before == after) and
//                                          name diffed. Same shape as
//                                          the panel's ctx-rename.
//                                          Empty name is meaningful —
//                                          it clears the user-supplied
//                                          name; UI falls back to the
//                                          auto-derived region name
//                                          (mirrors the panel rule).
//
// COLLECTION QUERIES (on `swatches`):
//   count                                — total number of swatches
//                                          across BOTH tiers
//                                          (defaults + custom). Matches
//                                          SwatchLibrary::swatch_count.
//   all_ids                              — comma-separated SwatchIds in
//                                          library iteration order
//                                          (defaults first, then
//                                          custom). Same sentinel shape
//                                          as LayersScriptable's
//                                          all_iids — string return
//                                          waiting for the future
//                                          `foreach` grammar.
//   custom_ids                           — comma-separated SwatchIds in
//                                          the custom tier only. Useful
//                                          when scripts want to iterate
//                                          editable swatches without
//                                          tripping defaults-tier
//                                          refusal on every mutating
//                                          verb.
//   default_ids                          — comma-separated SwatchIds in
//                                          the defaults tier only.
//                                          Read-only — every mutating
//                                          verb refuses on these.
//   find_by_name "<name>"                — id of the first swatch (any
//                                          tier) whose header.name
//                                          exactly matches. Same
//                                          first-hit contract as
//                                          LayersScriptable
//                                          ::find_by_name. Returns ""
//                                          on miss. Names aren't unique
//                                          by construction (the library
//                                          enforces uniqueness on ids,
//                                          not names); duplicates
//                                          return the first iteration
//                                          hit.
//
// ELEMENT VERBS (on `swatches.<id>`):
//   rename "<name>"                      — write swatch.header.name.
//                                          Refuses on defaults-tier
//                                          (returns Null). Pushes
//                                          EditSwatchCommand with
//                                          colour unchanged, name
//                                          diffed.
//   color "<hex>"                        — write the swatch's colour
//                                          from a hex string. Refuses
//                                          on defaults-tier. Pushes
//                                          EditSwatchCommand with name
//                                          unchanged, colour diffed.
//                                          Invalid hex (parse failure)
//                                          is silently rejected, same
//                                          shape as
//                                          GuidesScriptable's color
//                                          verb. SolidSwatch only;
//                                          future gradient swatches
//                                          will need their own verbs.
//
// ELEMENT QUERIES (on `swatches.<id>`):
//   name           — string       swatch.header.name
//   color          — string       solid swatch's colour as "#rrggbb"
//                                  or "#rrggbbaa". Returns "" for
//                                  non-solid (future gradient) variants
//                                  — same defensive "feature not yet
//                                  implemented" empty as
//                                  EditSwatchCommand::apply uses.
//   is_default     — bool         true iff the swatch lives in the
//                                  defaults tier. Useful for scripts
//                                  that want to gate their own
//                                  mutations on whether the target is
//                                  editable.
//   iid            — string       swatch id (the addressing key). Named
//                                  `iid` for shape-symmetry with
//                                  LayerProxy / GuideProxy — those
//                                  return the SceneNode's internal_id;
//                                  this returns the SwatchId. Both are
//                                  "the per-instance key for this
//                                  proxy"; the canon convention reads
//                                  cleanly with the same word.
//
// ── Out of scope for s221 m1 ────────────────────────────────────────────────
//
// **Palettes** — SwatchLibrary's palette CRUD (add_palette,
// add_to_palette, remove_from_palette, reorder_in_palette,
// duplicate_palette, plus the active_palette / recents per-project
// state) is NOT exposed by this Scriptable. Exposing it would either
// inflate SwatchesScriptable significantly or need a separate
// `palettes` Scriptable. Deferred until after this lands and the shape
// has settled.
//
// **Gradient swatch variants** — Swatch is a std::variant that today
// holds only SolidSwatch. Future GradientSwatch / PathBlendSwatch
// cases will need their own proxy element verbs (stops, angle, etc.).
// This Scriptable handles only SolidSwatch; non-solid variants return
// Null on `color` writes and "" on `color` reads. The variant case
// guard is per-verb (std::get_if), not at the can_resolve level — a
// gradient swatch IS still addressable as `swatches.<id>`, queries
// like `name` and `is_default` work; only the colour ones don't.
//
// ── On active-project scope ─────────────────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable: all verbs operate
// on `project->swatches` (the library hangs off the project, NOT off
// any specific document). Swatches are project-scoped state; the
// "active document" doesn't enter into swatch lookup. This is a
// simplification compared to layers (which require active_doc) — the
// `swatches.<id>` proxy needs no doc routing at all.

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

class SwatchesScriptable : public Scriptable {
public:
    using ProjectGetter = std::function<Curvz::CurvzProject*()>;

    // Registers as "swatches" via the Scriptable base ctor. The project
    // getter is called fresh on every verb invocation — never cached —
    // so MainWindow can swap its m_project unique_ptr freely (open
    // another project, close project) without invalidating us. The
    // getter is allowed to return nullptr; verbs degrade gracefully
    // (count returns 0, all_ids returns "", etc.).
    //
    // `history` is non-owning and may be nullptr (test harness path).
    // Lives at MainWindow scope alongside the project unique_ptr; the
    // CommandHistory object itself has a stable address (it's a value
    // member, not a pointer that gets swapped), so storing a raw
    // pointer is safe for the SwatchesScriptable's lifetime. Unlike
    // GuidesScriptable's captured-but-unused history pointer, this one
    // is DEREFERENCED on every mutating verb (see "Commands and undo"
    // in the design block above).
    SwatchesScriptable(ProjectGetter get_project,
                       Curvz::CommandHistory* history);
    ~SwatchesScriptable() override = default;

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
