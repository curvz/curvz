// scripting/StylesScriptable.hpp ─────────────────────────────────────────────
//
// s222 m1 — fourth row-bound model Scriptable, second library-collection
// Scriptable. Direct application of the pattern s221 m1 established for
// swatches; the styles commands already exist from S81 m4c-3
// (AddStyleCommand / UpdateStyleCommand / RemoveStyleCommand) and are
// used by StylesPanel today.
//
// Wraps the active project's `StyleLibrary` as a collection Scriptable
// under abbrev `styles`, materialising transient `styles.<id>` proxies
// for per-instance operations via the s216 m1 router hooks
// (Scriptable::can_resolve / proxy_for).
//
// ── How styles relate to swatches (and where they differ) ───────────────────
//
// Same shape as SwatchesScriptable:
//   * Library collection, not SceneNode collection — addressing is
//     `project->styles.find_style(id)`, no doc-tree walk.
//   * Two tiers — app (hardcoded at StyleLibrary construction, read-only)
//     and user (per-project, fully mutable). Mutating verbs refuse on
//     app-tier ids; the user-facing escape hatch is `duplicate`.
//   * Execute-then-push commands — Add/Update/RemoveStyleCommand all
//     carry the mutation in their execute() body, same as the swatch
//     equivalents.
//
// Differences from swatches:
//   * Different terminology. StyleLibrary uses "app" / "user" everywhere
//     (app_styles_in_category, user_styles_in_category, app_categories,
//     user_categories, app_style_count, user_style_count) and Style.hpp
//     uses is_built_in() for the discriminant. The Scriptable mirrors
//     the model's vocabulary: `app_ids` / `user_ids` (not custom_ids /
//     default_ids) and `is_built_in` (not is_default). "Verbs come from
//     the application's UI vocabulary" applied to the model's term-base.
//   * RemoveStyleCommand takes a full Style snapshot, not just the id —
//     so `delete` reads the live Style first and passes it by value.
//     SwatchesScriptable just passes the id to RemoveSwatchCommand which
//     does its own snapshot work internally.
//   * UpdateStyleCommand takes full before/after Style snapshots, not a
//     name/colour diff bundle. The `rename` verb (and the s222 m1 `category`
//     verb) capture the live Style, build the after by mutating one field
//     of a copy, and push the pair. More memory than the swatch shape but
//     uniform with how StylesPanel itself builds its update commands.
//   * No `all_style_ids()` accessor on StyleLibrary today — the library
//     iterates by category (app_styles_in_category / user_styles_in_category)
//     to match the panel's chooser-driven UI. The Scriptable assembles
//     the flat id list by walking categories; see styles_collect_ids in
//     StylesScriptable.cpp.
//
// ── Panel visibility — after-mutation navigation (fix-1) ────────────────────
//
// The s221 fix-1 rationale called out a structural risk: a Scriptable
// that adds something to the library but doesn't include the panel-side
// step the panel itself does to make the new entry visible will produce
// a "command ran, nothing changed visually" mismatch. For swatches, the
// fix was to mirror SwatchesPanel::on_new_swatch's add_to_palette call —
// without it, the new swatch lives in the library but never appears in
// the panel grid (which renders the active palette's contents, not the
// full library).
//
// The styles panel ALSO renders a filtered subset — only the styles in
// the currently-selected category (m_active_category) appear in the
// chip grid. The first cut of this Scriptable shipped without an
// equivalent visibility step, on the reasoning that the styles panel's
// filtering was meaningfully different — the category dropdown
// rebuilds on every signal_style_added emit, so the new style's
// category appears as a dropdown option immediately, and the user can
// click to it. Library: fully populated. Dropdown: showing the new
// category. User experience: "I ran the script, the dropdown sits on
// the same category as before, and I see nothing happened."
//
// That last sentence is the actual user-facing outcome — the dropdown
// being _populated_ is not the same as the user _seeing_ the result.
// The s221 lesson generalises: library-collection Scriptables must
// drive panel visibility through whatever the panel uses to filter,
// not just through library state. The mechanism is different (swatches:
// add_to_palette; styles: set_active_category), but the principle is
// the same.
//
// **fix-1 (s222 m1):** after a successful `new` or `duplicate`, look
// up the new style's category and call
// `StylesPanel::set_active_category(cat, /*is_app_tier=*/false)`. The
// duplicate always lands in the user tier, and `new` always lands in
// the user tier (mutating verbs refuse on app-tier ids), so the
// is_app_tier flag is hardcoded false. The panel refresh inside
// set_active_category re-syncs the dropdown selection and rebuilds
// the chip grid with the new style visible.
//
// Mechanism — different from swatches:
//   * SwatchesScriptable fix-1: library-side. add_to_palette is a
//     SwatchLibrary method; the Scriptable mutates only library state,
//     and the panel observes via the same signal it already listens
//     on. No panel pointer needed.
//   * StylesScriptable fix-1: panel-side. set_active_category mutates
//     panel state (m_active_category, m_active_is_app_tier); the
//     library is fully populated already. So the Scriptable needs a
//     panel getter — same shape as the project getter (resolved fresh,
//     nullptr-tolerant for test-harness paths). The wiring lives in
//     MainWindow's construction site.
//
// The getter form (rather than a stored pointer) follows the same
// philosophy as the project getter: if MainWindow's StylesPanel
// reference is ever swapped, the getter lambda resolves the live
// pointer freshly. In practice m_styles is a value member with a
// stable lifetime, but the getter shape is uniform with the rest of
// the construction surface and costs almost nothing.
//
// Edge cases:
//   * Library rejected the add (lib.find_style(new_id) returns nullptr
//     after execute): skip navigation.
//   * Panel getter returns nullptr (test harness / pre-construction):
//     skip navigation. Library mutation already happened; the
//     additive navigation is the only thing that no-ops.
//   * `rename` and `category` (proxy or collection): no navigation.
//     These verbs operate on already-visible styles; the user's
//     viewport is already correct, and a forced category-switch on
//     `category` would actually be _worse_ (move-from-current-view
//     surprise). The panel's signal_style_changed listener handles
//     the chip re-render in place.
//   * Cross-doc / cross-project: the panel's library pointer is
//     swapped on project change. The Scriptable's project getter
//     returns the new project; the panel getter returns the same
//     panel (panel lifetime tracks MainWindow, not the project).
//     set_active_category writes panel state that will sit there until
//     the next refresh, which the new project's library will drive.
//     No corruption.
//
// Backlog: when ThemesScriptable lands (also library-collection;
// ThemeLibrary's panel similarly filters by category-like sections),
// the same after-mutation navigation step will likely apply. The
// panel-getter pattern this fix-1 establishes is the carry-forward
// shape.
//
// ── Addressing ──────────────────────────────────────────────────────────────
//
//   styles                  — the collection Scriptable. Set verbs and
//                             set queries. ONE registry entry per app
//                             session; held as a member by MainWindow,
//                             registered for the lifetime of the window.
//
//   styles.<id>             — per-instance proxy, materialised on demand
//                             by the collection. Reads / writes the
//                             specific style's name / category through
//                             the project's StyleLibrary. NOT registered
//                             — the registry only knows about `styles`.
//                             A `list` from the Scripter shows `styles`
//                             but never shows `styles.<id>` entries
//                             (same pilot success condition as
//                             layers/guides/swatches).
//
// ── Lifetime and the project pointer ────────────────────────────────────────
//
// Same shape as LayersScriptable / GuidesScriptable / SwatchesScriptable:
// a project getter (`std::function<CurvzProject*()>`) resolved fresh on
// every verb call, not a captured raw pointer. MainWindow's m_project
// unique_ptr gets reset and reassigned at runtime (close-project,
// load-project); the getter survives that, the Scriptable does not need
// re-registration.
//
// MainWindow constructs the StylesScriptable once (after m_project is
// initialised) and the registry holds the entry for the window's
// lifetime.
//
// ── Commands and undo ───────────────────────────────────────────────────────
//
// Every mutating style verb pushes a `CommandHistory` command. S81 m4c-3
// made all four CRUD operations undoable from the panel; this Scriptable
// rides on that exact same plumbing:
//
//   new / duplicate    → AddStyleCommand (standard ctor)
//   delete             → RemoveStyleCommand (full Style snapshot — see
//                        note above; the command's snapshot is the
//                        whole Style, not just an id)
//   rename             → UpdateStyleCommand carrying before/after with
//                        only header.name diffed
//   category (proxy)   → UpdateStyleCommand carrying before/after with
//                        only header.category diffed
//
// The push pattern is `cmd->execute(); m_history->push(std::move(cmd))`,
// matching StylesPanel::action_create_empty's on_committed callback
// (which is the closest in-tree precedent and itself the canonical
// "execute-then-push" example cited in SwatchesScriptable.cpp's design
// block). The Style commands all carry their mutation in execute(); we
// invoke execute() to do the mutation (and, for AddStyleCommand, to
// capture the minted id via m_assigned_id) before pushing.
//
// The `m_history` pointer is captured at construction and outlives the
// Scriptable (CommandHistory is a value member of MainWindow). Same
// non-owning lifetime story as the other model scriptables.
//
// ── App-tier guard ──────────────────────────────────────────────────────────
//
// Every mutating verb refuses on an app-tier id, returning Null without
// pushing a command. The UI's escape hatch is `duplicate` — the user
// duplicates an app style to get a user-tier copy they can edit. The
// Scriptable mirrors that same affordance.
//
// Read queries work on both tiers: `count`, `all_ids`, `find_by_name`,
// proxy `name`/`category`/`is_built_in`/`iid` all resolve through
// StyleLibrary's find_style (user-first, then app).
//
// App-vs-user partition is exposed through two parallel set queries:
// `user_ids` and `app_ids`. Scripts that want to iterate only the
// editable styles use `user_ids`; scripts that want a full picture use
// `all_ids`.
//
// ── Verb / query surface ────────────────────────────────────────────────────
//
// Verb names come from the panel's own UI vocabulary (kebab menu +
// chip context menu): `new`, `delete`, `duplicate`, `rename`, plus the
// `category` verb that mirrors the panel's "Category…" right-click
// item. Same "verbs come from the application's UI vocabulary, not API
// conventions" rule established for LayersScriptable in s217 m2.
//
// COLLECTION VERBS (on `styles`):
//   new                                  — create a new user-tier style.
//                                          Optional args:
//                                            args[0] = name (string,
//                                                      empty allowed)
//                                            args[1] = category (string,
//                                                      empty = uncategorised)
//                                          Returns the new id, or "" on
//                                          failure (library minted an
//                                          empty id; defensive only).
//                                          Defaults: empty name, empty
//                                          category, Paint::None fill,
//                                          default StrokeAppearance,
//                                          shadow disabled. This matches
//                                          StylesPanel::action_create_empty's
//                                          seed shape (sans the
//                                          panel-derived name and
//                                          category prefill, which are
//                                          UI conveniences the script
//                                          can replicate via args).
//   delete "<id>"                        — remove a user-tier style.
//                                          Refuses on app-tier ids
//                                          (returns Null without
//                                          pushing). Reads the live
//                                          Style first to snapshot it
//                                          into RemoveStyleCommand;
//                                          unlike the swatch path,
//                                          RemoveStyleCommand carries
//                                          the whole Style value (not
//                                          just an id) so undo can
//                                          re-add the full appearance.
//   duplicate "<id>"                     — duplicate a style from either
//                                          tier into the user tier. The
//                                          duplicate has a fresh id;
//                                          " copy" is appended to the
//                                          name (empty name stays
//                                          empty — same shape as the
//                                          panel's action_duplicate
//                                          handler). Category is
//                                          preserved verbatim from the
//                                          source; an app→user duplicate
//                                          inherits the app category
//                                          name ("Built-in"), which is
//                                          the same behaviour as the
//                                          panel's right-click
//                                          "Duplicate to user tier".
//                                          Returns the new id, or "" on
//                                          failure.
//   rename "<id>" "<name>"               — rename a user-tier style.
//                                          Refuses on app-tier ids.
//                                          Pushes UpdateStyleCommand
//                                          with full before/after
//                                          snapshots; only header.name
//                                          differs. Skips the push if
//                                          name is unchanged (matches
//                                          the panel's "no-op on
//                                          unchanged" guard).
//   category "<id>" "<cat>"              — set the category on a
//                                          user-tier style. Refuses on
//                                          app-tier ids. Pushes
//                                          UpdateStyleCommand with full
//                                          before/after snapshots; only
//                                          header.category differs.
//                                          Empty cat is meaningful — it
//                                          moves the style to the
//                                          "(uncategorised)" bucket.
//                                          Mirrors the panel's
//                                          action_set_category.
//   find_by_name "<name>"                — id of the first style (any
//                                          tier) whose header.name
//                                          exactly matches. Same
//                                          first-hit contract as
//                                          SwatchesScriptable::find_by_name.
//                                          Returns "" on miss. Names
//                                          aren't unique by construction
//                                          (the library enforces
//                                          uniqueness on ids, not
//                                          names); duplicates return
//                                          the first iteration hit.
//
// COLLECTION QUERIES (on `styles`):
//   count                                — total number of styles
//                                          across BOTH tiers (app +
//                                          user). Sum of app_style_count
//                                          and user_style_count.
//   all_ids                              — comma-separated StyleIds in
//                                          library iteration order (app
//                                          first, then user, each in
//                                          insertion order within tier).
//                                          Same sentinel shape as
//                                          SwatchesScriptable::all_ids
//                                          — string return waiting for
//                                          the future `foreach` grammar.
//   user_ids                             — comma-separated StyleIds in
//                                          the user tier only. Useful
//                                          when scripts want to iterate
//                                          only the editable styles
//                                          without tripping app-tier
//                                          refusal on every mutating
//                                          verb. Parallel to
//                                          SwatchesScriptable::custom_ids;
//                                          named after the StyleLibrary
//                                          term (`user_styles`,
//                                          `user_categories`,
//                                          `user_style_count`) not the
//                                          swatches term.
//   app_ids                              — comma-separated StyleIds in
//                                          the app tier only. Read-only
//                                          — every mutating verb
//                                          refuses on these. Parallel
//                                          to SwatchesScriptable::default_ids;
//                                          named after the StyleLibrary
//                                          term.
//
// ELEMENT VERBS (on `styles.<id>`):
//   rename "<name>"                      — write header.name. Refuses on
//                                          app-tier (returns Null).
//                                          Pushes UpdateStyleCommand.
//                                          Skip-no-op when name didn't
//                                          change.
//   category "<cat>"                     — write header.category. Refuses
//                                          on app-tier. Pushes
//                                          UpdateStyleCommand. Empty cat
//                                          is meaningful — moves to the
//                                          uncategorised bucket. Skip-
//                                          no-op when category didn't
//                                          change.
//
// ELEMENT QUERIES (on `styles.<id>`):
//   name           — string       style.header.name
//   category       — string       style.header.category (empty string
//                                  for uncategorised; same convention
//                                  as the panel's "(uncategorised)"
//                                  display label).
//   is_built_in    — bool         true iff the style lives in the app
//                                  tier. Named after Style.hpp's
//                                  is_built_in() free function and
//                                  StyleLibrary::is_built_in() member
//                                  — the canonical discriminant
//                                  vocabulary in the styles model.
//                                  Compare SwatchesScriptable's
//                                  `is_default`, which follows the
//                                  swatches model's vocabulary.
//   iid            — string       style id (the addressing key). Named
//                                  `iid` for shape-symmetry with
//                                  LayerProxy / GuideProxy / SwatchProxy
//                                  — every per-instance proxy returns
//                                  its addressing key under `iid`.
//
// ── Out of scope for s222 m1 ────────────────────────────────────────────────
//
// **Appearance fields** — Style carries a Paint fill, a StrokeAppearance
// (paint + width + cap + join + miter), and a ShadowAppearance (enabled
// + dx + dy + blur + colour + opacity). The proxy surface in m1 covers
// only the header fields (name, category). Per-field verbs for fill /
// stroke / shadow can come in s223 or later; the pattern is established
// and adding `stroke_width`, `shadow_enabled`, etc. is mechanical once
// the scope is opened. The handoff for s221 → s222 explicitly named a
// header-only first cut as acceptable.
//
// The first place a richer surface would matter is for scripts that
// programmatically construct styles from data (e.g. import a palette
// definition file and create one style per entry). Today such a script
// would create the style then bind it to an object and use the
// inspector/UI to set the appearance — clunky but workable; the
// follow-up Scriptable extension closes that gap.
//
// **Bindings** — SceneNode::bound_style ties scene-tree objects to
// library styles; that's a SceneNode-level concern, not a library-level
// one. Scripted binding will need its own surface (probably on the
// per-scene-node proxy machinery that doesn't exist yet); the Style
// library Scriptable doesn't touch bindings.
//
// **Categories as first-class entities** — categories are headers in
// the panel UI, not library entries. There's no "category list" to
// CRUD; categories exist iff at least one style has that header.category
// string. The Scriptable's `category` verb mutates a style's category,
// which implicitly creates / destroys the category from the panel's
// rendering perspective. Mass-renames of a category across many styles
// (the panel has an action for this) would compose multiple
// UpdateStyleCommands; if a script needs that it can iterate
// `user_ids` and call `category` per id, but a single atomic operation
// would need a CompositeCommand wrapper — deferred.
//
// ── On active-project scope ─────────────────────────────────────────────────
//
// Same shape as SwatchesScriptable: all verbs operate on
// `project->styles` (the library hangs off the project, NOT off any
// specific document). Styles are project-scoped state; the "active
// document" doesn't enter into style lookup. The `styles.<id>` proxy
// needs no doc routing at all.

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
class  StylesPanel;
}

namespace curvz::scripting {

class StylesScriptable : public Scriptable {
public:
    using ProjectGetter = std::function<Curvz::CurvzProject*()>;
    // s222 m1 fix-1 — panel-getter for the after-mutation navigation
    // step (see "Panel visibility" block above). Resolved fresh on
    // every call site that needs it; may return nullptr (the test
    // harness path or any state where the panel isn't constructed
    // yet). nullptr returns disable the navigation; the library
    // mutation still happens, the command still pushes — the
    // navigation is the additive "make it visible" step on top.
    using PanelGetter   = std::function<Curvz::StylesPanel*()>;

    // Registers as "styles" via the Scriptable base ctor. The project
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
    // pointer is safe for the StylesScriptable's lifetime. Every
    // mutating verb DEREFERENCES this pointer (same shape as
    // SwatchesScriptable; different from GuidesScriptable's
    // captured-but-unused pointer).
    //
    // `get_panel` is the s222 m1 fix-1 hook for after-mutation panel
    // navigation. Same signature shape as `get_project` — resolved
    // fresh on every call, may return nullptr (test harness / pre-
    // construction states). Currently exercised only by `new` and
    // `duplicate`, which set the panel's active category to the
    // new style's category so the user immediately sees the result
    // of the script. See the "Panel visibility" block above for the
    // full rationale and how this differs from the swatches fix-1
    // (which is a library-side fix, not a panel-side one).
    StylesScriptable(ProjectGetter get_project,
                     Curvz::CommandHistory* history,
                     PanelGetter get_panel = nullptr);
    ~StylesScriptable() override = default;

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
    PanelGetter            m_get_panel; // s222 m1 fix-1 — nullable
};

} // namespace curvz::scripting
